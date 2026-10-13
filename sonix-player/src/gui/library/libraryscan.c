#include "libraryscan.h"

#include <dirent.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/library/music.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/main_menu.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/topbar.h"
#include "src/gui/shell/toast.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/podcastdl.h"
#include "src/system/core/lang.h"
#include "src/system/library/library.h"
#include "src/system/device/power.h"

// Fast enough that the number never looks stuck, slow enough that redrawing it
// costs nothing next to reading tags off a card.
#define SCAN_POLL_MS 200

lv_obj_t *libraryscan_screen;

static lv_obj_t *count_label;
static lv_obj_t *status_label;
static lv_obj_t *ok_button;
static lv_obj_t *cancel_button;
static lv_timer_t *poll_timer;

static const char *sd_root;

static void show_finished(int found) {
	lv_label_set_text_fmt(count_label, "%d", found);
	lv_label_set_text(status_label, found == 1 ? tr("libraryscan_track_found") : tr("libraryscan_tracks_found"));

	lv_obj_set_hidden(cancel_button, true);
	lv_obj_set_hidden(ok_button, false);

	// The scan is over; the screen may go back to timing out.
	power_hold_screen_on(false);
}

static void poll_cb(lv_timer_t *timer) {
	int found = library_scan_found();
	lv_label_set_text_fmt(count_label, "%d", found);

	if (library_scan_running()) {
		return;
	}

	lv_timer_pause(timer);
	show_finished(found);
}

// Back to the Music page, which now shows the library, with the menu as the
// only page behind it: walking back through a finished scan makes no sense.
static void back_to_music(void) {
	screen_history_reset();
	switch_screen_no_history(music_screen);
}

static void ok_cb(lv_event_t *e) {
	(void)e;
	back_to_music();
}

// Stopping is not throwing away: the scan thread winds up at the next file and
// commits everything it has already read, so the index holds exactly the
// tracks the counter was showing.
static void cancel_cb(lv_event_t *e) {
	(void)e;

	library_scan_stop();
	lv_timer_pause(poll_timer);
	power_hold_screen_on(false);

	back_to_music();
}

void libraryscan_begin(void) {
	lv_label_set_text(count_label, "0");
	lv_label_set_text(status_label, tr("libraryscan_tracks_found"));
	lv_obj_set_hidden(ok_button, true);
	lv_obj_set_hidden(cancel_button, false);

	if (!library_scan_start(sd_root)) {
		// Nothing to scan (no card, or a scan is somehow already going).
		lv_label_set_text(status_label, tr("no_card_to_scan"));
		lv_obj_set_hidden(cancel_button, true);
		lv_obj_set_hidden(ok_button, false);
		return;
	}

	// Reading a card takes minutes and nobody is touching the screen while it
	// happens; the idle timer must not blank the panel halfway through.
	power_hold_screen_on(true);

	lv_timer_reset(poll_timer);
	lv_timer_resume(poll_timer);
}

// ---------------------------------------------------------------------------
// Which folders: the card's top-level folders, ticked, before the scan starts
// ---------------------------------------------------------------------------

#define PICK_CHROME_H 250 // the card's title, note, buttons and padding
// Rows are made this many at a time, more as the list nears its end.
#define PICK_BATCH 30

static gui_config_t *pick_cfg;
static lv_obj_t *pick_veil;
static lv_obj_t *pick_list;
static lv_obj_t *pick_scan_btn;
static char **pick_names;
static bool *pick_on;
static lv_obj_t **pick_marks;
static int pick_count;
static int pick_capacity;
static int pick_built; // rows made so far, the first pick_built names

static int name_cmp(const void *a, const void *b) { return strcasecmp(*(const char *const *)a, *(const char *const *)b); }

static void pick_clear(void) {
	for (int i = 0; i < pick_count; i++) {
		free(pick_names[i]);
	}
	pick_count = 0;
	pick_built = 0;
}

static bool pick_add(const char *name) {
	if (pick_count == pick_capacity) {
		int grown = pick_capacity ? pick_capacity * 2 : 64;
		char **names = realloc(pick_names, (size_t)grown * sizeof(*names));
		if (names) {
			pick_names = names;
		}
		bool *on = realloc(pick_on, (size_t)grown * sizeof(*on));
		if (on) {
			pick_on = on;
		}
		lv_obj_t **marks = realloc(pick_marks, (size_t)grown * sizeof(*marks));
		if (marks) {
			pick_marks = marks;
		}
		if (!names || !on || !marks) {
			return false;
		}
		pick_capacity = grown;
	}
	pick_names[pick_count] = strdup(name);
	if (!pick_names[pick_count]) {
		return false;
	}
	pick_on[pick_count] = false;
	pick_marks[pick_count] = NULL;
	pick_count++;
	return true;
}

// The folders at the root of the card, alphabetically: not the hidden ones,
// not the ones a desktop leaves behind, and not the audiobook folder or
// Podcast, which the music scan never reads. An audiobook folder deeper down
// ("Books/Audio") leaves its parent on the list: the parent may hold music too,
// and the scan passes over the audiobooks inside it by itself.
static void pick_read_folders(void) {
	pick_clear();
	DIR *dir = sd_root ? opendir(sd_root) : NULL;
	if (!dir) {
		return;
	}
	char audiobooks[AUDIOBOOKDB_FOLDER_MAX];
	audiobookdb_folder(audiobooks, sizeof(audiobooks));
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.' || playlist_is_junk_name(de->d_name) ||
			audiobookdb_folder_matches(audiobooks, de->d_name) || strcasecmp(de->d_name, PODCASTDL_FOLDER) == 0) {
			continue;
		}
		char path[768];
		if (snprintf(path, sizeof(path), "%s/%s", sd_root, de->d_name) >= (int)sizeof(path)) {
			continue;
		}
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
			continue;
		}
		if (!pick_add(de->d_name)) {
			break;
		}
	}
	closedir(dir);
	if (pick_count > 1) {
		qsort(pick_names, (size_t)pick_count, sizeof(pick_names[0]), name_cmp);
	}
}

// Ticked: the folders saved last time, or every folder when nothing was saved.
static void pick_load_selection(void) {
	int saved_count = 0;
	char **saved = library_scan_folders(&saved_count);
	for (int i = 0; i < pick_count; i++) {
		pick_on[i] = saved_count == 0;
		for (int j = 0; j < saved_count && !pick_on[i]; j++) {
			pick_on[i] = strcasecmp(pick_names[i], saved[j]) == 0;
		}
	}
	library_scan_folders_free(saved, saved_count);
}

static void pick_paint(void) {
	int on = 0;
	for (int i = 0; i < pick_count; i++) {
		if (i < pick_built && pick_marks[i]) {
			lv_obj_set_style_image_opa(pick_marks[i], pick_on[i] ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
		}
		on += pick_on[i];
	}
	lv_obj_set_style_opa(pick_scan_btn, (pick_count == 0 || on > 0) ? LV_OPA_COVER : LV_OPA_40, 0);
}

static void pick_row_cb(lv_event_t *e) {
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	if (i >= 0 && i < pick_count) {
		pick_on[i] = !pick_on[i];
		pick_paint();
	}
}

static void pick_close(void) { lv_obj_set_hidden(pick_veil, true); }

static void pick_veil_cb(lv_event_t *e) {
	if (lv_event_get_target(e) == pick_veil) {
		pick_close();
	}
}

static void pick_cancel_cb(lv_event_t *e) {
	(void)e;
	pick_close();
}

// Every folder ticked is the whole card, the files at its root included.
static void pick_scan_cb(lv_event_t *e) {
	(void)e;
	const char **chosen = malloc((size_t)(pick_count ? pick_count : 1) * sizeof(*chosen));
	if (!chosen) {
		return;
	}
	int count = 0;
	for (int i = 0; i < pick_count; i++) {
		if (pick_on[i]) {
			chosen[count++] = pick_names[i];
		}
	}
	if (pick_count > 0 && count == 0) {
		free(chosen);
		toast_error(tr("libraryscan_choose_a_folder"));
		return;
	}
	library_scan_folders_set(chosen, count == pick_count ? 0 : count);
	free(chosen);
	pick_close();
	switch_screen(libraryscan_screen);
}

static lv_obj_t *pick_button(lv_obj_t *parent, const char *text, bool accent, lv_event_cb_t cb) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_height(btn, 56);
	lv_obj_set_flex_grow(btn, 1);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_set_style_bg_color(btn, accent ? theme()->accent : theme()->surface_pressed, 0);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, tr(text));
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_obj_set_style_text_color(label, accent ? lv_color_white() : theme()->text_primary, 0);
	lv_obj_center(label);
	return btn;
}

static void pick_scroll_cb(lv_event_t *e);

static void pick_build(void) {
	gui_config_t *cfg = pick_cfg;
	pick_veil = lv_obj_create(lv_layer_top());
	lv_obj_set_size(pick_veil, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_color(pick_veil, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(pick_veil, LV_OPA_60, 0);
	lv_obj_set_style_border_width(pick_veil, 0, 0);
	lv_obj_set_style_radius(pick_veil, 0, 0);
	lv_obj_set_style_pad_all(pick_veil, 0, 0);
	lv_obj_set_scrollable(pick_veil, false);
	lv_obj_set_clickable(pick_veil, true);
	lv_obj_set_hidden(pick_veil, true);
	lv_obj_add_event_cb(pick_veil, pick_veil_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *card = lv_obj_create(pick_veil);
	lv_obj_set_size(card, cfg->screen_width - 2 * cfg->padding, LV_SIZE_CONTENT);
	lv_obj_set_style_max_height(card, cfg->screen_height - cfg->top_bar_height - 2 * cfg->padding, 0);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 16, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, 20, 0);
	lv_obj_set_style_pad_row(card, 12, 0);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_clickable(card, true);
	lv_obj_set_event_bubble(card, false);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_align(card, LV_ALIGN_CENTER, 0, cfg->top_bar_height / 2);

	lv_obj_t *title = lv_label_create(card);
	lv_label_set_text(title, tr("musicsettings_scan_the_music_library"));
	lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(title, lv_pct(100));
	lv_obj_add_style(title, &theme_style_text, 0);
	lv_obj_set_style_text_font(title, &font_ui_24, 0);

	lv_obj_t *note = lv_label_create(card);
	lv_label_set_text(note, tr("libraryscan_choose_folders_note"));
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_18, 0);

	// The folders scroll; the title and the buttons stay put.
	pick_list = lv_obj_create(card);
	lv_obj_remove_style_all(pick_list);
	lv_obj_set_width(pick_list, lv_pct(100));
	lv_obj_set_height(pick_list, LV_SIZE_CONTENT);
	// What the panel leaves once the title, the note and the buttons are in.
	lv_obj_set_style_max_height(pick_list, cfg->screen_height - cfg->top_bar_height - 2 * cfg->padding - PICK_CHROME_H,
								0);
	lv_obj_set_style_pad_row(pick_list, 6, 0);
	lv_obj_set_flex_flow(pick_list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scroll_dir(pick_list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(pick_list, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_add_event_cb(pick_list, pick_scroll_cb, LV_EVENT_SCROLL, NULL);

	lv_obj_t *buttons = lv_obj_create(card);
	lv_obj_remove_style_all(buttons);
	lv_obj_set_size(buttons, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_column(buttons, 12, 0);
	lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
	pick_button(buttons, "cancel", false, pick_cancel_cb);
	pick_scan_btn = pick_button(buttons, "scan", true, pick_scan_cb);
}

// Makes up to `n` more rows, after the ones already there.
static void pick_append(int n) {
	int last = pick_built + n < pick_count ? pick_built + n : pick_count;
	for (int i = pick_built; i < last; i++) {
		lv_obj_t *row = lv_btn_create(pick_list);
		lv_obj_set_size(row, lv_pct(100), 60);
		lv_obj_set_style_bg_color(row, theme()->surface_pressed, 0);
		lv_obj_set_style_radius(row, 10, 0);
		lv_obj_set_style_shadow_width(row, 0, 0);
		lv_obj_set_style_border_width(row, 0, 0);
		lv_obj_set_style_pad_hor(row, 16, 0);
		lv_obj_set_style_pad_column(row, 10, 0);
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_scroll_on_focus(row, true);
		lv_obj_add_event_cb(row, pick_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

		lv_obj_t *folder = lv_image_create(row);
		lv_image_set_src(folder, &icon_folder);
		lv_obj_add_style(folder, &theme_style_icon, 0);

		lv_obj_t *label = lv_label_create(row);
		lv_label_set_text(label, pick_names[i]);
		lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
		lv_obj_set_flex_grow(label, 1);
		lv_obj_set_height(label, lv_font_get_line_height(&font_ui_22));
		lv_obj_add_style(label, &theme_style_text, 0);
		lv_obj_set_style_text_font(label, &font_ui_22, 0);

		pick_marks[i] = lv_image_create(row);
		lv_image_set_src(pick_marks[i], &icon_check);
		lv_obj_add_style(pick_marks[i], &theme_style_icon, 0);
		lv_obj_set_style_image_recolor(pick_marks[i], theme()->accent, 0);
		lv_obj_set_style_image_recolor_opa(pick_marks[i], LV_OPA_COVER, 0);
		lv_obj_set_style_image_opa(pick_marks[i], pick_on[i] ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
	}
	pick_built = last;
}

// Within a screenful of the last row made, the next batch.
static void pick_scroll_cb(lv_event_t *e) {
	(void)e;
	if (pick_built < pick_count && lv_obj_get_scroll_bottom(pick_list) < lv_obj_get_height(pick_list)) {
		pick_append(PICK_BATCH);
	}
}

static void pick_fill(void) {
	lv_obj_clean(pick_list);
	pick_built = 0;
	pick_append(PICK_BATCH);
}

void libraryscan_choose_folders(void) {
	if (!pick_veil) {
		pick_build();
	}
	pick_read_folders();
	pick_load_selection();
	pick_fill();
	lv_obj_scroll_to_y(pick_list, 0, LV_ANIM_OFF);
	pick_paint();
	lv_obj_set_hidden(pick_veil, false);
	lv_obj_move_foreground(pick_veil);
}

// Both buttons sit at the same place at the bottom of the screen; only one is
// ever visible.
static lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_color_t colour, lv_event_cb_t cb,
							 gui_config_t *cfg) {
	lv_obj_t *button = lv_btn_create(parent);
	lv_obj_set_size(button, 240, 68);
	lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, -(cfg->padding * 2));
	lv_obj_add_style(button, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_bg_color(button, colour, 0);
	lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0); // Adwaita pill button
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *label = lv_label_create(button);
	lv_label_set_text(label, tr(text));
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	lv_obj_set_style_text_color(label, lv_color_white(), 0);
	lv_obj_center(label);

	return button;
}

// ---------------------------------------------------------------------------
// What the user is told about the runs on the scan thread
//
// Detect changes works in the background: a glyph in the status bar while it
// looks over the card, a check mark for a few seconds when it is done, and no
// notice either way. Filing the index again after the scan options changed was
// asked for on that page, so it keeps its notice and its outcome; filed again
// on its own, at the end of a Detect changes run, it says nothing.
// ---------------------------------------------------------------------------

typedef struct {
	library_update_event_t event;
	int added;
	int removed;
	int updated;
} update_note_t;

static bool checking;	   // a Detect changes run is under way
static bool reorganizing;  // library_reorganize() put its notice up

static void update_note_cb(void *user) {
	update_note_t *note = user;

	switch (note->event) {
	case LIBRARY_UPDATE_LOOKING:
		checking = true;
		topbar_set_library_check(TOPBAR_LIBRARY_CHECKING);
		break;

	case LIBRARY_UPDATE_ADDING:
		break;

	case LIBRARY_UPDATE_FINISHED:
		checking = false;
		topbar_set_library_check(TOPBAR_LIBRARY_CHECKED);
		break;

	case LIBRARY_UPDATE_STOPPED:
		if (reorganizing) {
			reorganizing = false;
			toast_busy_end();
		} else if (checking) {
			checking = false;
			topbar_set_library_check(TOPBAR_LIBRARY_IDLE);
		}
		break;

	case LIBRARY_UPDATE_REORGANIZING:
		reorganizing = true;
		toast_busy_dismissable("libraryscan_reorganizing");
		break;

	case LIBRARY_UPDATE_REORGANIZED:
		if (reorganizing) {
			reorganizing = false;
			toast_busy_end();
			toast_success("libraryscan_reorganized");
		}
		break;

	case LIBRARY_UPDATE_SCANNED:
		// The first scan there has ever been turns Detect changes on, unless it
		// was already set either way.
		if (!library_detect_changes_chosen()) {
			library_set_detect_changes(true);
		}
		break;
	}
	free(note);
}

// On the scan thread: everything it says goes over to the interface thread.
static void update_listener(library_update_event_t event, int added, int removed, int updated) {
	update_note_t *note = malloc(sizeof(*note));
	if (!note) {
		return;
	}
	note->event = event;
	note->added = added;
	note->removed = removed;
	note->updated = updated;
	if (!gui_post(update_note_cb, note)) {
		free(note);
	}
}

// How long after startup the card is looked over: past the first screen and
// the track being restored, which want the card first.
#define BOOT_CHECK_DELAY_MS 5000

// What library_card_returned() does for a card that comes back: Detect changes
// when it is on, and the index filed again if it was filed under other
// settings. A card mounted only after startup goes through it when it is
// attached instead (storage), and finds the library closed here.
static void boot_check_cb(lv_timer_t *timer) {
	(void)timer;
	// A library scanned before Detect changes came on by itself gets it the
	// same way, unless it was set either way since.
	if (!library_detect_changes_chosen() && library_is_open() && library_track_count() > 0) {
		library_set_detect_changes(true);
	}
	library_card_returned(sd_root);
}

void libraryscan_init(gui_config_t *cfg) {
	sd_root = cfg->sd_root_path;
	pick_cfg = cfg;
	library_set_update_listener(update_listener);

	lv_obj_add_style(libraryscan_screen, &theme_style_screen, 0);

	// Centred: this page has no back chevron beside it to align with.
	lv_obj_t *title = settingsrow_title(libraryscan_screen, cfg, "scan_2");
	lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

	int content_top = settingsrow_content_top(cfg);

	lv_obj_t *container = lv_obj_create(libraryscan_screen);
	lv_obj_set_size(container, lv_pct(100), cfg->screen_height - content_top);
	lv_obj_align(container, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(container, 0, 0);
	lv_obj_set_style_border_width(container, 0, 0);
	lv_obj_set_style_radius(container, 0, 0);
	lv_obj_set_style_pad_all(container, cfg->padding, 0);
	lv_obj_set_style_pad_gap(container, 10, 0);
	lv_obj_set_scrollable(container, false);
	lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *note = lv_image_create(container);
	lv_image_set_src(note, &icon_music_note);
	lv_obj_add_style(note, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(note, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_bottom(note, 16, 0);

	// The count is the whole point of the page, so it gets the accent colour
	// and the largest type on it.
	count_label = lv_label_create(container);
	lv_label_set_text(count_label, "0");
	lv_obj_set_style_text_color(count_label, theme()->accent, 0);
	lv_obj_set_style_text_font(count_label, &font_ui_32, 0);

	status_label = lv_label_create(container);
	lv_label_set_text(status_label, tr("libraryscan_tracks_found"));
	lv_obj_add_style(status_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(status_label, &font_ui_26, 0);

	// The two buttons share the foot of the page: cancel while the scan runs, OK
	// once it is done.
	cancel_button = make_button(libraryscan_screen, "cancel", lv_color_make(210, 66, 58), cancel_cb, cfg);
	ok_button = make_button(libraryscan_screen, "ok", theme()->accent, ok_cb, cfg);
	lv_obj_set_hidden(ok_button, true);

	poll_timer = lv_timer_create(poll_cb, SCAN_POLL_MS, NULL);
	lv_timer_pause(poll_timer);

	// The card may have been written while the player was off.
	lv_timer_t *boot_check = lv_timer_create(boot_check_cb, BOOT_CHECK_DELAY_MS, NULL);
	lv_timer_set_repeat_count(boot_check, 1);
}

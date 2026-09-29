#include "libraryscan.h"

#include <dirent.h>
#include <stdio.h>
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
#include "src/gui/shell/toast.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/podcastdl.h"
#include "src/system/core/lang.h"
#include "src/system/library/library.h"
#include "src/system/device/power.h"
#include "src/gui/shell/uiscale.h"

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

	lv_obj_add_flag(cancel_button, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag(ok_button, LV_OBJ_FLAG_HIDDEN);

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
	lv_obj_add_flag(ok_button, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag(cancel_button, LV_OBJ_FLAG_HIDDEN);

	if (!library_scan_start(sd_root)) {
		// Nothing to scan (no card, or a scan is somehow already going).
		lv_label_set_text(status_label, tr("no_card_to_scan"));
		lv_obj_add_flag(cancel_button, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(ok_button, LV_OBJ_FLAG_HIDDEN);
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

#define PICK_MAX 64
#define PICK_CHROME_H ui_px(250) // the card's title, note, buttons and padding

static gui_config_t *pick_cfg;
static lv_obj_t *pick_veil;
static lv_obj_t *pick_list;
static lv_obj_t *pick_scan_btn;
static char pick_names[PICK_MAX][256];
static bool pick_on[PICK_MAX];
static lv_obj_t *pick_marks[PICK_MAX];
static int pick_count;

static int name_cmp(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

// The folders at the root of the card, alphabetically: not the hidden ones,
// not the ones a desktop leaves behind, and not Audiobooks or Podcast, which
// the music scan never reads.
static void pick_read_folders(void) {
	pick_count = 0;
	DIR *dir = sd_root ? opendir(sd_root) : NULL;
	if (!dir) {
		return;
	}
	struct dirent *de;
	while ((de = readdir(dir)) != NULL && pick_count < PICK_MAX) {
		if (de->d_name[0] == '.' || playlist_is_junk_name(de->d_name) ||
			strcasecmp(de->d_name, AUDIOBOOKDB_FOLDER) == 0 ||
			strcasecmp(de->d_name, PODCASTDL_FOLDER) == 0 || strlen(de->d_name) >= sizeof(pick_names[0])) {
			continue;
		}
		char path[768];
		snprintf(path, sizeof(path), "%s/%s", sd_root, de->d_name);
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
			continue;
		}
		snprintf(pick_names[pick_count], sizeof(pick_names[0]), "%s", de->d_name);
		pick_count++;
	}
	closedir(dir);
	qsort(pick_names, (size_t)pick_count, sizeof(pick_names[0]), name_cmp);
}

// Ticked: the folders saved last time, or every folder when nothing was saved.
static void pick_load_selection(void) {
	const char *saved = library_scan_folders();
	for (int i = 0; i < pick_count; i++) {
		pick_on[i] = !saved[0];
	}
	const char *p = saved;
	while (*p) {
		const char *end = strchr(p, '/');
		size_t len = end ? (size_t)(end - p) : strlen(p);
		for (int i = 0; i < pick_count; i++) {
			if (strlen(pick_names[i]) == len && strncasecmp(pick_names[i], p, len) == 0) {
				pick_on[i] = true;
			}
		}
		if (!end) {
			break;
		}
		p = end + 1;
	}
}

static void pick_paint(void) {
	int on = 0;
	for (int i = 0; i < pick_count; i++) {
		if (pick_marks[i]) {
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

static void pick_close(void) { lv_obj_add_flag(pick_veil, LV_OBJ_FLAG_HIDDEN); }

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
	const char *chosen[PICK_MAX];
	int count = 0;
	for (int i = 0; i < pick_count; i++) {
		if (pick_on[i]) {
			chosen[count++] = pick_names[i];
		}
	}
	if (pick_count > 0 && count == 0) {
		toast_error(tr("libraryscan_choose_a_folder"));
		return;
	}
	library_scan_folders_set(chosen, count == pick_count ? 0 : count);
	pick_close();
	switch_screen(libraryscan_screen);
}

static lv_obj_t *pick_button(lv_obj_t *parent, const char *text, bool accent, lv_event_cb_t cb) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_height(btn, ui_px(56));
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

static void pick_build(void) {
	gui_config_t *cfg = pick_cfg;
	pick_veil = lv_obj_create(lv_layer_top());
	lv_obj_set_size(pick_veil, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_color(pick_veil, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(pick_veil, LV_OPA_60, 0);
	lv_obj_set_style_border_width(pick_veil, 0, 0);
	lv_obj_set_style_radius(pick_veil, 0, 0);
	lv_obj_set_style_pad_all(pick_veil, 0, 0);
	lv_obj_remove_flag(pick_veil, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(pick_veil, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_event_cb(pick_veil, pick_veil_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *card = lv_obj_create(pick_veil);
	lv_obj_set_size(card, cfg->screen_width - 2 * cfg->padding, LV_SIZE_CONTENT);
	lv_obj_set_style_max_height(card, cfg->screen_height - cfg->top_bar_height - 2 * cfg->padding, 0);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, ui_px(16), 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, ui_px(20), 0);
	lv_obj_set_style_pad_row(card, ui_px(12), 0);
	lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);
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
	lv_obj_set_style_pad_row(pick_list, ui_px(6), 0);
	lv_obj_set_flex_flow(pick_list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scroll_dir(pick_list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(pick_list, LV_SCROLLBAR_MODE_AUTO);

	lv_obj_t *buttons = lv_obj_create(card);
	lv_obj_remove_style_all(buttons);
	lv_obj_set_size(buttons, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_column(buttons, ui_px(12), 0);
	lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
	pick_button(buttons, "cancel", false, pick_cancel_cb);
	pick_scan_btn = pick_button(buttons, "scan", true, pick_scan_cb);
}

static void pick_fill(void) {
	lv_obj_clean(pick_list);
	memset(pick_marks, 0, sizeof(pick_marks));
	for (int i = 0; i < pick_count; i++) {
		lv_obj_t *row = lv_btn_create(pick_list);
		lv_obj_set_size(row, lv_pct(100), ui_px(60));
		lv_obj_set_style_bg_color(row, theme()->surface_pressed, 0);
		lv_obj_set_style_radius(row, ui_px(10), 0);
		lv_obj_set_style_shadow_width(row, 0, 0);
		lv_obj_set_style_border_width(row, 0, 0);
		lv_obj_set_style_pad_hor(row, ui_px(16), 0);
		lv_obj_set_style_pad_column(row, ui_px(10), 0);
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
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
	}
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
	lv_obj_remove_flag(pick_veil, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(pick_veil);
}

// Both buttons sit at the same place at the bottom of the screen; only one is
// ever visible.
static lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_color_t colour, lv_event_cb_t cb,
							 gui_config_t *cfg) {
	lv_obj_t *button = lv_btn_create(parent);
	lv_obj_set_size(button, ui_px(240), ui_px(68));
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

void libraryscan_init(gui_config_t *cfg) {
	sd_root = cfg->sd_root_path;
	pick_cfg = cfg;

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
	lv_obj_set_style_pad_gap(container, ui_px(10), 0);
	lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *note = lv_image_create(container);
	lv_image_set_src(note, &icon_music_note);
	lv_obj_add_style(note, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(note, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_bottom(note, ui_px(16), 0);

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
	lv_obj_add_flag(ok_button, LV_OBJ_FLAG_HIDDEN);

	poll_timer = lv_timer_create(poll_cb, SCAN_POLL_MS, NULL);
	lv_timer_pause(poll_timer);
}

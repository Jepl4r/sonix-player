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
#include "src/gui/shell/sx_card.h"
#include "src/gui/shell/sx_styles.h"
#include "src/gui/shell/sx_toast.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/topbar.h"
#include "src/gui/shell/toast.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/podcastdl.h"
#include "src/system/core/lang.h"
#include "src/system/library/library.h"
#include "src/system/device/power.h"

// ---------------------------------------------------------------------------
// The scan page
//
// 2.0 (principle 6): asking for a scan starts it -- from the Music page's
// empty state, its "what a rescan would add" card, or Settings > Music > Scan
// options -- with the folders chosen last time (Scan options > Folders to
// scan, below; the whole card until somebody chooses). No question first: a
// scan changes nothing a scan cannot put back.
//
// The page is one card: "Scanning...", the tracks found so far, the folder
// being read in the mono line, a bar measured against the library's size
// before the scan (none on a first scan, which has nothing to measure
// against), and Stop. When the scan ends the page gives way to the Music page
// and a toast says how many tracks there are. The page keeps the whole panel
// while it runs (no back chevron, no mini player): the scan holds the screen
// on and owns the card.
// ---------------------------------------------------------------------------

// Fast enough that the number never looks stuck, slow enough that redrawing it
// costs nothing next to reading tags off a card.
#define SCAN_POLL_MS 250

lv_obj_t *libraryscan_screen;

static lv_obj_t *scan_card;
static lv_obj_t *scan_folder; // the folder being read
static lv_obj_t *scan_bar;
static lv_timer_t *poll_timer;
static int scan_expected; // tracks before the scan: the bar's measure; 0 unknown

static const char *sd_root;

static void card_update(int found) {
	char text[96];
	lv_snprintf(text, sizeof(text), tr("libraryscan_found"), found);
	sx_card_subtitle(scan_card, text, false);
	if (scan_expected > 0) {
		// Short of the end until it is the end: the card may hold more now.
		int percent = found * 100 / scan_expected;
		sx_card_bar_set(scan_bar, percent > 95 ? 95 : percent);
	}
	lv_obj_set_hidden(scan_bar, scan_expected <= 0);
}

// Back to the Music page, which now shows the library, with the menu as the
// only page behind it: walking back through a finished scan makes no sense.
static void back_to_music(void) {
	screen_history_reset();
	switch_screen_no_history(music_screen);
}

static void poll_cb(lv_timer_t *timer) {
	int found = library_scan_found();
	if (library_scan_running()) {
		card_update(found);
		// Where on the card, without the mount point in front.
		char folder[256];
		library_scan_current_folder(folder, sizeof(folder));
		const char *shown = folder;
		size_t root = sd_root ? strlen(sd_root) : 0;
		if (root && strncmp(folder, sd_root, root) == 0) {
			shown = folder[root] == '/' ? folder + root + 1 : folder + root;
		}
		lv_label_set_text(scan_folder, shown);
		return;
	}

	lv_timer_pause(timer);
	// The scan is over; the screen may go back to timing out.
	power_hold_screen_on(false);
	char text[96];
	lv_snprintf(text, sizeof(text), tr("libraryscan_found"), found);
	sx_toast(text);
	back_to_music();
}

// Stopping is not throwing away: the scan thread winds up at the next file and
// commits everything it has already read, so the index holds exactly the
// tracks the counter was showing. The poll sees it end.
static void stop_cb(lv_event_t *e) {
	(void)e;
	library_scan_stop();
}

void libraryscan_begin(void) {
	scan_expected = library_is_open() ? library_track_count() : 0;
	card_update(0);
	lv_label_set_text(scan_folder, "");

	if (!library_scan_start(sd_root)) {
		// Nothing to scan (no card, or a scan is somehow already going).
		toast_error("no_card_to_scan");
		back_to_music();
		return;
	}

	// Reading a card takes minutes and nobody is touching the screen while it
	// happens; the idle timer must not blank the panel halfway through.
	power_hold_screen_on(true);

	lv_timer_reset(poll_timer);
	lv_timer_resume(poll_timer);
}

void libraryscan_choose_folders(void) { switch_screen(libraryscan_screen); }

static void build_scan_page(gui_config_t *cfg) {
	lv_obj_add_style(libraryscan_screen, &theme_style_screen, 0);
	// Beside where the chevron would be, like every page; this one has none.
	settingsrow_title_corner_slots(settingsrow_title(libraryscan_screen, cfg, "music"), cfg, 0);

	lv_obj_t *box = lv_obj_create(libraryscan_screen);
	lv_obj_remove_style_all(box);
	lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_align(box, LV_ALIGN_TOP_LEFT, 0, settingsrow_content_top(cfg));
	lv_obj_set_style_pad_hor(box, cfg->padding, 0);
	lv_obj_set_scrollable(box, false);

	scan_card = sx_card_create(box);
	sx_card_title(scan_card, tr("libraryscan_scanning"));
	sx_card_subtitle(scan_card, "", false);
	scan_bar = sx_card_bar(scan_card, 0);
	sx_card_action(scan_card, tr("libraryscan_stop"), stop_cb, NULL);
	// Under the head: the folder being read, small, cut short at the end.
	scan_folder = lv_label_create(scan_card);
	lv_obj_set_width(scan_folder, lv_pct(100));
	lv_label_set_long_mode(scan_folder, LV_LABEL_LONG_DOT);
	lv_obj_add_style(scan_folder, &sx_style_text2, 0);
	lv_obj_set_style_text_font(scan_folder, &font_ui_17, 0);
	lv_label_set_text(scan_folder, "");
	lv_obj_move_to_index(scan_folder, 1);

	poll_timer = lv_timer_create(poll_cb, SCAN_POLL_MS, NULL);
	lv_timer_pause(poll_timer);
}

// ---------------------------------------------------------------------------
// Folders to scan: the card's top-level folders, each a switch
//
// Its own page now, reached from Scan options, rather than a question before
// every scan. A switch is saved as it is flipped (library_scan_folders_set):
// every folder on is the whole card, the files at its root included; turning
// the last one off is refused with a toast -- a scan of nothing is no scan.
// The rows are made when the page opens, a batch at a time as it scrolls, and
// let go when it closes.
// ---------------------------------------------------------------------------

// Rows are made this many at a time, more as the list nears its end.
#define PICK_BATCH 30

lv_obj_t *libraryscan_folders_screen;

static lv_obj_t *pick_list;
static char **pick_names;
static bool *pick_on;
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
		if (!names || !on) {
			return false;
		}
		pick_capacity = grown;
	}
	pick_names[pick_count] = strdup(name);
	if (!pick_names[pick_count]) {
		return false;
	}
	pick_on[pick_count] = false;
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

// On: the folders saved last time, or every folder when nothing was saved.
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

static void pick_save(void) {
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
	library_scan_folders_set(chosen, count == pick_count ? 0 : count);
	free(chosen);
}

static void pick_switch_cb(lv_event_t *e) {
	lv_obj_t *sw = lv_event_get_target(e);
	int i = (int)(intptr_t)lv_obj_get_user_data(sw);
	if (i < 0 || i >= pick_count) {
		return;
	}
	bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
	if (!on) {
		int others = 0;
		for (int k = 0; k < pick_count; k++) {
			others += k != i && pick_on[k];
		}
		if (others == 0) {
			lv_obj_add_state(sw, LV_STATE_CHECKED);
			toast_error("libraryscan_choose_a_folder");
			return;
		}
	}
	pick_on[i] = on;
	pick_save();
}

// The whole card flips its switch, as a switch card does.
static void pick_card_cb(lv_event_t *e) {
	lv_obj_t *sw = lv_event_get_user_data(e);
	if (!sw || lv_event_get_target(e) == sw) {
		return;
	}
	if (lv_obj_has_state(sw, LV_STATE_CHECKED)) {
		lv_obj_remove_state(sw, LV_STATE_CHECKED);
	} else {
		lv_obj_add_state(sw, LV_STATE_CHECKED);
	}
	lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, NULL);
}

// Makes up to `n` more rows, after the ones already there.
static void pick_append(int n) {
	int last = pick_built + n < pick_count ? pick_built + n : pick_count;
	for (int i = pick_built; i < last; i++) {
		lv_obj_t *card = sx_card_create(pick_list);
		sx_card_title(card, pick_names[i]);
		lv_obj_t *sw = sx_card_switch(card, pick_switch_cb);
		lv_obj_set_user_data(sw, (void *)(intptr_t)i);
		lv_obj_set_state(sw, LV_STATE_CHECKED, pick_on[i]);
		sx_card_on_click(card, pick_card_cb, sw);
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

static void pick_rows_clean(void) {
	// The note stays: it is the first child.
	while (lv_obj_get_child_count(pick_list) > 1) {
		lv_obj_delete(lv_obj_get_child(pick_list, -1));
	}
	pick_built = 0;
}

static void folders_load_start_cb(lv_event_t *e) {
	(void)e;
	pick_rows_clean();
	pick_read_folders();
	pick_load_selection();
	pick_append(PICK_BATCH);
	lv_obj_scroll_to_y(pick_list, 0, LV_ANIM_OFF);
}

static void folders_unloaded_cb(lv_event_t *e) {
	(void)e;
	pick_rows_clean();
	pick_clear();
}

void libraryscan_open_folders(void) { switch_screen(libraryscan_folders_screen); }

static void build_folders_page(gui_config_t *cfg) {
	libraryscan_folders_screen = lv_obj_create(NULL);
	pick_list = settingsrow_page(libraryscan_folders_screen, cfg, "libraryscan_folders");
	settingsrow_title_corner_slots(settingsrow_page_title(libraryscan_folders_screen), cfg, 0);
	lv_obj_t *note = lv_label_create(pick_list);
	lv_obj_set_width(note, lv_pct(100));
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_add_style(note, &sx_style_text2, 0);
	lv_obj_set_style_text_font(note, &font_ui_21, 0);
	lv_obj_set_style_pad_hor(note, 6, 0);
	lv_obj_set_style_pad_bottom(note, 4, 0);
	lv_label_set_text(note, tr("libraryscan_folders_note"));
	lv_obj_add_event_cb(pick_list, pick_scroll_cb, LV_EVENT_SCROLL, NULL);
	lv_obj_add_event_cb(libraryscan_folders_screen, folders_load_start_cb, LV_EVENT_SCREEN_LOAD_START, NULL);
	lv_obj_add_event_cb(libraryscan_folders_screen, folders_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(libraryscan_folders_screen);
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
	library_set_update_listener(update_listener);

	build_scan_page(cfg);
	build_folders_page(cfg);

	// The card may have been written while the player was off.
	lv_timer_t *boot_check = lv_timer_create(boot_check_cb, BOOT_CHECK_DELAY_MS, NULL);
	lv_timer_set_repeat_count(boot_check, 1);
}

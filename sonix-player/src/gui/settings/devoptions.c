#include "devoptions.h"

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/settings/processespage.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/device/adb.h"
#include "src/system/core/lang.h"
#include "src/system/library/library.h"
#include "src/system/core/logging.h"
#include "src/system/core/config.h"
#include "src/system/device/screenshot.h"

lv_obj_t *devoptions_screen;

static lv_obj_t *adb_switch;
static lv_obj_t *screenshot_switch;
static lv_obj_t *log_switch;
static lv_obj_t *db_log_switch;
static lv_obj_t *log_name_label; // rises to make room for the path when logging
static lv_obj_t *log_path_label;
static lv_timer_t *adb_poll_timer;

// Turning ADB on or off happens on its own thread and takes a second or two:
// adbd has to appear, or be waited out and then insisted upon. The switch
// follows what is really running, so while that is in flight it has to be
// asked again rather than left showing the state from before the tap.
#define ADB_POLL_MS 700

// How long the switch keeps showing what was asked for before it gives up and
// shows what is there. Long enough for the slowest stop (two waits of two
// seconds, then the gadget taken apart), short enough that a request which
// never comes true cannot leave the switch lying.
#define ADB_PENDING_MAX_MS 8000

static bool adb_pending;	// a tap is still being carried out
static bool adb_wanted;		// what that tap asked for
static uint32_t adb_asked_at;

static void update_adb_row(void) {
	if (!adb_switch) {
		return;
	}

	bool running = adb_is_running();

	// Between the tap and the daemon agreeing with it there are one or two
	// seconds during which the truth is still the old state. Showing it pulls
	// the switch back under the user's finger, which reads as the toggle
	// refusing the tap -- so the request stands until it comes true.
	if (adb_pending) {
		if (running == adb_wanted || lv_tick_elaps(adb_asked_at) > ADB_PENDING_MAX_MS) {
			adb_pending = false;
		} else {
			running = adb_wanted;
		}
	}

	if (running) {
		lv_obj_add_state(adb_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(adb_switch, LV_STATE_CHECKED);
	}
}

static void update_log_row(void) {
	bool on = logging_to_sd();

	if (log_switch) {
		if (on) {
			lv_obj_add_state(log_switch, LV_STATE_CHECKED);
		} else {
			lv_obj_remove_state(log_switch, LV_STATE_CHECKED);
		}
	}

	// The path only means something while the log is actually being written.
	// With logging on, the name moves up to the top of the row and the path
	// slides in underneath as its subtitle; off, the row looks like any other.
	if (log_path_label && log_name_label) {
		if (on) {
			lv_label_set_text(log_path_label, logging_path());
			lv_obj_remove_flag(log_path_label, LV_OBJ_FLAG_HIDDEN);
			lv_obj_align(log_name_label, LV_ALIGN_TOP_LEFT, 0, 0);
		} else {
			lv_obj_add_flag(log_path_label, LV_OBJ_FLAG_HIDDEN);
			lv_obj_align(log_name_label, LV_ALIGN_LEFT_MID, 0, 0);
		}
	}
}

static void adb_toggled_cb(lv_event_t *e) {
	(void)e;
	adb_wanted = lv_obj_has_state(adb_switch, LV_STATE_CHECKED);
	adb_pending = true;
	adb_asked_at = lv_tick_get();
	adb_set_enabled(adb_wanted);
}

static void screenshot_toggled_cb(lv_event_t *e) {
	(void)e;
	screenshot_set_enabled(lv_obj_has_state(screenshot_switch, LV_STATE_CHECKED));
}

static void log_toggled_cb(lv_event_t *e) {
	(void)e;
	logging_set_to_sd(lv_obj_has_state(log_switch, LV_STATE_CHECKED));
	update_log_row();
}

static void db_log_toggled_cb(lv_event_t *e) {
	(void)e;
	library_set_log_database(lv_obj_has_state(db_log_switch, LV_STATE_CHECKED));
}

static void adb_poll_cb(lv_timer_t *timer) {
	(void)timer;
	update_adb_row();
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	update_adb_row();
	update_log_row();
	lv_timer_resume(adb_poll_timer);
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	lv_timer_pause(adb_poll_timer);
}

void devoptions_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(devoptions_screen, cfg, "developer_options");

	settingsrow_toggle(container, "devoptions_adb", &adb_switch, adb_toggled_cb);

	// Screenshots: the option carries the key combination as a fixed subtitle.
	// Without it the switch would enable something without saying how to use it.
	lv_obj_t *shot_card = settingsrow_toggle(container, "devoptions_enable_screenshots", &screenshot_switch, screenshot_toggled_cb);
	lv_obj_t *shot_name = lv_obj_get_child(shot_card, 0);
	lv_obj_t *shot_toggle = lv_obj_get_child(shot_card, 1);

	lv_obj_t *shot_hint = lv_label_create(shot_card);
	lv_label_set_text(shot_hint, tr("devoptions_screenshot_keys"));
	lv_obj_set_width(shot_hint, lv_pct(100));
	lv_obj_set_style_pad_right(shot_hint, lv_obj_get_style_pad_right(shot_name, LV_PART_MAIN), 0);
	lv_obj_add_style(shot_hint, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(shot_hint, &font_ui_18, 0);

	// Name on top, keys at the bottom, in a column the card grows around: the
	// keys take two lines in a long language or with large text, and a row of
	// fixed height would draw them over the name. The row's own height stays
	// the least it is, and the switch keeps to the middle of it.
	int32_t row_height = lv_obj_get_style_height(shot_card, LV_PART_MAIN);
	lv_obj_set_height(shot_card, LV_SIZE_CONTENT);
	lv_obj_set_style_min_height(shot_card, row_height, 0);
	lv_obj_set_flex_flow(shot_card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(shot_card, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_row(shot_card, 2, 0);
	lv_obj_add_flag(shot_toggle, LV_OBJ_FLAG_IGNORE_LAYOUT);

	if (screenshot_enabled()) {
		lv_obj_add_state(screenshot_switch, LV_STATE_CHECKED);
	}
	lv_obj_t *log_card = settingsrow_toggle(container, "devoptions_log_to_microsd", &log_switch, log_toggled_cb);
	log_name_label = lv_obj_get_child(log_card, 0);

	// The path lives inside the row, as the option's subtitle -- it is the
	// first thing anybody needs to know when asked to send the log, and only
	// shows while the log is actually being written.
	log_path_label = lv_label_create(log_card);
	lv_label_set_long_mode(log_path_label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(log_path_label, lv_pct(72));
	lv_obj_add_style(log_path_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(log_path_label, &font_ui_18, 0);
	lv_obj_align(log_path_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);

	// Under the log switch, because it only means anything with the log on: the
	// scan names every file it reads, which is how a crash during a scan can be
	// pinned to the file that caused it.
	settingsrow_toggle(container, "devoptions_database_log", &db_log_switch, db_log_toggled_cb);
	if (library_log_database()) {
		lv_obj_add_state(db_log_switch, LV_STATE_CHECKED);
	}

	// A note under the card rather than a subtitle inside it: the sentence is
	// a warning about when to use the switch, not a description of it, and it
	// is too long to sit on the row without crowding the name.
	lv_obj_t *db_note = lv_label_create(container);
	lv_label_set_long_mode(db_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(db_note, lv_pct(100));
	lv_obj_add_style(db_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(db_note, &font_ui_22, 0);
	lv_label_set_text(db_note, tr("devoptions_dbtrace_note"));

	// Opens the page showing RAM and running processes. A navigation row with a
	// chevron, like the developer options entry on the previous page.
	settingsrow_add(container, "processes", NULL, switch_screen_cb, processespage_screen);

	update_adb_row();
	update_log_row();

	adb_poll_timer = lv_timer_create(adb_poll_cb, ADB_POLL_MS, NULL);
	lv_timer_pause(adb_poll_timer);

	lv_obj_add_event_cb(devoptions_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(devoptions_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
}

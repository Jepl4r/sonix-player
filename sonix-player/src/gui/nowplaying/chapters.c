#include "chapters.h"

#include <stdio.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/playback/audiobook.h"
#include "src/system/playback/device_state.h"
#include "src/system/core/lang.h"

lv_obj_t *chapters_screen;

// Books run to dozens of chapters, occasionally a couple of hundred; the list
// is plainly rebuilt each time it opens, like the audiobook index itself. The
// cap is there for a pathological file, not for any real book.
#define MAX_ROWS 400
#define ROW_HEIGHT 68
#define ROW_RADIUS 12

static lv_obj_t *chapter_list;
static lv_obj_t *empty_label;
static int listed_count;

static void format_clock(double seconds, char *out, size_t out_size) {
	if (seconds < 0) {
		seconds = 0;
	}
	long total = (long)seconds;
	long hours = total / 3600;
	long minutes = (total % 3600) / 60;
	long secs = total % 60;

	if (hours > 0) {
		snprintf(out, out_size, "%ld:%02ld:%02ld", hours, minutes, secs);
	} else {
		snprintf(out, out_size, "%ld:%02ld", minutes, secs);
	}
}

static void chapter_clicked_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}

	int index = (int)(intptr_t)lv_event_get_user_data(e);
	double start = 0;
	if (!audiobook_chapter(index, NULL, 0, &start)) {
		return;
	}

	device_state_seek(start);
	// The position is written straight away rather than at the next ten-second
	// tick: jumping to a chapter and then switching the player off immediately
	// is normal with a book.
	audiobook_note_position(start, 0, true);

	player_refresh_now_playing();
	back_btn_cb(NULL); // back to the player, which is where the jump is heard
}

static void rebuild(void) {
	lv_obj_clean(chapter_list);
	empty_label = NULL;

	int count = audiobook_chapter_count();
	if (count > MAX_ROWS) {
		count = MAX_ROWS;
	}
	listed_count = count;

	if (count == 0) {
		empty_label = lv_label_create(chapter_list);
		lv_label_set_text(empty_label, tr("chapters_empty"));
		lv_obj_set_style_text_align(empty_label, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_width(empty_label, lv_pct(100));
		lv_obj_add_style(empty_label, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(empty_label, &font_ui_24, 0);
		lv_obj_set_style_pad_top(empty_label, 80, 0);
		return;
	}

	device_state_t state;
	device_state_get(&state);
	int playing = audiobook_chapter_at(state.progress_current_secs);

	for (int i = 0; i < count; i++) {
		char title[192] = "";
		double start = 0;
		if (!audiobook_chapter(i, title, sizeof(title), &start)) {
			break;
		}

		lv_obj_t *row = lv_btn_create(chapter_list);
		lv_obj_set_size(row, lv_pct(100), ROW_HEIGHT);
		lv_obj_add_style(row, &theme_style_card, 0);
		lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_set_style_radius(row, ROW_RADIUS, 0);
		lv_obj_set_style_border_width(row, 0, 0);
		lv_obj_set_style_shadow_width(row, 0, 0);
		lv_obj_set_style_pad_all(row, 14, 0);
		lv_obj_set_style_pad_column(row, 12, 0);
		lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
		lv_obj_add_event_cb(row, chapter_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

		lv_obj_t *number = lv_label_create(row);
		lv_label_set_text_fmt(number, "%d", i + 1);
		lv_obj_set_width(number, 44);
		lv_obj_set_style_text_align(number, LV_TEXT_ALIGN_RIGHT, 0);
		lv_obj_add_style(number, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(number, &font_ui_24, 0);

		lv_obj_t *label = lv_label_create(row);
		lv_label_set_text(label, title[0] ? title : tr("chapter"));
		lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
		lv_obj_set_flex_grow(label, 1);
		// One line, then dots. Without a height the label wraps instead, and a
		// long chapter name then grows past the fixed row it lives in.
		lv_obj_set_height(label, 32);
		lv_obj_add_style(label, &theme_style_text, 0);
		lv_obj_set_style_text_font(label, &font_ui_24, 0);

		char clock[24];
		format_clock(start, clock, sizeof(clock));
		lv_obj_t *time_label = lv_label_create(row);
		lv_label_set_text(time_label, clock);
		lv_obj_add_style(time_label, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(time_label, &font_ui_18, 0);

		// The chapter being listened to is marked the way the queue marks the
		// playing track: the accent on the words, not a second widget.
		if (i == playing) {
			lv_obj_set_style_text_color(number, theme()->accent, 0);
			lv_obj_set_style_text_color(label, theme()->accent, 0);
			lv_obj_set_style_text_color(time_label, theme()->accent, 0);
		}
	}
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	rebuild();

	// Open on the chapter being listened to rather than at the top: on a book
	// with sixty chapters, the one that matters is the one playing.
	device_state_t state;
	device_state_get(&state);
	int playing = audiobook_chapter_at(state.progress_current_secs);
	if (playing > 0 && playing < listed_count) {
		lv_obj_t *row = lv_obj_get_child(chapter_list, playing);
		if (row) {
			lv_obj_scroll_to_view(row, LV_ANIM_OFF);
		}
	}
}

void chapters_open(void) {
	player_sheet_close(false);
	switch_screen(chapters_screen);
	switcher_set_player_return(chapters_screen);
}

void chapters_init(gui_config_t *cfg) {
	lv_obj_add_style(chapters_screen, &theme_style_screen, 0);
	settingsrow_title(chapters_screen, cfg, "chapters");

	int content_top = settingsrow_content_top(cfg);

	chapter_list = lv_obj_create(chapters_screen);
	lv_obj_set_size(chapter_list, lv_pct(100), cfg->screen_height - content_top);
	lv_obj_align(chapter_list, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(chapter_list, 0, 0);
	lv_obj_set_style_border_width(chapter_list, 0, 0);
	lv_obj_set_style_radius(chapter_list, 0, 0);
	lv_obj_set_style_pad_hor(chapter_list, cfg->padding, 0);
	lv_obj_set_style_pad_ver(chapter_list, 0, 0);
	lv_obj_set_style_pad_gap(chapter_list, 8, 0);
	lv_obj_set_scroll_dir(chapter_list, LV_DIR_VER);
	lv_obj_set_flex_flow(chapter_list, LV_FLEX_FLOW_COLUMN);
	// Set here, not in the row builder: a book with no chapters creates no
	// rows, and the page still has to answer the swipe-back.
	lv_obj_add_flag(chapter_list, LV_OBJ_FLAG_EVENT_BUBBLE);

	// No player-sheet swipe here, for the queue page's reason: it would slide
	// the sheet over the list being read. The swipe-back still works.
	switcher_attach_back_gesture(chapter_list);
	lv_obj_add_event_cb(chapters_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}

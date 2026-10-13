#include "touch.h"

#include <stdint.h>

#include "lvgl/lvgl.h"
// lv_hit_test_info_t is opaque in LVGL 9.2 and later; its fields are here.
#include "lvgl/src/core/lv_obj_event_private.h"
#include "lvgl/src/misc/lv_area_private.h"

#include "src/system/core/config.h"

// The header row's buttons: 56 px glyph buttons, 6 px apart, their top edge a
// margin below the status bar (see settingsrow_content_top()).
#define HEADER_BUTTON 56
#define HEADER_GAP 6
// Above and below them: the status bar ends 15 px over the row and the page's
// content starts 15 px under it, so 8 px each way stays clear of both.
#define HEADER_REACH 8

static int8_t page_padding;
static int8_t top_bar_height;
static int large_rows = -1; // -1 until read from the config

void touch_init(gui_config_t *cfg) {
	page_padding = cfg->padding;
	top_bar_height = cfg->top_bar_height;
}

// The amounts ride in the callback's user data, a byte a side, rather than in
// memory of their own that would have to be freed with the object.
static uintptr_t pad_pack(int left, int right, int top, int bottom) {
	return (uintptr_t)(left & 0xff) | (uintptr_t)(right & 0xff) << 8 | (uintptr_t)(top & 0xff) << 16 |
		   (uintptr_t)(bottom & 0xff) << 24;
}

// LVGL's own test has already passed the point when this runs: the object's
// box grown by the largest of the four amounts on every side
// (lv_obj_set_ext_click_area below). This narrows that square margin to the
// amount meant for each side.
static void pad_hit_test_cb(lv_event_t *e) {
	lv_hit_test_info_t *info = lv_event_get_hit_test_info(e);
	if (!info) {
		return;
	}
	lv_obj_t *obj = lv_event_get_current_target(e);
	uintptr_t pad = (uintptr_t)lv_event_get_user_data(e);
	lv_area_t area;
	lv_obj_get_coords(obj, &area);
	area.x1 -= (int32_t)(pad & 0xff);
	area.x2 += (int32_t)((pad >> 8) & 0xff);
	area.y1 -= (int32_t)((pad >> 16) & 0xff);
	area.y2 += (int32_t)((pad >> 24) & 0xff);
	info->res = lv_area_is_point_on(&area, info->point, 0);
}

void touch_pad(lv_obj_t *obj, int left, int right, int top, int bottom) {
	if (!obj) {
		return;
	}
	if (left < 0) {
		left = 0;
	}
	if (right < 0) {
		right = 0;
	}
	if (top < 0) {
		top = 0;
	}
	if (bottom < 0) {
		bottom = 0;
	}
	int most = left;
	most = right > most ? right : most;
	most = top > most ? top : most;
	most = bottom > most ? bottom : most;
	lv_obj_remove_event_cb(obj, pad_hit_test_cb);
	lv_obj_set_ext_click_area(obj, most);
	if (left == most && right == most && top == most && bottom == most) {
		// Even all round: LVGL's own margin says it all, and nothing has to
		// be asked of the object on every press.
		lv_obj_set_adv_hittest(obj, false);
		return;
	}
	lv_obj_set_adv_hittest(obj, true);
	lv_obj_add_event_cb(obj, pad_hit_test_cb, LV_EVENT_HIT_TEST, (void *)pad_pack(left, right, top, bottom));
}

void touch_grow(lv_obj_t *obj, int w, int h, int limit) {
	int shorter = w < h ? w : h;
	int reach = shorter >= TOUCH_TARGET ? 0 : (TOUCH_TARGET - shorter + 1) / 2;
	if (reach > limit) {
		reach = limit;
	}
	if (obj && reach > 0) {
		lv_obj_set_ext_click_area(obj, reach);
	}
}

void touch_header(lv_obj_t *screen) {
	if (!screen) {
		return;
	}
	int row_y = page_padding + top_bar_height;
	uint32_t count = lv_obj_get_child_count(screen);
	for (uint32_t i = 0; i < count; i++) {
		lv_obj_t *child = lv_obj_get_child(screen, (int32_t)i);
		// A button already done has its own hit test by now (touch_pad() with
		// uneven sides sets one), and a plain header button never has one: that
		// is the mark, with no flag of its own to keep.
		if (lv_obj_is_adv_hittest(child) || !lv_obj_check_type(child, &lv_button_class)) {
			continue;
		}
		// Asked of the styles the page set and not of the laid-out position: a
		// page loaded for the first time has not been laid out yet.
		if (lv_obj_get_style_width(child, LV_PART_MAIN) != HEADER_BUTTON ||
			lv_obj_get_style_height(child, LV_PART_MAIN) != HEADER_BUTTON ||
			lv_obj_get_style_y(child, LV_PART_MAIN) != row_y ||
			lv_obj_get_style_align(child, LV_PART_MAIN) != LV_ALIGN_TOP_RIGHT) {
			continue;
		}
		// The button against the right margin reaches the edge of the screen;
		// the others meet their neighbours half way across the gap.
		bool outermost = lv_obj_get_style_x(child, LV_PART_MAIN) == -page_padding;
		touch_pad(child, HEADER_GAP / 2, outermost ? page_padding : HEADER_GAP / 2, HEADER_REACH, HEADER_REACH);
	}
}

bool touch_large_rows(void) {
	if (large_rows < 0) {
		large_rows = config_get_int("ui", "large_rows", 0) != 0;
	}
	return large_rows != 0;
}

void touch_set_large_rows(bool on) {
	large_rows = on ? 1 : 0;
	config_set_int("ui", "large_rows", large_rows);
	config_save();
}

int touch_row_height(void) { return touch_large_rows() ? TOUCH_ROW_HEIGHT_LARGE : TOUCH_ROW_HEIGHT; }

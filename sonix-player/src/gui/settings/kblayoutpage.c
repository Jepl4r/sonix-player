#include "kblayoutpage.h"

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/input/kblayout.h"
#include "src/system/core/lang.h"

lv_obj_t *kblayoutpage_screen;

// ---------------------------------------------------------------------------
// Two lists on one canvas
//
// The rows are placed by hand rather than by a flex, for the same reason the
// control centre's tiles are: a row being dragged has to be able to sit between
// two others, and the gap it would leave has to be somewhere a drop can land.
// Both lists live on the same canvas, one under the other with a heading each,
// so a row crossing from one to the other is one coordinate and not a change of
// parent.
// ---------------------------------------------------------------------------

#define ROW_HEIGHT 72
#define ROW_GAP 8
#define ROW_PITCH (ROW_HEIGHT + ROW_GAP)
#define ROW_RADIUS 12
#define HEADING_H 40
#define GRIP_SIZE 64

// The second list is a drop target even when it is empty, so it is never
// shorter than one row.
#define LIST_MIN_ROWS 1

// How far the finger has to travel before a release counts as a drop.
#define DROP_MIN_TRAVEL 8

typedef struct {
	lv_obj_t *row;
	lv_obj_t *name;
	lv_obj_t *grip;
} entry_t;

static lv_obj_t *body;
static lv_obj_t *others_heading;
static entry_t entries[KB_LAYOUT_COUNT];

// Which layout each row is showing, and where. Rebuilt at every repaint, so
// the drag reads them rather than a number baked in at build time.
static kblayout_t row_layout[KB_LAYOUT_COUNT];
static bool row_in_use[KB_LAYOUT_COUNT];
static int row_count;

static lv_obj_t *drag_layer;
static lv_obj_t *drag_ghost;
static kblayout_t drag_layout;
static bool drag_moved;
static lv_point_t drag_from;
static int drag_grab_dx, drag_grab_dy;

static void refresh(void);
static void refresh_async(void *unused);

// Where each part of the canvas starts. The second heading follows the first
// list, so both move as layouts cross between them.
static int in_use_top(void) { return HEADING_H; }

static int others_heading_y(void) {
	int n = kblayout_in_use_count();
	return in_use_top() + n * ROW_PITCH + 8;
}

static int others_top(void) { return others_heading_y() + HEADING_H; }

static void refresh(void) {
	int in_use_n = kblayout_in_use_count();
	int others_n = kblayout_others_count();
	row_count = 0;

	for (int i = 0; i < in_use_n && row_count < KB_LAYOUT_COUNT; i++, row_count++) {
		row_layout[row_count] = kblayout_in_use_at(i);
		row_in_use[row_count] = true;
		lv_obj_set_y(entries[row_count].row, in_use_top() + i * ROW_PITCH);
	}
	for (int i = 0; i < others_n && row_count < KB_LAYOUT_COUNT; i++, row_count++) {
		row_layout[row_count] = kblayout_others_at(i);
		row_in_use[row_count] = false;
		lv_obj_set_y(entries[row_count].row, others_top() + i * ROW_PITCH);
	}

	for (int i = 0; i < KB_LAYOUT_COUNT; i++) {
		if (i >= row_count) {
			lv_obj_add_flag(entries[i].row, LV_OBJ_FLAG_HIDDEN);
			continue;
		}
		lv_obj_remove_flag(entries[i].row, LV_OBJ_FLAG_HIDDEN);
		lv_label_set_text(entries[i].name, kblayout_name(row_layout[i]));

		// The first of the ones in use is the one every keyboard opens in, and
		// the accent is what says so -- without it the order of the list would
		// look like a preference rather than a decision.
		bool is_default = i == 0;
		lv_obj_set_style_text_color(entries[i].name, is_default ? theme()->accent : theme()->text_primary, 0);
	}

	lv_obj_set_y(others_heading, others_heading_y());

	int rows_below = others_n < LIST_MIN_ROWS ? LIST_MIN_ROWS : others_n;
	lv_obj_set_height(body, others_top() + rows_below * ROW_PITCH + 8);
}

static void refresh_async(void *unused) {
	(void)unused;
	refresh();
}

// Puts the ghost where the finger is holding it. See ccsettings.c: the
// difference is read back rather than computed, so nothing here has to know
// about the parent's insets.
static void ghost_follow(lv_point_t point) {
	lv_area_t here;
	lv_obj_get_coords(drag_ghost, &here);
	int32_t dx = (point.x - drag_grab_dx) - here.x1;
	int32_t dy = (point.y - drag_grab_dy) - here.y1;
	lv_obj_set_pos(drag_ghost, lv_obj_get_x(drag_ghost) + dx, lv_obj_get_y(drag_ghost) + dy);
}

// Which list a drop landed in, and at which position within it.
static void drop(kblayout_t layout, lv_point_t point) {
	lv_area_t area;
	lv_obj_get_coords(body, &area);
	int y = point.y - area.y1;

	if (y < others_heading_y()) {
		int at = (y - in_use_top() + ROW_PITCH / 2) / ROW_PITCH;
		if (at < 0) {
			at = 0;
		}
		kblayout_move_in_use(layout, at);
	} else {
		int at = (y - others_top() + ROW_PITCH / 2) / ROW_PITCH;
		if (at < 0) {
			at = 0;
		}
		kblayout_move_others(layout, at);
	}

	// A keyboard already open may have been typing in a layout that has just
	// been sent away.
	keyboard_refresh_layout();
}

static void drag_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *grip = lv_event_get_current_target(e);

	lv_indev_t *indev = lv_indev_active();
	lv_point_t point = {0, 0};
	if (indev) {
		lv_indev_get_point(indev, &point);
	}

	if (code == LV_EVENT_PRESSED) {
		int index = -1;
		for (int i = 0; i < row_count; i++) {
			if (entries[i].grip == grip) {
				index = i;
			}
		}
		if (index < 0) {
			return;
		}

		lv_area_t row_area;
		lv_obj_get_coords(entries[index].row, &row_area);
		drag_layout = row_layout[index];
		drag_moved = false;
		drag_from = point;
		drag_grab_dx = point.x - row_area.x1;
		drag_grab_dy = point.y - row_area.y1;

		// A copy on the drag layer, because the row lives on a canvas that
		// clips it; the original stays where it is, dimmed, so the list does
		// not jump before the drop decides anything.
		drag_ghost = lv_obj_create(drag_layer);
		lv_obj_set_size(drag_ghost, lv_obj_get_width(entries[index].row), ROW_HEIGHT);
		lv_obj_add_style(drag_ghost, &theme_style_card, 0);
		lv_obj_set_style_radius(drag_ghost, ROW_RADIUS, 0);
		lv_obj_set_style_border_width(drag_ghost, 2, 0);
		lv_obj_set_style_border_color(drag_ghost, theme()->accent, 0);
		lv_obj_set_style_shadow_width(drag_ghost, 0, 0);
		lv_obj_set_style_pad_hor(drag_ghost, 20, 0);
		lv_obj_remove_flag(drag_ghost, LV_OBJ_FLAG_SCROLLABLE);

		lv_obj_t *label = lv_label_create(drag_ghost);
		lv_label_set_text(label, kblayout_name(drag_layout));
		lv_obj_add_style(label, &theme_style_text, 0);
		lv_obj_set_style_text_font(label, &font_ui_24, 0);
		lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);

		ghost_follow(point);
		lv_obj_set_style_opa(entries[index].row, LV_OPA_40, 0);
		return;
	}

	if (!drag_ghost) {
		return;
	}

	if (code == LV_EVENT_PRESSING) {
		ghost_follow(point);
		if (LV_ABS(point.y - drag_from.y) > DROP_MIN_TRAVEL || LV_ABS(point.x - drag_from.x) > DROP_MIN_TRAVEL) {
			drag_moved = true;
		}
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		lv_obj_delete(drag_ghost);
		drag_ghost = NULL;
		for (int i = 0; i < KB_LAYOUT_COUNT; i++) {
			lv_obj_remove_local_style_prop(entries[i].row, LV_STYLE_OPA, 0);
		}

		if (code == LV_EVENT_RELEASED && drag_moved) {
			drop(drag_layout, point);
		}
		lv_async_call(refresh_async, NULL);
	}
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	refresh();
}

static lv_obj_t *make_heading(lv_obj_t *parent, const char *tag, int y) {
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text(label, tr(tag));
	lv_obj_add_style(label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_obj_set_pos(label, 4, y + 10);
	return label;
}

void kblayoutpage_init(gui_config_t *cfg) {
	kblayoutpage_screen = lv_obj_create(NULL);
	lv_obj_add_style(kblayoutpage_screen, &theme_style_screen, 0);

	lv_obj_t *container = settingsrow_page(kblayoutpage_screen, cfg, "keyboard_layout");
	lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);

	int width = cfg->screen_width - 2 * cfg->padding;

	body = lv_obj_create(container);
	lv_obj_set_width(body, width);
	lv_obj_set_height(body, ROW_PITCH);
	lv_obj_set_style_bg_opa(body, 0, 0);
	lv_obj_set_style_border_width(body, 0, 0);
	lv_obj_set_style_pad_all(body, 0, 0);
	lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

	make_heading(body, "in_use", 0);
	others_heading = make_heading(body, "kblayout_others", 0);

	for (int i = 0; i < KB_LAYOUT_COUNT; i++) {
		lv_obj_t *row = lv_obj_create(body);
		lv_obj_set_size(row, width, ROW_HEIGHT);
		lv_obj_set_x(row, 0);
		lv_obj_add_style(row, &theme_style_card, 0);
		lv_obj_set_style_radius(row, ROW_RADIUS, 0);
		lv_obj_set_style_border_width(row, 0, 0);
		lv_obj_set_style_shadow_width(row, 0, 0);
		lv_obj_set_style_pad_hor(row, 20, 0);
		lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

		entries[i].row = row;

		entries[i].name = lv_label_create(row);
		lv_obj_add_style(entries[i].name, &theme_style_text, 0);
		lv_obj_set_style_text_font(entries[i].name, &font_ui_24, 0);
		lv_obj_align(entries[i].name, LV_ALIGN_LEFT_MID, 0, 0);

		// The handle. Not a button and does nothing when tapped: it is the part
		// of the row that can be taken hold of, and it looks like one so that
		// it is looked for.
		entries[i].grip = lv_obj_create(row);
		lv_obj_set_size(entries[i].grip, GRIP_SIZE, ROW_HEIGHT);
		lv_obj_align(entries[i].grip, LV_ALIGN_RIGHT_MID, 20, 0);
		lv_obj_set_style_bg_opa(entries[i].grip, 0, 0);
		lv_obj_set_style_border_width(entries[i].grip, 0, 0);
		lv_obj_set_style_pad_all(entries[i].grip, 0, 0);
		lv_obj_remove_flag(entries[i].grip, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_add_flag(entries[i].grip, LV_OBJ_FLAG_CLICKABLE);

		lv_obj_t *grip_icon = lv_image_create(entries[i].grip);
		lv_image_set_src(grip_icon, &icon_grip);
		lv_obj_add_style(grip_icon, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(grip_icon, LV_OPA_COVER, 0);
		lv_obj_center(grip_icon);

		lv_obj_add_event_cb(entries[i].grip, drag_cb, LV_EVENT_PRESSED, NULL);
		lv_obj_add_event_cb(entries[i].grip, drag_cb, LV_EVENT_PRESSING, NULL);
		lv_obj_add_event_cb(entries[i].grip, drag_cb, LV_EVENT_RELEASED, NULL);
		lv_obj_add_event_cb(entries[i].grip, drag_cb, LV_EVENT_PRESS_LOST, NULL);
	}

	// Over everything, and transparent to the finger: only the ghost is ever on
	// it, and only while one is being dragged.
	drag_layer = lv_obj_create(kblayoutpage_screen);
	lv_obj_set_size(drag_layer, lv_pct(100), lv_pct(100));
	lv_obj_set_pos(drag_layer, 0, 0);
	lv_obj_set_style_bg_opa(drag_layer, 0, 0);
	lv_obj_set_style_border_width(drag_layer, 0, 0);
	lv_obj_set_style_pad_all(drag_layer, 0, 0);
	lv_obj_remove_flag(drag_layer, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_remove_flag(drag_layer, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(drag_layer, LV_OBJ_FLAG_IGNORE_LAYOUT);

	lv_obj_add_event_cb(kblayoutpage_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(kblayoutpage_screen);

	refresh();
}

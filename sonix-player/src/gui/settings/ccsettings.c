#include "ccsettings.h"

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/quickpanel.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"

lv_obj_t *ccsettings_screen;

// ---------------------------------------------------------------------------
// The grid
//
// Four tiles to a line, laid out by hand rather than by a flex: a tile being
// dragged has to be able to land in a named place, and a place has to keep its
// coordinates whether or not anything is standing in it. Both cards use the
// same cell, so a tile does not change size as it crosses between them.
// ---------------------------------------------------------------------------

#define TILE_SIZE 84
#define GRID_COLUMNS 4
#define CELL_HEIGHT 104
#define CARD_PAD 12
#define CARD_RADIUS 12

// The two cards. The top one is the panel itself and is always eight places;
// the bottom one holds whatever is not in the panel, and three lines is room
// for every button there is.
#define IN_ROWS ((QP_SLOT_COUNT + GRID_COLUMNS - 1) / GRID_COLUMNS)
#define OUT_ROWS 3
#define CARD_HEIGHT(rows) ((rows) * CELL_HEIGHT + 2 * CARD_PAD)

// How far the finger has to travel before a release counts as a drop. A press
// that never moved is a tap, and a tap must not rearrange anything.
#define DROP_MIN_TRAVEL 10

static lv_obj_t *in_card;
static lv_obj_t *out_card;

// One tile per place in the grid, plus one per button for the card below. Both
// pools are built once and rebound, so nothing is created or destroyed while a
// finger is on the screen.
static lv_obj_t *slot_tiles[QP_SLOT_COUNT];
static lv_obj_t *out_tiles[QP_BTN_COUNT];

// Ghosts are dragged on this rather than on the screen: it has no padding of
// its own, so a position set on it is the position on the panel, and nothing
// has to work back from the screen's own insets.
static lv_obj_t *drag_layer;
static lv_obj_t *drag_ghost;

// What is being dragged, and from where. `drag_slot` is -1 when it came from
// the card below.
static quickpanel_button_t drag_button;
static int drag_slot;
static bool drag_moved;
static lv_point_t drag_from;
static int drag_grab_dx, drag_grab_dy;

static void refresh(void);
static void refresh_async(void *unused);

// Where the tile for cell `index` of a card sits inside that card.
static int cell_x(lv_obj_t *card, int index) {
	int inner = lv_obj_get_width(card) - 2 * CARD_PAD;
	int cell = inner / GRID_COLUMNS;
	return CARD_PAD + (index % GRID_COLUMNS) * cell + (cell - TILE_SIZE) / 2;
}

static int cell_y(int index) { return CARD_PAD + (index / GRID_COLUMNS) * CELL_HEIGHT + (CELL_HEIGHT - TILE_SIZE) / 2; }

// Which place of the top card a point is over, or -1 for none. Worked out from
// the cell the point falls in rather than from the tiles, so an empty place
// catches a drop exactly as a full one does.
static int slot_at_point(lv_point_t point) {
	lv_area_t area;
	lv_obj_get_coords(in_card, &area);
	if (point.x < area.x1 || point.x > area.x2 || point.y < area.y1 || point.y > area.y2) {
		return -1;
	}

	int inner = lv_obj_get_width(in_card) - 2 * CARD_PAD;
	int cell = inner / GRID_COLUMNS;
	int col = (point.x - area.x1 - CARD_PAD) / cell;
	int row = (point.y - area.y1 - CARD_PAD) / CELL_HEIGHT;
	if (col < 0) {
		col = 0;
	} else if (col >= GRID_COLUMNS) {
		col = GRID_COLUMNS - 1;
	}
	if (row < 0) {
		row = 0;
	}

	int slot = row * GRID_COLUMNS + col;
	return slot < QP_SLOT_COUNT ? slot : -1;
}

static bool point_in(lv_obj_t *obj, lv_point_t point) {
	lv_area_t area;
	lv_obj_get_coords(obj, &area);
	return point.x >= area.x1 && point.x <= area.x2 && point.y >= area.y1 && point.y <= area.y2;
}

// ---------------------------------------------------------------------------
// Tiles
// ---------------------------------------------------------------------------

// Paints a tile as a button or as an empty place. The empty one is a ring
// rather than nothing at all: it says the panel has room here and that this is
// where a button dropped into it will go.
static void tile_set_button(lv_obj_t *tile, quickpanel_button_t which) {
	lv_obj_t *icon = lv_obj_get_child(tile, 0);
	bool empty = which >= QP_BTN_COUNT;

	if (empty) {
		lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
		lv_obj_set_style_bg_opa(tile, 0, 0);
		lv_obj_set_style_border_width(tile, 2, 0);
		lv_obj_set_style_border_color(tile, theme()->text_secondary, 0);
		lv_obj_set_style_border_opa(tile, LV_OPA_40, 0);
		lv_obj_remove_flag(tile, LV_OBJ_FLAG_CLICKABLE);
		return;
	}

	lv_image_set_src(icon, quickpanel_button_icon(which));
	lv_obj_remove_flag(icon, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_local_style_prop(tile, LV_STYLE_BG_OPA, 0);
	lv_obj_set_style_border_width(tile, 0, 0);
	lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
}

static lv_obj_t *make_tile(lv_obj_t *parent) {
	lv_obj_t *tile = lv_obj_create(parent);
	lv_obj_set_size(tile, TILE_SIZE, TILE_SIZE);
	lv_obj_set_style_radius(tile, LV_RADIUS_CIRCLE, 0);
	lv_obj_add_style(tile, &theme_style_switch, 0); // the panel's own neutral circle
	lv_obj_set_style_shadow_width(tile, 0, 0);
	lv_obj_set_style_border_width(tile, 0, 0);
	lv_obj_set_style_pad_all(tile, 0, 0);
	lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *icon = lv_image_create(tile);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
	lv_obj_center(icon);

	return tile;
}

// ---------------------------------------------------------------------------
// Dragging
// ---------------------------------------------------------------------------

// Puts the ghost where the finger is holding it.
//
// Worked out from where the ghost actually landed rather than from the
// coordinates it was given: lv_obj_set_pos() is relative to the parent's
// content area, and reading back the difference costs one subtraction and
// removes the need to know anything about that parent.
static void ghost_follow(lv_point_t point) {
	lv_area_t here;
	lv_obj_get_coords(drag_ghost, &here);
	int32_t dx = (point.x - drag_grab_dx) - here.x1;
	int32_t dy = (point.y - drag_grab_dy) - here.y1;
	lv_obj_set_pos(drag_ghost, lv_obj_get_x(drag_ghost) + dx, lv_obj_get_y(drag_ghost) + dy);
}

// Where a drop lands. `from_slot` is -1 for a tile that came from the card
// below.
static void drop(quickpanel_button_t button, int from_slot, lv_point_t point) {
	int slot = slot_at_point(point);

	if (slot >= 0) {
		if (from_slot >= 0) {
			quickpanel_slot_move(from_slot, slot);
		} else {
			quickpanel_slot_place(button, slot);
		}
		return;
	}

	// Into the card below: out of the panel, leaving its place open. A tile
	// dropped back where it came from, or anywhere that is neither card, stays
	// where it was.
	if (from_slot >= 0 && point_in(out_card, point)) {
		quickpanel_slot_clear(from_slot);
	}
}

static void drag_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *tile = lv_event_get_current_target(e);

	lv_indev_t *indev = lv_indev_active();
	lv_point_t point = {0, 0};
	if (indev) {
		lv_indev_get_point(indev, &point);
	}

	if (code == LV_EVENT_PRESSED) {
		// Which tile this is, worked out from the pools rather than carried in
		// the callback's user data: the tiles are rebound at every change, and
		// a number baked in at build time would go stale with the first drop.
		drag_slot = -1;
		drag_button = QP_BTN_NONE;
		for (int i = 0; i < QP_SLOT_COUNT; i++) {
			if (slot_tiles[i] == tile) {
				drag_slot = i;
				drag_button = quickpanel_slot_at(i);
			}
		}
		for (int i = 0; i < quickpanel_hidden_count(); i++) {
			if (out_tiles[i] == tile) {
				drag_button = quickpanel_hidden_at(i);
			}
		}
		if (drag_button >= QP_BTN_COUNT) {
			return; // an empty place: there is nothing to pick up
		}

		lv_area_t tile_area;
		lv_obj_get_coords(tile, &tile_area);
		drag_moved = false;
		drag_from = point;
		drag_grab_dx = point.x - tile_area.x1;
		drag_grab_dy = point.y - tile_area.y1;

		// A copy on the drag layer rather than the tile itself: the tile lives
		// inside one of the two cards, and LVGL clips a child to its parent, so
		// the real one would be cut off the moment it left the card it has to
		// be dragged out of. The one left behind goes empty, so the grid shows
		// where the button would come back to.
		drag_ghost = make_tile(drag_layer);
		tile_set_button(drag_ghost, drag_button);
		lv_obj_set_style_bg_color(drag_ghost, theme()->accent, 0);
		lv_obj_set_style_bg_opa(drag_ghost, LV_OPA_COVER, 0);
		lv_obj_set_style_image_recolor(lv_obj_get_child(drag_ghost, 0), lv_color_white(), 0);
		ghost_follow(point);

		lv_obj_add_flag(tile, LV_OBJ_FLAG_HIDDEN);
		if (drag_slot >= 0) {
			tile_set_button(slot_tiles[drag_slot], QP_BTN_NONE);
			lv_obj_remove_flag(slot_tiles[drag_slot], LV_OBJ_FLAG_HIDDEN);
		}
		return;
	}

	if (!drag_ghost) {
		return;
	}

	if (code == LV_EVENT_PRESSING) {
		ghost_follow(point);
		if (LV_ABS(point.x - drag_from.x) > DROP_MIN_TRAVEL || LV_ABS(point.y - drag_from.y) > DROP_MIN_TRAVEL) {
			drag_moved = true;
		}
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		lv_obj_delete(drag_ghost);
		drag_ghost = NULL;

		if (code == LV_EVENT_RELEASED && drag_moved) {
			drop(drag_button, drag_slot, point);
		}

		// Not here: rebinding the cards touches the very tile whose release is
		// being handled. The repaint waits for the event to unwind.
		lv_async_call(refresh_async, NULL);
	}
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

static void refresh(void) {
	for (int i = 0; i < QP_SLOT_COUNT; i++) {
		lv_obj_remove_flag(slot_tiles[i], LV_OBJ_FLAG_HIDDEN);
		lv_obj_set_pos(slot_tiles[i], cell_x(in_card, i), cell_y(i));
		tile_set_button(slot_tiles[i], quickpanel_slot_at(i));
	}

	int out_count = quickpanel_hidden_count();
	for (int i = 0; i < QP_BTN_COUNT; i++) {
		if (i >= out_count) {
			lv_obj_add_flag(out_tiles[i], LV_OBJ_FLAG_HIDDEN);
			continue;
		}
		lv_obj_remove_flag(out_tiles[i], LV_OBJ_FLAG_HIDDEN);
		lv_obj_set_pos(out_tiles[i], cell_x(out_card, i), cell_y(i));
		tile_set_button(out_tiles[i], quickpanel_hidden_at(i));
	}
}

static void refresh_async(void *unused) {
	(void)unused;
	refresh();
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	refresh();
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

// `tag` may be NULL: the panel's own card needs no heading, since the page is
// named after it and the card is the first thing on it.
static lv_obj_t *make_card(lv_obj_t *parent, const char *tag, int rows) {
	if (tag) {
		lv_obj_t *heading = lv_label_create(parent);
		lv_label_set_text(heading, tr(tag));
		lv_obj_add_style(heading, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(heading, &font_ui_22, 0);
		lv_obj_set_style_pad_hor(heading, 4, 0);
		lv_obj_set_style_margin_top(heading, 8, 0);
	}

	lv_obj_t *card = lv_obj_create(parent);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_height(card, CARD_HEIGHT(rows));
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, CARD_RADIUS, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, 0, 0);
	lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
	return card;
}

static void arm_tile(lv_obj_t *tile) {
	lv_obj_add_event_cb(tile, drag_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(tile, drag_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(tile, drag_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(tile, drag_cb, LV_EVENT_PRESS_LOST, NULL);
}

void ccsettings_init(gui_config_t *cfg) {
	ccsettings_screen = lv_obj_create(NULL);
	lv_obj_add_style(ccsettings_screen, &theme_style_screen, 0);

	lv_obj_t *container = settingsrow_page(ccsettings_screen, cfg, "control_centre");
	lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);

	in_card = make_card(container, NULL, IN_ROWS);
	out_card = make_card(container, "controlcentre_not_in_use", OUT_ROWS);

	// The widths are needed before anything can be placed in them, and the
	// layout has not run yet.
	lv_obj_update_layout(container);

	for (int i = 0; i < QP_SLOT_COUNT; i++) {
		slot_tiles[i] = make_tile(in_card);
		arm_tile(slot_tiles[i]);
	}
	for (int i = 0; i < QP_BTN_COUNT; i++) {
		out_tiles[i] = make_tile(out_card);
		arm_tile(out_tiles[i]);
	}

	// Over everything, and transparent to the finger: only the ghost is ever on
	// it, and only while one is being dragged.
	drag_layer = lv_obj_create(ccsettings_screen);
	lv_obj_set_size(drag_layer, lv_pct(100), lv_pct(100));
	lv_obj_set_pos(drag_layer, 0, 0);
	lv_obj_set_style_bg_opa(drag_layer, 0, 0);
	lv_obj_set_style_border_width(drag_layer, 0, 0);
	lv_obj_set_style_pad_all(drag_layer, 0, 0);
	lv_obj_remove_flag(drag_layer, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_remove_flag(drag_layer, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(drag_layer, LV_OBJ_FLAG_IGNORE_LAYOUT);

	lv_obj_add_event_cb(ccsettings_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(ccsettings_screen);

	refresh();
}

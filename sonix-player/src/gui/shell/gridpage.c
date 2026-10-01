#include "gridpage.h"

#include "lvgl/lvgl.h"

#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"
#include "src/gui/shell/uiscale.h"

#define GRID_GAP ui_px(14)
#define TILE_RADIUS ui_px(12) // Adwaita card radius

static void unavailable_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active() || coverflow_drag_active()) {
		return; // this was a swipe across the tile, not a tap on it
	}

	const char *label = lv_event_get_user_data(e);
	char message[160];

	// tr() on the way in as well: what is stored on the tile is the tag, and
	// the message has to name the tile the way the tile does.
	lv_snprintf(message, sizeof(message), tr("grid_not_yet_available"), tr(label));
	gui_notify_popup(message);
}

static void action_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active() || coverflow_drag_active()) {
		return; // a swipe, not a tap
	}

	void (*action)(void) = (void (*)(void))lv_event_get_user_data(e);
	action();
}

// The two children add_tile() puts on every tile, in the order it makes them.
#define TILE_ICON_CHILD 0
#define TILE_LABEL_CHILD 1

void gridpage_set_tile(lv_obj_t *grid, int index, const lv_image_dsc_t *icon, const char *label) {
	if (!grid || index < 0 || index >= (int)lv_obj_get_child_count(grid)) {
		return;
	}
	lv_obj_t *tile = lv_obj_get_child(grid, index);
	if (!tile) {
		return;
	}
	if (icon) {
		lv_image_set_src(lv_obj_get_child(tile, TILE_ICON_CHILD), icon);
	}
	if (label) {
		lv_label_set_text(lv_obj_get_child(tile, TILE_LABEL_CHILD), tr(label));
	}
}

// Inside a tile: its edge to the content, and the picture to the caption.
#define TILE_PAD ui_px(8)
#define TILE_GAP ui_px(8)

// The tallest picture every tile of the grid can carry beside its caption.
//
// A caption that wraps to a second line needs the room of that line, and on a
// short panel (the original R3's, laid out at 480x640) the picture does not
// leave it: the second line runs off the bottom of the tile. The pictures give
// way -- all of them, so the grid keeps one size -- and only as much as the
// longest caption asks. Never below half, where a picture stops being one.
static int32_t icon_room(const grid_entry_t *entries, int count, int width, int height) {
	int32_t room = LV_COORD_MAX;
	int32_t tallest = 0;
	for (int i = 0; i < count; i++) {
		lv_point_t text;
		lv_text_get_size(&text, tr(entries[i].label), &font_ui_24_bold, 0, 0, width - 2 * TILE_PAD,
						 LV_TEXT_FLAG_NONE);
		int32_t left = height - 2 * TILE_PAD - TILE_GAP - text.y;
		room = left < room ? left : room;
		if (entries[i].icon && (int32_t)entries[i].icon->header.h > tallest) {
			tallest = (int32_t)entries[i].icon->header.h;
		}
	}
	return room < tallest / 2 ? tallest / 2 : room;
}

static void add_tile(lv_obj_t *grid, const grid_entry_t *entry, int width, int height, int32_t icon_max) {
	lv_obj_t *tile = lv_btn_create(grid);
	lv_obj_set_size(tile, width, height);
	lv_obj_add_style(tile, &theme_style_card, 0);
	lv_obj_add_style(tile, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(tile, TILE_RADIUS, 0);
	lv_obj_set_style_shadow_width(tile, 0, 0);
	lv_obj_set_style_pad_all(tile, TILE_PAD, 0);

	// Presses bubble up to the grid so a swipe can start on a tile: the whole
	// page has to be draggable, not just the gaps between the tiles.
	lv_obj_add_flag(tile, LV_OBJ_FLAG_EVENT_BUBBLE);

	lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_gap(tile, TILE_GAP, 0);
	lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

	// The tile artwork keeps its own colours, so unlike the interface glyphs it
	// is not run through the theme's recolour.
	lv_obj_t *icon = lv_image_create(tile);
	lv_image_set_src(icon, entry->icon);

	// The caption wraps rather than running out of the tile: a translated label
	// such as "Album-Interpreten" does not fit on one line. Centred, because a
	// tile is read as a block under its picture.
	lv_obj_t *label = lv_label_create(tile);
	lv_label_set_text(label, tr(entry->label));
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_24_bold, 0);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

	// Drawn smaller when the grid is too short for its pictures and captions
	// (see icon_room()).
	int32_t icon_h = entry->icon ? (int32_t)entry->icon->header.h : 0;
	if (icon_h > 0 && icon_max < icon_h) {
		lv_obj_set_size(icon, (int32_t)entry->icon->header.w * icon_max / icon_h, icon_max);
		lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_STRETCH);
	}

	if (entry->target) {
		lv_obj_add_event_cb(tile, switch_screen_cb, LV_EVENT_CLICKED, *entry->target);
	} else if (entry->action) {
		lv_obj_add_event_cb(tile, action_cb, LV_EVENT_CLICKED, (void *)entry->action);
	} else {
		lv_obj_add_event_cb(tile, unavailable_cb, LV_EVENT_CLICKED, (void *)entry->label);
		lv_obj_set_style_opa(icon, LV_OPA_40, 0);
		lv_obj_add_style(label, &theme_style_text_dim, 0);
	}
}

lv_obj_t *gridpage_build(lv_obj_t *screen, gui_config_t *cfg, const grid_entry_t *entries, int count, int columns,
						 int rows, bool clear_corner_buttons) {
	lv_obj_add_style(screen, &theme_style_screen, 0);

	int top = clear_corner_buttons ? settingsrow_content_top(cfg) : cfg->top_bar_height;

	lv_obj_t *grid = lv_obj_create(screen);
	lv_obj_set_size(grid, lv_pct(100), cfg->screen_height - top);
	lv_obj_align(grid, LV_ALIGN_TOP_LEFT, 0, top);
	lv_obj_set_style_bg_opa(grid, 0, 0);
	lv_obj_set_style_border_width(grid, 0, 0);
	lv_obj_set_style_radius(grid, 0, 0);
	lv_obj_set_style_pad_all(grid, cfg->padding, 0);
	// Under the corner buttons settingsrow_content_top() has already left the
	// padding below them, as it does on the settings pages; a second one on top
	// of it would only be an empty band over the first row.
	if (clear_corner_buttons) {
		lv_obj_set_style_pad_top(grid, 0, 0);
	}
	lv_obj_set_style_pad_gap(grid, GRID_GAP, 0);
	lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
	// START on the main axis, not CENTER: the tile size is computed for a full
	// `columns` x `rows` grid, so a page with fewer entries reads as that grid
	// with empty places. Centring would drift an odd tile into the middle of
	// its row, giving that page a shape no other one has.
	lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

	int usable_w = cfg->screen_width - (2 * cfg->padding);
	int usable_h = (cfg->screen_height - top) - (clear_corner_buttons ? 1 : 2) * cfg->padding;

	int tile_w = (usable_w - (columns - 1) * GRID_GAP) / columns;
	int tile_h = (usable_h - (rows - 1) * GRID_GAP) / rows;

	int32_t icon_max = icon_room(entries, count, tile_w, tile_h);
	for (int i = 0; i < count; i++) {
		add_tile(grid, &entries[i], tile_w, tile_h, icon_max);
	}

	// The player can be pulled in from any tiled page.
	player_sheet_attach_drag(grid, true);
	// Presses die here (the tiles bubble only one level up), so the grid must
	// carry the swipe-back drag itself, like the lists do.
	switcher_attach_back_gesture(grid);

	// Handed back for the same reason: a caller that wants a gesture of its own
	// on the page has to put it on this object, because nothing below it
	// bubbles any further.
	return grid;
}

lv_obj_t *gridpage_empty_panel(lv_obj_t *screen, gui_config_t *cfg, const lv_image_dsc_t *icon, const char *text,
							   lv_event_cb_t scan_cb) {
	int top = settingsrow_content_top(cfg);

	lv_obj_t *panel = lv_obj_create(screen);
	lv_obj_remove_style_all(panel);
	lv_obj_set_size(panel, lv_pct(100), cfg->screen_height - top);
	lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 0, top);
	lv_obj_set_style_pad_hor(panel, cfg->padding * 2, 0);
	lv_obj_set_style_pad_bottom(panel, cfg->padding * 2, 0);
	lv_obj_set_style_pad_row(panel, ui_px(22), 0);
	lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);

	lv_obj_t *picture = lv_image_create(panel);
	lv_image_set_src(picture, icon);
	lv_obj_add_style(picture, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(picture, LV_OPA_COVER, 0);

	lv_obj_t *label = lv_label_create(panel);
	lv_label_set_text(label, tr(text));
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(label, &font_ui_24, 0);

	lv_obj_t *button = lv_btn_create(panel);
	lv_obj_set_size(button, ui_px(240), ui_px(68));
	lv_obj_add_style(button, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_bg_color(button, theme()->accent, 0);
	lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_add_event_cb(button, scan_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *button_label = lv_label_create(button);
	lv_label_set_text(button_label, tr("scan"));
	lv_obj_set_style_text_font(button_label, &font_ui_24, 0);
	lv_obj_set_style_text_color(button_label, lv_color_white(), 0);
	lv_obj_center(button_label);

	return panel;
}

void gridpage_show_empty(lv_obj_t *grid, lv_obj_t *panel, bool empty) {
	if (grid) {
		if (empty) {
			lv_obj_add_flag(grid, LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_remove_flag(grid, LV_OBJ_FLAG_HIDDEN);
		}
	}
	if (panel) {
		if (empty) {
			// The accent may have changed since the panel was built.
			lv_obj_set_style_bg_color(lv_obj_get_child(panel, 2), theme()->accent, 0);
			lv_obj_remove_flag(panel, LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
		}
	}
}

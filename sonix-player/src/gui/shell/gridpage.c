#include "gridpage.h"

#include "lvgl/lvgl.h"

#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"

#define GRID_GAP 14
#define COMPACT_TILE_ICON_MAX 46

static bool compact_grid(void) { return bp_is_tempotec_v1(); }
static int grid_gap(void) { return compact_grid() ? 6 : GRID_GAP; }

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

static void add_tile(lv_obj_t *grid, const grid_entry_t *entry, int width, int height) {
	const bool compact = compact_grid();
	lv_obj_t *tile = lv_btn_create(grid);
	lv_obj_set_size(tile, width, height);
	lv_obj_add_style(tile, &theme_style_card, 0);
	lv_obj_add_style(tile, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(tile, bp_tile_radius(), 0);
	lv_obj_set_style_shadow_width(tile, 0, 0);
	lv_obj_set_style_pad_all(tile, compact ? 3 : 8, 0);

	// Presses bubble up to the grid so a swipe can start on a tile: the whole
	// page has to be draggable, not just the gaps between the tiles.
	lv_obj_set_event_bubble(tile, true);

	lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_gap(tile, compact ? 2 : 8, 0);
	lv_obj_set_scrollable(tile, false);

	// The source menu art is 112-128 px.  On a compact three-row page an entire
	// tile can be only about 75 px high, so both its drawing and its layout box
	// have to shrink.  lv_image_set_scale() changes only the drawing: leaving
	// the object's intrinsic 128 px box in flex layout pushes the caption below
	// the tile even though the visible icon looks small.  This explicit box is
	// what keeps the caption in the card on the V1.
	lv_obj_t *icon = lv_image_create(tile);
	lv_image_set_src(icon, entry->icon);
	if (compact && entry->icon) {
		int source_w = (int)entry->icon->header.w;
		int source_h = (int)entry->icon->header.h;
		int max_side = LV_MAX(source_w, source_h);
		if (max_side > COMPACT_TILE_ICON_MAX) {
			int icon_w = LV_MAX(1, source_w * COMPACT_TILE_ICON_MAX / max_side);
			int icon_h = LV_MAX(1, source_h * COMPACT_TILE_ICON_MAX / max_side);
			lv_obj_set_size(icon, icon_w, icon_h);
			lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
			lv_image_set_scale(icon, (uint32_t)(LV_SCALE_NONE * COMPACT_TILE_ICON_MAX / max_side));
		}
	}

	// Compact captions are deliberately one line with an ellipsis.  A wrapped
	// French or German caption used to put the second line's descenders outside
	// the short V1 tile.  Pinning the height and zeroing line spacing gives
	// LV_LABEL_LONG_DOT an exact clipping boundary.
	lv_obj_t *label = lv_label_create(tile);
	lv_label_set_text(label, tr(entry->label));
	lv_obj_add_style(label, &theme_style_text, 0);
	const lv_font_t *label_font = compact ? bp_tile_label_font() : &font_ui_24_bold;
	lv_obj_set_style_text_font(label, label_font, 0);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	if (compact) {
		lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
		lv_obj_set_style_text_line_space(label, 0, 0);
		lv_obj_set_height(label, lv_font_get_line_height(label_font));
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
	int gap = grid_gap();
	lv_obj_set_style_pad_gap(grid, gap, 0);
	lv_obj_set_scrollable(grid, false);

	lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
	// START on the main axis, not CENTER: the tile size is computed for a full
	// `columns` x `rows` grid, so a page with fewer entries reads as that grid
	// with empty places. Centring would drift an odd tile into the middle of
	// its row, giving that page a shape no other one has.
	lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

	int usable_w = cfg->screen_width - (2 * cfg->padding);
	int usable_h = (cfg->screen_height - top) - (2 * cfg->padding);

	int tile_w = (usable_w - (columns - 1) * gap) / columns;
	int tile_h = (usable_h - (rows - 1) * gap) / rows;

	for (int i = 0; i < count; i++) {
		// Five entries in a 2x3 page used to leave a conspicuous dead card-sized
		// hole at bottom right.  Let the final destination span that row; pages
		// with four entries still intentionally leave their third row empty.
		bool lone_last = count == columns * rows - 1 && i == count - 1;
		add_tile(grid, &entries[i], lone_last ? usable_w : tile_w, tile_h);
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
	lv_obj_set_style_pad_row(panel, 22, 0);
	lv_obj_set_scrollable(panel, false);
	lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_hidden(panel, true);

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
	lv_obj_set_size(button, 240, 68);
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
			lv_obj_set_hidden(grid, true);
		} else {
			lv_obj_set_hidden(grid, false);
		}
	}
	if (panel) {
		if (empty) {
			// The accent may have changed since the panel was built.
			lv_obj_set_style_bg_color(lv_obj_get_child(panel, 2), theme()->accent, 0);
			lv_obj_set_hidden(panel, false);
		} else {
			lv_obj_set_hidden(panel, true);
		}
	}
}

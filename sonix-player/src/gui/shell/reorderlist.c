#include "reorderlist.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"

#define ROW_HEIGHT 72
#define ROW_GAP 8
#define ROW_PITCH (ROW_HEIGHT + ROW_GAP)
#define ROW_RADIUS 12
#define ROW_PAD 20
#define ICON_BOX 56
// Tile pictures are 112 px: shown at half that.
#define ARTWORK_SCALE (LV_SCALE_NONE / 2)
#define HEADING_H 40

// The grip is only as large as its glyph and a margin round it; the room kept
// for it at the end of the row is wider.
#define GRIP_SIZE 48
#define GRIP_SPACE 64

// Every list is a drop target even when it is empty, so none is ever shorter
// than one row.
#define LIST_MIN_ROWS 1

// How far the finger has to travel before a release counts as a drop.
#define DROP_MIN_TRAVEL 8

// How close to the top or bottom of the page a held row has to be before the
// page moves under it, and how far it moves each time the event comes round.
#define EDGE_PX 70
#define EDGE_STEP 18

typedef struct {
	lv_obj_t *row;
	lv_obj_t *icon;
	lv_obj_t *name;
	lv_obj_t *grip;
} entry_t;

typedef struct {
	reorderlist_def_t def;
	lv_obj_t *page;
	lv_obj_t *body;
	lv_obj_t *headings[REORDERLIST_MAX_LISTS];
	lv_obj_t *counts[REORDERLIST_MAX_LISTS];
	lv_obj_t *drop_line;
	entry_t *entries;

	// Which item each row is showing: the first list's, then the next's.
	// Rebuilt at every repaint, so the drag reads them rather than a number
	// baked in at build time.
	int *row_list;
	int *row_position;
	int row_count;

	// The row being carried, or -1.
	int drag_row;
	bool drag_moved;
	lv_point_t drag_from;
} reorderlist_t;

// Where each part of the canvas starts. Each heading follows the list above
// it, so all of them move as rows cross between lists.
static int heading_y(const reorderlist_t *rl, int list) {
	int y = 0;
	for (int l = 0; l < list; l++) {
		int n = rl->def.count(l);
		y += HEADING_H + (n < LIST_MIN_ROWS ? LIST_MIN_ROWS : n) * ROW_PITCH + 8;
	}
	return y;
}

static int list_top(const reorderlist_t *rl, int list) { return heading_y(rl, list) + HEADING_H; }

static void refresh(reorderlist_t *rl) {
	rl->row_count = 0;
	for (int l = 0; l < rl->def.lists; l++) {
		int n = rl->def.count(l);
		for (int i = 0; i < n && rl->row_count < rl->def.max_rows; i++, rl->row_count++) {
			rl->row_list[rl->row_count] = l;
			rl->row_position[rl->row_count] = i;
			lv_obj_set_y(rl->entries[rl->row_count].row, list_top(rl, l) + i * ROW_PITCH);
		}
		lv_obj_set_y(rl->headings[l], heading_y(rl, l) + 10);
		if (rl->counts[l]) {
			lv_obj_set_y(rl->counts[l], heading_y(rl, l) + 10);
			lv_label_set_text_fmt(rl->counts[l], "%d/%d", n, rl->def.limits[l]);
		}
	}

	for (int i = 0; i < rl->def.max_rows; i++) {
		if (i >= rl->row_count) {
			lv_obj_set_hidden(rl->entries[i].row, true);
			continue;
		}
		lv_obj_set_hidden(rl->entries[i].row, false);
		lv_image_set_src(rl->entries[i].icon, rl->def.icon(rl->row_list[i], rl->row_position[i]));
		lv_label_set_text(rl->entries[i].name, tr(rl->def.name(rl->row_list[i], rl->row_position[i])));
	}

	lv_obj_set_height(rl->body, heading_y(rl, rl->def.lists));

	if (rl->def.refreshed) {
		rl->def.refreshed();
	}
}

void reorderlist_refresh(lv_obj_t *list) {
	reorderlist_t *rl = list ? lv_obj_get_user_data(list) : NULL;
	if (rl) {
		refresh(rl);
	}
}

static void refresh_async(void *data) { refresh(data); }

// ---------------------------------------------------------------------------
// Carrying a row
// ---------------------------------------------------------------------------

// Where a row would land if the finger lifted at `point`: which list, and the
// gap it would go into, counted with the carried row still in its place.
typedef struct {
	int list;
	int at;
} target_t;

static target_t target_at(const reorderlist_t *rl, lv_point_t point) {
	lv_area_t area;
	lv_obj_get_coords(rl->body, &area);
	int y = point.y - area.y1;

	target_t t = {0, 0};
	for (int l = 1; l < rl->def.lists; l++) {
		if (y >= heading_y(rl, l)) {
			t.list = l;
		}
	}
	int count = rl->def.count(t.list);
	t.at = (y - list_top(rl, t.list) + ROW_PITCH / 2) / ROW_PITCH;
	if (t.at < 0) {
		t.at = 0;
	}
	if (t.at > count) {
		t.at = count;
	}
	return t;
}

// The accent rule in the gap a drop would go into. On the canvas, so it
// scrolls with the rows.
static void drop_line_show(reorderlist_t *rl, target_t t) {
	lv_obj_set_pos(rl->drop_line, 0, list_top(rl, t.list) + t.at * ROW_PITCH - ROW_GAP / 2 - 2);
	lv_obj_set_hidden(rl->drop_line, false);
	lv_obj_move_foreground(rl->drop_line);
}

static void carried_look(reorderlist_t *rl, int index, bool on) {
	lv_obj_t *row = rl->entries[index].row;
	lv_obj_set_style_border_width(row, on ? 2 : 0, 0);
	if (on) {
		lv_obj_set_style_border_color(row, theme()->accent, 0);
		lv_obj_set_style_border_opa(row, LV_OPA_COVER, 0);
	}
}

// The gap is counted with the carried row in place; once it is lifted out,
// every gap below it in its own list is one earlier.
static void drop(reorderlist_t *rl, int index, target_t t) {
	int from_list = rl->row_list[index];
	int from = rl->row_position[index];
	int at = t.at;
	if (t.list == from_list && from < at) {
		at--;
	}
	if (t.list == from_list && at == from) {
		return;
	}
	rl->def.move(from_list, from, t.list, at);
}

// The page follows the finger when a held row reaches either end of it.
static void edge_scroll(reorderlist_t *rl, lv_point_t point) {
	lv_area_t view;
	lv_obj_get_coords(rl->page, &view);
	int step = 0;
	if (point.y < view.y1 + EDGE_PX) {
		step = -EDGE_STEP;
	} else if (point.y > view.y2 - EDGE_PX) {
		step = EDGE_STEP;
	}
	if (step == 0) {
		return;
	}
	int scroll = lv_obj_get_scroll_y(rl->page);
	int limit = scroll + lv_obj_get_scroll_bottom(rl->page);
	int wanted = scroll + step;
	wanted = wanted < 0 ? 0 : (wanted > limit ? limit : wanted);
	if (wanted != scroll) {
		lv_obj_scroll_to_y(rl->page, wanted, LV_ANIM_OFF);
	}
}

static void drag_cb(lv_event_t *e) {
	reorderlist_t *rl = lv_event_get_user_data(e);
	lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *grip = lv_event_get_current_target(e);

	lv_indev_t *indev = lv_indev_active();
	lv_point_t point = {0, 0};
	if (indev) {
		lv_indev_get_point(indev, &point);
	}

	if (code == LV_EVENT_PRESSED) {
		rl->drag_row = -1;
		for (int i = 0; i < rl->row_count; i++) {
			if (rl->entries[i].grip == grip) {
				rl->drag_row = i;
			}
		}
		if (rl->drag_row < 0) {
			return;
		}
		rl->drag_moved = false;
		rl->drag_from = point;
		carried_look(rl, rl->drag_row, true);
		return;
	}

	if (rl->drag_row < 0) {
		return;
	}

	if (code == LV_EVENT_PRESSING) {
		edge_scroll(rl, point);
		if (LV_ABS(point.y - rl->drag_from.y) > DROP_MIN_TRAVEL ||
			LV_ABS(point.x - rl->drag_from.x) > DROP_MIN_TRAVEL) {
			rl->drag_moved = true;
		}
		if (rl->drag_moved) {
			drop_line_show(rl, target_at(rl, point));
		}
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		int index = rl->drag_row;
		rl->drag_row = -1;
		lv_obj_set_hidden(rl->drop_line, true);
		carried_look(rl, index, false);

		if (code == LV_EVENT_RELEASED && rl->drag_moved) {
			drop(rl, index, target_at(rl, point));
		}
		// Not here: the repaint rebinds the very row whose release is being
		// handled. It waits for the event to unwind.
		lv_async_call(refresh_async, rl);
	}
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

static void loaded_cb(lv_event_t *e) { refresh(lv_event_get_user_data(e)); }

static void deleted_cb(lv_event_t *e) {
	reorderlist_t *rl = lv_event_get_user_data(e);
	lv_async_call_cancel(refresh_async, rl);
	lv_obj_remove_event_cb_with_user_data(lv_obj_get_screen(rl->page), loaded_cb, rl);
	lv_free(rl->entries);
	lv_free(rl->row_list);
	lv_free(rl->row_position);
	lv_free(rl);
}

static lv_obj_t *make_heading(lv_obj_t *parent, const char *tag) {
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text(label, tr(tag));
	lv_obj_add_style(label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_obj_set_x(label, 4);
	return label;
}

static void make_row(reorderlist_t *rl, entry_t *entry, int width) {
	lv_obj_t *row = lv_obj_create(rl->body);
	lv_obj_set_size(row, width, ROW_HEIGHT);
	lv_obj_set_x(row, 0);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_set_style_radius(row, ROW_RADIUS, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, ROW_PAD, 0);
	lv_obj_set_style_pad_ver(row, 0, 0);
	lv_obj_set_scrollable(row, false);
	entry->row = row;

	lv_obj_t *box = lv_obj_create(row);
	lv_obj_remove_style_all(box);
	lv_obj_set_size(box, ICON_BOX, ROW_HEIGHT);
	lv_obj_align(box, LV_ALIGN_LEFT_MID, 0, 0);
	lv_obj_set_clickable(box, false);

	entry->icon = lv_image_create(box);
	if (rl->def.artwork) {
		// A tile's picture keeps its colours, as on the tile.
		lv_image_set_scale(entry->icon, ARTWORK_SCALE);
	} else {
		lv_obj_add_style(entry->icon, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(entry->icon, LV_OPA_COVER, 0);
	}
	lv_obj_center(entry->icon);

	entry->name = lv_label_create(row);
	lv_obj_add_style(entry->name, &theme_style_text, 0);
	lv_obj_set_style_text_font(entry->name, &font_ui_24, 0);
	lv_label_set_long_mode(entry->name, LV_LABEL_LONG_DOT);
	lv_obj_set_width(entry->name, width - ROW_PAD - ICON_BOX - 16 - GRIP_SPACE);
	lv_obj_align(entry->name, LV_ALIGN_LEFT_MID, ICON_BOX + 16, 0);

	// The handle, and the only part of the row that can be taken hold of. Not
	// a button: a tap on it does nothing. A drag on it must not find the page
	// and scroll it, hence no scroll chain.
	entry->grip = lv_obj_create(row);
	lv_obj_set_size(entry->grip, GRIP_SIZE, GRIP_SIZE);
	// Centred in its room, which runs into the row's right padding.
	lv_obj_align(entry->grip, LV_ALIGN_RIGHT_MID, ROW_PAD - (GRIP_SPACE - GRIP_SIZE) / 2, 0);
	lv_obj_set_style_bg_opa(entry->grip, 0, 0);
	lv_obj_set_style_border_width(entry->grip, 0, 0);
	lv_obj_set_style_pad_all(entry->grip, 0, 0);
	lv_obj_set_scrollable(entry->grip, false);
	lv_obj_set_scroll_chain_ver(entry->grip, false);
	lv_obj_set_scroll_chain_hor(entry->grip, false);
	lv_obj_set_clickable(entry->grip, true);

	lv_obj_t *grip_icon = lv_image_create(entry->grip);
	lv_image_set_src(grip_icon, &icon_grip);
	lv_obj_add_style(grip_icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(grip_icon, LV_OPA_COVER, 0);
	lv_obj_center(grip_icon);

	lv_obj_add_event_cb(entry->grip, drag_cb, LV_EVENT_PRESSED, rl);
	lv_obj_add_event_cb(entry->grip, drag_cb, LV_EVENT_PRESSING, rl);
	lv_obj_add_event_cb(entry->grip, drag_cb, LV_EVENT_RELEASED, rl);
	lv_obj_add_event_cb(entry->grip, drag_cb, LV_EVENT_PRESS_LOST, rl);
}

lv_obj_t *reorderlist_create(lv_obj_t *page, int width, const reorderlist_def_t *def) {
	reorderlist_t *rl = lv_malloc_zeroed(sizeof(*rl));
	if (!rl) {
		return NULL;
	}
	rl->def = *def;
	if (rl->def.lists > REORDERLIST_MAX_LISTS) {
		rl->def.lists = REORDERLIST_MAX_LISTS;
	}
	rl->page = page;
	rl->drag_row = -1;
	rl->entries = lv_malloc_zeroed((size_t)def->max_rows * sizeof(*rl->entries));
	rl->row_list = lv_malloc_zeroed((size_t)def->max_rows * sizeof(*rl->row_list));
	rl->row_position = lv_malloc_zeroed((size_t)def->max_rows * sizeof(*rl->row_position));

	rl->body = lv_obj_create(page);
	lv_obj_set_width(rl->body, width);
	lv_obj_set_height(rl->body, ROW_PITCH);
	lv_obj_set_style_bg_opa(rl->body, 0, 0);
	lv_obj_set_style_border_width(rl->body, 0, 0);
	lv_obj_set_style_pad_all(rl->body, 0, 0);
	lv_obj_set_scrollable(rl->body, false);
	lv_obj_set_user_data(rl->body, rl);
	lv_obj_add_event_cb(rl->body, deleted_cb, LV_EVENT_DELETE, rl);

	for (int l = 0; l < rl->def.lists; l++) {
		rl->headings[l] = make_heading(rl->body, def->headings[l]);
		// How many of the places are taken, level with the heading.
		if (def->limits[l] > 0) {
			rl->counts[l] = lv_label_create(rl->body);
			lv_obj_add_style(rl->counts[l], &theme_style_text_dim, 0);
			lv_obj_set_style_text_font(rl->counts[l], &font_ui_22, 0);
			lv_obj_align(rl->counts[l], LV_ALIGN_TOP_RIGHT, -4, 0);
		}
	}

	for (int i = 0; i < def->max_rows; i++) {
		make_row(rl, &rl->entries[i], width);
	}

	rl->drop_line = lv_obj_create(rl->body);
	lv_obj_set_size(rl->drop_line, width, 4);
	lv_obj_set_scrollable(rl->drop_line, false);
	lv_obj_set_clickable(rl->drop_line, false);
	lv_obj_set_style_border_width(rl->drop_line, 0, 0);
	lv_obj_set_style_shadow_width(rl->drop_line, 0, 0);
	lv_obj_set_style_pad_all(rl->drop_line, 0, 0);
	lv_obj_add_style(rl->drop_line, &theme_style_accent_bg, 0);
	lv_obj_set_style_radius(rl->drop_line, 2, 0);
	lv_obj_set_hidden(rl->drop_line, true);

	lv_obj_add_event_cb(lv_obj_get_screen(page), loaded_cb, LV_EVENT_SCREEN_LOADED, rl);

	refresh(rl);
	return rl->body;
}

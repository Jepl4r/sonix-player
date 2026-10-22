#include "homescreensettings.h"

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/homescreen.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"

lv_obj_t *homescreensettings_screen;

// ---------------------------------------------------------------------------
// Three lists on one canvas
//
// The Music page's rearranging, with a third list: the main menu's tiles on
// top, in their order, the More page's under them, the hidden ones last, a
// heading each. Each row carries the tile's picture, at half size, and its
// name.
//
// A row is taken by its grip and nowhere else, wears the accent border while it
// is carried, and an accent rule shows where it would land. All three lists
// live on one canvas, so a row crossing from one to another is one coordinate
// and not a change of parent. The page scrolls by itself while a row is held
// near its top or bottom edge.
//
// Under the canvas, a switch for each tile inside Streaming and Wireless,
// greyed while its section is hidden.
// ---------------------------------------------------------------------------

#define ROW_HEIGHT 72
#define ROW_GAP 8
#define ROW_PITCH (ROW_HEIGHT + ROW_GAP)
#define ROW_RADIUS 12
#define ROW_PAD 20
#define ICON_BOX 56
// The tiles' own pictures are 112 px: shown at half that.
#define ICON_SCALE (LV_SCALE_NONE / 2)
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

// Every tile of the main menu and the More page.
#define MAX_TILE_ROWS 16

// The switches under the canvas.
#define MAX_SECTION_ROWS 24

#define PLACES HOMESCREEN_PLACE_COUNT

static const char *const HEADINGS[PLACES] = {
	[HOMESCREEN_HOME] = "homescreen",
	[HOMESCREEN_MORE] = "more",
	[HOMESCREEN_HIDDEN] = "homescreen_hidden",
};

typedef struct {
	lv_obj_t *row;
	lv_obj_t *icon;
	lv_obj_t *name;
	lv_obj_t *grip;
} entry_t;

static lv_obj_t *page;
static lv_obj_t *body;
static lv_obj_t *headings[PLACES];
static lv_obj_t *counts[PLACES]; // NULL for the hidden list, which has no limit
static lv_obj_t *drop_line;
static entry_t entries[MAX_TILE_ROWS];

// Which tile each row is showing: the main menu's first, then More's, then the
// hidden ones. Rebuilt at every repaint, so the drag reads them rather than a
// number baked in at build time.
static const char *row_tile[MAX_TILE_ROWS];
static int row_count;

// The row being carried, or -1.
static int drag_row = -1;
static bool drag_moved;
static lv_point_t drag_from;

typedef struct {
	lv_obj_t *card;
	lv_obj_t *toggle;
	const char *section;
	const char *label;
} section_row_t;

static section_row_t section_rows[MAX_SECTION_ROWS];
static int section_row_count;

static void refresh(void);
static void refresh_async(void *unused);

// Where each part of the canvas starts. Each heading follows the list above
// it, so all of them move as tiles cross between lists.
static int heading_y(int place) {
	int y = 0;
	for (int p = 0; p < place; p++) {
		int n = homescreen_count((homescreen_place_t)p);
		y += HEADING_H + (n < LIST_MIN_ROWS ? LIST_MIN_ROWS : n) * ROW_PITCH + 8;
	}
	return y;
}

static int list_top(int place) { return heading_y(place) + HEADING_H; }

// A section's switches greyed while the section is hidden: they keep their own
// setting for when it comes back.
static void refresh_sections(void) {
	for (int i = 0; i < section_row_count; i++) {
		section_row_t *row = &section_rows[i];
		if (homescreen_place(row->section) != HOMESCREEN_HIDDEN) {
			lv_obj_remove_state(row->toggle, LV_STATE_DISABLED);
			lv_obj_set_style_opa(row->card, LV_OPA_COVER, 0);
		} else {
			lv_obj_add_state(row->toggle, LV_STATE_DISABLED);
			lv_obj_set_style_opa(row->card, LV_OPA_40, 0);
		}
	}
}

static void refresh(void) {
	row_count = 0;
	for (int p = 0; p < PLACES; p++) {
		int n = homescreen_count((homescreen_place_t)p);
		for (int i = 0; i < n && row_count < MAX_TILE_ROWS; i++, row_count++) {
			row_tile[row_count] = homescreen_at((homescreen_place_t)p, i);
			lv_obj_set_y(entries[row_count].row, list_top(p) + i * ROW_PITCH);
		}
		lv_obj_set_y(headings[p], heading_y(p) + 10);
		if (counts[p]) {
			lv_obj_set_y(counts[p], heading_y(p) + 10);
			lv_label_set_text_fmt(counts[p], "%d/%d", n, HOMESCREEN_SLOTS);
		}
	}

	for (int i = 0; i < MAX_TILE_ROWS; i++) {
		if (i >= row_count) {
			lv_obj_set_hidden(entries[i].row, true);
			continue;
		}
		lv_obj_set_hidden(entries[i].row, false);
		lv_image_set_src(entries[i].icon, homescreen_icon(row_tile[i]));
		lv_label_set_text(entries[i].name, tr(row_tile[i]));
	}

	lv_obj_set_height(body, heading_y(PLACES) + 8);
	refresh_sections();
}

static void refresh_async(void *unused) {
	(void)unused;
	refresh();
}

// ---------------------------------------------------------------------------
// Carrying a row
// ---------------------------------------------------------------------------

// Where a row would land if the finger lifted at `point`: which list, and the
// gap it would go into, counted with the carried row still in its place.
typedef struct {
	homescreen_place_t place;
	int at;
} target_t;

static target_t target_at(lv_point_t point) {
	lv_area_t area;
	lv_obj_get_coords(body, &area);
	int y = point.y - area.y1;

	target_t t;
	t.place = HOMESCREEN_HOME;
	for (int p = 1; p < PLACES; p++) {
		if (y >= heading_y(p)) {
			t.place = (homescreen_place_t)p;
		}
	}
	int count = homescreen_count(t.place);
	t.at = (y - list_top(t.place) + ROW_PITCH / 2) / ROW_PITCH;
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
static void drop_line_show(target_t t) {
	lv_obj_set_pos(drop_line, 0, list_top(t.place) + t.at * ROW_PITCH - ROW_GAP / 2 - 2);
	lv_obj_set_hidden(drop_line, false);
	lv_obj_move_foreground(drop_line);
}

static void carried_look(int index, bool on) {
	lv_obj_t *row = entries[index].row;
	lv_obj_set_style_border_width(row, on ? 2 : 0, 0);
	if (on) {
		lv_obj_set_style_border_color(row, theme()->accent, 0);
		lv_obj_set_style_border_opa(row, LV_OPA_COVER, 0);
	}
}

// The gap is counted with the carried row in place; once it is lifted out,
// every gap below it in its own list is one earlier.
static void drop(int index, target_t t) {
	const char *tile = row_tile[index];
	homescreen_place_t from_place = homescreen_place(tile);
	int from = index;
	for (int p = 0; p < (int)from_place; p++) {
		from -= homescreen_count((homescreen_place_t)p);
	}
	int at = t.at;
	if (t.place == from_place && from < at) {
		at--;
	}
	if (t.place == from_place && at == from) {
		return;
	}
	switch (homescreen_move(tile, t.place, at)) {
	case HOMESCREEN_HOME_FULL:
		toast_error("homescreen_home_full");
		break;
	case HOMESCREEN_MORE_FULL:
		toast_error("homescreen_more_full");
		break;
	case HOMESCREEN_SETTINGS_HIDDEN:
		toast_error("homescreen_settings_hidden");
		break;
	case HOMESCREEN_MORE_IN_MORE:
		toast_error("homescreen_more_in_more");
		break;
	case HOMESCREEN_SETTINGS_UNREACHABLE:
		toast_error("homescreen_settings_needs_more");
		break;
	case HOMESCREEN_MOVED:
	case HOMESCREEN_UNKNOWN:
		break;
	}
}

// The page follows the finger when a held row reaches either end of it.
static void edge_scroll(lv_point_t point) {
	lv_area_t view;
	lv_obj_get_coords(page, &view);
	int step = 0;
	if (point.y < view.y1 + EDGE_PX) {
		step = -EDGE_STEP;
	} else if (point.y > view.y2 - EDGE_PX) {
		step = EDGE_STEP;
	}
	if (step == 0) {
		return;
	}
	int scroll = lv_obj_get_scroll_y(page);
	int limit = scroll + lv_obj_get_scroll_bottom(page);
	int wanted = scroll + step;
	wanted = wanted < 0 ? 0 : (wanted > limit ? limit : wanted);
	if (wanted != scroll) {
		lv_obj_scroll_to_y(page, wanted, LV_ANIM_OFF);
	}
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
		drag_row = -1;
		for (int i = 0; i < row_count; i++) {
			if (entries[i].grip == grip) {
				drag_row = i;
			}
		}
		if (drag_row < 0) {
			return;
		}
		drag_moved = false;
		drag_from = point;
		carried_look(drag_row, true);
		return;
	}

	if (drag_row < 0) {
		return;
	}

	if (code == LV_EVENT_PRESSING) {
		edge_scroll(point);
		if (LV_ABS(point.y - drag_from.y) > DROP_MIN_TRAVEL || LV_ABS(point.x - drag_from.x) > DROP_MIN_TRAVEL) {
			drag_moved = true;
		}
		if (drag_moved) {
			drop_line_show(target_at(point));
		}
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		int index = drag_row;
		drag_row = -1;
		lv_obj_set_hidden(drop_line, true);
		carried_look(index, false);

		if (code == LV_EVENT_RELEASED && drag_moved) {
			drop(index, target_at(point));
		}
		// Not here: the repaint rebinds the very row whose release is being
		// handled. It waits for the event to unwind.
		lv_async_call(refresh_async, NULL);
	}
}

// ---------------------------------------------------------------------------
// A section's own tiles
// ---------------------------------------------------------------------------

static void section_toggled_cb(lv_event_t *e) {
	lv_obj_t *toggle = lv_event_get_target(e);
	for (int i = 0; i < section_row_count; i++) {
		if (section_rows[i].toggle == toggle) {
			homescreen_set_section_tile_visible(section_rows[i].section, section_rows[i].label,
												lv_obj_has_state(toggle, LV_STATE_CHECKED));
			return;
		}
	}
}

// Named with the section in front, "Streaming → Tidal".
static void add_section_row(lv_obj_t *container, const char *section, const char *label) {
	if (section_row_count >= MAX_SECTION_ROWS) {
		return;
	}
	section_row_t *row = &section_rows[section_row_count++];
	row->section = section;
	row->label = label;
	row->card = settingsrow_toggle(container, label, &row->toggle, section_toggled_cb);
	if (homescreen_section_tile_visible(section, label)) {
		lv_obj_add_state(row->toggle, LV_STATE_CHECKED);
	}
	lv_label_set_text_fmt(lv_obj_get_child(row->card, 0), "%s → %s", tr(section), tr(label));
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

static void loaded_cb(lv_event_t *e) {
	(void)e;
	refresh();
}

static lv_obj_t *make_heading(lv_obj_t *parent, const char *tag) {
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text(label, tr(tag));
	lv_obj_add_style(label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_obj_set_x(label, 4);
	return label;
}

static void make_row(entry_t *entry, int width) {
	lv_obj_t *row = lv_obj_create(body);
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

	// The tile's picture keeps its colours, as on the tile.
	entry->icon = lv_image_create(box);
	lv_image_set_scale(entry->icon, ICON_SCALE);
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

	lv_obj_add_event_cb(entry->grip, drag_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(entry->grip, drag_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(entry->grip, drag_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(entry->grip, drag_cb, LV_EVENT_PRESS_LOST, NULL);
}

void homescreensettings_init(gui_config_t *cfg) {
	page = settingsrow_page(homescreensettings_screen, cfg, "homescreen");

	int width = cfg->screen_width - 2 * cfg->padding;

	body = lv_obj_create(page);
	lv_obj_set_width(body, width);
	lv_obj_set_height(body, ROW_PITCH);
	lv_obj_set_style_bg_opa(body, 0, 0);
	lv_obj_set_style_border_width(body, 0, 0);
	lv_obj_set_style_pad_all(body, 0, 0);
	lv_obj_set_scrollable(body, false);

	for (int p = 0; p < PLACES; p++) {
		headings[p] = make_heading(body, HEADINGS[p]);
		// How many of the six places are taken, level with the heading.
		if (p != HOMESCREEN_HIDDEN) {
			counts[p] = lv_label_create(body);
			lv_obj_add_style(counts[p], &theme_style_text_dim, 0);
			lv_obj_set_style_text_font(counts[p], &font_ui_22, 0);
			lv_obj_align(counts[p], LV_ALIGN_TOP_RIGHT, -4, 0);
		}
	}

	for (int i = 0; i < MAX_TILE_ROWS; i++) {
		make_row(&entries[i], width);
	}

	drop_line = lv_obj_create(body);
	lv_obj_set_size(drop_line, width, 4);
	lv_obj_set_scrollable(drop_line, false);
	lv_obj_set_clickable(drop_line, false);
	lv_obj_set_style_border_width(drop_line, 0, 0);
	lv_obj_set_style_shadow_width(drop_line, 0, 0);
	lv_obj_set_style_pad_all(drop_line, 0, 0);
	lv_obj_add_style(drop_line, &theme_style_accent_bg, 0);
	lv_obj_set_style_radius(drop_line, 2, 0);
	lv_obj_set_hidden(drop_line, true);

	// The tiles inside the sections, under their own heading. Music's are not
	// here: Music settings > Rearrange main page already hides and orders them.
	lv_obj_t *section_heading = lv_label_create(page);
	lv_label_set_text(section_heading, tr("homescreen_section_tiles"));
	lv_obj_add_style(section_heading, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(section_heading, &font_ui_22, 0);
	// The full width, or the page's column centres it.
	lv_obj_set_width(section_heading, lv_pct(100));
	lv_obj_set_style_pad_left(section_heading, 4, 0);
	for (int p = 0; p < PLACES; p++) {
		int n = homescreen_count((homescreen_place_t)p);
		for (int i = 0; i < n; i++) {
			const char *section = homescreen_at((homescreen_place_t)p, i);
			int children = homescreen_section_tile_count(section);
			for (int j = 0; j < children; j++) {
				add_section_row(page, section, homescreen_section_tile_label(section, j));
			}
		}
	}
	lv_obj_set_hidden(section_heading, section_row_count == 0);

	lv_obj_add_event_cb(homescreensettings_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(homescreensettings_screen);

	refresh();
}

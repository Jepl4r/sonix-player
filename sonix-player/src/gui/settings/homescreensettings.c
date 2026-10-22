#include "homescreensettings.h"

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/homescreen.h"
#include "src/gui/shell/reorderlist.h"
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
// name. The dragging is reorderlist's.
//
// Under the canvas, a switch for each tile inside Streaming and Wireless,
// greyed while its section is hidden.
// ---------------------------------------------------------------------------

// The switches under the canvas.
#define MAX_SECTION_ROWS 24

// Every tile of the main menu and the More page.
#define MAX_TILE_ROWS 16

typedef struct {
	lv_obj_t *card;
	lv_obj_t *toggle;
	const char *section;
	const char *label;
} section_row_t;

static section_row_t section_rows[MAX_SECTION_ROWS];
static int section_row_count;

static int count(int list) { return homescreen_count((homescreen_place_t)list); }

static const lv_image_dsc_t *icon(int list, int position) {
	return homescreen_icon(homescreen_at((homescreen_place_t)list, position));
}

static const char *name(int list, int position) { return homescreen_at((homescreen_place_t)list, position); }

static void move(int from_list, int from, int to_list, int to) {
	switch (homescreen_move(homescreen_at((homescreen_place_t)from_list, from), (homescreen_place_t)to_list, to)) {
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

// ---------------------------------------------------------------------------
// A section's own tiles
// ---------------------------------------------------------------------------

// A section's switches greyed while the section is hidden: they keep their own
// setting for when it comes back. After every repaint of the lists.
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

// Named with the section in front, "Streaming -> Tidal"; the language file
// says how the two are joined.
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
	lv_label_set_text_fmt(lv_obj_get_child(row->card, 0), tr("homescreen_section_tile"), tr(section), tr(label));
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

void homescreensettings_init(gui_config_t *cfg) {
	lv_obj_t *page = settingsrow_page(homescreensettings_screen, cfg, "homescreen");

	// How many of the six places are taken, level with the main menu's and the
	// More page's headings.
	const reorderlist_def_t def = {
		.lists = HOMESCREEN_PLACE_COUNT,
		.headings = {[HOMESCREEN_HOME] = "homescreen", [HOMESCREEN_MORE] = "more", [HOMESCREEN_HIDDEN] = "homescreen_hidden"},
		.limits = {[HOMESCREEN_HOME] = HOMESCREEN_SLOTS, [HOMESCREEN_MORE] = HOMESCREEN_SLOTS},
		.max_rows = MAX_TILE_ROWS,
		.artwork = true,
		.count = count,
		.icon = icon,
		.name = name,
		.move = move,
		.refreshed = refresh_sections,
	};
	reorderlist_create(page, cfg->screen_width - 2 * cfg->padding, &def);

	// The tiles inside the sections, under their own heading. Music's are not
	// here: Music settings > Rearrange main page already hides and orders them.
	lv_obj_t *section_heading = lv_label_create(page);
	lv_label_set_text(section_heading, tr("homescreen_section_tiles"));
	lv_obj_add_style(section_heading, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(section_heading, &font_ui_22, 0);
	// The full width, or the page's column centres it.
	lv_obj_set_width(section_heading, lv_pct(100));
	lv_obj_set_style_pad_left(section_heading, 4, 0);
	for (int p = 0; p < HOMESCREEN_PLACE_COUNT; p++) {
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
	// The switches were made after the lists' first painting.
	refresh_sections();

	switcher_attach_back_gesture(homescreensettings_screen);
}

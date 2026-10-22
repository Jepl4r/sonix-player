#include "ccsettings.h"

#include "lvgl/lvgl.h"

#include "src/gui/shell/quickpanel.h"
#include "src/gui/shell/reorderlist.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"

lv_obj_t *ccsettings_screen;

// ---------------------------------------------------------------------------
// Two lists on one canvas
//
// The same page as the keyboard layouts: the buttons in the panel on top, the
// ones left out underneath, a heading each. Each row carries the button's glyph
// and its name, so what a glyph is for can be read here rather than guessed.
// Every button has a row, more than fit on the screen, so the page scrolls.
// The dragging is reorderlist's.
// ---------------------------------------------------------------------------

#define IN_USE 0

static quickpanel_button_t button_at(int list, int position) {
	return list == IN_USE ? quickpanel_in_use_at(position) : quickpanel_hidden_at(position);
}

static int count(int list) { return list == IN_USE ? quickpanel_in_use_count() : quickpanel_hidden_count(); }

static const lv_image_dsc_t *icon(int list, int position) {
	return quickpanel_button_icon(button_at(list, position));
}

static const char *name(int list, int position) { return quickpanel_button_tag(button_at(list, position)); }

static void move(int from_list, int from, int to_list, int to) {
	quickpanel_button_t button = button_at(from_list, from);
	if (to_list == IN_USE) {
		quickpanel_move_in_use(button, to);
	} else {
		quickpanel_move_hidden(button, to);
	}
}

void ccsettings_init(gui_config_t *cfg) {
	ccsettings_screen = lv_obj_create(NULL);
	lv_obj_add_style(ccsettings_screen, &theme_style_screen, 0);

	lv_obj_t *page = settingsrow_page(ccsettings_screen, cfg, "control_centre");

	// How many of the sixteen places are taken, level with the first heading.
	const reorderlist_def_t def = {
		.lists = 2,
		.headings = {"in_use", "controlcentre_not_in_use"},
		.limits = {QP_SLOT_COUNT, 0},
		.max_rows = QP_BTN_COUNT,
		.artwork = false,
		.count = count,
		.icon = icon,
		.name = name,
		.move = move,
	};
	reorderlist_create(page, cfg->screen_width - 2 * cfg->padding, &def);

	switcher_attach_back_gesture(ccsettings_screen);
}

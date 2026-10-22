#include "musictiles.h"

#include "lvgl/lvgl.h"

#include "src/gui/library/music.h"
#include "src/gui/shell/reorderlist.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"

lv_obj_t *musictiles_screen;

// ---------------------------------------------------------------------------
// Two lists on one canvas
//
// The control centre's page, for the Music page's tiles: the tiles on the page
// on top, in their order, the ones left out underneath, a heading each. Each
// row carries the tile's picture, at half size, and its name. The dragging is
// reorderlist's.
// ---------------------------------------------------------------------------

#define IN_USE 0

static music_tile_t tile_at(int list, int position) {
	return list == IN_USE ? music_tiles_in_use_at(position) : music_tiles_hidden_at(position);
}

static int count(int list) { return list == IN_USE ? music_tiles_in_use_count() : music_tiles_hidden_count(); }

static const lv_image_dsc_t *icon(int list, int position) { return music_tile_icon(tile_at(list, position)); }

static const char *name(int list, int position) { return music_tile_tag(tile_at(list, position)); }

static void move(int from_list, int from, int to_list, int to) {
	music_tile_t tile = tile_at(from_list, from);
	if (to_list == IN_USE) {
		music_tiles_move_in_use(tile, to);
	} else if (!music_tiles_move_hidden(tile, to)) {
		toast_error("musictiles_keep_one");
	}
}

void musictiles_init(gui_config_t *cfg) {
	musictiles_screen = lv_obj_create(NULL);
	lv_obj_add_style(musictiles_screen, &theme_style_screen, 0);

	lv_obj_t *page = settingsrow_page(musictiles_screen, cfg, "musicsettings_rearrange_main_page");
	// No buttons in the corner, so the heading may run to the edge: the name
	// of the page is long in every language.
	settingsrow_title_corner_slots(settingsrow_page_title(musictiles_screen), cfg, 0);

	// How many of the tiles are on the page, level with the first heading.
	const reorderlist_def_t def = {
		.lists = 2,
		.headings = {"in_use", "controlcentre_not_in_use"},
		.limits = {MUSIC_TILE_COUNT, 0},
		.max_rows = MUSIC_TILE_COUNT,
		.artwork = true,
		.count = count,
		.icon = icon,
		.name = name,
		.move = move,
	};
	reorderlist_create(page, cfg->screen_width - 2 * cfg->padding, &def);

	switcher_attach_back_gesture(musictiles_screen);
}

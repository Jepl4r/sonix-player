#ifndef HOMESCREEN_H
#define HOMESCREEN_H

#include <stdbool.h>

#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Which tiles the main menu, the More page and the sections behind them show,
// and in what order (Settings > More > Homescreen).
//
// The main menu's sections and the More page's tiles share three lists: the
// main menu, the More page, and hidden. A section used rarely can step down
// into More and a More tile used daily can step up, and both pages show their
// tiles in the order of their list. Both keep their two by three grid and never
// scroll: six tiles each, and a seventh is refused. Settings is never hidden,
// since without it nothing could be put back: it is on the main menu, or on the
// More page while the More tile is on the main menu to reach it by. The More
// tile itself is on the main menu or hidden.
//
// The tiles inside Streaming and Wireless are only shown or hidden, on their
// own section page.
//
// Hiding a tile only leaves it out of its grid: the page behind it is still
// built, and still reachable from anywhere else that leads to it. A section
// whose tiles are all off opens on an empty grid.
//
// Every grid is built empty, hands its full list of tiles over once, and is
// filled here, again whenever a list or a switch changes.

// How many tiles the main menu and the More page each hold.
#define HOMESCREEN_SLOTS 6

typedef enum {
	HOMESCREEN_HOME,
	HOMESCREEN_MORE,
	HOMESCREEN_HIDDEN,
	HOMESCREEN_PLACE_COUNT,
} homescreen_place_t;

// What a move came to.
typedef enum {
	HOMESCREEN_MOVED,
	HOMESCREEN_HOME_FULL,
	HOMESCREEN_MORE_FULL,
	HOMESCREEN_SETTINGS_HIDDEN,
	HOMESCREEN_MORE_IN_MORE,
	// Settings in More with the More tile not on the main menu: one way or the
	// other, out of reach.
	HOMESCREEN_SETTINGS_UNREACHABLE,
	// No such tile, or no such list: nothing moved.
	HOMESCREEN_UNKNOWN,
} homescreen_move_t;

// The main menu and the More page, with the tiles that start on each. `entries`
// is copied.
void homescreen_register_home(lv_obj_t *grid, gui_config_t *cfg, const grid_entry_t *entries, int count);
void homescreen_register_more(lv_obj_t *grid, gui_config_t *cfg, const grid_entry_t *entries, int count);

// A section's own grid. `section` is the section's tile on the main menu
// ("streaming"). `entries` is copied.
void homescreen_register_section(lv_obj_t *grid, gui_config_t *cfg, const char *section, const grid_entry_t *entries,
								 int count, int columns, int rows, bool clear_corner_buttons);

// The three lists, in order. Tiles are named by their label tag, which is
// never translated.
int homescreen_count(homescreen_place_t place);
const char *homescreen_at(homescreen_place_t place, int position);
const lv_image_dsc_t *homescreen_icon(const char *label);
homescreen_place_t homescreen_place(const char *label);

// Takes the tile out of its list and puts it at `position` in `place`'s.
homescreen_move_t homescreen_move(const char *label, homescreen_place_t place, int position);

// A section's own tiles, shown or not.
int homescreen_section_tile_count(const char *section);
const char *homescreen_section_tile_label(const char *section, int index);
bool homescreen_section_tile_visible(const char *section, const char *label);
void homescreen_set_section_tile_visible(const char *section, const char *label, bool visible);

#endif /* HOMESCREEN_H */

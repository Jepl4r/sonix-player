#include "morepage.h"

#include "lvgl/lvgl.h"

#include "src/gui/audio/dacpage.h"
#include "src/gui/audio/metronomepage.h"
#include "src/gui/ebook/ebookpage.h"
#include "src/gui/flappybird/flappybird.h"
#include "src/gui/library/filespage.h"
#include "src/gui/gearboy/gearboypage.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/system/core/config.h"

lv_obj_t *morepage_screen;

typedef struct {
	grid_entry_t entry;
	bool game;
} more_tile_t;

static const more_tile_t TILES[] = {
	{{"dac", &icon_menu_dac, &dacpage_screen, NULL}, false},
#if GB_CORE
	// The Game Boy emulator. Present only when the binary was built with
	// GB=1 (see the Makefile).
	{{"gearboy", &icon_menu_gearboy, &gearboypage_screen, NULL}, true},
#endif
	// An action rather than a screen: the shelf reads the Ebook folder again
	// every time it is opened, so a book copied onto the card turns up
	// without a restart.
	{{"books", &icon_menu_books, NULL, ebookpage_open}, false},
	// An action for the same reason the shelf is one: the listing is read
	// when the page is entered, so a card that changed under the player
	// shows what is on it now.
	{{"file_explorer", &icon_menu_file_explorer, NULL, filespage_open}, false},
	// An action: the artwork is read from the resource tree on the way in.
	{{"flappy_bird", &icon_menu_flappy_bird, NULL, flappybird_open}, true},
	{{"metronome", &icon_menu_metronome, &metronomepage_screen, NULL}, false},
};

#define TILE_COUNT ((int)(sizeof(TILES) / sizeof(TILES[0])))

static lv_obj_t *tile_grid;
static gui_config_t *grid_cfg;

bool morepage_games_shown(void) { return config_get_bool("parental", "show_games", true); }

// The tiles to show, in order, into `entries`: all of them, or all but the
// games. Returns how many.
static int shown_entries(grid_entry_t *entries) {
	bool games = morepage_games_shown();
	int count = 0;
	for (int i = 0; i < TILE_COUNT; i++) {
		if (games || !TILES[i].game) {
			entries[count++] = TILES[i].entry;
		}
	}
	return count;
}

void morepage_set_games_shown(bool shown) {
	config_set_bool("parental", "show_games", shown);
	config_save();

	if (!tile_grid) {
		return;
	}
	grid_entry_t entries[TILE_COUNT];
	gridpage_set_entries(tile_grid, grid_cfg, entries, shown_entries(entries), 2, 3, true);
}

void morepage_init(gui_config_t *cfg) {
	grid_entry_t entries[TILE_COUNT];
	int count = shown_entries(entries);

	// Two columns by three rows, like the Music and Wireless pages, so a tile
	// here is exactly the size of a tile there. The grid is laid out as full
	// even with fewer entries, so the only tile sits where it would with six --
	// top left -- instead of floating in the middle of a page of its own.
	tile_grid = gridpage_build(morepage_screen, cfg, entries, count, 2, 3, true);
	grid_cfg = cfg;

	settingsrow_title(morepage_screen, cfg, "more");
}

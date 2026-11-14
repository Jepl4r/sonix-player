#include "streaming.h"

#include "lvgl/lvgl.h"

#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/streaming/podcastpage.h"
#include "src/gui/streaming/qobuzpage.h"
#include "src/gui/streaming/radiopage.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/streaming/subsonicpage.h"
#include "src/gui/shell/theme.h"
#include "src/gui/streaming/tidalpage.h"

lv_obj_t *streaming_screen;

// Where the Subsonic tile sits in the grid below, in build order.
#define SUBSONIC_TILE 4

// The blue of the Navidrome logo the tile carries (#0084FF).
#define NAV_BRAND_BLUE 0x0084ff

static lv_obj_t *subsonic_tile;

// The logo is drawn edge to edge in its own blue, so the tile around it
// borrows that blue instead of the neutral card: deep steel over the dark
// palette, pale blue over the light one, both far enough from the logo's
// #0084FF that the disc still reads as a disc. It cannot be a shared style --
// the colour moves with the palette -- so it is set here and set again after
// every palette change.
static void subsonic_tile_tint(void) {
	if (!subsonic_tile) {
		return;
	}

	const theme_palette_t *palette = theme();
	int rest = palette->dark ? 70 : 30;
	int pressed = palette->dark ? 110 : 70;

	lv_obj_set_style_bg_color(subsonic_tile, lv_color_mix(lv_color_hex(NAV_BRAND_BLUE), palette->surface, rest), 0);
	lv_obj_set_style_bg_color(subsonic_tile, lv_color_mix(lv_color_hex(NAV_BRAND_BLUE), palette->surface_pressed, pressed),
							  LV_STATE_PRESSED);
}

void streaming_init(gui_config_t *cfg) {
	qobuzpage_init(cfg);
	tidalpage_init(cfg);
	podcastpage_init(cfg);
	subsonicpage_init(cfg);

	const grid_entry_t entries[] = {
		{"tidal", &icon_menu_tidal, &tidal_screen, NULL},
		{"qobuz", &icon_menu_qobuz, &qobuz_screen, NULL},
		{"radio", &icon_menu_radio, &radiopage_screen, NULL},
		{"podcasts", &icon_menu_podcast, &podcast_screen, NULL},
		{"subsonic", &icon_menu_navidrome, &subsonic_screen, NULL},
	};

	// Two columns and three rows of tile, exactly like Music and Wireless, so a
	// tile here is the same size as a tile there. Five of them fill the first
	// two rows and the first cell of the third; the rest of it stays empty
	// rather than the tiles growing to fill it.
	lv_obj_t *grid = gridpage_build(streaming_screen, cfg, entries, (int)(sizeof(entries) / sizeof(entries[0])), 2, 3, true);

	// The grid is built, so the Subsonic tile exists: paint it in the logo's
	// blue and keep it in step with the palette from here on.
	subsonic_tile = grid ? lv_obj_get_child(grid, SUBSONIC_TILE) : NULL;
	subsonic_tile_tint();

	static bool tint_registered;
	if (!tint_registered) {
		theme_register_refresh(subsonic_tile_tint);
		tint_registered = true;
	}

	settingsrow_title(streaming_screen, cfg, "streaming");
}

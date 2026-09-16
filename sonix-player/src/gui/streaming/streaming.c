#include "streaming.h"

#include "lvgl/lvgl.h"

#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/streaming/podcastpage.h"
#include "src/gui/streaming/qobuzpage.h"
#include "src/gui/streaming/radiopage.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/streaming/tidalpage.h"

lv_obj_t *streaming_screen;

void streaming_init(gui_config_t *cfg) {
	qobuzpage_init(cfg);
	tidalpage_init(cfg);
	podcastpage_init(cfg);

	const grid_entry_t entries[] = {
		{"tidal", &icon_menu_tidal, &tidal_screen, NULL},
		{"qobuz", &icon_menu_qobuz, &qobuz_screen, NULL},
		{"radio", &icon_menu_radio, &radiopage_screen, NULL},
		{"podcasts", &icon_menu_podcast, &podcast_screen, NULL},
	};

	// Two columns and three rows of tile, exactly like Music and Wireless, so a
	// tile here is the same size as a tile there. Four of them fill the first
	// two rows; the third stays empty rather than the tiles growing to fill it.
	gridpage_build(streaming_screen, cfg, entries, (int)(sizeof(entries) / sizeof(entries[0])), 2, 3, true);

	settingsrow_title(streaming_screen, cfg, "streaming");
}

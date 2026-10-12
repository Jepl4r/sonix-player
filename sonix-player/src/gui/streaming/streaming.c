#include "streaming.h"

#include "lvgl/lvgl.h"

#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/streaming/podcastpage.h"
#include "src/gui/streaming/qobuzpage.h"
#include "src/gui/streaming/radiopage.h"
#include "src/gui/streaming/subsonicpage.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/streaming/tidalpage.h"
#include "src/gui/streaming/yandexpage.h"

lv_obj_t *streaming_screen;

void streaming_init(gui_config_t *cfg) {
	qobuzpage_init(cfg);
	tidalpage_init(cfg);
	podcastpage_init(cfg);
	subsonicpage_init(cfg);
	yandexpage_init(cfg);

	const grid_entry_t entries[] = {
		{"tidal", &icon_menu_tidal, &tidal_screen, NULL},
		{"qobuz", &icon_menu_qobuz, &qobuz_screen, NULL},
		{"radio", &icon_menu_radio, &radiopage_screen, NULL},
		{"podcasts", &icon_menu_podcast, &podcast_screen, NULL},
		// The user's own server: Navidrome, Gonic, Airsonic...
		{"subsonic", &icon_menu_subsonic, &subsonic_screen, NULL},
		{"yandex", &icon_menu_yandex, &yandex_screen, NULL}, // last: dropped without keys
	};
	int count = (int)(sizeof(entries) / sizeof(entries[0])) - (yandexpage_available() ? 0 : 1);

	// Two columns and three rows of tile, exactly like Music and Wireless, so a
	// tile here is the same size as a tile there. Five or six of them fill
	// the grid; Yandex Music is left out when its keys are missing.
	gridpage_build(streaming_screen, cfg, entries, count, 2, 3, true);

	settingsrow_title(streaming_screen, cfg, "streaming");
}

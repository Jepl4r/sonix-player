#include "wireless.h"

#include "lvgl/lvgl.h"

#include "src/gui/wireless/airplay.h"
#include "src/gui/bluetooth/btsettings.h"
#include "src/gui/wireless/dlna.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/wireless/sonixlink.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/gui/wireless/wifitransfer.h"

lv_obj_t *wireless_screen;

void wireless_init(gui_config_t *cfg) {
	const grid_entry_t entries[] = {
		{"wi_fi", &icon_menu_wifi_settings, &wifisettings_screen, NULL},
		{"bluetooth", &icon_menu_bluetooth, &btsettings_screen, NULL},
		{"airplay", &icon_menu_airplay, &airplay_screen, NULL},
		{"transfer", &icon_menu_wifi_transfer, &wifitransfer_screen, NULL},
		{"sonixlink", &icon_menu_sonixlink, &sonixlink_screen, NULL},
		{"dlna", &icon_menu_dlna, &dlna_screen, NULL},
	};

	// Two columns and three rows' worth of tile, like the Music section, so a
	// tile here is exactly the size of a tile there. Six entries fill the grid
	// exactly.
	gridpage_build(wireless_screen, cfg, entries, (int)(sizeof(entries) / sizeof(entries[0])), 2, 3, true);

	settingsrow_title(wireless_screen, cfg, "wireless");
}

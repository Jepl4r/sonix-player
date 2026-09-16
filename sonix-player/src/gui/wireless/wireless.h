#ifndef WIRELESS_H
#define WIRELESS_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Wireless section: the same tiled page the main menu and Music use. Wi-Fi,
// Bluetooth, AirPlay, Wi-Fi transfer, DLNA and SonixLink each open their own
// page.

extern lv_obj_t *wireless_screen;

void wireless_init(gui_config_t *cfg);

#endif /* WIRELESS_H */

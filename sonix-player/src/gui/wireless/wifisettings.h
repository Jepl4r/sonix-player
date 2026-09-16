#ifndef WIFISETTINGS_H
#define WIFISETTINGS_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Wi-Fi page: the switch, what the radio is doing right now, and the list
// of networks -- the ones in the air and the ones wpa_supplicant already
// knows, merged into one list with the connected one on top.
//
// Tapping a network joins it; a secured one it has never seen opens the
// passphrase page first. Tapping one it is already on, or one it has saved,
// opens a small menu instead (connect / disconnect / forget).

extern lv_obj_t *wifisettings_screen;

void wifisettings_init(gui_config_t *cfg);

#endif /* WIFISETTINGS_H */

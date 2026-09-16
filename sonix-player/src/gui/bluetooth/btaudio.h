#ifndef BTAUDIO_H
#define BTAUDIO_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// "Bluetooth audio", behind the gear in the corner of the Bluetooth page --
// the same corner button the Music section has.
//
// Everything about A2DP lives here rather than on the device list: which codec
// the link runs, and whether the player's volume drives the headphones' own
// over AVRCP.

extern lv_obj_t *btaudio_screen;

void btaudio_init(gui_config_t *cfg);

#endif /* BTAUDIO_H */

#ifndef BTSETTINGS_H
#define BTSETTINGS_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Bluetooth page: the switch, the name this device answers to, the
// devices it is already paired with, and whatever a search turns up.
//
// Switching the radio on can take the best part of ten seconds -- the whole
// stack is reloaded, because the player powers it down at boot -- so the page
// says what it is doing while that happens.

extern lv_obj_t *btsettings_screen;

void btsettings_init(gui_config_t *cfg);

#endif /* BTSETTINGS_H */

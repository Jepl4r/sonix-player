#ifndef GUI_AIRPLAY_H
#define GUI_AIRPLAY_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The AirPlay receiver's screen.
//
// The page only hosts the switch: turning AirPlay on here leaves it on, and a
// phone can carry on playing while the player is used for something else. That
// is how the stock firmware behaves, and how a receiver has to work -- its job
// is to stay findable.

extern lv_obj_t *airplay_screen;

void airplay_page_init(gui_config_t *cfg);

#endif /* GUI_AIRPLAY_H */

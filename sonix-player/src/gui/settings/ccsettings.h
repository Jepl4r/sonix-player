#ifndef CCSETTINGS_H
#define CCSETTINGS_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Settings > More > Control centre: the round buttons of the pull-down panel,
// arranged by dragging them. The panel's eight places are the card at the top,
// with an empty ring wherever a button has been taken out; the buttons that are
// not in the panel are the card underneath. A tile is dragged between the two
// cards to take a button out or put one in, and between places in the top card
// to move it. The layout itself lives in quickpanel.c, which owns the buttons;
// this page is only the way to edit it.

extern lv_obj_t *ccsettings_screen;

void ccsettings_init(gui_config_t *cfg);

#endif /* CCSETTINGS_H */

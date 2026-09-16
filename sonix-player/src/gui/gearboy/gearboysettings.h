#ifndef GEARBOYSETTINGS_H
#define GEARBOYSETTINGS_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Gearboy settings: Game Boy palette, shader (the real panel's pixel grid),
// GBC colour correction, boot ROM at startup. Opened from the Gearboy page's
// gear icon and from the in-game menu's settings entry; changes take effect at
// once, including mid-game, except boot ROMs which apply from the next game.
extern lv_obj_t *gearboysettings_screen;

void gearboysettings_init(gui_config_t *cfg);

#endif /* GEARBOYSETTINGS_H */

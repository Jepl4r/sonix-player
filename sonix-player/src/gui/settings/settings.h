#ifndef SETTINGS_H
#define SETTINGS_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

extern lv_obj_t *settings_screen;

void settings_init(gui_config_t *cfg);

// Shows or hides the developer options entry according to whether it has been
// unlocked. The page is built once at startup, so it has to be told when the
// five taps on the build number unlock it.
void settings_refresh_devoptions(void);

#endif // SETTINGS_H

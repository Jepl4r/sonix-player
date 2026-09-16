#ifndef POWERSETTINGS_H
#define POWERSETTINGS_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Everything about how the device spends its battery: when the screen goes
// dark, how full to charge, and whether to switch itself off when left alone.
extern lv_obj_t *powersettings_screen;

void powersettings_init(gui_config_t *cfg);

// Pushes the saved values into the power manager. Called at startup so the
// settings apply without the page having been opened.
void powersettings_apply(void);

#endif /* POWERSETTINGS_H */

#ifndef DEVOPTIONS_H
#define DEVOPTIONS_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Settings that only matter while something is being debugged: ADB,
// screenshots, whether the log is written to the card, and the process list.
// Kept off the main settings page so that page stays about the player rather
// than about developing it.
extern lv_obj_t *devoptions_screen;

void devoptions_init(gui_config_t *cfg);

#endif /* DEVOPTIONS_H */

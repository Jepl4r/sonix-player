#ifndef APPEARANCE_H
#define APPEARANCE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Appearance page: theme (dark, the default, or light) as two
// Adwaita-style segmented buttons rather than a row that silently flips, plus
// the accent colour, the clock position and the battery percentage.

extern lv_obj_t *appearance_screen;

void appearance_init(gui_config_t *cfg);

#endif /* APPEARANCE_H */

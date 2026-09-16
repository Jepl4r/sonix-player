#ifndef GUI_GEARBOYPAGE_H
#define GUI_GEARBOYPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The game list: whatever is in Games/GB and Games/GBC on the card, titled
// from the database (see src/system/gbdb.h) rather than from the filename.

extern lv_obj_t *gearboypage_screen;

void gearboypage_init(gui_config_t *cfg);

#endif /* GUI_GEARBOYPAGE_H */

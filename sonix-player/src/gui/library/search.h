#ifndef SEARCH_H
#define SEARCH_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The search page: a text field with the on-screen keyboard, results grouped
// under Tracks / Albums / Artists as the query is typed. Opened from the
// magnifier button on the Music page.
extern lv_obj_t *search_screen;

void search_init(gui_config_t *cfg);
void search_open(void);

#endif // SEARCH_H

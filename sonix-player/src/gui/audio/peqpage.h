#ifndef PEQPAGE_H
#define PEQPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The parametric equaliser page and the one for editing a band. Builds both;
// the second is a single page reused by every band, since they are the
// same page with different numbers in it.
void peqpage_init(gui_config_t *cfg);

// The main page, for whoever navigates to it (the row on the Music page).
lv_obj_t *peqpage_screen(void);

#endif /* PEQPAGE_H */

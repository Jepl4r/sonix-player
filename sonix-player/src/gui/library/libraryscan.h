#ifndef LIBRARYSCAN_H
#define LIBRARYSCAN_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The page that indexes the card: a big note, a count that climbs while the
// scan runs, and an OK button once it is done.
extern lv_obj_t *libraryscan_screen;

void libraryscan_init(gui_config_t *cfg);

// Opening the page starts a scan. Called by the switcher when the page is
// loaded, so nothing else has to remember to kick it off.
void libraryscan_begin(void);

#endif /* LIBRARYSCAN_H */

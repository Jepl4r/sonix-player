#ifndef AUDIOBOOKS_H
#define AUDIOBOOKS_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The audiobooks section: four tiles (library, series, authors, continue
// listening) and a corner button for the finished books, over a books page and
// an authors-or-series page, an options page carrying the scan
// and the transport-button setting, the page that setting opens, and the scan
// progress page (the music one's layout with its own icon and words).
extern lv_obj_t *audiobooks_screen;
extern lv_obj_t *audiobooksettings_screen;
extern lv_obj_t *audiobookcontrols_screen;
extern lv_obj_t *audiobookscan_screen;

void audiobooks_init(gui_config_t *cfg);

#endif /* AUDIOBOOKS_H */

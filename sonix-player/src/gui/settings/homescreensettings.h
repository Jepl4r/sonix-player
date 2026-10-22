#ifndef HOMESCREENSETTINGS_H
#define HOMESCREENSETTINGS_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Settings > More > Homescreen: a switch for every tile of the main menu and
// of the sections behind it, each section's tiles listed under the section's
// own. Switching one off only hides the tile; see homescreen.h.
//
// The screen is created with the others in gui_init(), so the row on the More
// page can point at it; its rows are made here, after every grid has
// registered its tiles.

extern lv_obj_t *homescreensettings_screen;

void homescreensettings_init(gui_config_t *cfg);

#endif /* HOMESCREENSETTINGS_H */

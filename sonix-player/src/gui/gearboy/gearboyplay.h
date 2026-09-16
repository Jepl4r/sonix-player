#ifndef GUI_GEARBOYPLAY_H
#define GUI_GEARBOYPLAY_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The play screen: the Game Boy scaled three times at the top, the controls
// below.
//
// 160x144 times three is 480x432, exactly the panel width and 432 of its 720
// rows. The remaining 288 rows are the button pad, which happens to be almost
// exactly the screen-to-buttons proportion of a real Game Boy.

extern lv_obj_t *gearboyplay_screen;

void gearboyplay_init(gui_config_t *cfg);

// Starts a game and opens the screen. The title comes from the database.
void gearboyplay_open(const char *rom_path, const char *title);

#endif /* GUI_GEARBOYPLAY_H */

#ifndef GUI_GEARBOYPLAY_H
#define GUI_GEARBOYPLAY_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The play screen: the Game Boy scaled three times at the top, the controls
// below.
//
// 160x144 times three is 480x432, exactly the panel width on both players and
// 432 of its rows. What is left is the button pad: 288 rows on the R3 Pro II,
// which happens to be almost exactly the screen-to-buttons proportion of a real
// Game Boy, and 368 on the R1, where the extra 80 hold Select and Start.

extern lv_obj_t *gearboyplay_screen;

void gearboyplay_init(gui_config_t *cfg);

// Starts a game and opens the screen. The title comes from the database.
void gearboyplay_open(const char *rom_path, const char *title);

#endif /* GUI_GEARBOYPLAY_H */

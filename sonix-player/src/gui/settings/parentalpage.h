#ifndef PARENTALPAGE_H
#define PARENTALPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Settings > More > Parental controls: what a child using the player can
// reach. One switch for now, the games on the More page (see morepage.h).
//
// The row opens the page on the fifth tap in quick succession, not the first,
// and says nothing before that. Not security -- anyone who knows the taps can
// open it -- but a child tapping about the settings does not land there.

extern lv_obj_t *parentalpage_screen;

void parentalpage_init(gui_config_t *cfg);

// Adds the row that opens the page, on the settings page that holds it.
void parentalpage_add_row(lv_obj_t *container);

#endif /* PARENTALPAGE_H */

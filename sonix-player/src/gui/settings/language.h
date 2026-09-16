#ifndef LANGUAGE_H
#define LANGUAGE_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Settings > Language: which of the files under /usr/resource/sonix/language is in
// use. See src/system/lang.h for how the translations themselves work.
//
// Two ways in, the same way the clock has two: the settings page, and a panel
// that comes up by itself the first time the player runs. The language goes
// first there, before the date and time -- a clock cannot be set in a language
// the user cannot read.
extern lv_obj_t *language_screen;

void language_init(gui_config_t *cfg);

// Hands over the Settings row that opens this page, so its name can be
// kept as "Lingua/Language" -- see language.c. Call it right after making the
// row; without it the row keeps whatever name it was built with.
void language_bind_menu_row(lv_obj_t *row);

// True when nobody has ever chosen: the panel should come up on its own.
bool language_needed(void);

// Brings that panel up. Confirming it hands over to the clock, if the clock
// needs setting too.
void language_show_first_boot(void);

#endif /* LANGUAGE_H */

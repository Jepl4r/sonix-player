#ifndef CHAPTERS_H
#define CHAPTERS_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The chapters of the audiobook playing now, and a way to jump between them.
//
// It replaces the overflow menu while a book is loaded, because on a book the
// overflow menu's contents -- the queue, the repeat mode -- are the wrong
// questions, and the right one is "which chapter".
extern lv_obj_t *chapters_screen;

void chapters_init(gui_config_t *cfg);

// Opens it from the player: closes the sheet, so backing out of the list
// comes back to the player rather than to whatever page was behind it.
void chapters_open(void);

#endif /* CHAPTERS_H */

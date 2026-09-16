#ifndef REMAP_H
#define REMAP_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Settings > Other > Remap keys.
//
// The page shows the side of the device -- a photograph, not a drawing -- and
// writes what each of the three buttons does beside it. Tapping the button in
// the photograph, or the row next to it, opens the choice.
//
// A photograph rather than three named rows because the buttons have no names
// anyone would recognise. "Previous" and "Next" are what they do now, and using
// that as a label on the page for changing what they do is circular; "the top
// button" is accurate but has to be counted out with a finger. The photograph
// removes the question.

extern lv_obj_t *remap_screen;

void remap_init(gui_config_t *cfg);

#endif /* REMAP_H */

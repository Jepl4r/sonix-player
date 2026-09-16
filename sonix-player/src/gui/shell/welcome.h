#ifndef WELCOME_H
#define WELCOME_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The first thing seen on a freshly started device: a welcome greeting cycling
// through the shipped languages, and a button to begin.
//
// It asks nothing: the language is chosen on the next page. It says the device
// has started and that it speaks the holder's language, which is the only
// useful information before one has been picked.

void welcome_init(gui_config_t *cfg);

// Shows the page. The start button closes it and opens the language choice
// itself, so the caller has nothing to chain.
void welcome_show_first_boot(void);

#endif /* WELCOME_H */

#ifndef PROCESSESPAGE_H
#define PROCESSESPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The "Processes" page under Developer options: how much RAM is in use and
// which processes are holding it. Read-only, taken from /proc on every open,
// so no timer runs behind a page nobody is looking at.
extern lv_obj_t *processespage_screen;

void processespage_init(gui_config_t *cfg);

#endif /* PROCESSESPAGE_H */

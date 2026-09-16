#ifndef SYSTEMPAGE_H
#define SYSTEMPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The system page: everything about the device itself rather than how it sounds
// or looks. It holds the information page, firmware update and factory reset.
extern lv_obj_t *systempage_screen;

// The information page: the DAC, card space, the two version numbers.
extern lv_obj_t *sysinfo_screen;

void systempage_init(gui_config_t *cfg);

// Whether the developer options have been unlocked (five taps on the build
// number). settings.c asks this to decide whether to show the entry.
bool systempage_devoptions_unlocked(void);

#endif /* SYSTEMPAGE_H */

#ifndef WHEELPAGE_H
#define WHEELPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Settings > More > Wheel, on a player with a volume wheel (sysinfo.h): whether the
// wheel turns the volume or scrubs through the track, which way is up and
// which way is forward, and how far a click scrubs. The input threads do the
// turning (system.h); this page only chooses.

// NULL on a player without a wheel: the More page leaves its row out.
extern lv_obj_t *wheelpage_screen;

void wheelpage_init(gui_config_t *cfg);

// The saved choices, handed to the input threads. Once at startup, whether or
// not the page is ever built, and again on every change.
void wheelpage_apply(void);

#endif /* WHEELPAGE_H */

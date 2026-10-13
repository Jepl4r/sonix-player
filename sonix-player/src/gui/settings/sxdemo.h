#ifndef SXDEMO_H
#define SXDEMO_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// ---------------------------------------------------------------------------
// The UI 2.0 component sheet (Developer options > "UI 2.0 components")
//
// Every 2.0 component on one page, with sample data, for checking them on the
// device and against the design: rows in each of their states (with working
// swipes, star and long press), the settings cards, the bottom sheet, the
// toast with Undo, the volume bar, the dithered gradient. Nothing here touches
// playback, the library or the config. Built on its first visit.
// ---------------------------------------------------------------------------

extern lv_obj_t *sxdemo_screen;

void sxdemo_init(gui_config_t *cfg);

#endif /* SXDEMO_H */

#ifndef MSEBSETTINGS_H
#define MSEBSETTINGS_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The MSEB page's own settings, behind the gear in its corner: saving the
// current tuning under a name, loading one back, and how far the sliders are
// allowed to travel.
//
// Presets are one small file each in <card>/.local/MSEB, so they can be
// copied off the device and back.

void msebsettings_init(gui_config_t *cfg);

// The screen the gear opens.
lv_obj_t *msebsettings_screen(void);

// Called after a preset is loaded, so the MSEB page can put its sliders where
// the new values are.
void msebsettings_set_reload_cb(void (*cb)(void));

#endif /* MSEBSETTINGS_H */

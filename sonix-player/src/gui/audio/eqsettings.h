#ifndef EQSETTINGS_H
#define EQSETTINGS_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The equalizer page's own settings, behind the gear in its corner: the
// ready-made curves, saving the current one under a name, and loading it back.
//
// The user's presets are one small file each in <card>/.local/eq, so they can
// be copied off the device and back.

void eqsettings_init(gui_config_t *cfg);

// The screen the gear opens.
lv_obj_t *eqsettings_screen(void);

// Called after a preset is applied, so the equalizer page can put its sliders
// where the new gains are.
void eqsettings_set_reload_cb(void (*cb)(void));

#endif /* EQSETTINGS_H */

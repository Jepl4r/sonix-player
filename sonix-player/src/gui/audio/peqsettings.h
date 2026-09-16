#ifndef PEQSETTINGS_H
#define PEQSETTINGS_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The page behind the PEQ's gear icon, the same one the graphic equaliser and
// MSEB have in their corner: saving the parametric chain under a name and
// loading it back.
//
// Presets are one file per name in <card>/.local/peq, so they can be taken off
// the device and put back -- which for a compensation curve, written once and
// used for years, is what matters.
//
// No factory presets: the graphic equaliser's ready-made curves are ten gains
// at fixed frequencies and mean nothing on a parametric chain.

void peqsettings_init(gui_config_t *cfg);

// The screen the gear icon opens.
lv_obj_t *peqsettings_screen(void);

// Called after a preset has been loaded, so the PEQ page puts its sliders where
// the loaded values ended up.
void peqsettings_set_reload_cb(void (*cb)(void));

#endif /* PEQSETTINGS_H */

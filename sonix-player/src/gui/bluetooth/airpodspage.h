#ifndef AIRPODSPAGE_H
#define AIRPODSPAGE_H

#include "src/gui/shell/gui.h"
#include "src/system/bluetooth/airpods.h"

#include "lvgl/lvgl.h"

// The AirPods page: what each half of the headphones and the case have left,
// under a picture of the model that is actually in the room.
//
// It is reached from the Bluetooth audio settings, and the row that leads here
// only exists while a pair is connected -- which is to say while the protocol
// session in airpods.h is up.

extern lv_obj_t *airpodspage_screen;

void airpodspage_init(gui_config_t *cfg);

// The small picture of the whole pair, for the row that opens this page.
const lv_image_dsc_t *airpodspage_model_icon(airpods_model_t model);

#endif /* AIRPODSPAGE_H */

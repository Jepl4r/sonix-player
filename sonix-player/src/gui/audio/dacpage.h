#ifndef DACPAGE_H
#define DACPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// USB DAC mode: the page that owns the device while a computer is playing
// through it.
//
// It is deliberately a trap. Once DAC mode is on the player is not a player
// any more -- the audio comes from the cable and the transport buttons would
// act on nothing. So this page stays in front, the back gesture and the
// hardware keys are held (volume and power excepted, which still do what they
// say), and leaving asks first.
extern lv_obj_t *dacpage_screen;

void dacpage_init(gui_config_t *cfg);

// True while the page owns the device, for the key handling and the gesture
// guards elsewhere to ask.
bool dacpage_is_holding(void);

#endif /* DACPAGE_H */

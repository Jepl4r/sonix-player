#ifndef BTRECEIVERPAGE_H
#define BTRECEIVERPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Bluetooth receiver mode: the page that owns the device while a phone or a
// computer is playing through it.
//
// The same trap as the DAC page, for the same reason. While it is receiving,
// the music is not this player's: the transport keys are sent over AVRCP to
// the machine the music is on, the control centre and the now playing screen
// are not reachable because they are about a track that is not there, and
// leaving asks first -- walking out of this page silences the room.
extern lv_obj_t *btreceiverpage_screen;

void btreceiverpage_init(gui_config_t *cfg);

#endif /* BTRECEIVERPAGE_H */

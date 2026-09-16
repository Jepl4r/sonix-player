#ifndef GUI_DLNA_H
#define GUI_DLNA_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The DLNA receiver page.
//
// Like the AirPlay one: the switch is the feature and the page is only where it
// lives. Switched on, the player appears in the device list of any app that can
// send music over the network, and stays there after leaving this page.
//
// This module also holds the tick that actually carries out the commands
// arriving from the phone: they come in on a network thread, but acting on them
// means touching playback, which belongs to the UI thread. That is why the tick
// does not stop when the page is left.

extern lv_obj_t *dlna_screen;

void dlna_page_init(gui_config_t *cfg);

#endif /* GUI_DLNA_H */

#ifndef PANEL_H
#define PANEL_H

#include <stdbool.h>

// Who owns the touchscreen.
//
// Declared here, defined in main.c -- that is where touch is opened -- with an
// empty version for the host build. Same shape as display_rotation_supported()
// and friends in power.h, which live there for the same reason (and
// display_get_rotated() is what a caller reading the panel itself needs:
// rotation has to be applied by hand, or the controls come out upside down).
//
// These exist for the emulator. A Game Boy is held with two thumbs and LVGL is
// not: its evdev indev is a pointer, a single contact, which cannot press the
// d-pad and A/B together. So while a game runs the panel changes owner: LVGL's
// indev is switched off, the emulator opens the same evdev node itself and
// reads the real multitouch protocol; on exit everything goes back.

// Switches LVGL's touch indev on and off. Off, LVGL does not read the node and
// moves nothing: the panel is free.
void panel_touch_enable(bool enabled);

// The evdev node LVGL's indev opened ("/dev/input/event1", or event0 when the
// first was absent). NULL when there is no touch device.
const char *panel_touch_device(void);

#endif /* PANEL_H */

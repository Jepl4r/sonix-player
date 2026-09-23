#ifndef GBINPUT_H
#define GBINPUT_H

#include <stdbool.h>
#include <stdint.h>

// The touchscreen as a gamepad, while a game runs.
//
// LVGL is not enough: its evdev driver is an INDEV_TYPE_POINTER -- one contact,
// one position, pressed or not. That allows one button at a time, and a Game
// Boy is held with two thumbs: running and jumping means right and A together,
// which is the game, not an edge case. So while playing, the screen changes
// owner: panel_touch_enable(false) parks LVGL's indev, and this module opens
// the same evdev node itself and reads the real multitouch protocol, which the
// patched touch driver reports -- gt9xx_touch.ko on the R3 Pro II,
// cst8xx_touch.ko on the R1. Both are uncapped by a script in tools/; PATCHES.md
// has the account of each.
//
// Zones come from outside in screen coordinates, and the reader thread decides
// which buttons are down: the mapping lives where the contacts are, not across
// a bridge to the UI. A finger sliding from the d-pad to A behaves correctly
// on its own, because the whole state is recomputed on every packet instead of
// tracking press and release.

// Not a Game Boy button: "open the in-game menu". It shares the mask because a
// zone is a zone, but the reader strips it before handing the buttons to the
// emulator and calls the menu callback instead.
//
// It is needed because while this module owns the screen LVGL's indev is off:
// no drawn object responds to touch, so the only way to have a command that is
// not a Game Boy button is a zone like any other. The menu sits in the middle
// of the game screen, where no hand rests while playing.
#define GBINPUT_KEY_MENU 0x8000

typedef struct {
	int x, y, w, h;
	uint16_t keys; // mask of GB_KEY_* (src/gb/gbcore.h), or GBINPUT_KEY_MENU
} gbinput_zone_t;

// How many contacts are tracked: an array size, not a request to the driver.
// The R3 Pro II's panel reports up to five, the R1's up to two.
#define GBINPUT_MAX_CONTACTS 5

// The size of the screen in pixels: 480x720 on the R3 Pro II, 480x800 on the
// R1. Zones are in screen coordinates and raw touches are mapped onto this.
// Call before gbinput_start(); zero or negative leaves the value unchanged.
void gbinput_set_glass(int width, int height);

// Takes the screen. `zones` is copied, so the caller may free it. False when
// the evdev node does not open: play then has no controls, which is useless but
// not a crash, and the caller tells the user.
// `on_menu` is called once when a finger touches the menu zone, and runs on the
// reader thread: the receiver must go through gui_post() to touch anything in
// LVGL. May be NULL.
bool gbinput_start(const gbinput_zone_t *zones, int count, void (*on_menu)(void));

// Hands the screen back and stops the thread. Also clears the buttons, or the
// last direction held would stay held forever.
void gbinput_stop(void);

bool gbinput_active(void);

#endif /* GBINPUT_H */

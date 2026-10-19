#ifndef KEYMAP_H
#define KEYMAP_H

#include <stdbool.h>

// What the buttons on the side of the device do.
//
// On the R3 Pro II's right side, below the power button, there are three: one
// on its own and a rocker split in two. On the left there is the volume rocker.
// The kernel reports the three media keys as KEY_NEXTSONG, KEY_PLAYPAUSE and
// KEY_PREVIOUSSONG, and reports the skip keys swapped with respect to their
// physical positions -- the stock firmware compensates for that and so does
// this one (see system.c). The R1 has volume up, volume down, play and next,
// all on the right, and no previous key. Nothing here speaks in kernel codes:
// the buttons are named as the person holding the device sees them.
//
// Why it is configurable: a player kept in a pocket is operated by feel, and
// which commands are worth having under a finger differs. Someone listening to
// albums never skips a track and would rather have volume; someone on playlists
// skips constantly. Three buttons, and the choice costs one list.
//
// The in-line headphone remote does not come through here: it sends the same
// codes, but it is another device with another convention (one click, two
// clicks, three clicks), and remapping it together with the side buttons would
// change two things while meaning to change one.

typedef enum {
	// Right side: the media rocker.
	KEYMAP_BTN_PREV = 0, // the top one, on its own
	KEYMAP_BTN_PLAY,	 // the rocker's upper half
	KEYMAP_BTN_NEXT,	 // the lower half

	// Left side: the volume rocker. Flipping the screen does not swap these:
	// up stays up.
	KEYMAP_BTN_VOL_UP,
	KEYMAP_BTN_VOL_DOWN,

	KEYMAP_BTN_COUNT,
} keymap_button_t;

typedef enum {
	KEYMAP_ACTION_NONE = 0,
	KEYMAP_ACTION_PLAY_PAUSE,
	KEYMAP_ACTION_PREV,
	KEYMAP_ACTION_NEXT,
	KEYMAP_ACTION_VOLUME_UP,
	KEYMAP_ACTION_VOLUME_DOWN,
	// Starts the workout timer at its chosen length, or starts it over
	// (workouttimer.h). Last, so the names already in a config keep meaning
	// what they meant.
	KEYMAP_ACTION_WORKOUT_TIMER,
	KEYMAP_ACTION_COUNT,
} keymap_action_t;

// Reads the configuration. Call once at startup, before the key threads start.
void keymap_init(void);

keymap_action_t keymap_get(keymap_button_t button);

// Applies and saves. The change takes effect from the next press: the threads
// read the table on every press rather than caching it.
void keymap_set(keymap_button_t button, keymap_action_t action);

// The double click: one button whose second press, coming quickly after the
// first, does another action -- and held, holds that action (a seek for next
// or previous, a repeat for the volume). KEYMAP_BTN_COUNT or
// KEYMAP_ACTION_NONE switch it off. The button's own action then waits for
// the double-click window to close before it runs.
keymap_button_t keymap_double_button(void);
keymap_action_t keymap_double_action(void);
void keymap_set_double(keymap_button_t button, keymap_action_t action);

// Whether the double click is on: a button and an action other than none.
bool keymap_double_enabled(void);

// The action's name, in the source's Italian like every other string here; it
// passes through tr() where it is drawn.
const char *keymap_action_name(keymap_action_t action);

#endif /* KEYMAP_H */

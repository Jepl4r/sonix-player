#include "keymap.h"

#include <stdio.h>
#include <string.h>

#include "src/system/core/config.h"

// Written by the remapping page on the UI thread, read by the physical-button
// threads. No lock: these are small ints read and written whole, so no reader
// can ever see half of one.
static keymap_action_t actions[KEYMAP_BTN_COUNT];

// Config keys, named after the BUTTONS rather than their kernel codes: if the
// kernel ever reports them in a different order, the user's choice stays
// attached to the right button.
static const char *const KEYS[KEYMAP_BTN_COUNT] = {
	"button_prev", "button_play", "button_next", "button_vol_up", "button_vol_down",
};

// What the buttons have always done, and keep doing until remapped.
static const keymap_action_t DEFAULTS[KEYMAP_BTN_COUNT] = {
	KEYMAP_ACTION_PREV,
	KEYMAP_ACTION_PLAY_PAUSE,
	KEYMAP_ACTION_NEXT,
	KEYMAP_ACTION_VOLUME_UP,
	KEYMAP_ACTION_VOLUME_DOWN,
};

// The config stores the name, not the number: a readable file is half the
// reason this player writes .ini instead of bytes.
static const char *const ACTION_KEYS[KEYMAP_ACTION_COUNT] = {
	"none", "play_pause", "prev", "next", "volume_up", "volume_down", "workout_timer",
};

static keymap_action_t action_from_key(const char *name, keymap_action_t fallback) {
	if (!name || !name[0]) {
		return fallback;
	}
	for (int i = 0; i < KEYMAP_ACTION_COUNT; i++) {
		if (strcmp(name, ACTION_KEYS[i]) == 0) {
			return (keymap_action_t)i;
		}
	}
	return fallback;
}

// The double click, off until chosen. Same threading as the table above.
static keymap_button_t double_button = KEYMAP_BTN_COUNT;
static keymap_action_t double_action = KEYMAP_ACTION_NONE;

static keymap_button_t button_from_key(const char *name) {
	if (name) {
		for (int i = 0; i < KEYMAP_BTN_COUNT; i++) {
			if (strcmp(name, KEYS[i]) == 0) {
				return (keymap_button_t)i;
			}
		}
	}
	return KEYMAP_BTN_COUNT;
}

void keymap_init(void) {
	for (int i = 0; i < KEYMAP_BTN_COUNT; i++) {
		const char *saved = config_get("keys", KEYS[i], NULL);
		actions[i] = action_from_key(saved, DEFAULTS[i]);
	}
	double_button = button_from_key(config_get("keys", "double_button", NULL));
	double_action = action_from_key(config_get("keys", "double_action", NULL), KEYMAP_ACTION_NONE);
}

keymap_button_t keymap_double_button(void) { return double_button; }
keymap_action_t keymap_double_action(void) { return double_action; }

bool keymap_double_enabled(void) {
	return double_button < KEYMAP_BTN_COUNT && double_action != KEYMAP_ACTION_NONE;
}

void keymap_set_double(keymap_button_t button, keymap_action_t action) {
	if (button < 0 || button > KEYMAP_BTN_COUNT || action < 0 || action >= KEYMAP_ACTION_COUNT) {
		return;
	}
	double_button = button;
	double_action = action;
	config_set("keys", "double_button", button < KEYMAP_BTN_COUNT ? KEYS[button] : "none");
	config_set("keys", "double_action", ACTION_KEYS[action]);
	config_save();
}

keymap_action_t keymap_get(keymap_button_t button) {
	if (button < 0 || button >= KEYMAP_BTN_COUNT) {
		return KEYMAP_ACTION_NONE;
	}
	return actions[button];
}

void keymap_set(keymap_button_t button, keymap_action_t action) {
	if (button < 0 || button >= KEYMAP_BTN_COUNT) {
		return;
	}
	if (action < 0 || action >= KEYMAP_ACTION_COUNT) {
		return;
	}
	actions[button] = action;
	config_set("keys", KEYS[button], ACTION_KEYS[action]);
	config_save();
}

// Display names. A table rather than a switch on purpose: the translation
// string extractor (tools/extract_strings.py) reads tables -- this one is
// listed there among TABLES -- and cannot follow the `return`s of a switch. An
// entry that never reaches the language files stays untranslated in every
// language, and nobody notices until a user of that language sees it.
static const char *const KEYMAP_ACTION_NAMES[KEYMAP_ACTION_COUNT] = {
	"keymap_nothing", "keymap_play_pause", "keymap_previous_track", "keymap_next_track", "keymap_volume_up", "keymap_volume_down", "keymap_workout_timer",
};

const char *keymap_action_name(keymap_action_t action) {
	if (action < 0 || action >= KEYMAP_ACTION_COUNT) {
		return "";
	}
	return KEYMAP_ACTION_NAMES[action];
}

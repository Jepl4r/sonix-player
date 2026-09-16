#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdbool.h>

#include "lvgl/lvgl.h"

// The themed on-screen keyboard, built by hand out of real buttons: LVGL's
// stock keyboard can carry neither icon keys nor the palette. Both the search
// page and the playlist naming dialog create one.
//
// Each instance owns its keys, its preview bubble and its target textarea.
// The accent key on the bottom right is the page's own: the search page puts
// a magnifier on it, the naming dialog the word "OK".

typedef struct keyboard_s keyboard_t;

// Builds a keyboard `height` tall, bottom-aligned inside `parent` (a screen,
// or any container -- the preview bubble is placed on the parent so it can
// overhang the top row). `field` is the textarea the keys type into.
// `accept_icon` OR `accept_text` decides what the accent key shows: pass one
// and leave the other NULL. `on_accept` is called when it is pressed.
keyboard_t *keyboard_create(lv_obj_t *parent, int width, int height, lv_obj_t *field,
							const lv_image_dsc_t *accept_icon, const char *accept_text, lv_event_cb_t on_accept,
							void *user);

// The tray object itself (for aligning, moving to the foreground...).
lv_obj_t *keyboard_obj(keyboard_t *kb);

void keyboard_set_visible(keyboard_t *kb, bool visible);
bool keyboard_is_visible(keyboard_t *kb);

// Points the keys at another textarea.
void keyboard_set_field(keyboard_t *kb, lv_obj_t *field);

// Back to lower case letters, no shift -- what a freshly opened page wants.
void keyboard_reset(keyboard_t *kb);

// Rereads [other] keyboard_t9 and shows the right panel (QWERTY or T9) on every
// keyboard in the player. Call when the setting changes.
void keyboard_refresh_type(void);

// The same for the alphabets: every keyboard is laid out again, and one typing
// in a layout that has just been taken out of use goes back to the default.
// Call when the keyboard-language page changes the lists.
void keyboard_refresh_layout(void);

// The caret every text field in the interface wears: accent coloured, a
// little thicker than LVGL's hairline, and blinking. Applied to the focused
// state as well, because LVGL's own theme styles LV_PART_CURSOR|
// LV_STATE_FOCUSED and would otherwise put its thin default caret straight
// back the moment the field is touched.
void keyboard_style_caret(lv_obj_t *field);

// Turns a field's caret on or off. Needed where there are two fields (the
// Qobuz login page): LVGL blinks every textarea's caret from the moment it is
// created -- focus has nothing to do with it -- and two carets blinking
// together do not say which field is being typed into.
void keyboard_show_caret(lv_obj_t *field, bool on);

// Puts a field into password mode (bullets, with the last character readable
// for `show_ms` -- zero means never, which is what the pages here pass) and
// gives it the right tracking: the U+2022 bullet in this
// typeface is as wide as an ideograph, and uncorrected the bullets sit far too
// far apart.
void keyboard_style_password(lv_obj_t *field, uint32_t show_ms);

// Call when a field's password mode is changed from outside (the eye button):
// the tracking applies only while the bullets are shown.
void keyboard_refresh_password(lv_obj_t *field);

#endif // KEYBOARD_H

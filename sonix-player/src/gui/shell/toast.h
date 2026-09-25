#ifndef TOAST_H
#define TOAST_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The small card that appears in the middle of the screen for a moment and
// goes away again: a line of text, with or without a glyph above it, on the
// same themed card the confirmation dialog uses.
//
// Built once and reused. Three of them, because a card that says something went
// wrong should not be wearing a green tick:
//
//   success  a green circle-check -- a track added to a playlist
//   error    a red circle-alert   -- a passphrase the access point refused
//   plain    no glyph at all      -- an action that simply happened and has
//                                   nothing to celebrate or warn about, such
//                                   as a network forgotten or left
//
// The text is a translation tag.

void toast_init(gui_config_t *cfg);

void toast_success(const char *text);
void toast_error(const char *text);
void toast_plain(const char *text);

// The same green card with a glyph the caller picks, for a confirmation whose
// own symbol says more than a tick does: a bookmark saved wears a bookmark.
void toast_glyph(const lv_image_dsc_t *glyph, const char *text);

// The card with a spinner instead of a glyph, for work the user waits on. It
// stays, and a tap does not take it away, until toast_busy_end() or the next
// toast replaces it.
void toast_busy(const char *text);
void toast_busy_end(void);

#endif /* TOAST_H */

#ifndef TOAST_H
#define TOAST_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// A line of text that appears for a moment and goes away again. Since 2.0 it
// is the toast at the bottom of the screen (sx_toast.h), with the glyph at the
// start of the line; new code that wants "Undo" calls sx_toast_undo().
//
// Three kinds, because a toast that says something went wrong should not be
// wearing a green tick:
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

// The same with a glyph the caller picks, for a confirmation whose
// own symbol says more than a tick does: a bookmark saved wears a bookmark.
void toast_glyph(const lv_image_dsc_t *glyph, const char *text);

// The toast with a spinner instead of a glyph, for work the user waits on. It
// stays, and a tap does not take it away, until toast_busy_end() or the next
// toast replaces it.
void toast_busy(const char *text);
void toast_busy_end(void);

// The same for work that goes on in the background whether or not anybody
// waits for it: a tap on it or a pull down puts it away, and the work carries
// on. toast_busy_showing() says whether it is still up -- put away by a tap,
// it is not brought back.
void toast_busy_dismissable(const char *text);
bool toast_busy_showing(void);

#endif /* TOAST_H */

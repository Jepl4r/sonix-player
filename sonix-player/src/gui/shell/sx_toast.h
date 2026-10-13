#ifndef GUI_SX_TOAST_H
#define GUI_SX_TOAST_H

#include <stdbool.h>

#include "lvgl/lvgl.h"

// ---------------------------------------------------------------------------
// Sonix UI 2.0: the toast
//
// Principle 6, "a change is a toast with Undo": a light line near the bottom
// of the screen -- above the mini player, above the player's controls when
// the player is up -- saying what just happened, for SX_TOAST_MS (3.5 s; a
// long sentence a little longer). With an undo callback it carries "Undo" on
// the right and a 3 px bar under the text counting the time down; tapping
// "Undo" calls back and takes the toast away.
//
// One instance on lv_layer_top(), built once; a new toast replaces the one up
// (an undo not taken is simply dropped). A pull down of SX_TOAST_DISMISS_PX
// puts it away early. It never blocks the page: a tap off it goes to the page.
//
// The older toasts (toast.h: success, error, plain, busy) and the notices
// (gui_notify_popup) are this toast now, with a glyph at the start where they
// had one.
//
// Strings are shown as given: translate first. UI thread only (from another
// thread, gui_notify_popup() posts). Nothing runs while it is not shown; while
// it is, a timer for the end and, with "Undo", the bar's animation.
// ---------------------------------------------------------------------------

// Builds it, hidden. gui_init(); safe to call again.
void sx_toast_init(void);

// A plain toast.
void sx_toast(const char *text);

// With "Undo": `undo(user)` runs if it is tapped before the toast goes.
void sx_toast_undo(const char *text, void (*undo)(void *user), void *user);

// With a glyph at the start, recoloured to `colour` (a glyph larger than the
// line is scaled down to fit).
void sx_toast_glyph(const lv_image_dsc_t *glyph, lv_color_t colour, const char *text);

// A toast that stays, with a spinner, until sx_toast_busy_end() or the next
// toast. `dismissable`: a pull down may put it away (the work goes on).
void sx_toast_busy(const char *text, bool dismissable);
void sx_toast_busy_end(void);
bool sx_toast_busy_showing(void);

// Takes whatever is up away now (no undo).
void sx_toast_dismiss(void);

bool sx_toast_showing(void);

#endif /* GUI_SX_TOAST_H */

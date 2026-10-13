#ifndef GUI_SX_FRAME_H
#define GUI_SX_FRAME_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// ---------------------------------------------------------------------------
// Sonix UI 2.0: the frame
//
// What every page shares around its content, top to bottom:
//
//   0    the status bar, SX_STATUSBAR_H (topbar.c; a tap opens the control
//        centre, a pull down drags it in)
//   44   the header, SX_HEADER_H: the round back target (switcher.c), the
//        title (settingsrow_title), the header's own buttons
//   120  the body, SX_BODY_TOP (59 on the home page); 15 px side margins
//   H-88 the bottom slot, SX_MINI_SLOT_H: the mini player (sx_mini.h) while
//        something has played, or, on the home and Music pages while nothing
//        ever has, the "Shuffle all music" card
//
// The bottom slot is the part pages have to know about: while the mini player
// is up, the last 88 px of the screen are its. A page asks
// sx_frame_body_bottom() when laying out, or registers its scroller and its
// bottom-anchored controls here and forgets about it -- they follow the slot
// as the mini player comes and goes. Pages that do neither are handled on the
// way in (sx_frame_page_shown): a scrollable list reaching the bottom of the
// screen gets the room at its end, a control aligned to the bottom is lifted.
//
// Navigation (switcher.h): forward slides in from the right, back from the
// left, SX_ANIM_NAV_MS; the swipe back starts only in the SX_EDGE_BACK_W strip
// on the left edge; the player comes up from the bottom and is pulled down.
//
// UI thread only. Costs nothing while nothing changes: no timers.
// ---------------------------------------------------------------------------

// Builds the mini player and the frame's state. gui_init(), after the status
// bar and the back button.
void sx_frame_init(gui_config_t *cfg);

// The y the body starts at under a 2.0 header.
int sx_frame_body_top(void);

// How many pixels at the bottom of the screen are not the page's now:
// SX_MINI_SLOT_H while the mini player is up, 0 otherwise.
int sx_frame_body_bottom(void);

// Called after sx_frame_body_bottom() changes. Up to 16 callbacks; for a page
// that lays itself out by hand rather than registering below.
typedef void (*sx_frame_cb_t)(void);
void sx_frame_on_body_change(sx_frame_cb_t cb);

// Keeps `scroller`'s bottom padding at what it is now plus the slot, so its
// last row can be scrolled clear of the mini player. Idempotent; forgotten
// when the object is deleted.
void sx_frame_reserve_bottom(lv_obj_t *scroller);

// Keeps `obj` (aligned to the bottom of its page) lifted by the slot, so the
// mini player does not cover it. Idempotent; forgotten on delete.
void sx_frame_lift(lv_obj_t *obj);

// Excludes `obj` from the automatic treatment on the way in: for a page that
// deals with the slot itself, or a bottom panel that must stay put.
void sx_frame_exempt(lv_obj_t *obj);

// The handoff's idle slot: while nothing has ever played, the home and Music
// pages show their "Shuffle all music" card where the mini player would be.
// The page builds the card (phase 2 wires its contents) as a child of
// `screen`, SX_MINI_H tall, and hands it over: the frame places it in the slot
// and shows it exactly while the page is on screen and the player is idle.
void sx_frame_set_idle_card(lv_obj_t *screen, lv_obj_t *card);

// Something stands in the bottom slot now: the mini player, or an idle card
// on the page shown. The toast stands above it then.
bool sx_frame_slot_occupied(void);

// Nothing has played since the player was set up (or the last track was
// forgotten): there is no mini player, and the idle cards show.
bool sx_frame_idle(void);

// For switcher.c: `screen` is about to be shown. Places the slot's occupant
// and applies the automatic treatment above.
void sx_frame_page_shown(lv_obj_t *screen);

// For sx_mini.c: whether the mini player is up changed.
void sx_frame_slot_changed(void);

#endif /* GUI_SX_FRAME_H */

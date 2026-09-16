#ifndef COVERFLOW_H
#define COVERFLOW_H

#include <stdbool.h>

#include "lvgl/lvgl.h"

#include "src/gui/shell/gui.h"

// The album carousel: a column of records, the one in the middle facing the
// user and the ones above and below turned away from it, walked up and down
// with a finger.
//
// The turn is a projection, not a squeeze: the edge of a turned record that
// faces AWAY from the middle is as wide as the middle record, the edge facing
// the middle is about two thirds of that, and the picture between them crowds
// towards the far edge. So the inner edges converge and the column runs back
// towards a point behind the middle record.
//
// LVGL cannot draw that -- its transforms are affine, and no affine map narrows
// one edge against another -- so it is worked out per pixel, every frame of a
// drag. The cost is bounded: only two records are ever part way through their
// turn, their two heights always add up to the same number, so the per-frame
// work is constant, and there is none at all while the column is still.
// coverflow.c has the arithmetic.
//
// What gets turned is the RECORD, not the cover: a square built once per place,
// holding the card and, printed on it, the artwork -- or the album glyph when
// there is none. So the container turns with the picture, the glyph turns with
// the card, and a place whose cover has not arrived shows a blank record rather
// than somebody else's sleeve.
//
// Every cover is still decoded ONCE, at one size, and kept in the thumbnail
// database.
//
// It is not a page but a sheet, like the control centre: it comes up over the
// Musica grid following the finger, and goes back down the same way.

void coverflow_init(gui_config_t *cfg);

// Brings the sheet up on its own, without a finger on it. Safe to call when the
// library is closed: it says so rather than opening an empty page.
void coverflow_open(void);

// The setting behind it. Off by default: it is a second way to reach the albums,
// not a replacement for the list, and it costs a database of decoded covers.
bool coverflow_enabled(void);
void coverflow_set_enabled(bool on);

// Puts the pull that opens the carousel on a page's grid -- the object its
// tiles bubble their presses to. Only the Musica page asks for it. The gesture
// starts only in the bottom band of the screen and only after a good upward
// travel, so taps on the tiles are untouched; while the setting is off it
// refuses, so switching it does not need the page rebuilt.
//
// The way back out is the grip at the top of the carousel, pulled downwards.
// The page has no chevron and no swipe-back: up and down belong to the column,
// and an arrow drawn over the top record is an arrow in the way of the one
// thing the page is for.
void coverflow_attach_edge(lv_obj_t *grid, gui_config_t *cfg);

// True while that pull is being followed, and for one tick afterwards. The
// gesture starts on a tile, and the tile still gets its CLICKED when the finger
// lifts, so every handler on the Musica grid has to ask -- otherwise the sheet
// comes up with Generi or Esplora opened behind it.
bool coverflow_drag_active(void);

#endif /* COVERFLOW_H */

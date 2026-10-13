#ifndef TOUCH_H
#define TOUCH_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// ---------------------------------------------------------------------------
// Touch targets
//
// A fingertip on this panel (480 x 800 on 3.5 inches, about 267 pixels to the
// inch) is some 7 mm across: 72 to 74 pixels. Most of what is drawn to be
// pressed is smaller than that -- a 56 px corner glyph, a 44 px ellipsis on a
// row, a 36 px tall switch -- and drawing it bigger would only crowd the
// page. What grows instead is the area that answers a press: these helpers
// stretch it past the object's edges without moving or resizing anything on
// screen.
//
// Two things bound how far it can go. A neighbour: LVGL hands a press to the
// topmost object whose area holds it, so two stretched areas that overlap
// would take each other's taps -- areas between neighbours stop half way
// across the gap. And the parent: a press is only looked for among the
// children of an object that holds it itself, so an area stretched beyond its
// parent's box is never reached; the parent has to be big enough too.
//
// UI thread only, like everything that touches LVGL.
// ---------------------------------------------------------------------------

// What a target aims for, each way: the width of a fingertip.
#define TOUCH_TARGET 72

// Called once, before any page is built.
void touch_init(gui_config_t *cfg);

// Stretches the area that answers a press on `obj` by the given amount past
// each of its edges. Uneven amounts are the point: the corner buttons sit 6 px
// apart side by side but have room above and below. Calling it again replaces
// the earlier amounts.
void touch_pad(lv_obj_t *obj, int left, int right, int top, int bottom);

// The same, evenly on every side, by as much as it takes for a `w` x `h`
// object to reach TOUCH_TARGET in both directions -- but never past `limit`
// on a side, which is half the gap to whatever is next to it. For a control
// that stands alone in its row (a row's ellipsis or star, a switch).
void touch_grow(lv_obj_t *obj, int w, int h, int limit);

// The round buttons of the header row: the ones a page puts top right beside
// its title (search, settings, sort, "+", the selection-mode actions). Every
// page builds them with its own helper, so rather than each of them being
// told, the switcher calls this with each page it loads and the buttons are
// found by where they sit and how big they are: 56 x 56 at the header's
// height, straight on the screen. Since 2.0 each is made a SX_HEADER_BTN round
// target in the 2.0 header (flush right, one 72 px slot each, a pressed fill)
// -- a fingertip by itself. A button already done is passed over.
void touch_header(lv_obj_t *screen);

// ---------------------------------------------------------------------------
// Large rows
//
// Interface settings > "Large rows": the rows of the library's lists and of
// the folder browser stand 120 px instead of 100 -- fewer on a screen, each a
// bigger target and easier to read at arm's length. Kept as [ui] large_rows,
// off by default. A list takes the height when it is opened; one already open
// keeps the one it was opened with until it is opened again.
// ---------------------------------------------------------------------------

#define TOUCH_ROW_HEIGHT 100
#define TOUCH_ROW_HEIGHT_LARGE 120

bool touch_large_rows(void);
void touch_set_large_rows(bool on);

// The height a list row is built at now: one of the two above.
int touch_row_height(void);

#endif /* TOUCH_H */

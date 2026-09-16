#ifndef EASTEREGG_H
#define EASTEREGG_H

#include "lvgl/lvgl.h"

// The thank-you hidden in About.
//
// Five taps on a row and then a press held down bring up a donation QR code
// over the page. A tap anywhere outside it puts it away and leaves a red heart
// bouncing in the middle of the screen for a second. It is hidden rather than
// a settings button on purpose: whoever finds it was looking.

// Wires the gesture to a settings row. The row must be clickable, which is
// what settingsrow_add() builds and what info_row() takes away.
void easteregg_attach(lv_obj_t *row);

// Forgets a half-finished gesture. The page calls this when it is opened, so
// taps left over from the last visit do not count towards this one.
void easteregg_reset(void);

#endif /* EASTEREGG_H */

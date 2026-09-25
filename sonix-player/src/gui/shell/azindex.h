#ifndef AZINDEX_H
#define AZINDEX_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"
#include "src/system/library/library.h"

#include "lvgl/lvgl.h"

// The A-Z strip for a list that is not one of the library's: a column of
// letters down the right edge that shows while the list moves, jumps to a
// letter on a tap and walks the alphabet under a sliding finger, with the
// letter drawn large in the middle of the screen. It looks and behaves like
// the one on the track lists (medialist.c), and files names into the same
// buckets: '#', A to Z, and one for everything past Z.
//
// The list is a scrolling object whose rows are all `row_pitch` apart from the
// top. The owner says where each letter starts with azindex_set_rows(), and
// calls azindex_flash() from its scroll handler.

typedef struct azindex azindex_t;

// Builds the strip and the big letter on `screen`, both hidden. `jumped` is
// called after every jump, for a windowed list that has to bind its rows
// again; NULL when the list needs nothing.
azindex_t *azindex_create(lv_obj_t *screen, gui_config_t *cfg, lv_obj_t *list, int row_pitch,
						  void (*jumped)(void));

// Where each bucket's rows start, -1 for a bucket with none, in the order the
// buckets run from '#' to past Z. `descending` turns the strip round to match a
// Z-A list. A list shorter than AZINDEX_MIN_ROWS, or `first` NULL, gets no
// strip at all.
#define AZINDEX_MIN_ROWS 30
void azindex_set_rows(azindex_t *ix, const int first[LIBRARY_INDEX_BUCKETS], int count, bool descending);

// The list moved: the strip shows, and goes again a moment after it stops.
void azindex_flash(azindex_t *ix);

#endif /* AZINDEX_H */

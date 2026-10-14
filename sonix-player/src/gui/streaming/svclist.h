#ifndef SVCLIST_H
#define SVCLIST_H

#include <stdbool.h>
#include <stddef.h>

#include "lvgl/lvgl.h"

#include "src/gui/shell/sx_row.h"

// ---------------------------------------------------------------------------
// A streaming service's list: sx_row rows over a list of any length
//
// Every streaming page lists things the same way -- tracks, albums, artists,
// playlists, stations, episodes -- and before 2.0 each built its own rows: a
// widget per entry (two hundred of them on a Subsonic page, seven objects
// each) or, on the Yandex and radio pages, a window of its own. This is that
// window once, for all of them, made of the 2.0 row (sx_row.h) so the gesture
// matrix is the same everywhere: tap, swipe right "Play next", swipe left "Add
// to the end", hold for the sheet, the star.
//
// A fixed pool of rows rides the scroll: entry `index` is drawn by row
// `index % pool`, placed at its y, and given its contents by the page's
// bind() only when it comes into view -- a liked list of five thousand costs
// the same pool as a list of ten. sx_row's setters create nothing, so a
// rebind is a handful of label and style changes. The row's user data is the
// entry's index: every handler gets it back as (int)(intptr_t)user.
//
// Optionally the list is grouped: heading() names the group an entry starts
// ("TRACKS · 40"), drawn as the handoff's mono caps line above it, and the
// entries below move down by its height. The headings come from a small pool
// of labels too.
//
// The scroller keeps its last row clear of the mini player by itself
// (sx_frame_reserve_bottom). UI thread only. Costs nothing while it is not
// scrolled: no timer.
// ---------------------------------------------------------------------------

typedef struct svclist svclist_t;

typedef struct {
	// Fills `row` for entry `index` (title, subtitle, lead, trail, star,
	// playing, swipes). Called when the entry comes into view, and again on
	// svclist_rebind().
	void (*bind)(lv_obj_t *row, int index);
	// The heading over entry `index`, written into `out`; false for none.
	// NULL: the list has no groups.
	bool (*heading)(int index, char *out, size_t size);
	// The view came within a screenful of the end: the time to ask for the
	// next page. NULL: the list has no more pages.
	void (*near_end)(void);
	// Entries `first`..`last` are on screen now (after a scroll or a fill):
	// the time to ask for their pictures or their details. May be NULL.
	void (*visible)(int first, int last);
} svclist_cbs_t;

// Builds the list as a child of `parent`, filling it (the caller sizes and
// places it afterwards with svclist_obj()). `pool` rows: enough for a screen
// and a half. `handlers` and `cbs` are copied.
svclist_t *svclist_create(lv_obj_t *parent, const sx_row_handlers_t *handlers, const svclist_cbs_t *cbs, int pool,
						  bool compact);

// The scroller, to size and place.
lv_obj_t *svclist_obj(const svclist_t *list);

// The number of entries changed. `keep_scroll`: a page appended at the
// bottom (the view stays); otherwise a new list (back to the top). Every row
// in view is bound again.
void svclist_set_count(svclist_t *list, int count, bool keep_scroll);
int svclist_count(const svclist_t *list);

// Room above the first entry, for objects the page puts in the scroller
// itself (a card of shortcuts over a list of stations): they scroll with
// the rows. Takes effect at the next svclist_set_count().
void svclist_set_top(svclist_t *list, int top);

// The rows in view bound again: a star, a mark, the playing row changed.
void svclist_rebind(svclist_t *list);
// One entry, if it is in view.
void svclist_rebind_index(svclist_t *list, int index);

// The entries on screen now, -1 / -2 for none.
void svclist_visible(const svclist_t *list, int *first, int *last);

// An entry's index from its row (sx_row_get_user), -1 for none.
int svclist_index_of(lv_obj_t *row);

#endif /* SVCLIST_H */

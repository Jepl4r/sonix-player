#ifndef EBOOKCOVERS_H
#define EBOOKCOVERS_H

#include <stdbool.h>
#include <stdint.h>

#include "src/gui/nowplaying/cover.h"

// ---------------------------------------------------------------------------
// Covers, fetched behind the page, and only the ones being looked at
//
// A cover is inside the book, which means opening the book to see it -- the ZIP
// directory, container.xml and the OPF, for a picture. Thirty books is thirty of
// those, and doing them while a page is being built is a second or two of black
// screen. So the page goes up at once and this fills the pictures in behind.
//
// And not all of them: fetching a whole shelf costs one ZIP open and one decode
// per book, plus a screenful of decoded pixels nobody may ever look at. The page
// says which ones are on screen and the worker does those, in that order; the
// rest are fetched if and when the reader scrolls to them.
//
// The thread is DETACHED and never waited for. Joining it on leaving the page
// would block the interface until the worker finished whatever book it was in
// the middle of.
//
// Nothing the worker touches belongs to the page. It gets its own copy of the
// list, which it frees itself, and each result is a message the UI thread takes
// ownership of. What ties them together is a generation number: ebookcovers_stop()
// moves it on, and a worker or a message from a page that is gone finds a number
// that no longer matches and frees what it was carrying.
// ---------------------------------------------------------------------------

// Called on the UI THREAD, once per book, in the order they were asked for.
//
// Return true to KEEP `cover` -- then it is yours and you cover_free() it later.
// Return false and it is freed here. `cover` is NULL when the book has none, and
// the return is then ignored. `title` is what the book calls itself, or the file
// name when it does not say.
//
// The return value rather than a rule about what callers ought to do: whether
// the picture was taken is the one thing this cannot work out for itself, and
// guessing wrong is either a leak or a double free.
typedef bool (*ebookcovers_ready_cb)(int index, const char *title, const cover_image_t *cover);

// Registers `count` files inside `dir` and returns the token for THIS run.
// Nothing is read until something is asked for -- see ebookcovers_want().
// Replaces any run already going. Copies everything it needs; nothing passed in
// has to outlive the call.
uint32_t ebookcovers_start(const char *dir, const char (*files)[256], int count, int box_w, int box_h,
						   ebookcovers_ready_cb cb);

// Asks for the covers of `from`..`to` inclusive, soonest first. Cheap and
// idempotent: call it on every scroll. A book already fetched is not fetched
// again, and a range that no longer includes a book the worker has not reached
// simply means it is never read.
void ebookcovers_want(uint32_t token, int from, int to);

// The same, worked out from the objects themselves: whichever of `items` falls
// inside `view` -- or within `margin` pixels of it, so the next screenful is
// ready before it is reached -- is what gets asked for.
//
// The geometry lives here rather than in each page because both the shelf and
// the bookmarks list ask the same question of a scrolling list of tiles.
void ebookcovers_want_visible(uint32_t token, lv_obj_t *view, lv_obj_t *const *items, int count, int32_t margin);

// Stops the run `token` named, and only that one. Returns immediately: the
// worker stops between books, in its own time.
//
// The token rather than a bare "stop everything", because LVGL sends the new
// screen its SCREEN_LOADED before it sends the old one SCREEN_UNLOADED: a page
// that started its covers on the way in would otherwise have them cancelled a
// moment later by the page on the way out.
void ebookcovers_stop(uint32_t token);

#endif /* EBOOKCOVERS_H */

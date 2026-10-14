#ifndef ZVUKLIST_H
#define ZVUKLIST_H

#include <stdbool.h>
#include <stddef.h>

#include "src/system/streaming/zvuk.h"

// A list of Zvuk tracks that may be thousands long: the ids up front, the
// rest -- title, artist, album, cover -- fetched a page at a time when a row
// or playback wants it, and let go again when it has not been wanted for a
// while. yandexlist.h for Zvuk, and for the same reason: an account's liked
// tracks are thousands, and a zvuk_track_t is most of a kilobyte.
//
// An entry is twelve bytes (the id as a number, and a pointer) until its page
// is loaded; a loaded entry is its strings packed end to end, a couple of
// hundred bytes. At most ZVUKLIST_PAGES_KEPT pages of details are kept, the
// least recently used going first -- except in a list built from tracks whose
// details came whole (an album, a search), which keeps them all: it is short,
// and its track numbers come from nowhere else.
//
// Thread contract: one mutex inside, held for memory work only; any thread.
// zvuklist_fetch_page() blocks on the network: workers only. Counted: whoever
// keeps a list holds a reference, the last zvuklist_free() frees it.

#define ZVUKLIST_PAGE ZVUK_TRACKS_PER_CALL
#define ZVUKLIST_PAGES_KEPT 12

typedef struct zvuklist zvuklist_t;

zvuklist_t *zvuklist_new(void);
zvuklist_t *zvuklist_ref(zvuklist_t *list);
void zvuklist_free(zvuklist_t *list);
// The ids and the details loaded now, for a queue that must not change with
// the page's list. NULL when out of memory.
zvuklist_t *zvuklist_clone(zvuklist_t *list);

bool zvuklist_add_id(zvuklist_t *list, const char *track_id);
bool zvuklist_add_track(zvuklist_t *list, const zvuk_track_t *track);

int zvuklist_count(zvuklist_t *list);
bool zvuklist_id(zvuklist_t *list, int index, char *out, size_t size);
// The first entry with this id, -1 when none.
int zvuklist_find(zvuklist_t *list, const char *track_id);
// The details of entry `index` when they are here (`out` may be NULL).
bool zvuklist_track(zvuklist_t *list, int index, zvuk_track_t *out);
bool zvuklist_page_ready(zvuklist_t *list, int page);
// Fetches page `page`'s details (one getTracks). How many entries of it have
// details now, -1 on failure with zvuk_last_error() set.
int zvuklist_fetch_page(zvuklist_t *list, int page);
size_t zvuklist_memory(zvuklist_t *list);

#endif /* ZVUKLIST_H */

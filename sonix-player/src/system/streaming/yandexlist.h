#ifndef YANDEXLIST_H
#define YANDEXLIST_H

#include <stdbool.h>
#include <stddef.h>

#include "src/system/streaming/yandex.h"

// A list of Yandex tracks that may be thousands long: the ids up front, the
// rest -- title, artist, album, cover -- fetched a page at a time when it is
// wanted, and let go again when it has not been wanted for a while.
//
// Why: an account's liked tracks, a big playlist or an artist's whole
// catalogue are thousands of tracks. 1.2.5 asked for the details of the
// first 150 in one go (a yandex_track_t is over a kilobyte, the answer
// two), showed those and could play only those. Here an entry is eight bytes
// until a row shows it or playback reaches it; details live in pages of
// YANDEXLIST_PAGE, a few hundred bytes a track, and at most
// YANDEXLIST_PAGES_KEPT pages are kept -- the least recently used goes
// first. Playing the list queues all of it as ids; a track's details are
// fetched when playback gets near it.
//
// Ids are numbers for every catalogue track and are kept as such; anything
// else (a user's own upload) goes into a small side table.
//
// No LVGL: the Yandex pages of today and of UI 2.0 read it the same way.
//
// Thread contract: one mutex inside, held for memory work only; every call
// may be made from any thread. yandexlist_fetch_page() blocks on the network
// and is for worker threads. A list is counted: whoever keeps it -- the page
// showing it, a worker fetching a page of it -- holds a reference, and the
// last yandexlist_free() frees it.

#define YANDEXLIST_PAGE 50
#define YANDEXLIST_PAGES_KEPT 12

typedef struct yandexlist yandexlist_t;

// A new, empty list with one reference. NULL when out of memory.
yandexlist_t *yandexlist_new(void);
// One more reference (the same list back); NULL-safe.
yandexlist_t *yandexlist_ref(yandexlist_t *list);
// Drops a reference; the last one frees the list. NULL-safe.
void yandexlist_free(yandexlist_t *list);

// A copy of the ids and of the details loaded now: what a queue takes from
// the list it was started from, so the two can change apart. NULL when out
// of memory.
yandexlist_t *yandexlist_clone(yandexlist_t *list);

// Adds an entry: by id (`album_id` may be NULL or empty), or a track whose
// details are known already (an album's tracks, a batch of My Wave). False
// when out of memory or the id is empty.
bool yandexlist_add_id(yandexlist_t *list, const char *track_id, const char *album_id);
bool yandexlist_add_track(yandexlist_t *list, const yandex_track_t *track);

int yandexlist_count(yandexlist_t *list);

// The ids of entry `index`; `album` may be NULL. False past the end.
bool yandexlist_ids(yandexlist_t *list, int index, char *id, size_t id_size, char *album, size_t album_size);

// The first entry with this track id, -1 when none. A scan of the numbers:
// cheap even for thousands.
int yandexlist_find(yandexlist_t *list, const char *track_id);

// The details of entry `index` when they are here (`out` may be NULL to ask
// only). Counts as a use of its page.
bool yandexlist_track(yandexlist_t *list, int index, yandex_track_t *out);

// Whether page `page` (entries page*YANDEXLIST_PAGE onwards) is loaded.
bool yandexlist_page_ready(yandexlist_t *list, int page);

// Fetches the details of page `page` if they are not here (POST /tracks, one
// request). Returns how many entries of the page have details now, -1 on
// failure with yandex_last_error() set. Worker threads only.
int yandexlist_fetch_page(yandexlist_t *list, int page);

// What the list holds in memory, in bytes, for the log.
size_t yandexlist_memory(yandexlist_t *list);

#endif /* YANDEXLIST_H */

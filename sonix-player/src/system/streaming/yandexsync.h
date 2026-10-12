#ifndef YANDEXSYNC_H
#define YANDEXSYNC_H

#include <stdbool.h>

// What the player writes back to Yandex Music off the UI thread, and Yandex as
// the rest of the program sees it (provider.h).
//
// The twin of subsonicsync.h, smaller: the only write is the like -- the star
// on Now Playing, the quick panel's heart and My Wave's "Like" button all end
// here -- and Yandex playlists are not offered to "Add to playlist" (a cached
// track goes to the card's playlists, as before). The mirror of liked ids the
// star reads lives in yandex.c, beside the code that fills it.
//
// Callbacks run on the worker: the receiver bounces them with gui_post().

typedef void (*yandexsync_done_cb)(bool ok, const char *error, void *user);

// Flips the mirror at once, then tells Yandex; puts it back on failure.
// `album_id` may be empty (the like is then sent without it).
void yandexsync_like(const char *track_id, const char *album_id, bool want, yandexsync_done_cb done, void *user);

// Fetches the liked ids if they are not known yet. `done` (may be NULL)
// arrives on the worker, only when they arrived.
void yandexsync_likes_refresh(void (*done)(void));

// Puts Yandex Music in the streaming-provider registry. yandex_init() calls it.
void yandexsync_register_provider(void);

#endif /* YANDEXSYNC_H */

#ifndef SUBSONICSYNC_H
#define SUBSONICSYNC_H

#include <stdbool.h>

#include "src/system/streaming/subsonic.h"

// What the player writes back to the Subsonic server, off the UI thread: the
// star, scrobbles, "Add to playlist". The twin of qobuzsync.h, with two
// differences:
//
//   * a small queue (eight jobs) rather than a single slot. A scrobble is sent
//     on every track change, and with one slot it would silently replace a star
//     tapped a moment earlier;
//   * the ids are strings, and the favourites mirror holds song ids.
//
// Callbacks run on the worker: the receiver bounces them with gui_post().

typedef void (*subsonicsync_done_cb)(bool ok, const char *error, void *user);

// --- the starred-songs mirror ----------------------------------------------

bool subsonicsync_is_favorite(const char *song_id);
void subsonicsync_favorites_refresh(void (*done)(void));
void subsonicsync_forget(void);

// Flips the mirror at once, then tells the server; puts it back on failure.
void subsonicsync_favorite_toggle(const char *song_id, bool want, subsonicsync_done_cb done, void *user);
void subsonicsync_album_favorite(const char *album_id, bool want, subsonicsync_done_cb done, void *user);

// --- scrobbling ------------------------------------------------------------

void subsonicsync_scrobble(const char *song_id, bool submission);

// --- the server's playlists ------------------------------------------------

#define SUBSONICSYNC_PLAYLISTS_MAX 60

void subsonicsync_playlists_refresh(void (*done)(void));
bool subsonicsync_playlists_known(void);
int subsonicsync_playlists(subsonic_playlist_t *out, int max);
void subsonicsync_playlist_add(const char *playlist_id, const char *song_id, subsonicsync_done_cb done, void *user);
void subsonicsync_playlist_create_with(const char *name, const char *song_id, subsonicsync_done_cb done, void *user);

// Puts Subsonic in the streaming-provider registry (provider.h). subsonic_init()
// calls it.
void subsonicsync_register_provider(void);

#endif /* SUBSONICSYNC_H */

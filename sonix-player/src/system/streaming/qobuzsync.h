#ifndef QOBUZSYNC_H
#define QOBUZSYNC_H

#include <stdbool.h>

#include "src/system/streaming/qobuz.h"

// What the player writes back to the Qobuz account, off the UI thread.
//
// The star and "add to playlist" are one tap each. Sending them to the local
// database is wrong twice over for a Qobuz track: the stored path is a cache
// file that will be gone twenty tracks later, and no other device of the
// user's ever sees the favourite. They have to land on Qobuz.
//
// An HTTP request takes seconds and the UI thread cannot stall, so this holds
// one worker with a single-slot queue, and every job ends in a callback. That
// callback runs on the worker, not on the UI thread -- the receiver must bounce
// it itself with lv_async_call(), as the rest of the program does.
//
// The star also cannot wait for the network to know how to draw itself, so a
// mirror of the favourite track ids is kept in memory, filled once and updated
// on every tap. Until the mirror exists the star draws empty and the mirror is
// fetched in the background.

// --- the favourites mirror -------------------------------------------------

// Whether the mirror has been filled at least once.
bool qobuzsync_favorites_known(void);

// True when that track is among the favourites. False without a mirror.
bool qobuzsync_is_favorite(long track_id);

// Drops the mirror: call on logout.
void qobuzsync_forget(void);

// Fetches the mirror in the background if it is not there already. `done` is
// called only when something changed, and on the worker.
void qobuzsync_favorites_refresh(void (*done)(void));

// --- the jobs ---------------------------------------------------------------

// A job's outcome: `ok`, and when it is not, the reason as a displayable
// sentence.
typedef void (*qobuzsync_done_cb)(bool ok, const char *error, void *user);

// Adds or removes the track from the account's favourites. The mirror is
// updated immediately, before the network, so the star responds to the tap,
// and put back if the request fails.
void qobuzsync_favorite_toggle(long track_id, bool want, qobuzsync_done_cb done, void *user);

// Adds the track to one of the account's playlists.
void qobuzsync_playlist_add(long playlist_id, long track_id, qobuzsync_done_cb done, void *user);

// Creates a playlist on the account and puts the track in it.
void qobuzsync_playlist_create_with(const char *name, long track_id, qobuzsync_done_cb done, void *user);

// Adds or removes an album from the account's favourites. Albums have no
// mirror like tracks do: in the lists the menu already states which way it will
// go (add among search results, remove in the favourites list), so the answer
// is known without asking.
void qobuzsync_album_favorite(const char *album_id, bool want, qobuzsync_done_cb done, void *user);

// Deletes a playlist from the account.
void qobuzsync_playlist_remove(long playlist_id, qobuzsync_done_cb done, void *user);

// --- the account's playlists -----------------------------------------------

#define QOBUZSYNC_PLAYLISTS_MAX 60

// Fetches the list in the background. `done` arrives on the worker.
void qobuzsync_playlists_refresh(void (*done)(void));

// The last list received. Returns how many were copied.
int qobuzsync_playlists(qobuz_playlist_t *out, int max);

// Whether the list has been received at least once.
bool qobuzsync_playlists_known(void);

#endif /* QOBUZSYNC_H */

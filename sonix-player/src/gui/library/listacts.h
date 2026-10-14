#ifndef LISTACTS_H
#define LISTACTS_H

#include <stdbool.h>

#include "src/gui/shell/sx_sheet.h"
#include "src/system/library/library.h"
#include "src/system/playback/playlist.h"

#include "lvgl/lvgl.h"

// ---------------------------------------------------------------------------
// What every library list does with a track or a record (UI 2.0)
//
// The handoff's gesture matrix is the same on every list: a track row pulled
// right plays next, pulled left goes to the end of the queue, its star stars
// it, a long press brings up its sheet; a record does the same with all its
// tracks. medialist.c, search.c and the playlists do these through here, so
// the wording, the toasts and the undo are one thing.
//
// Principle 6: every change says so in a toast with "Undo", which takes the
// change back -- the queue as it was before (playlist_queue_undo), the star
// the way it was. Only the latest change can be undone: the toast that offers
// it is replaced by the next one.
//
// UI thread only.
// ---------------------------------------------------------------------------

// The tracks into the queue: "Play next" or "Add to the end", with the toast
// "Plays next: <label>" / "At the end of the queue: <label>" and its undo.
// `label` names what went in (a title, an album); NULL says how many. The
// queue is written down to the card at once. Returns how many went in.
int listacts_queue(const char *const *paths, int count, playlist_queue_where_t where, const char *label);

// The same for every track of a library handle, in its order (an album, an
// artist's records). Takes ownership of `ix`.
int listacts_queue_index(library_index_t *ix, playlist_queue_where_t where, const char *label);

// One record's tracks, in their running order, as a handle (NULL when there
// are none). `album_value` is an Albums row's name.
library_index_t *listacts_album_tracks(const char *album_value);

// Stars or unstars a track, with "Added to favourites: X" / "Removed from
// favourites: X" and its undo. Returns the new state.
bool listacts_star(const char *path, const char *name, const char *artist);

// Called after a star or an undo changed something a list shows: up to eight.
void listacts_on_change(void (*changed)(void));

// After a list started playback: the player is told, and the page stays where
// it is -- the mini player comes up with the track (the handoff: a tap on a
// row plays, it does not leave the list).
void listacts_played(void);

// "Shuffle all music": every track of the library in a random order, the
// shuffle mode on, with a toast. False (and a notice) when there is nothing.
bool listacts_shuffle_all(void);

// The track's sheet: favourite, play next, add to the end, add to a playlist,
// show album, show artist, details -- then `extra`, the page's own items
// (take it out of this playlist, select). Show album is left out on that
// album's own page (`here_album`, an album value, or NULL) and show artist on
// that artist's (`here_artist`). `subtitle` is shown under the title (NULL:
// the artist). The strings are copied.
void listacts_track_sheet(const char *path, const char *title, const char *artist, const char *subtitle,
						  const char *here_album, const char *here_artist, const sx_sheet_item_t *extra,
						  int extra_count);

// A record's sheet: play, shuffle, play next, add to the end, add to a
// playlist, add to favourites, show artist -- then `extra`. `album_value` is
// the album's value; `artist` its artist (shown and opened), or NULL.
void listacts_album_sheet(const char *album_value, const char *artist, const char *subtitle,
						  const sx_sheet_item_t *extra, int extra_count);

// Plays one record from its first track (`shuffled`: in a random order).
void listacts_play_album(const char *album_value, bool shuffled);

// The collected paths of a handle, for the callers that need them as a list
// (adding to a playlist, starring a record). The caller frees each and the
// array. At most `max`.
int listacts_collect(library_index_t *ix, char ***paths_out, int max);
void listacts_paths_free(char **paths, int count);

#endif /* LISTACTS_H */

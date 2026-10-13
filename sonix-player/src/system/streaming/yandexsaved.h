#ifndef YANDEXSAVED_H
#define YANDEXSAVED_H

#include <stdbool.h>
#include <stddef.h>

#include "src/system/streaming/yandex.h"

// Which Yandex Music tracks are saved on the card (yandexdl.h), and where: so
// that a track played from the Yandex pages plays from its saved file -- no
// network, no second copy in the cache -- and the pages can mark what is
// saved and list it with Wi-Fi off.
//
// Why an index of its own rather than the library's database: the database
// knows files and tags, not Yandex ids, and asking it would mean a query per
// row on every list; the library can also be switched off, rebuilt or limited
// to other folders. The index is one small text file on the card,
//
//   <card>/.local/yandex-saved.tsv
//
// one line per saved track: its id, its album's id, the numbers, the names a
// list shows offline, and the file's path relative to the card -- the card
// may be mounted elsewhere next time. A line is appended when a save
// completes, or when a save finds the file already there (which is also how
// files saved by 1.2.4, before there was an index, get into it: "Save" again
// costs no download). Everything else is lazy: the file is read on first use
// and kept in memory (a few hundred bytes a track); a path is checked only
// when it is about to be played or listed, and an entry whose file has gone
// (deleted on a computer, say) is dropped then and the track streams as
// before. A torn last line -- power lost mid-append -- is skipped on reading.
//
// Thread contract: every call may be made from any thread; one mutex inside,
// held for memory work and the small file writes only. Nothing here touches
// the network.

#define YANDEXSAVED_FILE ".local/yandex-saved.tsv"
// The folder in the music library the saves go into (yandexdl.h).
#define YANDEXSAVED_FOLDER "Yandex Music"

// The card, as the provider registry hands it. A different root forgets the
// loaded index (the next call reads the new card's).
void yandexsaved_set_root(const char *sd_root);

// A save completed (or found its file already there): `track` with the album
// details the file was tagged with, `path` absolute. Replaces an older entry
// of the same id.
void yandexsaved_add(const yandex_track_t *track, const char *album_artist, int year, int album_tracks, const char *path);

// How many tracks the index holds.
int yandexsaved_count(void);
// The track is in the index. Cheap -- no disk access -- for the marks.
bool yandexsaved_has(const char *track_id);
// How many of the album's tracks the index holds.
int yandexsaved_album_count(const char *album_id);
// The saved file of the track when the index has one and it can be read;
// a stale entry is dropped. `out` may be NULL.
bool yandexsaved_local(const char *track_id, char *out, size_t size);
// The track a saved file is, from its path: false for anything else.
bool yandexsaved_track_for_path(const char *path, char *id, size_t id_size, char *album_id, size_t album_size);

// The saved albums, one row each (track_count = tracks saved, artist = the
// album's artist), by artist then title. Tracks saved without an album
// (user uploads) are not among them: yandexsaved_tracks(NULL...) lists those.
int yandexsaved_albums(yandex_album_t *out, int max);
// The saved tracks of an album (NULL or "" = those of no album) in disc and
// track order, checked on the card: entries whose file is gone are dropped.
int yandexsaved_tracks(const char *album_id, yandex_track_t *out, int max);

// Deletes the saved file of the track (with a 1.2.4 ".tags" sidecar), its
// entry, and the folders it leaves empty up to "Yandex Music" -- the album's
// cover.jpg goes with the last track. True when the track is no longer saved.
bool yandexsaved_delete(const char *track_id);
// The same for every saved track of the album; returns how many went.
int yandexsaved_delete_album(const char *album_id);

// The saved files the Yandex page's queue plays (GUI thread sets them, the
// provider reads them from anywhere): only these are seen as Yandex tracks --
// the like, the badge, "Show album" -- the same file played from the Library
// stays a library file. NULL/0 clears.
void yandexsaved_claim(const char *const *paths, int count);
bool yandexsaved_claimed(const char *path);

#endif /* YANDEXSAVED_H */

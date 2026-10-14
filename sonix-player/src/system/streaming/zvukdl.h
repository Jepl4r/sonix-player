#ifndef ZVUKDL_H
#define ZVUKDL_H

#include <stdbool.h>
#include <stddef.h>

#include "src/system/streaming/zvuk.h"

// Zvuk tracks saved for good into the music library ("Saved on the card"), and
// the index of what is saved -- yandexdl.h and yandexsaved.h for Zvuk, in one
// module:
//
//   <music>/Zvuk/<Album artist>/<Album (Year)>/<NN - Title>.<flac|mp3>
//
// <music> is the first of the library scan's folders, else "Music". The file
// is the best the quality setting gets (FLAC unwrapped from its MP4), tagged
// on its way to the card -- Vorbis comments in a FLAC, ID3v2.4 in an MP3 --
// with the album's cover.jpg beside it. A track the cache holds complete in
// the right format is copied from there instead. Written under a hidden
// ".<name>.part" renamed when complete; one file at a time on a thread of its
// own at low priority, giving way to whatever is being listened to. Nothing
// runs and nothing is allocated until something is saved.
//
// The index, <card>/.local/zvuk-saved.tsv, one line per saved track (its ids,
// numbers, the names a list shows offline and the file's path relative to the
// card), is what lets the Zvuk pages play saved tracks from the card with no
// network -- and with no session: an expired token signs out, the saved
// tracks play on.
//
// Thread contract: any thread; one mutex inside. The listener runs on the
// worker (the receiver bounces it with gui_post()).

#define ZVUKDL_FOLDER "Zvuk"
#define ZVUKDL_INDEX ".local/zvuk-saved.tsv"

bool zvukdl_track(const zvuk_track_t *track);
bool zvukdl_album(const char *album_id, const char *title);

typedef struct {
	bool active;
	int done;
	int failed;
	int queued;
	int percent; // of the file in progress, -1 unknown
	char title[96];
} zvukdl_status_t;
void zvukdl_status(zvukdl_status_t *out);
bool zvukdl_busy(void);

typedef enum {
	ZVUKDL_QUEUED,	 // `count` more to save, `text` what
	ZVUKDL_SAVED,	 // one track is on the card
	ZVUKDL_FAILED,	 // `text`: "<title>: <reason>"
	ZVUKDL_FINISHED, // the queue emptied: `count` saved, `text` the folder
	ZVUKDL_STOPPED,	 // dropped (no card, signed out): `text` why
} zvukdl_event_t;
void zvukdl_set_listener(void (*listener)(zvukdl_event_t event, int count, const char *text));

void zvukdl_cancel_all(void);
void zvukdl_set_root(const char *sd_root);

// --- the index -------------------------------------------------------------

int zvukdl_saved_count(void);
bool zvukdl_saved(const char *track_id);
int zvukdl_album_saved_count(const char *album_id);
// The saved file, when it can still be read (a stale entry is dropped).
bool zvukdl_local(const char *track_id, char *out, size_t size);
bool zvukdl_track_for_path(const char *path, char *id, size_t id_size, char *album_id, size_t album_size);
// The saved albums (track_count = tracks saved), then by artist and title.
int zvukdl_albums(zvuk_album_t *out, int max);
// An album's saved tracks in order (NULL: those of no album).
int zvukdl_tracks(const char *album_id, zvuk_track_t *out, int max);
bool zvukdl_delete(const char *track_id);
int zvukdl_delete_album(const char *album_id);

// The saved files the Zvuk queue plays (GUI thread sets, anyone reads): only
// these are Zvuk tracks to the rest of the player -- the same file played
// from the Library stays the library's. NULL/0 clears.
void zvukdl_claim(const char *const *paths, int count);
bool zvukdl_claimed(const char *path);

#endif /* ZVUKDL_H */

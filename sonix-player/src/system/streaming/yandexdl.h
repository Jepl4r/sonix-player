#ifndef YANDEXDL_H
#define YANDEXDL_H

#include <stdbool.h>

#include "src/system/streaming/yandex.h"

// Yandex Music tracks saved for good into the music library, as ordinary
// tagged files the library scan files like any other:
//
//   <music>/Yandex Music/<Album artist>/<Album (Year)>/<NN - Title>.<ext>
//
// <music> is the first of the folders the library scan is limited to, when it
// is limited ("Settings > Library > Folders"), and otherwise "Music" at the
// root of the card -- so the files are always somewhere the scan reads. A
// record of several discs numbers its files "D-NN - Title". Names lose what
// FAT forbids (podcastdl_safe_name). Beside the files, the album's cover.jpg.
//
// The file is the best the quality setting gets (yandex_file_info): FLAC
// where there is FLAC. A track the cache already holds complete is copied
// from there instead of downloaded again, unless the setting is Lossless and
// the cached copy is not FLAC. Tags are written into the file: Vorbis
// comments in a FLAC, an ID3v2.4 tag in an MP3, iTunes-style atoms in an MP4
// (AAC) -- its moov rewritten on the way past, the offsets after it moved
// (see "MP4" in yandexdl.c). An MP4 that cannot be rewritten so is kept as it
// came with a "<file>.tags" sidecar instead, which the player's tag reader --
// and so the library -- applies over its own (metadata.c); 1.2.4 wrote one
// for every MP4, and those are still read. Every saved track goes into the
// index of saved tracks (yandexsaved.h), which the Yandex pages play from.
//
// One file at a time, on a thread of its own at low priority, started on the
// first request: nothing runs, and nothing is allocated, until something is
// saved. The download gives way to whatever is being listened to (see
// yandexcache_fetch), and is written under a hidden ".<name>.part" renamed
// when complete, so a cut download -- power, a pulled card -- never shows up
// in the library. Pulling the card, exporting it over USB or signing out
// drops the queue. When the queue is done the library looks for the new files
// (library_card_returned, i.e. Detect changes, when that is on).
//
// Thread contract: the calls below may be made from any thread; the listener
// runs on the worker (the receiver bounces it to the interface with
// gui_post()).

// Queues one track. False when the queue is full or there is no card.
bool yandexdl_track(const yandex_track_t *track);
// Queues a whole album: its track list is asked for on the worker.
bool yandexdl_album(const char *album_id, const char *title);

typedef struct {
	bool active;	// something is queued or downloading
	int done;		// files saved (or already there) since the queue was last empty
	int failed;		// files that could not be saved, same span
	int queued;		// tracks still waiting, an album not yet expanded counting as one
	int percent;	// of the file in progress, -1 when unknown
	char title[96]; // the file in progress
} yandexdl_status_t;
void yandexdl_status(yandexdl_status_t *out);
bool yandexdl_busy(void);

typedef enum {
	YANDEXDL_QUEUED,   // `count` more to save
	YANDEXDL_SAVED,	   // one track is on the card (or already was): `text` its id
	YANDEXDL_FAILED,   // `text`: "<title>: <reason>"
	YANDEXDL_FINISHED, // the queue emptied: `count` saved, `text` the folder
	YANDEXDL_STOPPED,  // dropped (no card, signed out): `text` why
} yandexdl_event_t;
void yandexdl_set_listener(void (*listener)(yandexdl_event_t event, int count, const char *text));

// Drops the queue and the file in progress (its .part goes).
void yandexdl_cancel_all(void);

// The card, as the provider registry hands it (start-up, card change). A
// different root, or none, drops everything queued for the old one.
void yandexdl_set_root(const char *sd_root);

#endif /* YANDEXDL_H */

#ifndef ZVUKCACHE_H
#define ZVUKCACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "src/system/streaming/zvuk.h"

// Zvuk tracks downloaded to the card and played like any other file: the
// Yandex cache (yandexcache.h) for Zvuk, with what is particular to it:
//
//   * one GET per file, resumed with a Range from where it stopped when the
//     connection drops; an address refused (they expire) is asked for again
//     -- the same quality only, or two files would be spliced into one;
//   * a "flac" file is FLAC inside a plain, non-fragmented MP4 (or, now and
//     then, a native FLAC). It is unwrapped on the way to the card into an
//     ordinary .flac (mp4flac.c): the header from the moov, then the mdat's
//     frames as they come, so it plays while it downloads. A file whose moov
//     comes after its mdat is read in the order the unwrapping needs, with a
//     Range past the audio to the moov and back -- nothing is held in memory;
//   * MP3 (320 or 128) is copied as it comes;
//   * a file being written has a "<file>.incompleto" marker beside it until it
//     is complete, the convention of the other caches: a power cut in the
//     middle leaves a file the cache knows not to trust.
//
// Its own directory, so a path says which service it came from.
//
// Thread contract: zvukcache_start() blocks for the prebuffer and must run
// on a worker thread; the rest is cheap and callable from anywhere.

#define ZVUKCACHE_DIR ".local/zvuk-cache"
#define ZVUKCACHE_MAX_BYTES (1024L * 1024L * 1024L)
#define ZVUKCACHE_PRUNE_HEADROOM (96L * 1024 * 1024)
#define ZVUKCACHE_CLEAR_EVERY 10
#define ZVUKCACHE_PROTECTED_MAX 64

void zvukcache_set_root(const char *sd_root);
bool zvukcache_ready(void);
const char *zvukcache_dir(void);

// "flac" for FLAC, "mp3" for the rest.
const char *zvukcache_extension(zvuk_quality_t got);
void zvukcache_path(const char *track_id, const char *ext, char *out, size_t size);
// The track's file when it is complete, under any extension.
bool zvukcache_find(const char *track_id, char *out, size_t size);

// Starts downloading and returns once enough is on the card to start playing.
// `out` gets the path. Worker thread.
bool zvukcache_start(const zvuk_track_t *track, const zvuk_stream_t *stream, char *out, size_t size);
const char *zvukcache_last_error(void);

// Downloads into `out` (the caller's FILE, left open) on the caller's thread,
// unwrapped as the cache would: for a copy kept in the library (zvukdl.h).
// Gives way to playback's downloads. `progress` may be NULL; `samples` (may
// be NULL) gets an unwrapped FLAC's counted length, 0 otherwise. False with
// `err` set on failure or after zvukcache_fetch_cancel().
bool zvukcache_fetch(const char *track_id, const zvuk_stream_t *stream, FILE *out,
					 void (*progress)(long written, long total, void *user), void *user, uint64_t *samples, char *err,
					 size_t err_size);
void zvukcache_fetch_cancel(void);

// The tags beside the file ("<file>.tags") and the cover ("<stem>.jpg").
void zvukcache_write_sidecars(const zvuk_track_t *track, const char *path);

bool zvukcache_owns(const char *path);
// One of the cache's audio files (not a sidecar, not a marker).
bool zvukcache_is_track(const char *path);
void zvukcache_track_id(const char *path, char *out, size_t size);
bool zvukcache_album_id(const char *path, char *out, size_t size);
void zvukcache_downloading_id(char *out, size_t size);
bool zvukcache_busy(void);
void zvukcache_abandon_all(void);

// Raised by the queue as soon as it asks for a track, lowered by its watch:
// see yandexcache_set_network_wanted().
void zvukcache_set_network_wanted(bool wanted);
bool zvukcache_network_wanted(void);

// The Qobuz policy for keeping the cache in bounds (qobuzcache.h).
void zvukcache_note_played(const char *path);
void zvukcache_set_protected(const char *const *paths, int count);
void zvukcache_clear(void);
void zvukcache_clear_on_exit(void);

#endif /* ZVUKCACHE_H */

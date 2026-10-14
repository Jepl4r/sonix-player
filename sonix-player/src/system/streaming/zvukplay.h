#ifndef ZVUKPLAY_H
#define ZVUKPLAY_H

#include <stdbool.h>

#include "src/system/playback/playlist.h"
#include "src/system/streaming/zvuk.h"
#include "src/system/streaming/zvuklist.h"

// Playing Zvuk: the player's queue of Zvuk tracks, each behind the name of its
// file, fetched when playback gets near it. yandexplay.h for Zvuk, without My
// Wave: the queue is a list of ids (zvuklist.h), a saved track plays from the
// card (zvukdl.h), the rest from the cache (zvukcache.h), the file guessed as
// "<id>.flac" until it is fetched and its real name known.
//
// Thread contract: GUI thread, but zvukplay_prepare_track() which device_state
// calls from whatever thread starts a track. Downloads run on the module's own
// worker. Nothing is allocated or started until something Zvuk is played; the
// once-a-second watch is a timer made then, not before.

typedef enum {
	ZVUKPLAY_STARTED, // the list started
	ZVUKPLAY_FAILED,  // the track tapped could not be fetched (`text`: why)
	ZVUKPLAY_SKIPPED, // a queued track could not be fetched and was skipped
} zvukplay_event_t;

typedef void (*zvukplay_listener_t)(zvukplay_event_t event, const char *text);

// Registers the prepare callback with device_state. Once, at startup.
void zvukplay_init(zvukplay_listener_t listener);

// Plays `list` (a copy is taken) from entry `start`.
bool zvukplay_list(zvuklist_t *list, int start);

// Tracks from a menu or a swipe: their files, for the queue, with the tracks
// made known so playback can fetch them. `paths_out` is one allocation
// (free() it); returns how many.
int zvukplay_paths_for(const zvuk_track_t *tracks, int count, char ***paths_out);

// The Zvuk track loaded now, when this queue knows it.
bool zvukplay_current(zvuk_track_t *out);
// The id of the track playing (any queue), "" when it is not a Zvuk one.
void zvukplay_playing_id(char *out, size_t size);

// Signed out: the claims on saved files go.
void zvukplay_signed_out(void);

bool zvukplay_prepare_track(const char *path);

#endif /* ZVUKPLAY_H */

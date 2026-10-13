#ifndef YANDEXPLAY_H
#define YANDEXPLAY_H

#include <stdbool.h>

#include "src/system/playback/playlist.h"
#include "src/system/streaming/yandex.h"
#include "src/system/streaming/yandexlist.h"

// Playing Yandex Music: the player's queue of Yandex tracks, each behind the
// name of its file, fetched when playback gets near it.
//
// The queue is a list of ids (yandexlist.h) -- a liked list of thousands is
// queued whole, eight bytes a track, and a track's details and file are
// fetched when playback is about to reach it. A track saved on the card plays
// from its file. My Wave's batches are appended as they come (yandexwave.h).
// Tracks queued from a menu ("Play next", "Add to end of queue") join the same
// set, so that whatever queue they land in, the player can fetch them.
//
// What used to live in the Yandex page, moved here so that any page -- today's
// or UI 2.0's -- plays the same way: the page hands a list and a start, and
// hears back through the listener.
//
// Thread contract: every call on the GUI thread, but
// yandexplay_prepare_track(), which device_state calls from whatever thread
// starts a track. Downloads run on the module's own worker; what they bring
// is handed back to the GUI thread with gui_post(). Nothing is allocated or
// started until something Yandex is played.

typedef enum {
	YANDEXPLAY_STARTED, // the list started: the page may show the player
	YANDEXPLAY_FAILED,	// the track tapped could not be fetched (`text`: why)
	YANDEXPLAY_SKIPPED, // a track in the queue could not be fetched and was skipped (`text`: why)
} yandexplay_event_t;

typedef void (*yandexplay_listener_t)(yandexplay_event_t event, const char *text);

// Registers the prepare callback with device_state. Once, at startup.
void yandexplay_init(yandexplay_listener_t listener);

// Plays `list` (a copy is taken) from entry `start`: the queue becomes all of
// it. The tapped track is fetched first unless it is on the card; the rest
// when playback nears them. `wave`: the list is My Wave's first batch, and
// the queue grows as the station sends more. False when nothing could be
// started (empty list, no memory).
bool yandexplay_list(yandexlist_t *list, int start, bool wave);

// Tracks from a menu: their files, for trackmenu_queue_paths(), with the
// tracks made known to the queue so that playback can fetch them when they
// come up. `paths_out` gets `count` strings in one allocation (free() it);
// returns how many.
int yandexplay_paths_for(const yandex_track_t *tracks, int count, char ***paths_out);

// The Yandex track loaded now, when it is one this queue knows. False
// otherwise (another queue, nothing loaded).
bool yandexplay_current(yandex_track_t *out);

// Whether the queue playing is My Wave.
bool yandexplay_is_wave(void);

// Signed out: the wave stops asking, the claims on saved files go.
void yandexplay_signed_out(void);

// Once a second, from a GUI timer: keeps the queued files from the cache's
// sweep, fetches the next track ahead of playback, keeps the network up while
// it is needed, tells My Wave what plays.
void yandexplay_watch(void);

// device_state's prepare callback: false (and the track fetched, playback
// started when it is here) when the file is not on the card yet.
bool yandexplay_prepare_track(const char *path);

#endif /* YANDEXPLAY_H */

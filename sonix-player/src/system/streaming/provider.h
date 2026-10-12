#ifndef STREAM_PROVIDER_H
#define STREAM_PROVIDER_H

#include <stdbool.h>
#include <stddef.h>

// The streaming services that play by downloading into a cache on the card
// (Qobuz, Tidal, Subsonic), seen from the rest of the program.
//
// Every one of them works the same way from the outside: a track is a file
// under `.local/<service>-cache/`, and the file's path is its identity -- the
// service can always map a path back to its own track id. What differs is what
// happens behind that: whose account the star writes to, which playlists "Add
// to playlist" offers, which page "Show album" opens.
//
// Before this table every place that cared (the player's star and badge, the
// control centre, the playlist picker, the track menu, power management, the
// card watcher, every exit path) carried its own "if Qobuz ... else if Tidal"
// chain, and adding a service meant finding all of them. Now a service fills in
// one stream_provider_t and registers it; those places loop over the registry.
//
// What is deliberately not here: browsing. Each service's pages are its own --
// the APIs share nothing a common list could be built on -- and the radio and
// podcasts are not cache-backed track services at all (no account, no star on
// a track), so they keep their own code.
//
// Thread contract: services register during start-up, on the main thread,
// before the GUI is built; from then on the registry is read-only and may be
// read from any thread. The hooks state their own threads: the cache ones
// (owns_path, downloading, network_wanted, ...) are safe anywhere, the account
// ones (fav_get, playlist_*) are called from the GUI thread and their `done`
// callbacks arrive on the service's worker -- the receiver bounces them with
// gui_post(), as everywhere else.

#define STREAM_PROVIDER_MAX 8

// A write's outcome: `ok`, and when it is not, the reason as a sentence fit to
// display (may be empty, then the caller uses the provider's fallback key).
typedef void (*stream_provider_done_cb)(bool ok, const char *error, void *user);

// One of the account's playlists, as the "Add to playlist" picker shows it.
// The id is the service's own as text: Qobuz numbers, Tidal UUIDs and Subsonic
// strings all fit.
typedef struct {
	char id[64];
	char name[128];
	int track_count;
} stream_provider_playlist_t;

// The parts only the GUI can supply, attached by the service's page when it is
// built (stream_provider_attach_gui). Kept apart so this header needs no LVGL:
// the pointers are LVGL objects seen as void.
typedef struct {
	// The mark shown over the artwork while one of this service's tracks
	// plays: an lv_image_dsc_t. NULL = no mark.
	const void *badge;

	// "Show album" from the player: opens the service's own album page and
	// returns the screen that back should lead to (an lv_obj_t *), or NULL when
	// it could not (not logged in, missing id).
	void *(*open_album)(const char *album_id, const char *title);
} stream_provider_gui_t;

typedef struct stream_provider {
	// "qobuz": the log, and the "Source" row of the track details.
	const char *id;

	// --- the cache --------------------------------------------------------

	// Where the card is; NULL or empty disables the service's cache. Called at
	// start-up and on every card change.
	void (*set_root)(const char *sd_root);

	// The cache directory, NULL without a card.
	const char *(*cache_dir)(void);

	// True when `path` lies inside this service's cache. Says nothing about
	// whether it is a track (a sidecar is owned too).
	bool (*owns_path)(const char *path);

	// A download is in progress right now.
	bool (*downloading)(void);

	// The service needs the network even with nothing downloading this instant
	// (between a track's end and the next one's first bytes). NULL = never.
	bool (*network_wanted)(void);

	// The cache does not survive a power cycle: every exit path calls this.
	void (*clear_on_exit)(void);

	// --- the account ------------------------------------------------------

	// True when `path` is one of this service's tracks with an id the account
	// hooks below can use. Every hook below is only called for such a path.
	bool (*is_track)(const char *path);

	// The star: what the account says, from the service's in-memory mirror
	// (false until it arrives -- fav_refresh fetches it).
	bool (*fav_get)(const char *path);

	// Fetches the mirror if it is not there yet; `done` (may be NULL) arrives on
	// the worker, only when something changed.
	void (*fav_refresh)(void (*done)(void));

	// Adds (`want`) or removes the favourite. The mirror flips at once, so a
	// redraw straight after shows the new state.
	void (*fav_toggle)(const char *path, bool want, stream_provider_done_cb done, void *user);

	// Translation key for a failed star with no reason given.
	const char *fav_failed_key;

	// The service's album id the track belongs to, from the sidecar.
	bool (*album_id)(const char *path, char *out, size_t size);

	// Translation key for "the album could not be opened".
	const char *album_failed_key;

	// --- the account's playlists (all NULL when there are none to write) --

	bool (*playlists_known)(void);
	void (*playlists_refresh)(void (*done)(void));
	int (*playlists)(stream_provider_playlist_t *out, int max);
	void (*playlist_add)(const char *playlist_id, const char *path, stream_provider_done_cb done, void *user);
	void (*playlist_create_with)(const char *name, const char *path, stream_provider_done_cb done, void *user);

	// Translation keys: the picker's title ("Qobuz playlist") and the fallback
	// for a refused write.
	const char *playlist_title_key;
	const char *change_refused_key;

	// Filled by stream_provider_attach_gui().
	stream_provider_gui_t gui;
} stream_provider_t;

// Adds a service. Start-up only (see the thread contract). The struct must live
// for the life of the program; it is not copied.
void stream_provider_register(stream_provider_t *provider);

// Attaches the GUI part to the service with that id. GUI thread, at page init.
void stream_provider_attach_gui(const char *id, const stream_provider_gui_t *gui);

int stream_provider_count(void);
const stream_provider_t *stream_provider_at(int index);

// The service whose cache holds `path`, NULL for anything else (a file of the
// user's own, a podcast episode, a radio).
const stream_provider_t *stream_provider_for_path(const char *path);

// Like stream_provider_for_path, but only for an actual track of the service
// (is_track): the one the star and "Add to playlist" should write to.
const stream_provider_t *stream_provider_for_track(const char *path);

// --- the loops every caller used to write by hand -------------------------

// True when some service's cache holds `path`.
bool stream_provider_owns(const char *path);

// True when some service is downloading.
bool stream_provider_any_downloading(void);

// True when some service is downloading or otherwise wants the network.
bool stream_provider_any_network_wanted(void);

void stream_provider_set_root_all(const char *sd_root);
void stream_provider_clear_on_exit_all(void);

#endif /* STREAM_PROVIDER_H */

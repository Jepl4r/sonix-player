#ifndef YANDEX_H
#define YANDEX_H

#include <stdbool.h>
#include <stddef.h>

// Yandex Music: sign-in, the library, My Wave and the address of a track's file.
//
// Modelled on tidal.h, and for the same reasons: the API answers JSON, a track
// is downloaded to the card (yandexcache.h) and played like any other file, and
// sign-in is a device code typed on a phone -- the player has no keyboard worth
// typing a password on.
//
// What is different from Tidal:
//   * the stream address is asked for with a signed request (get-file-info):
//     HMAC-SHA256 over the timestamp, the track and the formats asked for, with
//     a key the desktop application carries. That key, the client id and its
//     secret come from streaming-keys (streamkeys.h), never from this source;
//   * the user is addressed by a numeric uid (/account/status), which every
//     library path carries;
//   * My Wave is a "rotor" session that hands out tracks a batch at a time and
//     wants to hear what was played, skipped and liked;
//   * the API is quick to answer 429 (or a captcha page) to anything that
//     looks automated, so every request is spaced out and a refusal makes the
//     whole module wait before trying again (see throttle() in yandex.c).
//
// Thread contract: everything here blocks on the network and must be called
// from a worker thread, never from the GUI thread. The functions share one
// error buffer per thread (yandex_last_error) and one session, guarded inside.
//
// The base URLs come from device_config.ini, [yandex] base_url and auth_url,
// so the simulator can be pointed at a mock (tools/yandex_mock.py).

#define YANDEX_ID_MAX 40
#define YANDEX_TITLE_MAX 200
#define YANDEX_NAME_MAX 128
#define YANDEX_URL_MAX 512

typedef struct {
	char id[YANDEX_ID_MAX];		  // the track id, digits ("12345")
	char album_id[YANDEX_ID_MAX]; // its first album, digits; empty for a user upload
	char title[YANDEX_TITLE_MAX];
	char artist[YANDEX_NAME_MAX];
	char album[YANDEX_TITLE_MAX];
	char cover[YANDEX_URL_MAX]; // already a full https:// URL, see yandex_cover_url()
	int duration;				// seconds
	int track_number;
	bool available; // false = in the catalogue but not playable here
} yandex_track_t;

typedef struct {
	char id[YANDEX_ID_MAX];
	char title[YANDEX_TITLE_MAX];
	char artist[YANDEX_NAME_MAX];
	char cover[YANDEX_URL_MAX];
	int year;
	int track_count;
} yandex_album_t;

typedef struct {
	char id[YANDEX_ID_MAX];
	char name[YANDEX_NAME_MAX];
	char cover[YANDEX_URL_MAX];
} yandex_artist_t;

typedef struct {
	char owner[YANDEX_ID_MAX]; // the owner's uid: a playlist is (uid, kind)
	char kind[YANDEX_ID_MAX];
	char title[YANDEX_TITLE_MAX];
	char cover[YANDEX_URL_MAX];
	int track_count;
} yandex_playlist_t;

void yandex_init(void);

// True when streaming-keys carries a [yandex] section with all four values.
// Without it the tile is not shown at all.
bool yandex_configured(void);
bool yandex_logged_in(void);
const char *yandex_display_name(void); // empty when not signed in
void yandex_logout(void);
const char *yandex_last_error(void);

// ---------------------------------------------------------------------------
// Sign-in: OAuth device flow on oauth.yandex.ru
// ---------------------------------------------------------------------------

typedef struct {
	char user_code[24];
	char verification_url[256]; // what the QR encodes, already https://
	char device_code[128];		// internal: only yandex_login_poll() needs it
	int interval_secs;
	int expires_secs;
} yandex_login_t;

typedef enum {
	YANDEX_LOGIN_PENDING = 0,
	YANDEX_LOGIN_OK,
	YANDEX_LOGIN_EXPIRED,
	YANDEX_LOGIN_ERROR,
} yandex_login_state_t;

bool yandex_login_begin(yandex_login_t *out);
yandex_login_state_t yandex_login_poll(const yandex_login_t *login);

// ---------------------------------------------------------------------------
// Library. Each returns how many it put in `out` (at most `max`), -1 on error.
// ---------------------------------------------------------------------------

int yandex_liked_tracks(yandex_track_t *out, int max);
int yandex_playlists(yandex_playlist_t *out, int max);
int yandex_playlist_tracks(const char *owner, const char *kind, yandex_track_t *out, int max);
int yandex_album_tracks(const char *album_id, yandex_track_t *out, int max);
int yandex_artist_albums(const char *artist_id, yandex_album_t *out, int max);
// /search?type=all. The counts are capacities on the way in and what was
// filled on the way out.
bool yandex_search(const char *text, yandex_artist_t *artists, int *n_artists, yandex_album_t *albums, int *n_albums, yandex_track_t *tracks, int *n_tracks);

// Likes. `like` false removes it.
bool yandex_like_track(const char *track_id, const char *album_id, bool like);
bool yandex_dislike_track(const char *track_id, const char *album_id);
bool yandex_is_liked(const char *track_id); // from the last yandex_liked_tracks()

// ---------------------------------------------------------------------------
// My Wave (rotor). One session at a time.
// ---------------------------------------------------------------------------

// Starts a new session and returns its first batch.
int yandex_wave_start(yandex_track_t *out, int max);
// The next batch of the running session. `played` are the tracks the session
// already gave (the API wants them back in `queue`), newest last.
int yandex_wave_more(const char *const *played, int played_count, yandex_track_t *out, int max);

typedef enum {
	YANDEX_WAVE_STARTED = 0, // radioStarted, sent by yandex_wave_start itself
	YANDEX_WAVE_TRACK_STARTED,
	YANDEX_WAVE_TRACK_FINISHED,
	YANDEX_WAVE_SKIP,
	YANDEX_WAVE_LIKE,
	YANDEX_WAVE_DISLIKE,
} yandex_wave_event_t;

// Tells the session what happened to a track. `played_secs` matters for
// finished and skip; ignored otherwise.
bool yandex_wave_feedback(yandex_wave_event_t event, const char *track_id, const char *album_id, int played_secs);
bool yandex_wave_active(void);

// ---------------------------------------------------------------------------
// The file
// ---------------------------------------------------------------------------

typedef struct {
	char codec[24]; // "flac", "flac-mp4", "mp3", "aac-mp4", "he-aac-mp4", "aac"
	char url[1024];
	char key[72]; // hex AES-128 key when the transport is encraw, else empty
	int bitrate;  // kbps as declared, 0 when not
} yandex_file_t;

// Signs and sends get-file-info. False with yandex_last_error() set.
bool yandex_file_info(const char *track_id, yandex_file_t *out);

// "avatars.yandex.net/get-music-content/.../%%" -> https://... at `px`x`px`.
void yandex_cover_url(const char *uri, int px, char *out, size_t size);

#endif /* YANDEX_H */

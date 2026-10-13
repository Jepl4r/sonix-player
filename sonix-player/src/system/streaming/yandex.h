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
	int disc_number; // the album's volume, 1 for most; 0 when not said
	bool available;	 // false = in the catalogue but not playable here
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

int yandex_playlists(yandex_playlist_t *out, int max);
int yandex_album_tracks(const char *album_id, yandex_track_t *out, int max);

// The long lists come as references first (yandexlist.h keeps them): each
// entry is handed to `each` in order, its album id possibly empty, `track`
// non-NULL when the answer carried the details anyway. They return how many
// entries there were, -1 on error.
typedef void (*yandex_ref_cb)(const char *track_id, const char *album_id, const yandex_track_t *track, void *user);

// GET /users/{uid}/likes/tracks: every liked track, newest first. Fills the
// mirror the star reads (below) on the way; `each` may be NULL.
int yandex_liked_ids(yandex_ref_cb each, void *user);
// GET /users/{owner}/playlists/{kind}?rich-tracks=false.
int yandex_playlist_ids(const char *owner, const char *kind, yandex_ref_cb each, void *user);

// POST /tracks, 50 ids a request: the details of tracks known by id ("id" or
// "id:album"). Tracks the service no longer has are missing from `out`.
// Returns how many came, -1 when the first request failed.
int yandex_tracks(const char *const *refs, int count, yandex_track_t *out, int max);

// What a saved file needs to know about its album beyond what each track
// carries: who the record is by (not the track's artists: a compilation is
// "Various Artists"), its year and genre, how many discs, the cover.
typedef struct {
	char id[YANDEX_ID_MAX];
	char title[YANDEX_TITLE_MAX];
	char artist[YANDEX_NAME_MAX]; // the album's artists, or "Various Artists"
	char genre[48];				  // as the service names it ("rusrock"), may be empty
	char cover[YANDEX_URL_MAX];	  // full https:// URL, large
	int year;					  // 0 when not said
	int discs;					  // volumes, at least 1
	int track_count;
} yandex_album_meta_t;
// /albums/<id>/with-tracks, the album's fields only.
bool yandex_album_meta(const char *album_id, yandex_album_meta_t *out);
int yandex_artist_albums(const char *artist_id, yandex_album_t *out, int max);
// The rest of the collection. Lists of rows return how many they put in
// `out` (at most `max`), -1 on error.
//   likes/artists -- also fills what yandex_artist_liked() reads (`out` may
//   be NULL to fill only that); likes/albums the same for albums.
int yandex_liked_artists(yandex_artist_t *out, int max);
int yandex_liked_albums(yandex_album_t *out, int max);
// likes/playlists: the other people's playlists the account liked.
int yandex_liked_playlists(yandex_playlist_t *out, int max);
// /feed: the playlists the service makes for the account -- Playlist of the
// Day, Premiere, Deja Vu, Missed Likes...
int yandex_feed_playlists(yandex_playlist_t *out, int max);
// landing3/chart: the chart's tracks, in order, through `each`.
int yandex_chart(yandex_ref_cb each, void *user);
// landing3/new-releases, then the albums (POST /albums).
int yandex_new_releases(yandex_album_t *out, int max);
// An artist's tracks, the most listened to first; `total` (may be NULL) gets
// how many the artist has in all.
int yandex_artist_tracks(const char *artist_id, yandex_track_t *out, int max, int *total);
// All of an artist's track ids (up to 2000), through `each`.
int yandex_artist_track_ids(const char *artist_id, yandex_ref_cb each, void *user);

// Liking an artist or an album; logged. The two below answer from what the
// lists above read and what was liked since: cheap, any thread.
bool yandex_like_artist(const char *artist_id, bool like);
bool yandex_like_album(const char *album_id, bool like);
bool yandex_artist_liked(const char *artist_id);
bool yandex_album_liked(const char *album_id);
// Whether the liked artists, and the liked albums, have been read this session.
bool yandex_liked_artists_known(void);
bool yandex_liked_albums_known(void);

// /search?type=all. The counts are capacities on the way in and what was
// filled on the way out.
bool yandex_search(const char *text, yandex_artist_t *artists, int *n_artists, yandex_album_t *albums, int *n_albums, yandex_track_t *tracks, int *n_tracks);

// Likes. `like` false removes it. A like written updates the mirror below.
// A dislike also tells a running My Wave, with how long the track was heard
// (`played_secs`, the service wants it).
bool yandex_like_track(const char *track_id, const char *album_id, bool like);
bool yandex_dislike_track(const char *track_id, const char *album_id, int played_secs);

// The mirror of the liked ids the player's star reads: filled by
// yandex_liked_ids() or yandex_liked_refresh() (ids only, one request),
// empty until then; every id the account has, compactly. The three below are
// cheap, never touch the network and may be called from any thread, the
// GUI's included.
bool yandex_liked_refresh(void);
bool yandex_liked_known(void);
bool yandex_is_liked(const char *track_id);
// Flips one id in the mirror without asking anyone: what a tap on the star
// shows at once, put back by the caller if the write is refused.
void yandex_liked_mark(const char *track_id, bool liked);

// ---------------------------------------------------------------------------
// My Wave (rotor). One session at a time.
// ---------------------------------------------------------------------------

// Starts a new session and returns its first batch. `queue` (may be NULL):
// tracks already received, "id" or "id:album", oldest first -- what a
// session started again must not hand out again.
int yandex_wave_start(const char *const *queue, int queue_count, yandex_track_t *out, int max);
// The next batch of the running session. `queue` as above: the tracks the
// session already gave, the newest last. A session the service has forgotten
// or ended is replaced by a new one, told the queue; `restarted` says so.
// Logged, request and answer. -1 with yandex_last_error() on failure.
int yandex_wave_more(const char *const *queue, int queue_count, yandex_track_t *out, int max, bool *restarted);

typedef enum {
	YANDEX_WAVE_STARTED = 0, // radioStarted, sent by yandex_wave_start itself
	YANDEX_WAVE_TRACK_STARTED,
	YANDEX_WAVE_TRACK_FINISHED,
	YANDEX_WAVE_SKIP,
	YANDEX_WAVE_LIKE,
	YANDEX_WAVE_DISLIKE,
} yandex_wave_event_t;

// Tells the session what happened to a track, tagged with the batch the
// track came in. `played_secs` matters for finished, skip and dislike;
// ignored otherwise. Logged.
bool yandex_wave_feedback(yandex_wave_event_t event, const char *track_id, const char *album_id, int played_secs);
bool yandex_wave_active(void);

// ---------------------------------------------------------------------------
// The file
// ---------------------------------------------------------------------------

typedef struct {
	char codec[24];		// "flac", "flac-mp4", "mp3", "aac-mp4", "he-aac-mp4", "aac"
	char quality[16];	// what the service says it gave: "lossless", "hq", "nq"
	char transport[12]; // "raw" or "encraw"
	char url[1024];
	char key[72]; // hex AES key (32 digits: AES-128, 64: AES-256) for encraw, else empty
	int bitrate;  // kbps as declared, 0 when not (always 0 for FLAC)
} yandex_file_t;

// Signs and sends get-file-info for the quality below. Lossless goes over
// "encraw" and falls back to a plain request, logged, when that is refused or
// its key cannot be used. Logs what came and, when lossless was asked for and
// a lossy file came, says so. False with yandex_last_error() set.
bool yandex_file_info(const char *track_id, yandex_file_t *out);

// True once the account was found to have no Plus (asked the first time a
// lossless request came back lossy): lossless is a Plus feature.
bool yandex_needs_plus(void);

// What get-file-info asks for, [yandex] quality in device_config.ini:
// "lossless" (the default), "high" or "normal". The service picks the file
// from the codecs offered, so this is a ceiling, not a promise: a track with no
// FLAC still comes as AAC under Lossless. Read on every request, so a change
// applies from the next track fetched (what is already cached stays as it is).
typedef enum {
	YANDEX_QUALITY_LOSSLESS = 0, // FLAC over encraw, else the best lossy file
	YANDEX_QUALITY_HIGH,		 // AAC 256 or MP3 320
	YANDEX_QUALITY_NORMAL,		 // "nq": AAC around 192, lighter on card and air
} yandex_quality_t;
yandex_quality_t yandex_get_quality(void);
void yandex_set_quality(yandex_quality_t quality);

// What the last get-file-info actually gave, "FLAC" or "MP3 320 kbps", for the
// settings to show. Empty before the first track. Any thread.
void yandex_last_format(char *out, size_t size);

// "avatars.yandex.net/get-music-content/.../%%" -> https://... at `px`x`px`.
void yandex_cover_url(const char *uri, int px, char *out, size_t size);

#endif /* YANDEX_H */

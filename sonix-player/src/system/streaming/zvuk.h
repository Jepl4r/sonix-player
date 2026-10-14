#ifndef ZVUK_H
#define ZVUK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "src/system/streaming/cardimport.h"

// Zvuk (zvuk.com, formerly SberZvuk): the account, the collection, search and
// the address of a track's file. Fork only, built with SONIX_ZVUK (Makefile);
// every Zvuk source compiles to nothing without it.
//
// Modelled on yandex.h, with what is particular to Zvuk (the research, with
// its sources, is in ZVUK-API.md beside the UI 2.0 notes; the short of it):
//
//   * there is no sign-in an outside application can do. The site signs in
//     with a phone number and an SMS behind an anti-bot page that needs a real
//     browser; no device flow, no refresh token. What every open client uses
//     is the web session's own token, which the user copies from
//     zvuk.com/api/tiny/profile once signed in there. The player takes it from
//     a file on the card, zvuk_token.txt (cardimport.h) -- and from nowhere
//     else: no keyboard, no QR, no page on the phone. It is sent as the
//     X-Auth-Token header. No application keys: nothing of ours goes into
//     streaming-keys;
//   * almost everything is GraphQL (POST /api/v1/graphql); the profile is
//     REST (/api/tiny/profile). Both sit behind ServicePipe, an anti-bot
//     filter that answers 418 or an HTML page to anything that does not look
//     like the web player: every request carries a browser's User-Agent, an
//     Origin and a Referer of zvuk.com ([zvuk] user_agent overrides the
//     User-Agent). Whether the device's TLS gets through at all is what the
//     first log lines of a session say (see "diagnostics" in zvuk.c);
//   * one getStream request answers every quality at once: "flac" (FLAC in a
//     plain MP4, no DRM -- unwrapped by mp4flac.c on the way to the card),
//     "flacdrm" (DRM, never used), "high" (MP3 320, subscription) and "mid"
//     (MP3 128, everyone). The addresses expire: zvukcache.c asks again when
//     one is refused;
//   * ids are numbers everywhere, written as strings in GraphQL; kept as
//     text here, as uint64 in the mirrors and lists.
//
// Thread contract: everything that talks to the service blocks and must run
// on a worker thread. The mirrors of what the account liked are read from any
// thread under a lock and never touch the network. Each thread has its own
// error text (zvuk_last_error).
//
// [zvuk] base_url in device_config.ini points everything at another server,
// so the simulator can use tools/zvuk_mock.py.

#define ZVUK_ID_MAX 24
#define ZVUK_TITLE_MAX 160
#define ZVUK_NAME_MAX 128
#define ZVUK_URL_MAX 320
#define ZVUK_TOKEN_MAX 160

// The file on the card the token comes from.
#define ZVUK_TOKEN_FILE "zvuk_token.txt"

typedef struct {
	char id[ZVUK_ID_MAX];
	char album_id[ZVUK_ID_MAX];	 // the release it is on
	char artist_id[ZVUK_ID_MAX]; // its first artist
	char title[ZVUK_TITLE_MAX];
	char artist[ZVUK_NAME_MAX];
	char album[ZVUK_TITLE_MAX];
	char cover[ZVUK_URL_MAX]; // a full https:// URL, see zvuk_cover_url()
	int duration;			  // seconds
	int track_number;		  // its place on the release, 0 when not said
	int year;				  // the release's, 0 when not said
	bool available;			  // false = listed but nothing to play
} zvuk_track_t;

typedef struct {
	char id[ZVUK_ID_MAX];
	char title[ZVUK_TITLE_MAX];
	char artist[ZVUK_NAME_MAX];
	char cover[ZVUK_URL_MAX];
	int year;
	int track_count; // 0 when not said
} zvuk_album_t;

typedef struct {
	char id[ZVUK_ID_MAX];
	char name[ZVUK_NAME_MAX];
} zvuk_artist_t;

typedef struct {
	char id[ZVUK_ID_MAX];
	char title[ZVUK_TITLE_MAX];
	int track_count; // 0 when not said
} zvuk_playlist_t;

void zvuk_init(void);

// Always true: Zvuk needs no keys of the firmware's, only the user's token.
bool zvuk_configured(void);
bool zvuk_logged_in(void);
const char *zvuk_last_error(void);

// Who is signed in, for the account card: the profile's name (may be empty),
// the subscription's title (empty for none or unknown) and when it ends (Unix
// seconds, 0 unknown). Any thread.
void zvuk_account(char *name, size_t name_size, char *plan, size_t plan_size, long *expires);
// The subscription is known to be missing: FLAC or MP3 320 was asked for and
// MP3 128 came. Any thread.
bool zvuk_needs_subscription(void);
// The last session ended because the service refused the token (401/403),
// not by the user's hand: the page says to put a new token on the card.
// Cleared by the next sign-in. Any thread.
bool zvuk_session_expired(void);

void zvuk_logout(void);

// ---------------------------------------------------------------------------
// Signing in with a token from the card (cardimport.h)
// ---------------------------------------------------------------------------

// Cleans what the file held: spaces, quotes, "token=" in front, or a whole
// profile answer pasted in ({"result":{..."token":"..."}}) -- the token is
// taken out of it. False when nothing that looks like a token is left.
bool zvuk_token_clean(const char *in, char *out, size_t size);

// Tries `token`: one profile request. The outcomes are cardimport's: OK (the
// session is saved, 0600, beside device_config.ini, and starts), OFFLINE,
// REFUSED (401/403, or an anonymous token), BLOCKED (418 or an HTML page).
// `message` gets the sentence for the toast. Worker thread. The diagnostic
// block goes to the log whatever the outcome.
cardimport_result_t zvuk_sign_in(const char *token, char *message, size_t message_size);

// Reads the profile again (name, subscription), for the account card. Worker.
bool zvuk_refresh_profile(void);

// The developer options' "Zvuk: connection check": the profile, then
// getStream for a known track ([zvuk] check_track, else a liked one, else a
// fixed catalogue id), every step's outcome on a line of `report`. Logged in
// full. Worker thread. False when a step failed.
bool zvuk_check_connection(char *report, size_t size);

// ---------------------------------------------------------------------------
// Library. Lists return how many they put in `out` (at most `max`), -1 on
// error with zvuk_last_error() set.
// ---------------------------------------------------------------------------

// Long lists come as track ids (zvuklist.h keeps them).
typedef void (*zvuk_id_cb)(const char *track_id, void *user);

// The liked tracks, newest first, ids only. Fills the mirror the star reads.
int zvuk_liked_ids(zvuk_id_cb each, void *user);
// A playlist's tracks, in order, ids only.
int zvuk_playlist_ids(const char *playlist_id, zvuk_id_cb each, void *user);

// The details of tracks known by id, at most ZVUK_TRACKS_PER_CALL a call.
// Tracks the service no longer has are missing from `out`.
#define ZVUK_TRACKS_PER_CALL 50
int zvuk_tracks(const char *const *ids, int count, zvuk_track_t *out, int max);

// A release's tracks, in order; `meta` (may be NULL) gets the release itself.
int zvuk_album_tracks(const char *album_id, zvuk_track_t *out, int max, zvuk_album_t *meta);

// An artist's page in one request: its most listened-to tracks and its
// releases. Returns tracks + albums, -1 on error.
int zvuk_artist_page(const char *artist_id, zvuk_track_t *tracks, int max_tracks, int *n_tracks, zvuk_album_t *albums,
					 int max_albums, int *n_albums);

// What the account liked besides tracks: the collection's ids (one request,
// which fills the mirrors), then the details of the first `max`.
int zvuk_liked_albums(zvuk_album_t *out, int max);
int zvuk_liked_artists(zvuk_artist_t *out, int max);
// The account's playlists: its own and those it liked.
int zvuk_playlists(zvuk_playlist_t *out, int max);

typedef enum {
	ZVUK_SEARCH_TRACKS = 0,
	ZVUK_SEARCH_ALBUMS,
	ZVUK_SEARCH_ARTISTS,
	ZVUK_SEARCH_PLAYLISTS,
	ZVUK_SEARCH_KINDS,
} zvuk_search_kind_t;

// One kind of result. `out` is an array of that kind's struct (zvuk_track_t,
// zvuk_album_t, zvuk_artist_t, zvuk_playlist_t).
int zvuk_search(const char *text, zvuk_search_kind_t kind, void *out, int max);

// ---------------------------------------------------------------------------
// Likes: the account's collection, mirrored
// ---------------------------------------------------------------------------

typedef enum {
	ZVUK_ITEM_TRACK = 0,
	ZVUK_ITEM_RELEASE,
	ZVUK_ITEM_ARTIST,
	ZVUK_ITEM_PLAYLIST,
	ZVUK_ITEM_KINDS,
} zvuk_item_t;

// Adds (`like`) or removes an item. The mirror follows when the service says
// yes. Worker thread. Logged.
bool zvuk_like(zvuk_item_t kind, const char *id, bool like);

// Reads the collection's ids of every kind (one request), for the mirrors.
bool zvuk_collection_refresh(void);

// The mirrors. Cheap, any thread, never the network.
bool zvuk_is_liked(zvuk_item_t kind, const char *id);
bool zvuk_liked_known(zvuk_item_t kind);
// Flips one id without asking anyone: what a tap shows at once, put back by
// the caller if the write is refused.
void zvuk_liked_mark(zvuk_item_t kind, const char *id, bool liked);

// ---------------------------------------------------------------------------
// The file
// ---------------------------------------------------------------------------

typedef enum {
	ZVUK_QUALITY_FLAC = 0,
	ZVUK_QUALITY_HIGH, // MP3 320
	ZVUK_QUALITY_MID,  // MP3 128
	ZVUK_QUALITY_COUNT,
} zvuk_quality_t;

// [zvuk] quality in device_config.ini: "flac" (the default), "high", "mid".
// A ceiling: a track without FLAC comes as MP3 320, an account without the
// subscription as MP3 128. Read on every request.
zvuk_quality_t zvuk_get_quality(void);
void zvuk_set_quality(zvuk_quality_t quality);

typedef struct {
	char url[1024];
	zvuk_quality_t asked;
	zvuk_quality_t got;
	long expires_in; // seconds the address is good for, 0 unknown
} zvuk_stream_t;

// The address of the best file at or below the quality asked for (getStream).
// Logs what was asked and what came. False with zvuk_last_error() set.
bool zvuk_stream(const char *track_id, zvuk_stream_t *out);

// What the last stream request actually gave ("FLAC", "MP3 320 kbps"), for
// the home page. Empty before the first. Any thread.
void zvuk_last_format(char *out, size_t size);

// An image `src` as the API gives it (absolute or relative, with a {size}
// template, a size= parameter or neither) at `px`x`px`.
void zvuk_cover_url(const char *src, int px, char *out, size_t size);

// The headers a browser on zvuk.com sends, for the CDN downloads too
// (http_set_thread_stream_headers). Static storage.
const char *zvuk_stream_headers(void);

#endif /* ZVUK_H */

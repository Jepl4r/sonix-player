#ifndef TIDAL_H
#define TIDAL_H

#include <stdbool.h>
#include <stddef.h>

// Tidal.
//
// Same shape as qobuz.h -- search, browse, ask for a track's address, write to
// the account -- so both pages behave the same way. Underneath, three
// differences explain every odd choice in here.
//
// One: there is no username and password.
//
// Qobuz still accepts a direct login; Tidal closed that to third-party clients
// years ago. What is left is the OAuth2 device code flow, the same one
// televisions use: the player asks for a code, shows it on screen, and the user
// opens link.tidal.com on a phone and types it in. Meanwhile the player polls
// every two seconds to see whether it has been authorized. It needs a second
// device, but it is the only path available and the password never passes
// through here. On a device with no keyboard, typing five characters on a phone
// beats spelling out a password with the wheel.
//
// Two: the token expires and has to be renewed.
//
// The Qobuz token lasts until revoked. The Tidal one lasts a week, and comes
// with a second token (the refresh token) used to get a new one without redoing
// the whole flow. Renewal happens automatically when a request returns 401,
// invisible to both the caller and the user.
//
// Three: a track's address may not be an address.
//
// Qobuz answers with a URL and that is that. Tidal answers with a base64-encoded
// manifest holding one of two things:
//
//   * BTS (application/vnd.tidal.bts) -- JSON containing a single address: one
//     file, downloadable and playable as is. This is what LOW, HIGH and LOSSLESS
//     return;
//   * DASH (application/dash+xml) -- an MPD, a list of segments to download and
//     reassemble. This is what hi-res returns.
//
// Which one arrives cannot be predicted from the requested quality: it also
// depends on the application keys in use. Never guess -- read
// `manifestMimeType` and decide afterwards. tidal_stream_t below carries both
// cases, and tidalcache.c can download either.
//
// The application credentials (client_id, client_secret) are not in here: they
// are read from a file on the device, see streamkeys.h. Tidal revokes them from
// time to time, so when login starts failing that file is the first thing to
// check, not this code.
//
// Every function that talks to the network blocks. Call them from a worker
// thread, never from the GUI thread -- same rule as http.h, radio.c and
// qobuz.c.

// ---------------------------------------------------------------------------
// Quality
// ---------------------------------------------------------------------------

// The names belong to Tidal and go into the query verbatim. The numbers are
// local and exist only so the choice can be stored in the configuration.
//
// HI_RES is absent and must stay absent: it was the MQA tier, which Tidal shut
// down for everyone in mid-2024. Asking for it today gets either an error or a
// silent downgrade, which is worse.
typedef enum {
	TIDAL_QUALITY_LOW = 0,	   // AAC 96 kbps
	TIDAL_QUALITY_HIGH = 1,	   // AAC 320 kbps
	TIDAL_QUALITY_LOSSLESS = 2, // FLAC 16 bit / 44.1 kHz
	TIDAL_QUALITY_HIRES = 3,   // FLAC up to 24 bit / 192 kHz
} tidal_quality_t;

int tidal_get_quality(void);
void tidal_set_quality(int quality);

// The string Tidal expects ("LOSSLESS", "HI_RES_LOSSLESS", ...).
const char *tidal_quality_name(int quality);

// The file type expected at the currently selected quality, so a track's file
// name is known before the track has been requested -- that is what makes it
// possible to queue a whole album while it downloads. Same contract as
// qobuz_expected_mime().
//
// An estimate, not a promise: Tidal may send less than was asked for. The code
// that writes the file uses the real type, which arrives with the manifest.
const char *tidal_expected_mime(void);

// ---------------------------------------------------------------------------
// What the lists show
// ---------------------------------------------------------------------------

#define TIDAL_TITLE_MAX 200
#define TIDAL_NAME_MAX 128
#define TIDAL_URL_MAX 1024
#define TIDAL_ID_MAX 40

typedef struct {
	long id;
	char title[TIDAL_TITLE_MAX];
	char artist[TIDAL_NAME_MAX];
	char album[TIDAL_TITLE_MAX];
	// Text rather than a number, for the same reason as Qobuz: both pages then
	// pass the album identifier to the same functions.
	char album_id[TIDAL_ID_MAX];
	char cover[TIDAL_URL_MAX]; // already built, see tidal_cover_url()
	int duration;			   // seconds
	int track_number;
	bool hires;
	bool streamable; // false = present but not playable on this subscription
	int bit_depth;
	int sample_rate; // Hz
} tidal_track_t;

typedef struct {
	long id;
	char id_text[TIDAL_ID_MAX];
	char title[TIDAL_TITLE_MAX];
	char artist[TIDAL_NAME_MAX];
	char cover[TIDAL_URL_MAX];
	char released[16]; // year only: that is all a row shows
	int track_count;
	bool hires;
} tidal_album_t;

typedef struct {
	long id;
	char name[TIDAL_NAME_MAX];
	char image[TIDAL_URL_MAX];
	int album_count;
} tidal_artist_t;

typedef struct {
	// The only thing in all of Tidal without a numeric identifier: playlists are
	// addressed by UUID ("854e65fb-857a-4db3-..."), hence the string here and in
	// the functions that take it.
	char id[TIDAL_ID_MAX];
	char name[TIDAL_TITLE_MAX];
	char owner[TIDAL_NAME_MAX];
	char image[TIDAL_URL_MAX];
	int track_count;
} tidal_playlist_t;

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

// Reads the application credentials and the stored tokens. Once at startup,
// after config_init() and streamkeys_init().
void tidal_init(void);

// False when the keys file is missing: the service is not broken, it is off,
// and the menu entry has to say so instead of trying and failing.
bool tidal_configured(void);

bool tidal_logged_in(void);
const char *tidal_display_name(void); // empty when not logged in

// Forgets the tokens, both here and in the configuration.
void tidal_logout(void);

// Why the last call failed, already in a form fit to display. Empty on success.
// Per thread, like http_last_error().
const char *tidal_last_error(void);

// --- device code login -----------------------------------------------------

// What is shown on screen while waiting.
typedef struct {
	char user_code[16];			 // the five characters to type
	char verification_url[256];	 // "link.tidal.com" -- already prefixed with https://
	char device_code[128];		 // internal: only tidal_login_poll() needs it
	int interval_secs;			 // how often Tidal wants to be polled
	int expires_secs;			 // how long the code stays valid
} tidal_login_t;

typedef enum {
	TIDAL_LOGIN_PENDING = 0, // nobody has typed the code yet: poll again
	TIDAL_LOGIN_OK,			 // logged in; tokens already stored
	TIDAL_LOGIN_EXPIRED,	 // the code expired: ask for another
	TIDAL_LOGIN_ERROR,		 // tidal_last_error() says what went wrong
} tidal_login_state_t;

// Asks Tidal for a code. Blocks. False on error.
bool tidal_login_begin(tidal_login_t *out);

// Asks whether anyone has authorized that code. Blocks (one request), and must
// not be called more often than `interval_secs`. On TIDAL_LOGIN_OK the tokens
// are already written to the configuration and tidal_logged_in() is true.
tidal_login_state_t tidal_login_poll(const tidal_login_t *login);

// ---------------------------------------------------------------------------
// Requests. Each returns how many items it wrote, or -1 on error
// (`tidal_last_error()` says what happened). All of them block.
// ---------------------------------------------------------------------------

int tidal_search_tracks(const char *query, int offset, tidal_track_t *out, int max);
int tidal_search_albums(const char *query, int offset, tidal_album_t *out, int max);
int tidal_search_artists(const char *query, int offset, tidal_artist_t *out, int max);

// The contents of a single item are paged too: a three-hundred-track playlist
// does not fit in one response, and neither does a prolific artist. `offset`
// says which entry to start from, as for searches.
int tidal_album_tracks(const char *album_id, int offset, tidal_track_t *out, int max);
int tidal_artist_albums(long artist_id, int offset, tidal_album_t *out, int max);
int tidal_playlist_tracks(const char *playlist_id, int offset, tidal_track_t *out, int max);

int tidal_favorite_tracks(int offset, tidal_track_t *out, int max);
int tidal_favorite_albums(int offset, tidal_album_t *out, int max);
int tidal_favorite_artists(int offset, tidal_artist_t *out, int max);
int tidal_user_playlists(int offset, tidal_playlist_t *out, int max);

// The collections Tidal features. `type` is one of the local short names, not
// one of Tidal's: "new", "top", "recommended". See the table in tidal.c for how
// they become URLs.
int tidal_featured_albums(const char *type, int offset, tidal_album_t *out, int max);

// ---------------------------------------------------------------------------
// Where the file is
// ---------------------------------------------------------------------------

typedef enum {
	// A single file at `url`. Download and play, like Qobuz.
	TIDAL_STREAM_FILE = 0,

	// Segments to reassemble: first `init_url`, then `media_template` with
	// $Number$ replaced, from start_number to start_number+count-1.
	TIDAL_STREAM_DASH,
} tidal_stream_kind_t;

typedef struct {
	tidal_stream_kind_t kind;

	char mime[64];	  // "audio/flac", "audio/mp4"...
	char codecs[24];  // "flac", "mp4a.40.2"...
	int sample_rate;  // Hz, 0 when not declared
	int bit_depth;	  // 16 or 24, 0 when not declared

	// TIDAL_STREAM_FILE
	char url[TIDAL_URL_MAX];

	// TIDAL_STREAM_DASH
	char init_url[TIDAL_URL_MAX];
	char media_template[TIDAL_URL_MAX]; // still contains "$Number$"
	int start_number;
	int segment_count;
} tidal_stream_t;

// Asks where the track is. Blocks. False on error.
//
// The struct is large (nearly three kilobytes of URLs: signed DASH ones run past
// five hundred characters each). The caller allocates it, and it must not go on
// the stack of a small thread.
bool tidal_track_stream(long track_id, tidal_stream_t *out);

// The URL of a DASH segment: `media_template` with $Number$ substituted. False
// when it does not fit.
bool tidal_dash_segment_url(const tidal_stream_t *stream, int number, char *out, size_t size);

// ---------------------------------------------------------------------------
// Cover art
// ---------------------------------------------------------------------------

// Tidal does not send image URLs: it sends a UUID, and the URL is built by
// replacing the dashes with slashes. Valid sizes for album covers are 80, 160,
// 320, 640 and 1280; artist photos take different ones (160, 320, 480, 750) and
// answer 404 for 80 or 1280.
void tidal_cover_url(const char *uuid, int size_px, char *out, size_t out_size);

// The size currently being requested. Used by whoever writes the cover cache
// file: the size is part of the name, so changing it yields a new file instead
// of keeping the old one forever.
int tidal_cover_px(void);
void tidal_artist_image_url(const char *uuid, int size_px, char *out, size_t out_size);

// ---------------------------------------------------------------------------
// Writing to the account
//
// Same reason as Qobuz: for a Tidal track, the player's star and "add to
// playlist" must land here and not in the local database -- the cache file
// being played is transient, and a favorite pointing at a path that will
// disappear is not a favorite.
// ---------------------------------------------------------------------------

bool tidal_favorite_add_track(long track_id);
bool tidal_favorite_remove_track(long track_id);

// The identifiers of the favorite tracks, so the star can be drawn correctly:
// one request for the whole list instead of one per track. Returns how many it
// wrote, or -1.
int tidal_favorite_track_ids(long *out, int max);

bool tidal_favorite_add_album(const char *album_id);
bool tidal_favorite_remove_album(const char *album_id);

bool tidal_playlist_delete(const char *playlist_id);
bool tidal_playlist_create(const char *name, char *out_id, size_t out_size);
bool tidal_playlist_add_track(const char *playlist_id, long track_id);

#endif /* TIDAL_H */

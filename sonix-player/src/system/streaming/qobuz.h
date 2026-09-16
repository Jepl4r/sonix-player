#ifndef QOBUZ_H
#define QOBUZ_H

#include <stdbool.h>
#include <stddef.h>

// Qobuz.
//
// Reconstructed from the original binary (`../app/qobuz/lg_qobuz_api.c` in its
// asserts), which talks to the Qobuz API directly: no HiBy server in between,
// nothing to reimplement on the network side beyond the requests.
//
// Two deliberate differences from the original, both improvements:
//
//   * the original uses `http://www.qobuz.com/api.json/0.2` -- cleartext, with
//     the password in a header. This starts from https, with the certificate
//     verification http.c actually performs (the original disables it:
//     CURLOPT_SSL_VERIFYPEER 0 everywhere, and its rootfs has no CA bundle at
//     all);
//   * the original keeps username and password in cleartext in
//     /data/qobuz_info and logs in again at every boot. Here only the session
//     token is stored: if it expires the password is asked for once, and at no
//     point is the password written anywhere.
//
// The application credentials (app_id, app_secret) are not in here: they are
// read from a file on the device, see streamkeys.h.
//
// Every function that talks to the network blocks. Call them from a worker
// thread, never from the GUI thread -- the same rule as http.h and radio.c.

// ---------------------------------------------------------------------------
// Quality
//
// The numbers are Qobuz's own, the ones the original uses.
// ---------------------------------------------------------------------------

typedef enum {
	QOBUZ_FORMAT_MP3 = 5,	  // MP3 320 kbps
	QOBUZ_FORMAT_CD = 6,	  // FLAC 16 bit / 44.1 kHz
	QOBUZ_FORMAT_HIRES96 = 7, // FLAC up to 24/96
	QOBUZ_FORMAT_HIRES192 = 27, // FLAC up to 24/192
} qobuz_format_t;

// What is requested from Qobuz. Stored in the configuration; Qobuz serves the
// best the subscription allows up to what was asked for, so lowering it saves
// card space and waiting rather than giving up quality one was entitled to.
int qobuz_get_format(void);
void qobuz_set_format(int format_id);

// The file type expected at the currently selected quality. It gives the name a
// track's file will have before it has been requested, which is what allows
// queueing a whole album and downloading it while it plays, instead of queueing
// only what has already arrived.
//
// MP3 only when MP3 was requested; for everything else Qobuz sends FLAC (at a
// resolution that may be lower than requested, but always FLAC).
const char *qobuz_expected_mime(void);

// ---------------------------------------------------------------------------
// What the lists show
// ---------------------------------------------------------------------------

#define QOBUZ_TITLE_MAX 200
#define QOBUZ_NAME_MAX 128
#define QOBUZ_URL_MAX 512

typedef struct {
	long id;
	char title[QOBUZ_TITLE_MAX];
	char artist[QOBUZ_NAME_MAX];
	char album[QOBUZ_TITLE_MAX];
	char album_id[40]; // to reopen the album the track comes from
	char cover[QOBUZ_URL_MAX];
	int duration;			   // seconds
	int track_number;
	bool hires;
	bool streamable; // false = present but not playable on this subscription
	int bit_depth;
	int sample_rate; // Hz
} qobuz_track_t;

typedef struct {
	long id; // Qobuz album ids are alphanumeric, so id_text is the real one
	char id_text[40];
	char title[QOBUZ_TITLE_MAX];
	char artist[QOBUZ_NAME_MAX];
	char cover[QOBUZ_URL_MAX];
	char released[16];
	int track_count;
	bool hires;
} qobuz_album_t;

typedef struct {
	long id;
	char name[QOBUZ_NAME_MAX];
	char image[QOBUZ_URL_MAX];
	int album_count;
} qobuz_artist_t;

typedef struct {
	long id;
	char name[QOBUZ_TITLE_MAX];
	char owner[QOBUZ_NAME_MAX];
	char image[QOBUZ_URL_MAX];
	int track_count;
} qobuz_playlist_t;

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

// Reads the application credentials and the stored token. Once at startup,
// after config_init() and streamkeys_init().
void qobuz_init(void);

// False when the keys file is missing: the service is not broken, it is off,
// and the menu entry must say so instead of trying and failing.
bool qobuz_configured(void);

bool qobuz_logged_in(void);
const char *qobuz_display_name(void); // empty when not logged in

// Blocks. On success stores the token and returns true.
bool qobuz_login(const char *username, const char *password);

// Forgets the token, here and in the configuration.
void qobuz_logout(void);

// Why the last call failed, already in a form fit to display. Empty on success.
// Per thread, like http_last_error().
const char *qobuz_last_error(void);

// ---------------------------------------------------------------------------
// Requests. All return how many items they wrote, or -1 on error
// (`qobuz_last_error()` says what happened). All block.
// ---------------------------------------------------------------------------

int qobuz_search_tracks(const char *query, int offset, qobuz_track_t *out, int max);
int qobuz_search_albums(const char *query, int offset, qobuz_album_t *out, int max);
int qobuz_search_artists(const char *query, int offset, qobuz_artist_t *out, int max);

// The contents of a single item are paged too: a three-hundred-track playlist
// does not fit in one response, and neither does a prolific artist. `offset`
// says which entry to start from, as for searches.
int qobuz_album_tracks(const char *album_id, int offset, qobuz_track_t *out, int max);
int qobuz_artist_albums(long artist_id, int offset, qobuz_album_t *out, int max);
int qobuz_playlist_tracks(long playlist_id, int offset, qobuz_track_t *out, int max);

int qobuz_favorite_tracks(int offset, qobuz_track_t *out, int max);
int qobuz_favorite_albums(int offset, qobuz_album_t *out, int max);
int qobuz_favorite_artists(int offset, qobuz_artist_t *out, int max);
int qobuz_user_playlists(int offset, qobuz_playlist_t *out, int max);

// The collections Qobuz curates: `type` is one of its own ("new-releases",
// "editor-picks", "best-sellers", "press-awards", "most-streamed").
int qobuz_featured_albums(const char *type, int offset, qobuz_album_t *out, int max);

// The signed URL of a track's audio file. `mime` may be NULL.
//
// The URL is valid for a few minutes and must not be stored: request it every
// time the track is played.
bool qobuz_track_url(long track_id, char *url, size_t url_size, char *mime, size_t mime_size, int *format_out);

// ---------------------------------------------------------------------------
// Writing to the account
//
// For a Qobuz track, the player's favourite star and "Add to playlist" must end
// up here and not in the local database: the cache file being played is
// transient, and a favourite pointing at a path bound to disappear is not a
// favourite.
// ---------------------------------------------------------------------------

bool qobuz_favorite_add_track(long track_id);
bool qobuz_favorite_remove_track(long track_id);

// The ids of the favourite tracks, to know how to draw the star. One request
// for the whole list instead of one per track. Returns how many it wrote, or
// -1.
int qobuz_favorite_track_ids(long *out, int max);

// Albums, by textual id.
bool qobuz_favorite_add_album(const char *album_id);
bool qobuz_favorite_remove_album(const char *album_id);

bool qobuz_playlist_delete(long playlist_id);

bool qobuz_playlist_create(const char *name, long *out_id);
bool qobuz_playlist_add_track(long playlist_id, long track_id);

#endif /* QOBUZ_H */

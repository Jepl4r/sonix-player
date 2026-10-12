#ifndef SUBSONIC_H
#define SUBSONIC_H

#include <stdbool.h>
#include <stddef.h>

// A client for the Subsonic API, as the OpenSubsonic servers speak it:
// Navidrome, Gonic, Airsonic-Advanced, Ampache, LMS. The user's own music on the
// user's own server, which makes this the one streaming service with no
// application keys, no subscription and no region.
//
// What is used of the protocol, and why only that:
//
//   * JSON (f=json), API version 1.16.1, client name "sonix". JSON rather than
//     the original XML because json.c already exists and every OpenSubsonic
//     server answers it;
//   * token authentication: t = md5(password + salt), s = salt. The salt is
//     made once at sign-in and kept, with the token, in the configuration --
//     the password itself is never written anywhere. A fixed salt is what the
//     servers accept and is also what keeps cover URLs stable, so the list
//     covers cached by URL (qobuzart.c) are found again next time;
//   * the old plain-password form (p=enc:<hex>) only when the server refuses
//     tokens (error 41, e.g. an LDAP-backed account). That password then does
//     have to be kept -- hex-encoded, which is obfuscation, not protection --
//     and the settings page says so;
//   * stream: format=raw by default (the file exactly as on the server), or a
//     transcode chosen in the settings for a slow network.
//
// Ids are strings: each server invents its own. They are only meaningful on
// the server that made them, hence subsonic_track_key().
//
// Every function that talks to the network blocks. Call them from a worker
// thread, never from the GUI thread -- the same rule as http.h and qobuz.h.

#define SUBSONIC_ID_MAX 64
#define SUBSONIC_TITLE_MAX 200
#define SUBSONIC_NAME_MAX 128
#define SUBSONIC_URL_MAX 768
#define SUBSONIC_SERVER_MAX 256

typedef struct {
	char id[SUBSONIC_ID_MAX];
	char title[SUBSONIC_TITLE_MAX];
	char artist[SUBSONIC_NAME_MAX];
	char album[SUBSONIC_TITLE_MAX];
	char album_id[SUBSONIC_ID_MAX];
	char cover_id[SUBSONIC_ID_MAX];
	char suffix[12]; // "flac": the file's own, i.e. what format=raw sends
	int duration;	 // seconds
	int track_number;
	int bit_rate; // kbps, 0 when unknown
	bool starred;
} subsonic_track_t;

typedef struct {
	char id[SUBSONIC_ID_MAX];
	char title[SUBSONIC_TITLE_MAX];
	char artist[SUBSONIC_NAME_MAX];
	char cover_id[SUBSONIC_ID_MAX];
	int year;
	int song_count;
} subsonic_album_t;

typedef struct {
	char id[SUBSONIC_ID_MAX];
	char name[SUBSONIC_NAME_MAX];
	char cover_id[SUBSONIC_ID_MAX];
	int album_count;
} subsonic_artist_t;

typedef struct {
	char id[SUBSONIC_ID_MAX];
	char name[SUBSONIC_TITLE_MAX];
	char owner[SUBSONIC_NAME_MAX];
	char cover_id[SUBSONIC_ID_MAX];
	int song_count;
} subsonic_playlist_t;

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

// Reads the saved server and token, and registers the service with the
// provider registry. Once at start-up, after config_init().
void subsonic_init(void);

bool subsonic_logged_in(void);

// "alice @ music.example.org", for the settings page. Empty when signed out.
void subsonic_display_name(char *out, size_t size);

// The server's base URL as saved ("https://music.example.org"). Empty when
// signed out.
void subsonic_server(char *out, size_t size);

// Blocks. Checks the server with ping and, on success, keeps server, user, salt
// and token. `server` may be given without a scheme ("192.168.1.10:4533"),
// which means http://.
bool subsonic_login(const char *server, const char *user, const char *password);

// Forgets everything, here and in the configuration.
void subsonic_logout(void);

// Why the last call failed, fit to display. Per thread, like http_last_error().
const char *subsonic_last_error(void);

// The 16-hex-digit key naming a track's cache file: FNV-1a 64 of the server URL
// and the song id, so the same id on two servers never shares a file.
void subsonic_track_key(const char *song_id, char *out, size_t size);

// ---------------------------------------------------------------------------
// Streaming quality
// ---------------------------------------------------------------------------

typedef enum {
	SUBSONIC_QUALITY_ORIGINAL = 0, // format=raw: the file as it is on the server
	SUBSONIC_QUALITY_MP3_320,
	SUBSONIC_QUALITY_MP3_192,
	SUBSONIC_QUALITY_OPUS_128,
	SUBSONIC_QUALITY_COUNT,
} subsonic_quality_t;

int subsonic_get_quality(void);
void subsonic_set_quality(int quality);

// The suffix the file will have at the current quality: the track's own for
// the original, the transcode's otherwise. Gives the cache name before asking.
const char *subsonic_expected_suffix(const subsonic_track_t *track);

// ---------------------------------------------------------------------------
// Requests. Lists return how many items they wrote, or -1 on error. All block.
// `offset` skips that many entries, for paging.
// ---------------------------------------------------------------------------

bool subsonic_ping(void);

int subsonic_artists(int offset, subsonic_artist_t *out, int max);
int subsonic_artist_albums(const char *artist_id, int offset, subsonic_album_t *out, int max);
int subsonic_album_tracks(const char *album_id, int offset, subsonic_track_t *out, int max);

// `type` is getAlbumList2's own: "newest", "recent", "random", "frequent",
// "starred", "alphabeticalByName".
int subsonic_album_list(const char *type, int offset, subsonic_album_t *out, int max);

int subsonic_search_tracks(const char *query, int offset, subsonic_track_t *out, int max);
int subsonic_search_albums(const char *query, int offset, subsonic_album_t *out, int max);
int subsonic_search_artists(const char *query, int offset, subsonic_artist_t *out, int max);

int subsonic_playlists(int offset, subsonic_playlist_t *out, int max);
int subsonic_playlist_tracks(const char *playlist_id, int offset, subsonic_track_t *out, int max);

int subsonic_starred_tracks(int offset, subsonic_track_t *out, int max);
int subsonic_starred_albums(int offset, subsonic_album_t *out, int max);

// The ids of every starred song, for the star's mirror. Writes up to `max`
// ids of SUBSONIC_ID_MAX characters each into `out`; returns how many, or -1.
int subsonic_starred_ids(char (*out)[SUBSONIC_ID_MAX], int max);

// star / unstar. `album` = the id is an album's, not a song's.
bool subsonic_star(const char *id, bool album, bool want);

// "Now playing" (submission false, at the start) and the play count
// (submission true, once listened to).
bool subsonic_scrobble(const char *song_id, bool submission);

bool subsonic_playlist_add(const char *playlist_id, const char *song_id);
bool subsonic_playlist_create(const char *name, const char *song_id);

// ---------------------------------------------------------------------------
// URLs. No network: they are built, not fetched. Empty when signed out.
// ---------------------------------------------------------------------------

// The audio, at the current quality.
void subsonic_stream_url(const char *song_id, char *out, size_t size);

// The cover, `px` pixels square (300 for lists, 600 for the player). Stable for
// the session's salt, so it can name a cache file.
void subsonic_cover_url(const char *cover_id, int px, char *out, size_t size);

#endif /* SUBSONIC_H */

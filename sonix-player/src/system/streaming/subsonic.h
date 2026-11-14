#ifndef SUBSONIC_H
#define SUBSONIC_H

#include <stdbool.h>
#include <stddef.h>

// Subsonic, and the servers that speak its API: Navidrome, Airsonic, Gonic,
// Supysonic. The protocol is plain HTTP with form parameters, so the whole
// client is a thin layer over http.c and json.c.
//
// Authentication is the Subsonic token scheme: a random salt per request, and
// `t = md5(password + salt)`. The password itself never leaves this module in
// a request, and it is never written to the configuration -- see
// subsonicaccount.c.
//
// The parameters travel in a POST body, not in the query string. A query
// string is logged by proxies and by the server's own access log, and the
// token is replayable for as long as the password does not change.
//
// Every function that talks to the network blocks. Call them from a worker
// thread, never from the GUI thread -- the same rule as http.h and qobuz.h.

#define SUBSONIC_ID_MAX 64
#define SUBSONIC_NAME_MAX 192
#define SUBSONIC_URL_MAX 256
#define SUBSONIC_USER_MAX 96
#define SUBSONIC_PASSWORD_MAX 128

typedef struct {
	char id[SUBSONIC_ID_MAX];
	char name[SUBSONIC_NAME_MAX];
	int album_count;
} subsonic_artist_t;

typedef struct {
	char id[SUBSONIC_ID_MAX];
	char name[SUBSONIC_NAME_MAX];
	char artist[SUBSONIC_NAME_MAX];
	char cover_id[SUBSONIC_ID_MAX];
	int year;
	int song_count;
} subsonic_album_t;

typedef struct {
	char id[SUBSONIC_ID_MAX];
	char title[SUBSONIC_NAME_MAX];
	char artist[SUBSONIC_NAME_MAX];
	char album[SUBSONIC_NAME_MAX];
	char suffix[8]; // file extension the server will serve, "flac", "mp3"
	int duration;	// seconds
	int track;
} subsonic_song_t;

// Loads the server and the user from the configuration, and the password from
// the account file. Safe to call before anything is configured.
void subsonic_init(void);

// True when there is a server, a user and a password. Says nothing about the
// server being reachable.
bool subsonic_configured(void);

// Stores the account. The server and the user go to the configuration; the
// password goes to the account file, never to the configuration. False when
// the server is not a URL this module can use, with the reason in
// subsonic_last_error().
bool subsonic_set_account(const char *server, const char *user, const char *password);

// Removes both the configuration entries and the account file.
void subsonic_clear_account(void);

const char *subsonic_server(void);
const char *subsonic_user(void);

// Why the last call failed. Per thread, like http_last_error(): more than one
// worker can have a request in flight.
const char *subsonic_last_error(void);

// True when the server answers and accepts the credentials.
bool subsonic_ping(void);

// Index lists, alphabetically as the server returns them. Each copies at most
// `max` entries and returns how many were written; 0 means none or an error,
// and subsonic_last_error() tells which.
int subsonic_get_artists(subsonic_artist_t *out, int max);
int subsonic_get_albums(const char *artist_id, subsonic_album_t *out, int max);
int subsonic_get_songs(const char *album_id, subsonic_song_t *out, int max);

// Writes the cover art to `out_path`. False on any failure; the caller leaves
// the destination as it was.
bool subsonic_get_cover(const char *cover_id, const char *out_path);

// Downloads one song to `out_path`. The caller has already decided the
// destination and that there is room; this only fetches. `bytes_out` may be
// NULL.
bool subsonic_download(const char *song_id, const char *out_path, unsigned long long *bytes_out);

#endif /* SUBSONIC_H */

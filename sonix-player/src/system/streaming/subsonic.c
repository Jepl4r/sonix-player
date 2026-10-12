#include "subsonic.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/core/json.h"
#include "src/system/core/lang.h"
#include "src/system/core/md5.h"
#include "src/system/net/http.h"
#include "src/system/streaming/subsonicsync.h"

#define API_VERSION "1.16.1"
#define CLIENT_NAME "sonix"

// A big library's getArtists or a three-thousand-song playlist runs to a few
// megabytes of JSON; past this the reply is refused rather than eating the
// device's memory.
#define BODY_LIMIT (6 * 1024 * 1024)
#define TIMEOUT_SECS 20

// The session. Written by the worker that signs in, read by every thread that
// builds a URL (the page's worker, the prefetch thread, the sync worker), so it
// sits behind a lock and is copied out whole.
typedef struct {
	char server[SUBSONIC_SERVER_MAX]; // no trailing slash
	char user[SUBSONIC_NAME_MAX];
	char salt[24];
	char token[40];		// md5(password + salt), hex
	char password_hex[256]; // only for servers that refuse tokens
} session_t;

static pthread_mutex_t session_lock = PTHREAD_MUTEX_INITIALIZER;
static session_t session;
static int quality = SUBSONIC_QUALITY_ORIGINAL;

static __thread char last_error[192];

const char *subsonic_last_error(void) { return last_error; }

static void set_error(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(last_error, sizeof(last_error), fmt, ap);
	va_end(ap);
}

static void session_copy(session_t *out) {
	pthread_mutex_lock(&session_lock);
	*out = session;
	pthread_mutex_unlock(&session_lock);
}

void subsonic_init(void) {
	pthread_mutex_lock(&session_lock);
	snprintf(session.server, sizeof(session.server), "%s", config_get("subsonic", "server", ""));
	snprintf(session.user, sizeof(session.user), "%s", config_get("subsonic", "user", ""));
	snprintf(session.salt, sizeof(session.salt), "%s", config_get("subsonic", "salt", ""));
	snprintf(session.token, sizeof(session.token), "%s", config_get("subsonic", "token", ""));
	snprintf(session.password_hex, sizeof(session.password_hex), "%s", config_get("subsonic", "password_hex", ""));
	pthread_mutex_unlock(&session_lock);

	quality = (int)config_get_int("subsonic", "quality", SUBSONIC_QUALITY_ORIGINAL);
	if (quality < 0 || quality >= SUBSONIC_QUALITY_COUNT) {
		quality = SUBSONIC_QUALITY_ORIGINAL;
	}

	// The cache is the service's as soon as there is a card, signed in or not:
	// the exit paths and the card watcher must find it either way.
	subsonicsync_register_provider();

	printf("subsonic: %s\n", subsonic_logged_in() ? "saved session" : "no session");
}

bool subsonic_logged_in(void) {
	pthread_mutex_lock(&session_lock);
	bool in = session.server[0] && session.user[0] && (session.token[0] || session.password_hex[0]);
	pthread_mutex_unlock(&session_lock);
	return in;
}

void subsonic_display_name(char *out, size_t size) {
	session_t s;
	session_copy(&s);
	const char *host = strstr(s.server, "://");
	host = host ? host + 3 : s.server;
	if (s.user[0]) {
		snprintf(out, size, "%s @ %s", s.user, host);
	} else if (size) {
		out[0] = '\0';
	}
}

void subsonic_server(char *out, size_t size) {
	pthread_mutex_lock(&session_lock);
	snprintf(out, size, "%s", session.server);
	pthread_mutex_unlock(&session_lock);
}

// FNV-1a, 64 bits: a dozen lines, no table, and at a few thousand cached files
// the chance of two keys colliding is around one in a billion billion.
void subsonic_track_key(const char *song_id, char *out, size_t size) {
	session_t s;
	session_copy(&s);
	uint64_t h = 0xcbf29ce484222325ULL;
	const char *parts[] = {s.server, "\n", song_id ? song_id : ""};
	for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
		for (const unsigned char *p = (const unsigned char *)parts[i]; *p; p++) {
			h ^= *p;
			h *= 0x100000001b3ULL;
		}
	}
	snprintf(out, size, "%016llx", (unsigned long long)h);
}

// ---------------------------------------------------------------------------
// quality
// ---------------------------------------------------------------------------

int subsonic_get_quality(void) { return quality; }

void subsonic_set_quality(int q) {
	if (q < 0 || q >= SUBSONIC_QUALITY_COUNT) {
		return;
	}
	quality = q;
	config_set_int("subsonic", "quality", q);
	config_save();
}

const char *subsonic_expected_suffix(const subsonic_track_t *track) {
	switch (quality) {
	case SUBSONIC_QUALITY_MP3_320:
	case SUBSONIC_QUALITY_MP3_192:
		return "mp3";
	case SUBSONIC_QUALITY_OPUS_128:
		return "opus";
	default:
		return track && track->suffix[0] ? track->suffix : "flac";
	}
}

// ---------------------------------------------------------------------------
// URLs
// ---------------------------------------------------------------------------

// "<server>/rest/<method>?u=..&t=..&s=..&v=..&c=..&f=json" plus `extra`
// (already encoded, starting with '&' or empty).
static bool build_url(const session_t *s, const char *method, const char *extra, char *out, size_t size) {
	if (!s->server[0] || !s->user[0]) {
		if (size) {
			out[0] = '\0';
		}
		return false;
	}
	char user[SUBSONIC_NAME_MAX * 3];
	http_url_encode(s->user, user, sizeof(user));
	int n;
	if (s->token[0]) {
		n = snprintf(out, size, "%s/rest/%s?u=%s&t=%s&s=%s&v=" API_VERSION "&c=" CLIENT_NAME "&f=json%s", s->server,
					 method, user, s->token, s->salt, extra ? extra : "");
	} else {
		n = snprintf(out, size, "%s/rest/%s?u=%s&p=enc:%s&v=" API_VERSION "&c=" CLIENT_NAME "&f=json%s", s->server,
					 method, user, s->password_hex, extra ? extra : "");
	}
	return n > 0 && (size_t)n < size;
}

void subsonic_stream_url(const char *song_id, char *out, size_t size) {
	session_t s;
	session_copy(&s);
	char id[SUBSONIC_ID_MAX * 3];
	http_url_encode(song_id ? song_id : "", id, sizeof(id));
	char extra[SUBSONIC_ID_MAX * 3 + 64];
	switch (quality) {
	case SUBSONIC_QUALITY_MP3_320:
		snprintf(extra, sizeof(extra), "&id=%s&format=mp3&maxBitRate=320", id);
		break;
	case SUBSONIC_QUALITY_MP3_192:
		snprintf(extra, sizeof(extra), "&id=%s&format=mp3&maxBitRate=192", id);
		break;
	case SUBSONIC_QUALITY_OPUS_128:
		snprintf(extra, sizeof(extra), "&id=%s&format=opus&maxBitRate=128", id);
		break;
	default:
		// raw: the file untouched, which is what makes Range resumption
		// possible (a transcode has no fixed length to resume into).
		snprintf(extra, sizeof(extra), "&id=%s&format=raw", id);
		break;
	}
	build_url(&s, "stream", extra, out, size);
}

void subsonic_cover_url(const char *cover_id, int px, char *out, size_t size) {
	if (!cover_id || !cover_id[0]) {
		if (size) {
			out[0] = '\0';
		}
		return;
	}
	session_t s;
	session_copy(&s);
	char id[SUBSONIC_ID_MAX * 3];
	http_url_encode(cover_id, id, sizeof(id));
	char extra[SUBSONIC_ID_MAX * 3 + 32];
	snprintf(extra, sizeof(extra), "&id=%s&size=%d", id, px);
	build_url(&s, "getCoverArt", extra, out, size);
}

// ---------------------------------------------------------------------------
// the call
// ---------------------------------------------------------------------------

// Every reply is {"subsonic-response":{"status":"ok"|"failed", "error":{...}}}.
// On success `*resp` is the subsonic-response object; the caller frees with
// call_done() once it has read the document (which points into the body).
static bool call_with(const session_t *s, const char *method, const char *extra, char **body, json_doc_t *doc,
					  int *resp, int *error_code) {
	last_error[0] = '\0';
	if (error_code) {
		*error_code = 0;
	}
	char url[SUBSONIC_URL_MAX + 512];
	if (!build_url(s, method, extra, url, sizeof(url))) {
		set_error("%s", tr("subsonic_not_signed_in"));
		return false;
	}

	int status = 0;
	size_t len = 0;
	*body = NULL;
	if (!http_get_ex(url, NULL, true, body, &len, BODY_LIMIT, TIMEOUT_SECS, &status)) {
		const char *why = http_last_error();
		set_error("%s", why && why[0] ? why : tr("subsonic_cannot_reach_server"));
		return false;
	}
	if (!json_parse(*body, doc)) {
		free(*body);
		*body = NULL;
		set_error(tr("subsonic_server_answered"), status);
		return false;
	}
	*resp = json_get(doc, json_root(doc), "subsonic-response");
	char state[16] = "";
	json_obj_str(doc, *resp, "status", state, sizeof(state));
	if (*resp < 0 || strcmp(state, "ok") != 0) {
		int err = json_get(doc, *resp, "error");
		char message[160] = "";
		json_obj_str(doc, err, "message", message, sizeof(message));
		long code = json_obj_long(doc, err, "code", 0);
		if (error_code) {
			*error_code = (int)code;
		}
		// 40 is the one every user meets; the server's own sentence for the
		// rest, which is written for a human.
		if (code == 40) {
			set_error("%s", tr("subsonic_wrong_credentials"));
		} else if (message[0]) {
			set_error("%s", message);
		} else {
			set_error(tr("subsonic_server_answered"), status);
		}
		json_free(doc);
		free(*body);
		*body = NULL;
		return false;
	}
	return true;
}

static bool call(const char *method, const char *extra, char **body, json_doc_t *doc, int *resp) {
	session_t s;
	session_copy(&s);
	return call_with(&s, method, extra, body, doc, resp, NULL);
}

static void call_done(char *body, json_doc_t *doc) {
	json_free(doc);
	free(body);
}

// ---------------------------------------------------------------------------
// sign-in
// ---------------------------------------------------------------------------

static void make_salt(char *out, size_t size) {
	unsigned char raw[8] = {0};
	int fd = open("/dev/urandom", O_RDONLY);
	bool got = false;
	if (fd >= 0) {
		got = read(fd, raw, sizeof(raw)) == (ssize_t)sizeof(raw);
		close(fd);
	}
	if (!got) {
		// No urandom (it is always there on Linux, but): time and pid. A salt
		// only has to differ from other clients', not be secret.
		unsigned long v = (unsigned long)time(NULL) ^ ((unsigned long)getpid() << 16);
		memcpy(raw, &v, sizeof(v) < sizeof(raw) ? sizeof(v) : sizeof(raw));
	}
	size_t n = 0;
	for (size_t i = 0; i < sizeof(raw) && n + 2 < size; i++) {
		n += (size_t)snprintf(out + n, size - n, "%02x", raw[i]);
	}
}

static void normalise_server(const char *in, char *out, size_t size) {
	while (*in == ' ') {
		in++;
	}
	if (strncmp(in, "http://", 7) == 0 || strncmp(in, "https://", 8) == 0) {
		snprintf(out, size, "%s", in);
	} else {
		snprintf(out, size, "http://%s", in);
	}
	// No trailing slashes or spaces: "/rest/ping" is appended to it.
	size_t len = strlen(out);
	while (len > 0 && (out[len - 1] == '/' || out[len - 1] == ' ')) {
		out[--len] = '\0';
	}
}

bool subsonic_login(const char *server, const char *user, const char *password) {
	if (!server || !server[0] || !user || !user[0] || !password) {
		set_error("%s", tr("credentials_required"));
		return false;
	}
	session_t s;
	memset(&s, 0, sizeof(s));
	normalise_server(server, s.server, sizeof(s.server));
	snprintf(s.user, sizeof(s.user), "%s", user);
	make_salt(s.salt, sizeof(s.salt));

	char salted[512];
	snprintf(salted, sizeof(salted), "%s%s", password, s.salt);
	md5_hex(salted, strlen(salted), s.token);
	memset(salted, 0, sizeof(salted));

	char *body = NULL;
	json_doc_t doc;
	int resp = -1;
	int code = 0;
	bool ok = call_with(&s, "ping", NULL, &body, &doc, &resp, &code);
	if (!ok && code == 41) {
		// "Token authentication not supported for LDAP users": the old form,
		// and the password has to be kept for it.
		printf("subsonic: the server refuses tokens, falling back to the password\n");
		s.token[0] = '\0';
		s.salt[0] = '\0';
		size_t n = 0;
		for (const unsigned char *p = (const unsigned char *)password; *p && n + 2 < sizeof(s.password_hex); p++) {
			n += (size_t)snprintf(s.password_hex + n, sizeof(s.password_hex) - n, "%02x", *p);
		}
		ok = call_with(&s, "ping", NULL, &body, &doc, &resp, &code);
	}
	if (!ok) {
		return false;
	}
	char kind[32] = "";
	char version[32] = "";
	json_obj_str(&doc, resp, "type", kind, sizeof(kind));
	json_obj_str(&doc, resp, "serverVersion", version, sizeof(version));
	printf("subsonic: signed in to %s (%s %s, API %s)\n", s.server, kind[0] ? kind : "subsonic", version,
		   json_obj_bool(&doc, resp, "openSubsonic", false) ? "OpenSubsonic" : "classic");
	call_done(body, &doc);

	pthread_mutex_lock(&session_lock);
	session = s;
	pthread_mutex_unlock(&session_lock);

	config_set("subsonic", "server", s.server);
	config_set("subsonic", "user", s.user);
	config_set("subsonic", "salt", s.salt);
	config_set("subsonic", "token", s.token);
	config_set("subsonic", "password_hex", s.password_hex);
	config_save();
	return true;
}

void subsonic_logout(void) {
	pthread_mutex_lock(&session_lock);
	memset(&session, 0, sizeof(session));
	pthread_mutex_unlock(&session_lock);
	// The server is kept: signing back in is mostly typing the password again.
	config_set("subsonic", "salt", "");
	config_set("subsonic", "token", "");
	config_set("subsonic", "password_hex", "");
	config_save();
}

bool subsonic_ping(void) {
	char *body;
	json_doc_t doc;
	int resp;
	if (!call("ping", NULL, &body, &doc, &resp)) {
		return false;
	}
	call_done(body, &doc);
	return true;
}

// ---------------------------------------------------------------------------
// reading the entries
// ---------------------------------------------------------------------------

// An id as text whatever the server sent: OpenSubsonic says string, but older
// Ampache and Airsonic builds send numbers.
static void read_id(const json_doc_t *doc, int obj, const char *key, char *out, size_t size) {
	int tok = json_get(doc, obj, key);
	out[0] = '\0';
	if (tok < 0) {
		return;
	}
	if (doc->toks[tok].type == JSON_STRING) {
		json_str(doc, tok, out, size);
	} else if (doc->toks[tok].type == JSON_NUMBER) {
		int len = doc->toks[tok].end - doc->toks[tok].start;
		snprintf(out, size, "%.*s", len, doc->text + doc->toks[tok].start);
	}
}

static void read_track(const json_doc_t *doc, int obj, subsonic_track_t *t) {
	memset(t, 0, sizeof(*t));
	read_id(doc, obj, "id", t->id, sizeof(t->id));
	json_obj_str(doc, obj, "title", t->title, sizeof(t->title));
	json_obj_str(doc, obj, "artist", t->artist, sizeof(t->artist));
	json_obj_str(doc, obj, "album", t->album, sizeof(t->album));
	read_id(doc, obj, "albumId", t->album_id, sizeof(t->album_id));
	read_id(doc, obj, "coverArt", t->cover_id, sizeof(t->cover_id));
	json_obj_str(doc, obj, "suffix", t->suffix, sizeof(t->suffix));
	t->duration = (int)json_obj_long(doc, obj, "duration", 0);
	t->track_number = (int)json_obj_long(doc, obj, "track", 0);
	t->bit_rate = (int)json_obj_long(doc, obj, "bitRate", 0);
	t->starred = json_get(doc, obj, "starred") >= 0;
	if (!t->title[0]) {
		json_obj_str(doc, obj, "name", t->title, sizeof(t->title));
	}
}

static void read_album(const json_doc_t *doc, int obj, subsonic_album_t *a) {
	memset(a, 0, sizeof(*a));
	read_id(doc, obj, "id", a->id, sizeof(a->id));
	// getAlbumList2 and search3 say "name", the older calls "title".
	if (!json_obj_str(doc, obj, "name", a->title, sizeof(a->title)) || !a->title[0]) {
		json_obj_str(doc, obj, "title", a->title, sizeof(a->title));
	}
	json_obj_str(doc, obj, "artist", a->artist, sizeof(a->artist));
	read_id(doc, obj, "coverArt", a->cover_id, sizeof(a->cover_id));
	a->year = (int)json_obj_long(doc, obj, "year", 0);
	a->song_count = (int)json_obj_long(doc, obj, "songCount", 0);
}

static void read_artist(const json_doc_t *doc, int obj, subsonic_artist_t *a) {
	memset(a, 0, sizeof(*a));
	read_id(doc, obj, "id", a->id, sizeof(a->id));
	json_obj_str(doc, obj, "name", a->name, sizeof(a->name));
	read_id(doc, obj, "coverArt", a->cover_id, sizeof(a->cover_id));
	a->album_count = (int)json_obj_long(doc, obj, "albumCount", 0);
}

static void read_playlist(const json_doc_t *doc, int obj, subsonic_playlist_t *p) {
	memset(p, 0, sizeof(*p));
	read_id(doc, obj, "id", p->id, sizeof(p->id));
	json_obj_str(doc, obj, "name", p->name, sizeof(p->name));
	json_obj_str(doc, obj, "owner", p->owner, sizeof(p->owner));
	read_id(doc, obj, "coverArt", p->cover_id, sizeof(p->cover_id));
	p->song_count = (int)json_obj_long(doc, obj, "songCount", 0);
}

typedef void (*reader_fn)(const json_doc_t *doc, int obj, void *out);

// Copies elements [offset, offset + max) of array `arr`. For the calls with no
// paging of their own (getAlbum, getArtist, getPlaylist): the whole reply is
// already here, and the page just takes its slice.
static int read_slice(const json_doc_t *doc, int arr, int offset, void *out, size_t item, int max, reader_fn fn) {
	int total = json_len(doc, arr);
	int n = 0;
	for (int i = offset; i < total && n < max; i++) {
		fn(doc, json_at(doc, arr, i), (char *)out + (size_t)n * item);
		n++;
	}
	return n;
}

static void reader_track(const json_doc_t *d, int o, void *out) { read_track(d, o, out); }
static void reader_album(const json_doc_t *d, int o, void *out) { read_album(d, o, out); }
static void reader_artist(const json_doc_t *d, int o, void *out) { read_artist(d, o, out); }
static void reader_playlist(const json_doc_t *d, int o, void *out) { read_playlist(d, o, out); }

// One call, one array somewhere in the reply, one slice of it.
static int list_call(const char *method, const char *extra, const char *path, int offset, void *out, size_t item,
					 int max, reader_fn fn) {
	if (!out || max <= 0) {
		return 0;
	}
	char *body;
	json_doc_t doc;
	int resp;
	if (!call(method, extra, &body, &doc, &resp)) {
		return -1;
	}
	int n = read_slice(&doc, json_path(&doc, resp, path), offset, out, item, max, fn);
	call_done(body, &doc);
	return n;
}

static void id_param(const char *name, const char *value, char *out, size_t size) {
	char enc[SUBSONIC_ID_MAX * 3];
	http_url_encode(value ? value : "", enc, sizeof(enc));
	snprintf(out, size, "&%s=%s", name, enc);
}

// ---------------------------------------------------------------------------
// browsing
// ---------------------------------------------------------------------------

// getArtists groups the artists under index letters; the page wants one flat
// list, in the server's order.
int subsonic_artists(int offset, subsonic_artist_t *out, int max) {
	if (!out || max <= 0) {
		return 0;
	}
	char *body;
	json_doc_t doc;
	int resp;
	if (!call("getArtists", NULL, &body, &doc, &resp)) {
		return -1;
	}
	int index = json_path(&doc, resp, "artists.index");
	int seen = 0;
	int n = 0;
	for (int i = 0; i < json_len(&doc, index) && n < max; i++) {
		int arr = json_get(&doc, json_at(&doc, index, i), "artist");
		for (int k = 0; k < json_len(&doc, arr) && n < max; k++, seen++) {
			if (seen >= offset) {
				read_artist(&doc, json_at(&doc, arr, k), &out[n++]);
			}
		}
	}
	call_done(body, &doc);
	return n;
}

int subsonic_artist_albums(const char *artist_id, int offset, subsonic_album_t *out, int max) {
	char extra[SUBSONIC_ID_MAX * 3 + 8];
	id_param("id", artist_id, extra, sizeof(extra));
	return list_call("getArtist", extra, "artist.album", offset, out, sizeof(*out), max, reader_album);
}

int subsonic_album_tracks(const char *album_id, int offset, subsonic_track_t *out, int max) {
	char extra[SUBSONIC_ID_MAX * 3 + 8];
	id_param("id", album_id, extra, sizeof(extra));
	return list_call("getAlbum", extra, "album.song", offset, out, sizeof(*out), max, reader_track);
}

int subsonic_album_list(const char *type, int offset, subsonic_album_t *out, int max) {
	char extra[128];
	snprintf(extra, sizeof(extra), "&type=%s&size=%d&offset=%d", type, max, offset);
	return list_call("getAlbumList2", extra, "albumList2.album", 0, out, sizeof(*out), max, reader_album);
}

static int search(const char *query, const char *what, int offset, void *out, size_t item, int max,
				  const char *path, reader_fn fn) {
	char enc[600];
	http_url_encode(query ? query : "", enc, sizeof(enc));
	char extra[800];
	// Only the kind asked for: the other two counts at zero.
	snprintf(extra, sizeof(extra), "&query=%s&artistCount=%d&artistOffset=%d&albumCount=%d&albumOffset=%d"
								   "&songCount=%d&songOffset=%d",
			 enc, strcmp(what, "artist") == 0 ? max : 0, offset, strcmp(what, "album") == 0 ? max : 0, offset,
			 strcmp(what, "song") == 0 ? max : 0, offset);
	return list_call("search3", extra, path, 0, out, item, max, fn);
}

int subsonic_search_tracks(const char *query, int offset, subsonic_track_t *out, int max) {
	return search(query, "song", offset, out, sizeof(*out), max, "searchResult3.song", reader_track);
}

int subsonic_search_albums(const char *query, int offset, subsonic_album_t *out, int max) {
	return search(query, "album", offset, out, sizeof(*out), max, "searchResult3.album", reader_album);
}

int subsonic_search_artists(const char *query, int offset, subsonic_artist_t *out, int max) {
	return search(query, "artist", offset, out, sizeof(*out), max, "searchResult3.artist", reader_artist);
}

int subsonic_playlists(int offset, subsonic_playlist_t *out, int max) {
	return list_call("getPlaylists", NULL, "playlists.playlist", offset, out, sizeof(*out), max, reader_playlist);
}

int subsonic_playlist_tracks(const char *playlist_id, int offset, subsonic_track_t *out, int max) {
	char extra[SUBSONIC_ID_MAX * 3 + 8];
	id_param("id", playlist_id, extra, sizeof(extra));
	return list_call("getPlaylist", extra, "playlist.entry", offset, out, sizeof(*out), max, reader_track);
}

int subsonic_starred_tracks(int offset, subsonic_track_t *out, int max) {
	return list_call("getStarred2", NULL, "starred2.song", offset, out, sizeof(*out), max, reader_track);
}

int subsonic_starred_albums(int offset, subsonic_album_t *out, int max) {
	return list_call("getStarred2", NULL, "starred2.album", offset, out, sizeof(*out), max, reader_album);
}

int subsonic_starred_ids(char (*out)[SUBSONIC_ID_MAX], int max) {
	char *body;
	json_doc_t doc;
	int resp;
	if (!call("getStarred2", NULL, &body, &doc, &resp)) {
		return -1;
	}
	int arr = json_path(&doc, resp, "starred2.song");
	int n = 0;
	for (int i = 0; i < json_len(&doc, arr) && n < max; i++) {
		read_id(&doc, json_at(&doc, arr, i), "id", out[n], SUBSONIC_ID_MAX);
		if (out[n][0]) {
			n++;
		}
	}
	call_done(body, &doc);
	return n;
}

// ---------------------------------------------------------------------------
// writing
// ---------------------------------------------------------------------------

static bool simple_call(const char *method, const char *extra) {
	char *body;
	json_doc_t doc;
	int resp;
	if (!call(method, extra, &body, &doc, &resp)) {
		return false;
	}
	call_done(body, &doc);
	return true;
}

bool subsonic_star(const char *id, bool album, bool want) {
	char extra[SUBSONIC_ID_MAX * 3 + 16];
	id_param(album ? "albumId" : "id", id, extra, sizeof(extra));
	return simple_call(want ? "star" : "unstar", extra);
}

bool subsonic_scrobble(const char *song_id, bool submission) {
	char extra[SUBSONIC_ID_MAX * 3 + 64];
	id_param("id", song_id, extra, sizeof(extra));
	size_t len = strlen(extra);
	snprintf(extra + len, sizeof(extra) - len, "&submission=%s", submission ? "true" : "false");
	return simple_call("scrobble", extra);
}

bool subsonic_playlist_add(const char *playlist_id, const char *song_id) {
	char a[SUBSONIC_ID_MAX * 3 + 16];
	char b[SUBSONIC_ID_MAX * 3 + 16];
	id_param("playlistId", playlist_id, a, sizeof(a));
	id_param("songIdToAdd", song_id, b, sizeof(b));
	char extra[sizeof(a) + sizeof(b)];
	snprintf(extra, sizeof(extra), "%s%s", a, b);
	return simple_call("updatePlaylist", extra);
}

bool subsonic_playlist_create(const char *name, const char *song_id) {
	char enc[SUBSONIC_TITLE_MAX * 3];
	http_url_encode(name ? name : "", enc, sizeof(enc));
	char b[SUBSONIC_ID_MAX * 3 + 16];
	id_param("songId", song_id, b, sizeof(b));
	char extra[sizeof(enc) + sizeof(b) + 8];
	snprintf(extra, sizeof(extra), "&name=%s%s", enc, b);
	return simple_call("createPlaylist", extra);
}

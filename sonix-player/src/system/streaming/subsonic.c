#include "subsonic.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/core/json.h"
#include "src/system/core/md5.h"
#include "src/system/net/http.h"
#include "subsonicaccount.h"

// The protocol version this client speaks. 1.16.1 is what Navidrome and the
// Airsonic family implement; a lower one loses the index endpoints, and a
// higher one is not promised by anybody.
#define SUBSONIC_API_VERSION "1.16.1"
#define SUBSONIC_CLIENT_NAME "SonixPlayer"

// A JSON answer of a few hundred kilobytes is normal for an album list; two
// megabytes is already a library the player will not draw in one go.
#define SUBSONIC_JSON_LIMIT (2u * 1024u * 1024u)
#define SUBSONIC_TIMEOUT_SECS 20
#define SUBSONIC_SALT_BYTES 8

static char server_url[SUBSONIC_URL_MAX];
static char user_name[SUBSONIC_USER_MAX];
static char password[SUBSONIC_PASSWORD_MAX];

// Per thread, like http_last_error(): more than one worker can have a request
// in flight and they must not overwrite each other's reason.
static __thread char last_error[256];

const char *subsonic_last_error(void) { return last_error; }

static void set_error(const char *text) {
	if (!text) {
		text = "";
	}
	snprintf(last_error, sizeof(last_error), "%s", text);
}

static void clear_error(void) { last_error[0] = '\0'; }

const char *subsonic_server(void) { return server_url; }
const char *subsonic_user(void) { return user_name; }

bool subsonic_configured(void) { return server_url[0] && user_name[0] && password[0]; }

// ---------------------------------------------------------------------------
// the token
// ---------------------------------------------------------------------------

// Subsonic hashes the password with a salt chosen by the client, and wants the
// salt in the clear next to it. A fresh salt per request means a token seen in
// one request says nothing about the next; it does not stop a replay of the
// same URL, which is why the parameters travel in a body and not in the path.
static bool make_salt(char *out, size_t size) {
	unsigned char raw[SUBSONIC_SALT_BYTES];
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		set_error("no /dev/urandom");
		return false;
	}
	size_t got = 0;
	while (got < sizeof(raw)) {
		ssize_t n = read(fd, raw + got, sizeof(raw) - got);
		if (n <= 0) {
			close(fd);
			set_error("no entropy");
			return false;
		}
		got += (size_t)n;
	}
	close(fd);

	if (size < sizeof(raw) * 2 + 1) {
		set_error("salt buffer too small");
		return false;
	}
	static const char hex[] = "0123456789abcdef";
	for (size_t i = 0; i < sizeof(raw); i++) {
		out[i * 2] = hex[raw[i] >> 4];
		out[i * 2 + 1] = hex[raw[i] & 0x0f];
	}
	out[sizeof(raw) * 2] = '\0';
	return true;
}

// The parameters every request carries: who, the token, the protocol, the
// client name, and JSON as the answer format.
static bool build_auth(char *out, size_t size) {
	char salt[SUBSONIC_SALT_BYTES * 2 + 1];
	if (!make_salt(salt, sizeof(salt))) {
		return false;
	}

	char combined[SUBSONIC_PASSWORD_MAX + sizeof(salt)];
	int n = snprintf(combined, sizeof(combined), "%s%s", password, salt);
	if (n <= 0 || (size_t)n >= sizeof(combined)) {
		set_error("password too long");
		return false;
	}

	char token[MD5_HEX_LEN];
	md5_hex(combined, (size_t)n, token);

	char encoded_user[SUBSONIC_USER_MAX * 3 + 1];
	http_url_encode(user_name, encoded_user, sizeof(encoded_user));

	n = snprintf(out, size, "u=%s&t=%s&s=%s&v=" SUBSONIC_API_VERSION "&c=" SUBSONIC_CLIENT_NAME "&f=json", encoded_user, token, salt);
	return n > 0 && (size_t)n < size;
}

// ---------------------------------------------------------------------------
// one API call
// ---------------------------------------------------------------------------

// Reads the envelope the API wraps every answer in and returns its status.
// False means the server said no, and the reason is in last_error.
static bool response_ok(const char *body) {
	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		set_error("the server did not answer with JSON");
		return false;
	}

	int root = json_get(&doc, json_root(&doc), "subsonic-response");
	char status[16] = "";
	json_obj_str(&doc, root, "status", status, sizeof(status));
	if (strcmp(status, "ok") == 0) {
		json_free(&doc);
		return true;
	}

	// Navidrome and the others put a readable message here. It is their text,
	// not a translation: showing it beats a generic failure.
	int err = json_get(&doc, root, "error");
	char message[160] = "";
	json_obj_str(&doc, err, "message", message, sizeof(message));
	if (message[0]) {
		set_error(message);
	} else {
		set_error("the server refused the request");
	}
	json_free(&doc);
	return false;
}

static bool api_call(const char *endpoint, const char *params, char **out, size_t *out_len) {
	clear_error();
	if (!subsonic_configured()) {
		set_error("no account");
		return false;
	}

	char url[SUBSONIC_URL_MAX + 64];
	int n = snprintf(url, sizeof(url), "%s/rest/%s", server_url, endpoint);
	if (n <= 0 || (size_t)n >= sizeof(url)) {
		set_error("server URL too long");
		return false;
	}

	char body[SUBSONIC_USER_MAX * 3 + SUBSONIC_PASSWORD_MAX + 160];
	if (!build_auth(body, sizeof(body))) {
		return false;
	}
	if (params && params[0]) {
		size_t used = strlen(body);
		n = snprintf(body + used, sizeof(body) - used, "&%s", params);
		if (n <= 0 || (size_t)n >= sizeof(body) - used) {
			set_error("request too long");
			return false;
		}
	}

	http_req_t req;
	memset(&req, 0, sizeof(req));
	req.method = "POST";
	req.body = body;
	req.want_error_body = true;

	int status = 0;
	if (!http_request(url, &req, out, out_len, SUBSONIC_JSON_LIMIT, SUBSONIC_TIMEOUT_SECS, &status)) {
		// http.c keeps its own reason per thread; lift it so the page has one
		// place to ask and gets something specific.
		const char *why = http_last_error();
		set_error(why && why[0] ? why : "the server did not answer");
		return false;
	}
	if (status == 401 || status == 403) {
		set_error("wrong user or password");
		return false;
	}
	if (status != 200) {
		char text[64];
		snprintf(text, sizeof(text), "the server answered %d", status);
		set_error(text);
		return false;
	}
	return response_ok(*out);
}

// ---------------------------------------------------------------------------
// account
// ---------------------------------------------------------------------------

// A server address is a base URL and nothing else: no path, no query, no
// trailing slash. A path here means "/rest/..." lands somewhere the server
// does not serve, which reads as a refusal and sends the reader looking at the
// password.
static bool normalize_server(const char *in, char *out, size_t size) {
	if (!in || !in[0]) {
		set_error("no server address");
		return false;
	}
	if (strncmp(in, "http://", 7) != 0 && strncmp(in, "https://", 8) != 0) {
		set_error("the address must start with http:// or https://");
		return false;
	}
	snprintf(out, size, "%s", in);
	size_t len = strlen(out);
	while (len > 0 && out[len - 1] == '/') {
		out[--len] = '\0';
	}
	if (len == 0) {
		set_error("no server address");
		return false;
	}
	if (strchr(out, ' ')) {
		set_error("the address has a space in it");
		return false;
	}
	// A query or a fragment means somebody pasted a URL from a browser.
	if (strchr(out, '?') || strchr(out, '#')) {
		set_error("use the server address only, without ? or #");
		return false;
	}
	return true;
}

void subsonic_init(void) {
	snprintf(server_url, sizeof(server_url), "%s", config_get("subsonic", "server", ""));
	snprintf(user_name, sizeof(user_name), "%s", config_get("subsonic", "user", ""));
	password[0] = '\0';
	if (server_url[0] && user_name[0]) {
		subsonicaccount_load(password, sizeof(password));
	}

	printf("subsonic: %s, %s\n", subsonic_configured() ? "configured" : "no account", server_url[0] ? server_url : "no server");
}

bool subsonic_set_account(const char *server, const char *user, const char *pass) {
	char normalized[SUBSONIC_URL_MAX];
	if (!normalize_server(server, normalized, sizeof(normalized))) {
		return false;
	}
	if (!user || !user[0]) {
		set_error("no user name");
		return false;
	}
	if (!pass || !pass[0]) {
		set_error("no password");
		return false;
	}

	// The account file is written first: if it fails, the configuration must
	// not be left pointing at credentials that are not there.
	if (!subsonicaccount_save(normalized, user, pass)) {
		set_error("could not save the password");
		return false;
	}

	config_set("subsonic", "server", normalized);
	config_set("subsonic", "user", user);
	config_save();

	snprintf(server_url, sizeof(server_url), "%s", normalized);
	snprintf(user_name, sizeof(user_name), "%s", user);
	snprintf(password, sizeof(password), "%s", pass);
	return true;
}

void subsonic_clear_account(void) {
	subsonicaccount_clear();
	config_set("subsonic", "server", "");
	config_set("subsonic", "user", "");
	config_save();
	server_url[0] = '\0';
	user_name[0] = '\0';
	password[0] = '\0';
}

// ---------------------------------------------------------------------------
// reads
// ---------------------------------------------------------------------------

bool subsonic_ping(void) {
	char *body = NULL;
	size_t len = 0;
	bool ok = api_call("ping.view", NULL, &body, &len);
	free(body);
	return ok;
}

// The server answers getArtists with an index of letters, each holding the
// artists that start with it. The player wants one flat list in the order the
// server chose, so the index is walked and not shown.
int subsonic_get_artists(subsonic_artist_t *out, int max) {
	if (!out || max <= 0) {
		return 0;
	}
	char *body = NULL;
	size_t len = 0;
	if (!api_call("getArtists.view", NULL, &body, &len)) {
		free(body);
		return 0;
	}

	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		set_error("the server did not answer with JSON");
		free(body);
		return 0;
	}

	int written = 0;
	int root = json_root(&doc);
	int indexes = json_path(&doc, root, "subsonic-response.artists.index");
	int index_count = json_len(&doc, indexes);
	for (int i = 0; i < index_count && written < max; i++) {
		int index = json_at(&doc, indexes, i);
		int artists = json_get(&doc, index, "artist");
		int count = json_len(&doc, artists);
		for (int j = 0; j < count && written < max; j++) {
			int item = json_at(&doc, artists, j);
			subsonic_artist_t *a = &out[written];
			memset(a, 0, sizeof(*a));
			json_obj_str(&doc, item, "id", a->id, sizeof(a->id));
			json_obj_str(&doc, item, "name", a->name, sizeof(a->name));
			a->album_count = (int)json_obj_long(&doc, item, "albumCount", 0);
			if (a->id[0] && a->name[0]) {
				written++;
			}
		}
	}

	json_free(&doc);
	free(body);
	clear_error();
	return written;
}

static void read_album(const json_doc_t *doc, int item, subsonic_album_t *a) {
	memset(a, 0, sizeof(*a));
	json_obj_str(doc, item, "id", a->id, sizeof(a->id));
	json_obj_str(doc, item, "name", a->name, sizeof(a->name));
	json_obj_str(doc, item, "artist", a->artist, sizeof(a->artist));
	json_obj_str(doc, item, "coverArt", a->cover_id, sizeof(a->cover_id));
	a->year = (int)json_obj_long(doc, item, "year", 0);
	a->song_count = (int)json_obj_long(doc, item, "songCount", 0);
}

int subsonic_get_albums(const char *artist_id, subsonic_album_t *out, int max) {
	if (!artist_id || !artist_id[0] || !out || max <= 0) {
		return 0;
	}

	char encoded[SUBSONIC_ID_MAX * 3 + 1];
	http_url_encode(artist_id, encoded, sizeof(encoded));
	char params[SUBSONIC_ID_MAX * 3 + 16];
	snprintf(params, sizeof(params), "id=%s", encoded);

	char *body = NULL;
	size_t len = 0;
	if (!api_call("getArtist.view", params, &body, &len)) {
		free(body);
		return 0;
	}

	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		set_error("the server did not answer with JSON");
		free(body);
		return 0;
	}

	int written = 0;
	int albums = json_path(&doc, json_root(&doc), "subsonic-response.artist.album");
	int count = json_len(&doc, albums);
	for (int i = 0; i < count && written < max; i++) {
		read_album(&doc, json_at(&doc, albums, i), &out[written]);
		if (out[written].id[0]) {
			written++;
		}
	}

	json_free(&doc);
	free(body);
	clear_error();
	return written;
}

int subsonic_get_songs(const char *album_id, subsonic_song_t *out, int max) {
	if (!album_id || !album_id[0] || !out || max <= 0) {
		return 0;
	}

	char encoded[SUBSONIC_ID_MAX * 3 + 1];
	http_url_encode(album_id, encoded, sizeof(encoded));
	char params[SUBSONIC_ID_MAX * 3 + 16];
	snprintf(params, sizeof(params), "id=%s", encoded);

	char *body = NULL;
	size_t len = 0;
	if (!api_call("getAlbum.view", params, &body, &len)) {
		free(body);
		return 0;
	}

	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		set_error("the server did not answer with JSON");
		free(body);
		return 0;
	}

	int written = 0;
	int songs = json_path(&doc, json_root(&doc), "subsonic-response.album.song");
	int count = json_len(&doc, songs);
	for (int i = 0; i < count && written < max; i++) {
		int item = json_at(&doc, songs, i);
		subsonic_song_t *s = &out[written];
		memset(s, 0, sizeof(*s));
		json_obj_str(&doc, item, "id", s->id, sizeof(s->id));
		json_obj_str(&doc, item, "title", s->title, sizeof(s->title));
		json_obj_str(&doc, item, "artist", s->artist, sizeof(s->artist));
		json_obj_str(&doc, item, "album", s->album, sizeof(s->album));
		json_obj_str(&doc, item, "suffix", s->suffix, sizeof(s->suffix));
		s->duration = (int)json_obj_long(&doc, item, "duration", 0);
		s->track = (int)json_obj_long(&doc, item, "track", 0);
		if (s->id[0]) {
			written++;
		}
	}

	json_free(&doc);
	free(body);
	clear_error();
	return written;
}

// ---------------------------------------------------------------------------
// bodies that are not JSON
// ---------------------------------------------------------------------------

// Cover art and songs come back as bytes, so the JSON path is useless here:
// the response is streamed straight into a file. The body carries the same
// parameters as every other request.
static bool fetch_to_file(const char *endpoint, const char *params, const char *out_path, unsigned long long *bytes_out) {
	clear_error();
	if (!subsonic_configured()) {
		set_error("no account");
		return false;
	}

	char url[SUBSONIC_URL_MAX + 64];
	int n = snprintf(url, sizeof(url), "%s/rest/%s", server_url, endpoint);
	if (n <= 0 || (size_t)n >= sizeof(url)) {
		set_error("server URL too long");
		return false;
	}

	char body[SUBSONIC_USER_MAX * 3 + SUBSONIC_PASSWORD_MAX + 160];
	if (!build_auth(body, sizeof(body))) {
		return false;
	}
	if (params && params[0]) {
		size_t used = strlen(body);
		n = snprintf(body + used, sizeof(body) - used, "&%s", params);
		if (n <= 0 || (size_t)n >= sizeof(body) - used) {
			set_error("request too long");
			return false;
		}
	}

	http_stream_t stream;
	if (!http_stream_open_post(&stream, url, body, SUBSONIC_TIMEOUT_SECS)) {
		const char *why = http_last_error();
		set_error(why && why[0] ? why : "the server did not answer");
		return false;
	}

	FILE *file = fopen(out_path, "wb");
	if (!file) {
		http_stream_close(&stream);
		set_error("could not write to the card");
		return false;
	}

	unsigned long long total = 0;
	char buffer[16 * 1024];
	bool ok = true;
	for (;;) {
		int got = http_stream_read(&stream, buffer, (int)sizeof(buffer));
		if (got == 0) {
			break;
		}
		if (got < 0) {
			set_error("the download was cut off");
			ok = false;
			break;
		}
		if (fwrite(buffer, 1, (size_t)got, file) != (size_t)got) {
			set_error("the card is full");
			ok = false;
			break;
		}
		total += (unsigned long long)got;
	}
	http_stream_close(&stream);

	if (fclose(file) != 0 && ok) {
		set_error("the card is full");
		ok = false;
	}
	if (!ok) {
		remove(out_path);
		return false;
	}
	if (bytes_out) {
		*bytes_out = total;
	}
	return true;
}

bool subsonic_get_cover(const char *cover_id, const char *out_path) {
	if (!cover_id || !cover_id[0] || !out_path) {
		return false;
	}
	char encoded[SUBSONIC_ID_MAX * 3 + 1];
	http_url_encode(cover_id, encoded, sizeof(encoded));

	// 512 is the largest size every server in this family honours; asking for
	// the original can hand back a scan of several megabytes.
	char params[SUBSONIC_ID_MAX * 3 + 32];
	snprintf(params, sizeof(params), "id=%s&size=512", encoded);
	return fetch_to_file("getCoverArt.view", params, out_path, NULL);
}

bool subsonic_download(const char *song_id, const char *out_path, unsigned long long *bytes_out) {
	if (!song_id || !song_id[0] || !out_path) {
		return false;
	}
	char encoded[SUBSONIC_ID_MAX * 3 + 1];
	http_url_encode(song_id, encoded, sizeof(encoded));

	char params[SUBSONIC_ID_MAX * 3 + 16];
	snprintf(params, sizeof(params), "id=%s", encoded);
	return fetch_to_file("download.view", params, out_path, bytes_out);
}

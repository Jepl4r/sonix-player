#include "qobuz.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/system/core/config.h"
#include "src/system/net/http.h"
#include "src/system/core/json.h"
#include "src/system/core/md5.h"
#include "src/system/streaming/streamkeys.h"
#include "src/system/core/lang.h"

// Base address. Configurable for one reason only: pointing it at a fake server
// exercises the whole flow without a subscription (see tools/fake_qobuz.py).
#define QOBUZ_BASE_DEFAULT "https://www.qobuz.com/api.json/0.2"

// How long a reply is waited for, and how much of one is held in memory. A
// hundred-track album stays under 400 KB; the cap keeps a runaway response from
// becoming a memory problem.
#define QOBUZ_TIMEOUT_SECS 15
#define QOBUZ_BODY_LIMIT (2 * 1024 * 1024)

static char base_url[256];
static char app_id[64];
static char app_secret[128];
static char auth_token[256];
static char display_name[QOBUZ_NAME_MAX];
static int format_id = QOBUZ_FORMAT_HIRES192;

// Per-thread like http_last_error(), and for the same reason: more than one
// worker can have a request in flight, and they must not overwrite each other's
// result.
static __thread char last_error[256];

const char *qobuz_last_error(void) { return last_error; }

const char *qobuz_expected_mime(void) { return format_id == QOBUZ_FORMAT_MP3 ? "audio/mpeg" : "audio/flac"; }

static void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void set_error(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vsnprintf(last_error, sizeof(last_error), fmt, args);
	va_end(args);
}

static void clear_error(void) { last_error[0] = '\0'; }

// ---------------------------------------------------------------------------
// session
// ---------------------------------------------------------------------------

void qobuz_init(void) {
	snprintf(base_url, sizeof(base_url), "%s", config_get("qobuz", "base_url", QOBUZ_BASE_DEFAULT));

	const char *id = streamkeys_qobuz_app_id();
	const char *secret = streamkeys_qobuz_app_secret();
	snprintf(app_id, sizeof(app_id), "%s", id ? id : "");
	snprintf(app_secret, sizeof(app_secret), "%s", secret ? secret : "");

	// The token only, never the password: when it expires it is asked for once,
	// and meanwhile nothing on disk is worth an account login.
	snprintf(auth_token, sizeof(auth_token), "%s", config_get("qobuz", "token", ""));
	snprintf(display_name, sizeof(display_name), "%s", config_get("qobuz", "user", ""));

	format_id = (int)config_get_int("qobuz", "format", QOBUZ_FORMAT_HIRES192);
	if (format_id != QOBUZ_FORMAT_MP3 && format_id != QOBUZ_FORMAT_CD && format_id != QOBUZ_FORMAT_HIRES96 &&
		format_id != QOBUZ_FORMAT_HIRES192) {
		format_id = QOBUZ_FORMAT_HIRES192;
	}

	printf("qobuz: %s, %s\n", qobuz_configured() ? "configured" : "no application keys",
		   auth_token[0] ? "saved session" : "no session");
}

bool qobuz_configured(void) { return app_id[0] && app_secret[0]; }
bool qobuz_logged_in(void) { return qobuz_configured() && auth_token[0]; }
const char *qobuz_display_name(void) { return display_name; }

int qobuz_get_format(void) { return format_id; }

void qobuz_set_format(int wanted) {
	if (wanted != QOBUZ_FORMAT_MP3 && wanted != QOBUZ_FORMAT_CD && wanted != QOBUZ_FORMAT_HIRES96 &&
		wanted != QOBUZ_FORMAT_HIRES192) {
		return;
	}
	format_id = wanted;
	config_set_int("qobuz", "format", format_id);
	config_save();
}

void qobuz_logout(void) {
	auth_token[0] = '\0';
	display_name[0] = '\0';
	config_set("qobuz", "token", "");
	config_set("qobuz", "user", "");
	config_save();
	printf("qobuz: session closed\n");
}

// ---------------------------------------------------------------------------
// making a request
// ---------------------------------------------------------------------------

// The common headers: X-App-Id always, the token when there is one. Qobuz takes
// the token in the query too, but in a header it stays out of the logs of the
// proxies along the way.
static void build_headers(char *out, size_t size, const char *extra) {
	int n = snprintf(out, size, "X-App-Id: %s\r\n", app_id);
	if (auth_token[0] && n > 0 && (size_t)n < size) {
		n += snprintf(out + n, size - (size_t)n, "X-User-Auth-Token: %s\r\n", auth_token);
	}
	if (extra && n > 0 && (size_t)n < size) {
		snprintf(out + n, size - (size_t)n, "%s", extra);
	}
}

// Qobuz reports an error as {"status":"error","message":"...","code":401}. The
// message is written for a human and beats anything invented here.
static bool note_api_error(const json_doc_t *doc, int status) {
	int root = json_root(doc);
	char message[200] = "";
	if (json_obj_str(doc, root, "message", message, sizeof(message)) && message[0]) {
		set_error("%s", message);
		return true;
	}
	set_error(tr("qobuz_answered_with"), status);
	return true;
}

// Makes the request, parses the reply, and fills last_error on failure. On
// success the caller must free *body only after it is done with *doc: the
// document points into the text instead of copying it.
static bool api_call(const char *url, const char *extra_headers, char **body, json_doc_t *doc) {
	clear_error();

	if (!qobuz_configured()) {
		set_error("%s", tr("qobuz_the_qobuz_app_keys_are_missing"));
		return false;
	}

	char headers[768];
	build_headers(headers, sizeof(headers), extra_headers);

	int status = 0;
	size_t len = 0;
	if (!http_get_ex(url, headers, true, body, &len, QOBUZ_BODY_LIMIT, QOBUZ_TIMEOUT_SECS, &status)) {
		const char *why = http_last_error();
		set_error("%s", why && why[0] ? why : tr("qobuz_no_reply"));
		return false;
	}

	if (!json_parse(*body, doc)) {
		free(*body);
		*body = NULL;
		set_error("%s", tr("qobuz_unreadable_qobuz_reply"));
		return false;
	}

	if (status != 200) {
		note_api_error(doc, status);
		json_free(doc);
		free(*body);
		*body = NULL;
		return false;
	}

	// A 200 carrying status:error happens: Qobuz uses it for some refusals.
	char state[32] = "";
	json_obj_str(doc, json_root(doc), "status", state, sizeof(state));
	if (strcmp(state, "error") == 0) {
		note_api_error(doc, status);
		json_free(doc);
		free(*body);
		*body = NULL;
		return false;
	}

	return true;
}

static void api_done(char *body, json_doc_t *doc) {
	json_free(doc);
	free(body);
}

// ---------------------------------------------------------------------------
// sign-in
// ---------------------------------------------------------------------------

bool qobuz_login(const char *username, const char *password) {
	clear_error();

	if (!qobuz_configured()) {
		set_error("%s", tr("qobuz_the_qobuz_app_keys_are_missing"));
		return false;
	}
	if (!username || !*username || !password || !*password) {
		set_error("%s", tr("credentials_required"));
		return false;
	}

	char url[512];
	snprintf(url, sizeof(url), "%s/user/login?app_id=%s", base_url, app_id);

	// Username and password go in the headers, not in the query: that is the
	// form the original binary uses, so it is the one known to work with these
	// app keys. Qobuz also accepts ?username=..&password=<md5>, which is where
	// to look if the form below ever stops being accepted.
	//
	// device_manufacturer_id carries the app_secret: that is what the original
	// sends, not a typo -- its code at 0x00784bd0 does exactly this.
	char extra[768];
	snprintf(extra, sizeof(extra), "username: %s\r\npassword: %s\r\ndevice_manufacturer_id: %s\r\n", username,
			 password, app_secret);

	// No token during login: an old one, if any, has nothing to do with it.
	char saved[sizeof(auth_token)];
	snprintf(saved, sizeof(saved), "%s", auth_token);
	auth_token[0] = '\0';

	char *body = NULL;
	json_doc_t doc;
	if (!api_call(url, extra, &body, &doc)) {
		snprintf(auth_token, sizeof(auth_token), "%s", saved);
		return false;
	}

	int root = json_root(&doc);
	char token[sizeof(auth_token)] = "";
	json_obj_str(&doc, root, "user_auth_token", token, sizeof(token));
	if (!token[0]) {
		api_done(body, &doc);
		snprintf(auth_token, sizeof(auth_token), "%s", saved);
		set_error("%s", tr("qobuz_gave_no_session_token"));
		return false;
	}

	int user = json_get(&doc, root, "user");
	char name[QOBUZ_NAME_MAX] = "";
	if (user >= 0) {
		json_obj_str(&doc, user, "display_name", name, sizeof(name));
		if (!name[0]) {
			json_obj_str(&doc, user, "login", name, sizeof(name));
		}
		if (!name[0]) {
			json_obj_str(&doc, user, "email", name, sizeof(name));
		}
	}
	if (!name[0]) {
		snprintf(name, sizeof(name), "%s", username);
	}

	api_done(body, &doc);

	snprintf(auth_token, sizeof(auth_token), "%s", token);
	snprintf(display_name, sizeof(display_name), "%s", name);
	config_set("qobuz", "token", auth_token);
	config_set("qobuz", "user", display_name);
	config_save();

	printf("qobuz: logged in as %s\n", display_name);
	return true;
}

// ---------------------------------------------------------------------------
// from JSON into the structs
// ---------------------------------------------------------------------------

// Cover art: Qobuz offers several sizes, and which one is taken matters.
//
// A Qobuz cover does not end up in a list row: it ends up in the player, which
// draws it 480 px across on this screen, so "small" (230 px) does not carry
// enough pixels and comes out blocky.
//
// So "large" (600 px) first: more than enough, and a quarter of the weight of
// "mega" (2400), which would be bandwidth spent on pixels nobody sees. The
// other sizes follow as fallbacks, since not every album carries all of them.
static void read_cover(const json_doc_t *doc, int image, char *out, size_t size) {
	out[0] = '\0';
	if (image < 0) {
		return;
	}
	static const char *const SIZES[] = {"large", "extralarge", "small", "thumbnail", "mega"};
	for (size_t i = 0; i < sizeof(SIZES) / sizeof(SIZES[0]); i++) {
		if (json_obj_str(doc, image, SIZES[i], out, size) && out[0]) {
			return;
		}
	}
}

static void read_album(const json_doc_t *doc, int obj, qobuz_album_t *a) {
	memset(a, 0, sizeof(*a));
	if (obj < 0) {
		return;
	}

	// An album id is alphanumeric ("0060254798879"), not a number: keeping it as
	// text preserves its leading zeros.
	json_obj_str(doc, obj, "id", a->id_text, sizeof(a->id_text));
	a->id = json_obj_long(doc, obj, "id", 0);
	json_obj_str(doc, obj, "title", a->title, sizeof(a->title));
	json_obj_str(doc, json_get(doc, obj, "artist"), "name", a->artist, sizeof(a->artist));
	read_cover(doc, json_get(doc, obj, "image"), a->cover, sizeof(a->cover));
	a->track_count = (int)json_obj_long(doc, obj, "tracks_count", 0);
	a->hires = json_obj_bool(doc, obj, "hires", false);

	// The date arrives either as "2019-05-24" or as seconds since the epoch;
	// only the year is kept, which is all a list row shows.
	char released[32] = "";
	if (json_obj_str(doc, obj, "release_date_original", released, sizeof(released)) && released[0]) {
		snprintf(a->released, sizeof(a->released), "%.4s", released);
	} else {
		long stamp = json_obj_long(doc, obj, "released_at", 0);
		if (stamp > 0) {
			time_t when = (time_t)stamp;
			struct tm tm_when;
			if (gmtime_r(&when, &tm_when)) {
				snprintf(a->released, sizeof(a->released), "%d", tm_when.tm_year + 1900);
			}
		}
	}
}

static void read_track(const json_doc_t *doc, int obj, const qobuz_album_t *album_hint, qobuz_track_t *t) {
	memset(t, 0, sizeof(*t));
	if (obj < 0) {
		return;
	}

	t->id = json_obj_long(doc, obj, "id", 0);
	json_obj_str(doc, obj, "title", t->title, sizeof(t->title));
	t->duration = (int)json_obj_long(doc, obj, "duration", 0);
	t->track_number = (int)json_obj_long(doc, obj, "track_number", 0);
	t->hires = json_obj_bool(doc, obj, "hires", false);
	t->streamable = json_obj_bool(doc, obj, "streamable", true);
	t->bit_depth = (int)json_obj_long(doc, obj, "maximum_bit_depth", 0);

	// The rate arrives in kHz with a fraction (44.1); it becomes hertz here, so
	// the rest of the player sees it the way it sees a file's.
	double khz = json_double(doc, json_get(doc, obj, "maximum_sampling_rate"), 0.0);
	t->sample_rate = (int)(khz * 1000.0 + 0.5);

	// The performer when there is one, otherwise the album artist: a track
	// search returns the first, a track inside an album the second.
	if (!json_obj_str(doc, json_get(doc, obj, "performer"), "name", t->artist, sizeof(t->artist)) || !t->artist[0]) {
		json_obj_str(doc, json_get(doc, obj, "artist"), "name", t->artist, sizeof(t->artist));
	}

	int album = json_get(doc, obj, "album");
	if (album >= 0) {
		json_obj_str(doc, album, "title", t->album, sizeof(t->album));
		// The album id, not just the title: it is what lets "Show album" reopen
		// the right one -- more than one album is called "Greatest Hits".
		json_obj_str(doc, album, "id", t->album_id, sizeof(t->album_id));
		read_cover(doc, json_get(doc, album, "image"), t->cover, sizeof(t->cover));
		if (!t->artist[0]) {
			json_obj_str(doc, json_get(doc, album, "artist"), "name", t->artist, sizeof(t->artist));
		}
	} else if (album_hint) {
		// Inside /album/get tracks do not repeat the album: it is the one asked
		// for.
		snprintf(t->album, sizeof(t->album), "%s", album_hint->title);
		snprintf(t->album_id, sizeof(t->album_id), "%s", album_hint->id_text);
		snprintf(t->cover, sizeof(t->cover), "%s", album_hint->cover);
		if (!t->artist[0]) {
			snprintf(t->artist, sizeof(t->artist), "%s", album_hint->artist);
		}
	}
}

static void read_artist(const json_doc_t *doc, int obj, qobuz_artist_t *a) {
	memset(a, 0, sizeof(*a));
	if (obj < 0) {
		return;
	}
	a->id = json_obj_long(doc, obj, "id", 0);
	json_obj_str(doc, obj, "name", a->name, sizeof(a->name));
	a->album_count = (int)json_obj_long(doc, obj, "albums_count", 0);
	read_cover(doc, json_get(doc, obj, "image"), a->image, sizeof(a->image));
}

static void read_playlist(const json_doc_t *doc, int obj, qobuz_playlist_t *p) {
	memset(p, 0, sizeof(*p));
	if (obj < 0) {
		return;
	}
	p->id = json_obj_long(doc, obj, "id", 0);
	json_obj_str(doc, obj, "name", p->name, sizeof(p->name));
	p->track_count = (int)json_obj_long(doc, obj, "tracks_count", 0);
	json_obj_str(doc, json_get(doc, obj, "owner"), "name", p->owner, sizeof(p->owner));

	// Playlists carry "images300"/"images150", arrays of URLs, instead of the
	// "image" object everything else uses.
	int images = json_get(doc, obj, "images300");
	if (images < 0) {
		images = json_get(doc, obj, "images150");
	}
	if (images >= 0) {
		json_str(doc, json_at(doc, images, 0), p->image, sizeof(p->image));
	}
}

typedef void (*reader_fn)(const json_doc_t *doc, int obj, void *out, int index);

// Walks `container.items`, calling `read` on each entry.
static int read_items(const json_doc_t *doc, int container, reader_fn read, void *out, int max) {
	int items = json_get(doc, container, "items");
	if (items < 0) {
		return 0;
	}
	int count = json_len(doc, items);
	if (count > max) {
		count = max;
	}
	for (int i = 0; i < count; i++) {
		read(doc, json_at(doc, items, i), out, i);
	}
	return count;
}

static void track_reader(const json_doc_t *doc, int obj, void *out, int index) {
	read_track(doc, obj, NULL, &((qobuz_track_t *)out)[index]);
}
static void album_reader(const json_doc_t *doc, int obj, void *out, int index) {
	read_album(doc, obj, &((qobuz_album_t *)out)[index]);
}
static void artist_reader(const json_doc_t *doc, int obj, void *out, int index) {
	read_artist(doc, obj, &((qobuz_artist_t *)out)[index]);
}
static void playlist_reader(const json_doc_t *doc, int obj, void *out, int index) {
	read_playlist(doc, obj, &((qobuz_playlist_t *)out)[index]);
}

// ---------------------------------------------------------------------------
// search
// ---------------------------------------------------------------------------

// Qobuz search is a single endpoint with the type in the path, and the reply
// keys its results under the plural of that type.
static int search(const char *kind, const char *bucket, const char *query, int offset, reader_fn read, void *out,
				  int max) {
	if (!query || !*query || max <= 0) {
		return 0;
	}

	char encoded[512];
	http_url_encode(query, encoded, sizeof(encoded));

	char url[1024];
	snprintf(url, sizeof(url), "%s/%s/search?app_id=%s&query=%s&limit=%d&offset=%d", base_url, kind, app_id, encoded,
			 max, offset);

	char *body = NULL;
	json_doc_t doc;
	if (!api_call(url, NULL, &body, &doc)) {
		return -1;
	}

	int count = read_items(&doc, json_get(&doc, json_root(&doc), bucket), read, out, max);
	api_done(body, &doc);
	return count;
}

int qobuz_search_tracks(const char *query, int offset, qobuz_track_t *out, int max) {
	return search("track", "tracks", query, offset, track_reader, out, max);
}

int qobuz_search_albums(const char *query, int offset, qobuz_album_t *out, int max) {
	return search("album", "albums", query, offset, album_reader, out, max);
}

int qobuz_search_artists(const char *query, int offset, qobuz_artist_t *out, int max) {
	return search("artist", "artists", query, offset, artist_reader, out, max);
}

// ---------------------------------------------------------------------------
// content
// ---------------------------------------------------------------------------

int qobuz_album_tracks(const char *album_id, int offset, qobuz_track_t *out, int max) {
	if (!album_id || !*album_id || max <= 0 || offset < 0) {
		return 0;
	}

	char encoded[64];
	http_url_encode(album_id, encoded, sizeof(encoded));

	char url[768];
	snprintf(url, sizeof(url), "%s/album/get?app_id=%s&album_id=%s", base_url, app_id, encoded);

	char *body = NULL;
	json_doc_t doc;
	if (!api_call(url, NULL, &body, &doc)) {
		return -1;
	}

	// Tracks inside an album do not repeat the album, so it is read once here
	// and handed to every track; otherwise the list rows come out with no cover
	// and no artist.
	int root = json_root(&doc);
	qobuz_album_t album;
	read_album(&doc, root, &album);

	// album/get sends every track in one reply, so the page is cut out here by
	// skipping the first `offset`; from the outside this endpoint then behaves
	// like the ones that really paginate.
	int items = json_path(&doc, root, "tracks.items");
	int count = json_len(&doc, items) - offset;
	if (count < 0) {
		count = 0;
	}
	if (count > max) {
		count = max;
	}
	for (int i = 0; i < count; i++) {
		read_track(&doc, json_at(&doc, items, offset + i), &album, &out[i]);
	}

	api_done(body, &doc);
	return count;
}

int qobuz_artist_albums(long artist_id, int offset, qobuz_album_t *out, int max) {
	if (max <= 0 || offset < 0) {
		return 0;
	}

	char url[768];
	// limit and offset on artist/get apply to the requested extra, that is to
	// the albums: this is how this endpoint paginates.
	snprintf(url, sizeof(url), "%s/artist/get?app_id=%s&artist_id=%ld&extra=albums&limit=%d&offset=%d", base_url,
			 app_id, artist_id, max, offset);

	char *body = NULL;
	json_doc_t doc;
	if (!api_call(url, NULL, &body, &doc)) {
		return -1;
	}

	int count = read_items(&doc, json_get(&doc, json_root(&doc), "albums"), album_reader, out, max);
	api_done(body, &doc);
	return count;
}

int qobuz_playlist_tracks(long playlist_id, int offset, qobuz_track_t *out, int max) {
	if (max <= 0 || offset < 0) {
		return 0;
	}

	char url[768];
	snprintf(url, sizeof(url), "%s/playlist/get?app_id=%s&playlist_id=%ld&extra=tracks&limit=%d&offset=%d", base_url,
			 app_id, playlist_id, max, offset);

	char *body = NULL;
	json_doc_t doc;
	if (!api_call(url, NULL, &body, &doc)) {
		return -1;
	}

	int count = read_items(&doc, json_get(&doc, json_root(&doc), "tracks"), track_reader, out, max);
	api_done(body, &doc);
	return count;
}

// Favourites use one endpoint, with the type as a parameter.
static int favorites(const char *type, const char *bucket, int offset, reader_fn read, void *out, int max) {
	if (max <= 0) {
		return 0;
	}
	if (!qobuz_logged_in()) {
		set_error("%s", tr("qobuz_you_are_not_signed_in_to_qobuz"));
		return -1;
	}

	char url[768];
	snprintf(url, sizeof(url), "%s/favorite/getUserFavorites?app_id=%s&type=%s&limit=%d&offset=%d", base_url, app_id,
			 type, max, offset);

	char *body = NULL;
	json_doc_t doc;
	if (!api_call(url, NULL, &body, &doc)) {
		return -1;
	}

	int count = read_items(&doc, json_get(&doc, json_root(&doc), bucket), read, out, max);
	api_done(body, &doc);
	return count;
}

int qobuz_favorite_tracks(int offset, qobuz_track_t *out, int max) {
	return favorites("tracks", "tracks", offset, track_reader, out, max);
}

int qobuz_favorite_albums(int offset, qobuz_album_t *out, int max) {
	return favorites("albums", "albums", offset, album_reader, out, max);
}

int qobuz_favorite_artists(int offset, qobuz_artist_t *out, int max) {
	return favorites("artists", "artists", offset, artist_reader, out, max);
}

int qobuz_user_playlists(int offset, qobuz_playlist_t *out, int max) {
	if (max <= 0) {
		return 0;
	}
	if (!qobuz_logged_in()) {
		set_error("%s", tr("qobuz_you_are_not_signed_in_to_qobuz"));
		return -1;
	}

	char url[768];
	snprintf(url, sizeof(url), "%s/playlist/getUserPlaylists?app_id=%s&limit=%d&offset=%d", base_url, app_id, max,
			 offset);

	char *body = NULL;
	json_doc_t doc;
	if (!api_call(url, NULL, &body, &doc)) {
		return -1;
	}

	int count = read_items(&doc, json_get(&doc, json_root(&doc), "playlists"), playlist_reader, out, max);
	api_done(body, &doc);
	return count;
}

int qobuz_featured_albums(const char *type, int offset, qobuz_album_t *out, int max) {
	if (max <= 0) {
		return 0;
	}

	char url[768];
	snprintf(url, sizeof(url), "%s/album/getFeatured?app_id=%s&type=%s&limit=%d&offset=%d", base_url, app_id,
			 type ? type : "new-releases", max, offset);

	char *body = NULL;
	json_doc_t doc;
	if (!api_call(url, NULL, &body, &doc)) {
		return -1;
	}

	int count = read_items(&doc, json_get(&doc, json_root(&doc), "albums"), album_reader, out, max);
	api_done(body, &doc);
	return count;
}

// ---------------------------------------------------------------------------
// the signature
//
// Qobuz signs requests like this:
//
//     request_sig = MD5( <object> <method>
//                        <key+value of every parameter, in alphabetical order>
//                        <request_ts> <app_secret> )
//
// with no separators of any kind. app_id, user_auth_token, request_ts (which
// goes on the end by itself) and request_sig are left out of the sum.
//
// The real rule is "the parameters the server receives": the server redoes the
// sum over what reached it, so signature and query must come from the same
// list. The original binary sends only format_id and track_id (template at
// 0x008bfe40 in its .rodata) and signs both; `intent` is sent here as well, as
// current clients do, and signed along with the others.
// ---------------------------------------------------------------------------

typedef struct {
	char key[24];
	char value[48];
} qobuz_param_t;

// Bubble sort by key: there are only two or three entries.
static void sort_params(qobuz_param_t *params, int count) {
	for (int i = 0; i < count; i++) {
		for (int k = i + 1; k < count; k++) {
			if (strcmp(params[k].key, params[i].key) < 0) {
				qobuz_param_t tmp = params[i];
				params[i] = params[k];
				params[k] = tmp;
			}
		}
	}
}

static void sign_request(const char *object, const char *method, qobuz_param_t *params, int count, long timestamp,
						 char out[MD5_HEX_LEN]) {
	sort_params(params, count);

	char to_sign[512];
	int n = snprintf(to_sign, sizeof(to_sign), "%s%s", object, method);
	for (int i = 0; i < count && n > 0 && (size_t)n < sizeof(to_sign); i++) {
		n += snprintf(to_sign + n, sizeof(to_sign) - (size_t)n, "%s%s", params[i].key, params[i].value);
	}
	if (n > 0 && (size_t)n < sizeof(to_sign)) {
		snprintf(to_sign + n, sizeof(to_sign) - (size_t)n, "%ld%s", timestamp, app_secret);
	}

	md5_hex(to_sign, strlen(to_sign), out);
}

// ---------------------------------------------------------------------------
// the file URL
//
// The only signed request. The signature is the MD5 of
//
//     "trackgetFileUrl" "format_id" <fmt> "intent" stream "track_id" <id>
//     <timestamp> <secret>
//
// with no separators. Sorted alphabetically, those three parameters land in the
// same order as the literal template the original uses (at 0x008bfe40 in its
// .rodata). The timestamp also goes in the query in the clear, or the server
// cannot redo the sum.
// ---------------------------------------------------------------------------

bool qobuz_track_url(long track_id, char *url_out, size_t url_size, char *mime, size_t mime_size, int *format_out) {
	clear_error();

	if (!url_out || url_size == 0) {
		return false;
	}
	url_out[0] = '\0';
	if (mime && mime_size) {
		mime[0] = '\0';
	}

	if (!qobuz_logged_in()) {
		set_error("%s", tr("qobuz_you_are_not_signed_in_to_qobuz"));
		return false;
	}

	long now = (long)time(NULL);

	// The signed parameters. Both the signature and the query are built from
	// this one list rather than written out twice: that is exactly how it was
	// broken before -- `intent=stream` reached the address but not the
	// signature, and Qobuz answered "Invalid Request Signature parameter
	// (request_sig)".
	qobuz_param_t params[3];
	int count = 0;
	snprintf(params[count].key, sizeof(params[0].key), "%s", "format_id");
	snprintf(params[count].value, sizeof(params[0].value), "%d", format_id);
	count++;
	snprintf(params[count].key, sizeof(params[0].key), "%s", "intent");
	snprintf(params[count].value, sizeof(params[0].value), "%s", "stream");
	count++;
	snprintf(params[count].key, sizeof(params[0].key), "%s", "track_id");
	snprintf(params[count].value, sizeof(params[0].value), "%ld", track_id);
	count++;

	char signature[MD5_HEX_LEN];
	sign_request("track", "getFileUrl", params, count, now, signature);

	char url[1024];
	int n = snprintf(url, sizeof(url), "%s/track/getFileUrl?app_id=%s&user_auth_token=%s&request_sig=%s&request_ts=%ld",
					 base_url, app_id, auth_token, signature, now);
	for (int i = 0; i < count && n > 0 && (size_t)n < sizeof(url); i++) {
		n += snprintf(url + n, sizeof(url) - (size_t)n, "&%s=%s", params[i].key, params[i].value);
	}

	char *body = NULL;
	json_doc_t doc;
	if (!api_call(url, NULL, &body, &doc)) {
		return false;
	}

	int root = json_root(&doc);
	json_obj_str(&doc, root, "url", url_out, url_size);
	if (mime && mime_size) {
		json_obj_str(&doc, root, "mime_type", mime, mime_size);
	}
	if (format_out) {
		*format_out = (int)json_obj_long(&doc, root, "format_id", format_id);
	}

	// A track that exists but is not covered by this subscription comes back as
	// a 200 with no url; saying so is far more useful than "it did not work".
	bool ok = url_out[0] != '\0';
	if (!ok) {
		bool streamable = json_obj_bool(&doc, root, "streamable", true);
		set_error("%s", streamable ? tr("qobuz_no_track_address")
									 : tr("stream_not_in_subscription"));
	}

	api_done(body, &doc);
	return ok;
}

// ---------------------------------------------------------------------------
// favourites and playlists: writing, not only reading
//
// The star in the player and "Add to playlist" have to reach Qobuz, not the
// local database: a Qobuz track is not one of the user's files, and storing it
// as a local favourite means saving the path of a cache file that will be gone
// twenty tracks later.
//
// The addresses are the ones the original binary uses, taken from its .rodata
// word for word:
//
//     %s/favorite/create?app_id=%s&%s=%s&user_auth_token=%s
//     %s/favorite/delete?app_id=%s&%s=%s&user_auth_token=%s
//     %s/playlist/create?app_id=%s&user_auth_token=%s
//     %s/playlist/addTracks?app_id=%s&playlist_id=%s&track_ids=%s&user_auth_token=%s
//
// The "%s=%s" in the favourite URLs is the type/list pair: for a track it
// becomes "track_ids=<id>". None of these requests is signed -- only
// track/getFileUrl wants a signature.
// ---------------------------------------------------------------------------

// `kind` is "track_ids" or "album_ids": the type/list pair that fills the
// "%s=%s" of the original's template.
static bool favorite_write(const char *action, const char *kind, const char *ids) {
	if (!qobuz_logged_in()) {
		set_error("%s", tr("qobuz_you_are_not_signed_in_to_qobuz"));
		return false;
	}
	if (!ids || !*ids) {
		return false;
	}

	char encoded[128];
	http_url_encode(ids, encoded, sizeof(encoded));

	char url[1024];
	snprintf(url, sizeof(url), "%.256s/favorite/%s?app_id=%.63s&%.16s=%.127s&user_auth_token=%.255s", base_url, action,
			 app_id, kind, encoded, auth_token);

	char *body = NULL;
	json_doc_t doc;
	if (!api_call(url, NULL, &body, &doc)) {
		return false;
	}
	api_done(body, &doc);
	return true;
}

bool qobuz_favorite_add_track(long track_id) {
	char ids[32];
	snprintf(ids, sizeof(ids), "%ld", track_id);
	return favorite_write("create", "track_ids", ids);
}

bool qobuz_favorite_remove_track(long track_id) {
	char ids[32];
	snprintf(ids, sizeof(ids), "%ld", track_id);
	return favorite_write("delete", "track_ids", ids);
}

// Album ids are alphanumeric ("0060254798879"), so one is passed as text:
// treating it as a number would lose its leading zeros.
bool qobuz_favorite_add_album(const char *album_id) { return favorite_write("create", "album_ids", album_id); }
bool qobuz_favorite_remove_album(const char *album_id) { return favorite_write("delete", "album_ids", album_id); }

bool qobuz_playlist_delete(long playlist_id) {
	if (!qobuz_logged_in()) {
		set_error("%s", tr("qobuz_you_are_not_signed_in_to_qobuz"));
		return false;
	}

	char url[1024];
	snprintf(url, sizeof(url), "%.256s/playlist/delete?app_id=%.63s&playlist_id=%ld&user_auth_token=%.255s", base_url,
			 app_id, playlist_id, auth_token);

	char *body = NULL;
	json_doc_t doc;
	if (!api_call(url, NULL, &body, &doc)) {
		return false;
	}
	api_done(body, &doc);
	return true;
}

int qobuz_favorite_track_ids(long *out, int max) {
	if (!out || max <= 0) {
		return -1;
	}
	if (!qobuz_logged_in()) {
		set_error("%s", tr("qobuz_you_are_not_signed_in_to_qobuz"));
		return -1;
	}

	// The favourites list, for the ids alone. The largest page the API grants in
	// one go is asked for: this only decides whether the star is filled or
	// empty, and doing it per track would mean a request on every song change.
	char url[1024];
	snprintf(url, sizeof(url),
			 "%.256s/favorite/getUserFavorites?app_id=%.63s&type=tracks&limit=%d&offset=0&user_auth_token=%.255s",
			 base_url, app_id, max, auth_token);

	char *body = NULL;
	json_doc_t doc;
	if (!api_call(url, NULL, &body, &doc)) {
		return -1;
	}

	int count = 0;
	int tracks = json_get(&doc, json_root(&doc), "tracks");
	int items = json_get(&doc, tracks, "items");
	int total = json_len(&doc, items);
	for (int i = 0; i < total && count < max; i++) {
		long id = json_obj_long(&doc, json_at(&doc, items, i), "id", 0);
		if (id > 0) {
			out[count++] = id;
		}
	}

	api_done(body, &doc);
	return count;
}

bool qobuz_playlist_create(const char *name, long *out_id) {
	if (!qobuz_logged_in()) {
		set_error("%s", tr("qobuz_you_are_not_signed_in_to_qobuz"));
		return false;
	}
	if (!name || !*name) {
		set_error("%s", tr("the_playlist_needs_a_name"));
		return false;
	}

	char encoded[256];
	http_url_encode(name, encoded, sizeof(encoded));

	char url[1024];
	snprintf(url, sizeof(url),
			 "%.256s/playlist/create?app_id=%.63s&user_auth_token=%.255s&name=%.255s"
			 "&is_public=false&is_collaborative=false",
			 base_url, app_id, auth_token, encoded);

	char *body = NULL;
	json_doc_t doc;
	if (!api_call(url, NULL, &body, &doc)) {
		return false;
	}

	long id = json_obj_long(&doc, json_root(&doc), "id", 0);
	api_done(body, &doc);

	if (id <= 0) {
		set_error("%s", tr("qobuz_did_not_name_the_new_playlist"));
		return false;
	}
	if (out_id) {
		*out_id = id;
	}
	return true;
}

bool qobuz_playlist_add_track(long playlist_id, long track_id) {
	if (!qobuz_logged_in()) {
		set_error("%s", tr("qobuz_you_are_not_signed_in_to_qobuz"));
		return false;
	}

	char url[1024];
	snprintf(url, sizeof(url),
			 "%.256s/playlist/addTracks?app_id=%.63s&playlist_id=%ld&track_ids=%ld&user_auth_token=%.255s", base_url,
			 app_id, playlist_id, track_id, auth_token);

	char *body = NULL;
	json_doc_t doc;
	if (!api_call(url, NULL, &body, &doc)) {
		return false;
	}
	api_done(body, &doc);
	return true;
}

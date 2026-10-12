#include "yandex.h"

#include <ctype.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/core/json.h"
#include "src/system/core/lang.h"
#include "src/system/core/sha256.h"
#include "src/system/net/http.h"
#include "src/system/streaming/streamkeys.h"

// See yandex.h for what this is and the thread contract.

#define YANDEX_API_DEFAULT "https://api.music.yandex.net"
#define YANDEX_AUTH_DEFAULT "https://oauth.yandex.ru"
#define YANDEX_TOKEN_MAX 512
#define YANDEX_BODY_LIMIT (2 * 1024 * 1024)
#define YANDEX_TIMEOUT_SECS 15
#define SESSION_FILE "yandex_session.ini"

// A Yandex token lives for a year. Renewing it a month early means a player
// left in a drawer for eleven months still comes back signed in, and one
// renewal a year is nothing to the rate limit.
#define REFRESH_MARGIN_SECS (30L * 24 * 3600)

// The codecs asked for, best first. FLAC on its own comes first: it plays
// while downloading with no unwrapping; flac-mp4 is unwrapped by the cache,
// AAC in MP4 plays through decode/mp4.c, MP3 is the floor every track has.
#define YANDEX_CODECS "flac,flac-mp4,aac-mp4,he-aac-mp4,mp3"

static char api_url[256];
static char auth_url[256];

static pthread_mutex_t session_lock = PTHREAD_MUTEX_INITIALIZER;
static char access_token[YANDEX_TOKEN_MAX];
static char refresh_token[YANDEX_TOKEN_MAX];
static long token_expires_at;
static char user_uid[32];
static char display_name[128];

static __thread char last_error[256];

// The rotor session: one at a time, and only the worker touches it.
static char wave_session[96];
static char wave_batch[96];

// The ids of the liked tracks as of the last listing, for the star.
#define LIKED_MAX 2000
static char (*liked_ids)[16];
static int liked_count;

static void set_error(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(last_error, sizeof(last_error), fmt, ap);
	va_end(ap);
}

const char *yandex_last_error(void) { return last_error; }

// ---------------------------------------------------------------------------
// keys
// ---------------------------------------------------------------------------

static const char *client_id(void) { return streamkeys_yandex_client_id(); }
static const char *client_secret(void) { return streamkeys_yandex_client_secret(); }

bool yandex_configured(void) { return client_id() && client_secret() && streamkeys_yandex_sign_key() && streamkeys_yandex_client_header(); }

// ---------------------------------------------------------------------------
// the session file, beside device_config.ini (see tidal.c for why not in it)
// ---------------------------------------------------------------------------

static void session_path(char *out, size_t size) {
	out[0] = '\0';
	const char *cfg = config_path();
	if (!cfg || !cfg[0]) {
		return;
	}
	const char *slash = strrchr(cfg, '/');
	if (slash) {
		snprintf(out, size, "%.*s/%s", (int)(slash - cfg), cfg, SESSION_FILE);
	} else {
		snprintf(out, size, "%s", SESSION_FILE);
	}
}

// Call with session_lock held.
static void save_session(void) {
	char path[600], tmp[640];
	session_path(path, sizeof(path));
	if (!path[0]) {
		return;
	}
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		return;
	}
	fprintf(f, "access_token=%s\nrefresh_token=%s\nexpires_at=%ld\nuid=%s\nuser=%s\n", access_token, refresh_token, token_expires_at, user_uid, display_name);
	if (fclose(f) != 0 || rename(tmp, path) != 0) {
		remove(tmp);
	}
}

static void load_session(void) {
	char path[600];
	session_path(path, sizeof(path));
	FILE *f = path[0] ? fopen(path, "r") : NULL;
	if (!f) {
		return;
	}
	char line[YANDEX_TOKEN_MAX + 64];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		char *eq = strchr(line, '=');
		if (!eq) {
			continue;
		}
		*eq = '\0';
		const char *v = eq + 1;
		if (strcmp(line, "access_token") == 0) {
			snprintf(access_token, sizeof(access_token), "%s", v);
		} else if (strcmp(line, "refresh_token") == 0) {
			snprintf(refresh_token, sizeof(refresh_token), "%s", v);
		} else if (strcmp(line, "expires_at") == 0) {
			token_expires_at = strtol(v, NULL, 10);
		} else if (strcmp(line, "uid") == 0) {
			snprintf(user_uid, sizeof(user_uid), "%s", v);
		} else if (strcmp(line, "user") == 0) {
			snprintf(display_name, sizeof(display_name), "%s", v);
		}
	}
	fclose(f);
}

void yandex_init(void) {
	snprintf(api_url, sizeof(api_url), "%s", config_get("yandex", "base_url", YANDEX_API_DEFAULT));
	snprintf(auth_url, sizeof(auth_url), "%s", config_get("yandex", "auth_url", YANDEX_AUTH_DEFAULT));
	pthread_mutex_lock(&session_lock);
	load_session();
	pthread_mutex_unlock(&session_lock);
	printf("yandex: %s, %s\n", yandex_configured() ? "keys present" : "no keys (tile hidden)", yandex_logged_in() ? "saved session" : "no session");
}

bool yandex_logged_in(void) { return access_token[0] != '\0'; }

const char *yandex_display_name(void) { return display_name; }

void yandex_logout(void) {
	pthread_mutex_lock(&session_lock);
	access_token[0] = refresh_token[0] = user_uid[0] = display_name[0] = '\0';
	token_expires_at = 0;
	wave_session[0] = wave_batch[0] = '\0';
	save_session();
	pthread_mutex_unlock(&session_lock);
	free(liked_ids);
	liked_ids = NULL;
	liked_count = 0;
}

// ---------------------------------------------------------------------------
// pacing
// ---------------------------------------------------------------------------
//
// Yandex answers 429, or a "smart captcha" HTML page, to clients that ask too
// fast, and keeps doing so for a while once it has decided. So requests are
// spaced at least 400 ms apart (two to three a second at most), and a refusal
// makes every request wait out a growing pause -- 15 s, then 60, then 120 --
// instead of hammering a door that is shut.

static pthread_mutex_t pace_lock = PTHREAD_MUTEX_INITIALIZER;
static struct timespec last_request;
static time_t blocked_until;
static int strikes;

static void throttle(void) {
	pthread_mutex_lock(&pace_lock);
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	long since_ms = (now.tv_sec - last_request.tv_sec) * 1000 + (now.tv_nsec - last_request.tv_nsec) / 1000000;
	if (last_request.tv_sec && since_ms < 400) {
		usleep((useconds_t)(400 - since_ms) * 1000);
	}
	clock_gettime(CLOCK_MONOTONIC, &last_request);
	pthread_mutex_unlock(&pace_lock);
}

// False while a back-off is running: the request is not even sent.
static bool pace_allows(void) {
	pthread_mutex_lock(&pace_lock);
	bool ok = time(NULL) >= blocked_until;
	pthread_mutex_unlock(&pace_lock);
	if (!ok) {
		set_error("%s", tr("yandex_too_many_requests"));
	}
	return ok;
}

static void pace_refused(void) {
	static const int PAUSES[] = {15, 60, 120};
	pthread_mutex_lock(&pace_lock);
	int pause = PAUSES[strikes < 2 ? strikes : 2];
	strikes++;
	blocked_until = time(NULL) + pause;
	pthread_mutex_unlock(&pace_lock);
	fprintf(stderr, "yandex: rate limited, waiting %d s\n", pause);
	set_error("%s", tr("yandex_too_many_requests"));
}

static void pace_ok(void) {
	pthread_mutex_lock(&pace_lock);
	strikes = 0;
	pthread_mutex_unlock(&pace_lock);
}

static bool is_refusal(int status, const char *body) { return status == 429 || (body && (strstr(body, "showcaptcha") || strstr(body, "SmartCaptcha"))); }

// ---------------------------------------------------------------------------
// OAuth
// ---------------------------------------------------------------------------

static bool auth_post(const char *path, const char *form, char **body, json_doc_t *doc, int *status) {
	if (!pace_allows()) {
		return false;
	}
	throttle();
	char url[400];
	snprintf(url, sizeof(url), "%s/%s", auth_url, path);
	http_req_t req = {.method = "POST", .body = form, .want_error_body = true, .allow_empty_body = true};
	if (!http_request(url, &req, body, NULL, 64 * 1024, YANDEX_TIMEOUT_SECS, status)) {
		const char *why = http_last_error();
		set_error("%s", why && why[0] ? why : tr("yandex_no_reply"));
		return false;
	}
	if (is_refusal(*status, *body)) {
		free(*body);
		*body = NULL;
		pace_refused();
		return false;
	}
	if (!json_parse(*body, doc)) {
		free(*body);
		*body = NULL;
		set_error("%s", tr("yandex_unreadable_reply"));
		return false;
	}
	return true;
}

// Takes access_token, refresh_token and expires_in from a token answer.
static bool take_tokens(const json_doc_t *doc) {
	int root = json_root(doc);
	char access[YANDEX_TOKEN_MAX] = "", refresh[YANDEX_TOKEN_MAX] = "";
	json_obj_str(doc, root, "access_token", access, sizeof(access));
	json_obj_str(doc, root, "refresh_token", refresh, sizeof(refresh));
	long expires = json_obj_long(doc, root, "expires_in", 0);
	if (!access[0]) {
		return false;
	}
	pthread_mutex_lock(&session_lock);
	snprintf(access_token, sizeof(access_token), "%s", access);
	if (refresh[0]) {
		snprintf(refresh_token, sizeof(refresh_token), "%s", refresh);
	}
	token_expires_at = expires > 0 ? (long)time(NULL) + expires : 0;
	save_session();
	pthread_mutex_unlock(&session_lock);
	return true;
}

static void random_device_id(char *out, size_t len) {
	static const char ALNUM[] = "abcdefghijklmnopqrstuvwxyz0123456789";
	srand((unsigned)time(NULL) ^ (unsigned)getpid());
	for (size_t i = 0; i + 1 < len; i++) {
		out[i] = ALNUM[rand() % (int)(sizeof(ALNUM) - 1)];
	}
	out[len - 1] = '\0';
}

bool yandex_login_begin(yandex_login_t *out) {
	last_error[0] = '\0';
	memset(out, 0, sizeof(*out));
	if (!yandex_configured()) {
		set_error("%s", tr("api_keys_unavailable"));
		return false;
	}
	char device_id[11];
	random_device_id(device_id, sizeof(device_id));
	char form[256];
	snprintf(form, sizeof(form), "client_id=%s&device_id=%s&device_name=Sonix%%20Player", client_id(), device_id);

	char *body = NULL;
	json_doc_t doc;
	int status = 0;
	if (!auth_post("device/code", form, &body, &doc, &status)) {
		return false;
	}
	int root = json_root(&doc);
	json_obj_str(&doc, root, "device_code", out->device_code, sizeof(out->device_code));
	json_obj_str(&doc, root, "user_code", out->user_code, sizeof(out->user_code));
	char where[200] = "";
	json_obj_str(&doc, root, "verification_url", where, sizeof(where));
	if (where[0] && strncmp(where, "http", 4) != 0) {
		snprintf(out->verification_url, sizeof(out->verification_url), "https://%s", where);
	} else {
		snprintf(out->verification_url, sizeof(out->verification_url), "%s", where[0] ? where : "https://ya.ru/device");
	}
	out->interval_secs = (int)json_obj_long(&doc, root, "interval", 5);
	out->expires_secs = (int)json_obj_long(&doc, root, "expires_in", 300);
	if (out->interval_secs < 1) {
		out->interval_secs = 5;
	}
	json_free(&doc);
	free(body);
	if (status != 200 || !out->device_code[0] || !out->user_code[0]) {
		set_error(tr("yandex_answered_with"), status);
		return false;
	}
	return true;
}

static bool fetch_account(void);
static void id_of(const json_doc_t *doc, int obj, const char *key, char *out, size_t size);

yandex_login_state_t yandex_login_poll(const yandex_login_t *login) {
	last_error[0] = '\0';
	char form[512];
	snprintf(form, sizeof(form), "grant_type=device_code&code=%s&client_id=%s&client_secret=%s", login->device_code, client_id(), client_secret());
	char *body = NULL;
	json_doc_t doc;
	int status = 0;
	if (!auth_post("token", form, &body, &doc, &status)) {
		return YANDEX_LOGIN_ERROR;
	}
	yandex_login_state_t result = YANDEX_LOGIN_ERROR;
	if (status == 200) {
		result = take_tokens(&doc) ? YANDEX_LOGIN_OK : YANDEX_LOGIN_ERROR;
	} else {
		char error[64] = "";
		json_obj_str(&doc, json_root(&doc), "error", error, sizeof(error));
		if (strcmp(error, "authorization_pending") == 0 || strcmp(error, "slow_down") == 0) {
			result = YANDEX_LOGIN_PENDING;
		} else if (strcmp(error, "expired_token") == 0) {
			result = YANDEX_LOGIN_EXPIRED;
		} else {
			set_error(tr("yandex_answered_with"), status);
		}
	}
	json_free(&doc);
	free(body);

	// The uid is part of every library address: without it signing in is not
	// done. A failure here signs out again rather than leave half a session.
	if (result == YANDEX_LOGIN_OK && !fetch_account()) {
		char why[256];
		snprintf(why, sizeof(why), "%s", last_error);
		yandex_logout();
		snprintf(last_error, sizeof(last_error), "%s", why);
		return YANDEX_LOGIN_ERROR;
	}
	return result;
}

static bool token_refresh(void) {
	if (!refresh_token[0]) {
		return false;
	}
	char form[YANDEX_TOKEN_MAX + 256];
	snprintf(form, sizeof(form), "grant_type=refresh_token&refresh_token=%s&client_id=%s&client_secret=%s", refresh_token, client_id(), client_secret());
	char *body = NULL;
	json_doc_t doc;
	int status = 0;
	if (!auth_post("token", form, &body, &doc, &status)) {
		return false;
	}
	bool ok = status == 200 && take_tokens(&doc);
	json_free(&doc);
	free(body);
	printf("yandex: token refresh %s\n", ok ? "done" : "refused");
	return ok;
}

// ---------------------------------------------------------------------------
// API calls
// ---------------------------------------------------------------------------

// `json_body` NULL = GET (or a form POST when `form` is set).
static bool api(const char *method, const char *url, const char *form, const char *json_body, char **body, json_doc_t *doc) {
	last_error[0] = '\0';
	*body = NULL;
	if (!yandex_configured()) {
		set_error("%s", tr("api_keys_unavailable"));
		return false;
	}
	if (!yandex_logged_in()) {
		set_error("%s", tr("yandex_not_signed_in"));
		return false;
	}
	if (token_expires_at > 0 && (long)time(NULL) + REFRESH_MARGIN_SECS > token_expires_at) {
		token_refresh(); // a failure here is not fatal: the token may still be good
	}
	if (!pace_allows()) {
		return false;
	}

	for (int attempt = 0; attempt < 2; attempt++) {
		char headers[YANDEX_TOKEN_MAX + 256];
		snprintf(headers, sizeof(headers), "Authorization: OAuth %s\r\n%s\r\n", access_token, streamkeys_yandex_client_header());
		http_req_t req = {
			.method = method,
			.extra_headers = headers,
			.body = json_body ? json_body : form,
			.content_type = json_body ? "application/json" : NULL,
			.want_error_body = true,
			.allow_empty_body = true,
		};
		throttle();
		int status = 0;
		if (!http_request(url, &req, body, NULL, YANDEX_BODY_LIMIT, YANDEX_TIMEOUT_SECS, &status)) {
			const char *why = http_last_error();
			set_error("%s", why && why[0] ? why : tr("yandex_no_reply"));
			return false;
		}
		if (is_refusal(status, *body)) {
			free(*body);
			*body = NULL;
			pace_refused();
			return false;
		}
		if (status == 401 && attempt == 0) {
			free(*body);
			*body = NULL;
			if (token_refresh()) {
				continue;
			}
			set_error("%s", tr("yandex_session_expired"));
			return false;
		}
		pace_ok();
		if (status < 200 || status >= 300) {
			fprintf(stderr, "yandex: %s %s -> %d\n", method ? method : "GET", url, status);
			set_error(tr("yandex_answered_with"), status);
			free(*body);
			*body = NULL;
			return false;
		}
		if (!json_parse(*body, doc)) {
			free(*body);
			*body = NULL;
			set_error("%s", tr("yandex_unreadable_reply"));
			return false;
		}
		return true;
	}
	return false;
}

static void api_done(char *body, json_doc_t *doc) {
	json_free(doc);
	free(body);
}

static bool fetch_account(void) {
	char url[400];
	snprintf(url, sizeof(url), "%s/account/status", api_url);
	char *body;
	json_doc_t doc;
	if (!api(NULL, url, NULL, NULL, &body, &doc)) {
		return false;
	}
	int account = json_path(&doc, json_root(&doc), "result.account");
	char uid[32] = "", name[128] = "";
	id_of(&doc, account, "uid", uid, sizeof(uid));
	if (!json_obj_str(&doc, account, "displayName", name, sizeof(name)) || !name[0]) {
		json_obj_str(&doc, account, "login", name, sizeof(name));
	}
	api_done(body, &doc);
	if (!uid[0]) {
		set_error("%s", tr("yandex_unreadable_reply"));
		return false;
	}
	pthread_mutex_lock(&session_lock);
	snprintf(user_uid, sizeof(user_uid), "%s", uid);
	snprintf(display_name, sizeof(display_name), "%s", name);
	save_session();
	pthread_mutex_unlock(&session_lock);
	printf("yandex: signed in as uid %s\n", uid);
	return true;
}

// ---------------------------------------------------------------------------
// JSON to structs
// ---------------------------------------------------------------------------

void yandex_cover_url(const char *uri, int px, char *out, size_t size) {
	out[0] = '\0';
	if (!uri || !uri[0]) {
		return;
	}
	char dim[24];
	snprintf(dim, sizeof(dim), "%dx%d", px, px);
	const char *mark = strstr(uri, "%%");
	const char *scheme = strncmp(uri, "http", 4) == 0 ? "" : "https://";
	if (mark) {
		snprintf(out, size, "%s%.*s%s%s", scheme, (int)(mark - uri), uri, dim, mark + 2);
	} else {
		snprintf(out, size, "%s%s", scheme, uri);
	}
}

// An id may be written as a number or as a string, depending on the endpoint.
static void id_any(const json_doc_t *doc, int tok, char *out, size_t size) {
	out[0] = '\0';
	if (tok < 0) {
		return;
	}
	if (doc->toks[tok].type == JSON_NUMBER) {
		snprintf(out, size, "%lld", json_llong(doc, tok, 0));
	} else {
		json_str(doc, tok, out, size);
	}
}

static void id_of(const json_doc_t *doc, int obj, const char *key, char *out, size_t size) { id_any(doc, obj >= 0 ? json_get(doc, obj, key) : -1, out, size); }

static void artists_of(const json_doc_t *doc, int obj, char *out, size_t size) {
	out[0] = '\0';
	int arr = json_get(doc, obj, "artists");
	int n = arr >= 0 ? json_len(doc, arr) : 0;
	size_t used = 0;
	for (int i = 0; i < n && i < 3; i++) {
		char name[96] = "";
		json_obj_str(doc, json_at(doc, arr, i), "name", name, sizeof(name));
		if (name[0]) {
			used += (size_t)snprintf(out + used, used < size ? size - used : 0, "%s%s", used ? ", " : "", name);
		}
	}
}

static void track_from(const json_doc_t *doc, int t, yandex_track_t *out) {
	memset(out, 0, sizeof(*out));
	id_of(doc, t, "id", out->id, sizeof(out->id));
	json_obj_str(doc, t, "title", out->title, sizeof(out->title));
	artists_of(doc, t, out->artist, sizeof(out->artist));
	int albums = json_get(doc, t, "albums");
	int album = albums >= 0 && json_len(doc, albums) > 0 ? json_at(doc, albums, 0) : -1;
	if (album >= 0) {
		id_of(doc, album, "id", out->album_id, sizeof(out->album_id));
		json_obj_str(doc, album, "title", out->album, sizeof(out->album));
		int pos = json_path(doc, album, "trackPosition.index");
		out->track_number = pos >= 0 ? (int)json_long(doc, pos, 0) : 0;
	}
	char uri[300] = "";
	if (!json_obj_str(doc, t, "coverUri", uri, sizeof(uri)) && album >= 0) {
		json_obj_str(doc, album, "coverUri", uri, sizeof(uri));
	}
	yandex_cover_url(uri, 400, out->cover, sizeof(out->cover));
	out->duration = (int)(json_obj_long(doc, t, "durationMs", 0) / 1000);
	out->available = json_obj_bool(doc, t, "available", true);
}

static void album_from(const json_doc_t *doc, int a, yandex_album_t *out) {
	memset(out, 0, sizeof(*out));
	id_of(doc, a, "id", out->id, sizeof(out->id));
	json_obj_str(doc, a, "title", out->title, sizeof(out->title));
	artists_of(doc, a, out->artist, sizeof(out->artist));
	char uri[300] = "";
	json_obj_str(doc, a, "coverUri", uri, sizeof(uri));
	yandex_cover_url(uri, 200, out->cover, sizeof(out->cover));
	out->year = (int)json_obj_long(doc, a, "year", 0);
	out->track_count = (int)json_obj_long(doc, a, "trackCount", 0);
}

static void artist_from(const json_doc_t *doc, int a, yandex_artist_t *out) {
	memset(out, 0, sizeof(*out));
	id_of(doc, a, "id", out->id, sizeof(out->id));
	json_obj_str(doc, a, "name", out->name, sizeof(out->name));
	char uri[300] = "";
	int cover = json_get(doc, a, "cover");
	if (cover >= 0) {
		json_obj_str(doc, cover, "uri", uri, sizeof(uri));
	}
	yandex_cover_url(uri, 200, out->cover, sizeof(out->cover));
}

// Collects the track objects of an array whose elements are either the track
// itself or a wrapper holding it under "track" (playlists, the rotor).
static int tracks_from_array(const json_doc_t *doc, int arr, yandex_track_t *out, int max) {
	int n = arr >= 0 ? json_len(doc, arr) : 0;
	int got = 0;
	for (int i = 0; i < n && got < max; i++) {
		int el = json_at(doc, arr, i);
		int inner = json_get(doc, el, "track");
		track_from(doc, inner >= 0 ? inner : el, &out[got]);
		if (out[got].id[0]) {
			got++;
		}
	}
	return got;
}

// ---------------------------------------------------------------------------
// library
// ---------------------------------------------------------------------------

// POST /tracks, in batches of 100, for lists that arrive as bare ids.
static int tracks_by_id(char (*ids)[16], int count, yandex_track_t *out, int max) {
	int got = 0;
	for (int from = 0; from < count && got < max; from += 100) {
		char *form = malloc(32 + 17 * 100);
		if (!form) {
			break;
		}
		size_t used = (size_t)sprintf(form, "track-ids=");
		for (int i = from; i < count && i < from + 100; i++) {
			used += (size_t)sprintf(form + used, "%s%s", i > from ? "%2C" : "", ids[i]);
		}
		char url[400];
		snprintf(url, sizeof(url), "%s/tracks", api_url);
		char *body;
		json_doc_t doc;
		bool ok = api("POST", url, form, NULL, &body, &doc);
		free(form);
		if (!ok) {
			return got ? got : -1;
		}
		got += tracks_from_array(&doc, json_get(&doc, json_root(&doc), "result"), out + got, max - got);
		api_done(body, &doc);
	}
	return got;
}

int yandex_liked_tracks(yandex_track_t *out, int max) {
	char url[400];
	snprintf(url, sizeof(url), "%s/users/%s/likes/tracks", api_url, user_uid);
	char *body;
	json_doc_t doc;
	if (!api(NULL, url, NULL, NULL, &body, &doc)) {
		return -1;
	}
	int arr = json_path(&doc, json_root(&doc), "result.library.tracks");
	int n = arr >= 0 ? json_len(&doc, arr) : 0;
	if (n > LIKED_MAX) {
		n = LIKED_MAX;
	}
	if (!liked_ids) {
		liked_ids = malloc(LIKED_MAX * sizeof(*liked_ids));
	}
	liked_count = 0;
	for (int i = 0; liked_ids && i < n; i++) {
		id_of(&doc, json_at(&doc, arr, i), "id", liked_ids[liked_count], sizeof(liked_ids[0]));
		if (liked_ids[liked_count][0]) {
			liked_count++;
		}
	}
	api_done(body, &doc);
	return liked_ids ? tracks_by_id(liked_ids, liked_count < max ? liked_count : max, out, max) : -1;
}

bool yandex_is_liked(const char *track_id) {
	for (int i = 0; liked_ids && i < liked_count; i++) {
		if (strcmp(liked_ids[i], track_id) == 0) {
			return true;
		}
	}
	return false;
}

int yandex_playlists(yandex_playlist_t *out, int max) {
	char url[400];
	snprintf(url, sizeof(url), "%s/users/%s/playlists/list", api_url, user_uid);
	char *body;
	json_doc_t doc;
	if (!api(NULL, url, NULL, NULL, &body, &doc)) {
		return -1;
	}
	int arr = json_get(&doc, json_root(&doc), "result");
	int n = arr >= 0 ? json_len(&doc, arr) : 0;
	int got = 0;
	for (int i = 0; i < n && got < max; i++) {
		int p = json_at(&doc, arr, i);
		yandex_playlist_t *pl = &out[got];
		memset(pl, 0, sizeof(*pl));
		id_of(&doc, p, "kind", pl->kind, sizeof(pl->kind));
		id_any(&doc, json_path(&doc, p, "owner.uid"), pl->owner, sizeof(pl->owner));
		if (!pl->owner[0]) {
			snprintf(pl->owner, sizeof(pl->owner), "%s", user_uid);
		}
		json_obj_str(&doc, p, "title", pl->title, sizeof(pl->title));
		pl->track_count = (int)json_obj_long(&doc, p, "trackCount", 0);
		char uri[300] = "";
		json_str(&doc, json_path(&doc, p, "cover.uri"), uri, sizeof(uri));
		yandex_cover_url(uri, 200, pl->cover, sizeof(pl->cover));
		if (pl->kind[0]) {
			got++;
		}
	}
	api_done(body, &doc);
	return got;
}

int yandex_playlist_tracks(const char *owner, const char *kind, yandex_track_t *out, int max) {
	char url[400];
	snprintf(url, sizeof(url), "%s/users/%s/playlists/%s", api_url, owner, kind);
	char *body;
	json_doc_t doc;
	if (!api(NULL, url, NULL, NULL, &body, &doc)) {
		return -1;
	}
	int got = tracks_from_array(&doc, json_path(&doc, json_root(&doc), "result.tracks"), out, max);
	api_done(body, &doc);
	return got;
}

int yandex_album_tracks(const char *album_id, yandex_track_t *out, int max) {
	char url[400];
	snprintf(url, sizeof(url), "%s/albums/%s/with-tracks", api_url, album_id);
	char *body;
	json_doc_t doc;
	if (!api(NULL, url, NULL, NULL, &body, &doc)) {
		return -1;
	}
	int volumes = json_path(&doc, json_root(&doc), "result.volumes");
	int nv = volumes >= 0 ? json_len(&doc, volumes) : 0;
	int got = 0;
	for (int v = 0; v < nv && got < max; v++) {
		got += tracks_from_array(&doc, json_at(&doc, volumes, v), out + got, max - got);
	}
	api_done(body, &doc);
	return got;
}

int yandex_artist_albums(const char *artist_id, yandex_album_t *out, int max) {
	char url[400];
	snprintf(url, sizeof(url), "%s/artists/%s/direct-albums?page=0&page-size=%d", api_url, artist_id, max);
	char *body;
	json_doc_t doc;
	if (!api(NULL, url, NULL, NULL, &body, &doc)) {
		return -1;
	}
	int arr = json_path(&doc, json_root(&doc), "result.albums");
	int n = arr >= 0 ? json_len(&doc, arr) : 0;
	int got = 0;
	for (int i = 0; i < n && got < max; i++) {
		album_from(&doc, json_at(&doc, arr, i), &out[got++]);
	}
	api_done(body, &doc);
	return got;
}

// One /search call with type=all: artists, albums and tracks in one answer,
// which is a third of the requests three typed searches would cost.
bool yandex_search(const char *text, yandex_artist_t *artists, int *n_artists, yandex_album_t *albums, int *n_albums, yandex_track_t *tracks, int *n_tracks) {
	char q[300], url[700];
	http_url_encode(text, q, sizeof(q));
	snprintf(url, sizeof(url), "%s/search?text=%s&type=all&page=0", api_url, q);
	char *body;
	json_doc_t doc;
	if (!api(NULL, url, NULL, NULL, &body, &doc)) {
		return false;
	}
	int root = json_root(&doc);
	int arr = json_path(&doc, root, "result.artists.results");
	int n = arr >= 0 ? json_len(&doc, arr) : 0, got = 0;
	for (int i = 0; i < n && got < *n_artists; i++) {
		artist_from(&doc, json_at(&doc, arr, i), &artists[got++]);
	}
	*n_artists = got;
	arr = json_path(&doc, root, "result.albums.results");
	n = arr >= 0 ? json_len(&doc, arr) : 0;
	got = 0;
	for (int i = 0; i < n && got < *n_albums; i++) {
		album_from(&doc, json_at(&doc, arr, i), &albums[got++]);
	}
	*n_albums = got;
	*n_tracks = tracks_from_array(&doc, json_path(&doc, root, "result.tracks.results"), tracks, *n_tracks);
	api_done(body, &doc);
	return true;
}

static bool likes_write(const char *what, const char *action, const char *track_id, const char *album_id) {
	char url[400], form[128];
	snprintf(url, sizeof(url), "%s/users/%s/%s/tracks/%s", api_url, user_uid, what, action);
	if (album_id && album_id[0]) {
		snprintf(form, sizeof(form), "track-ids=%s%%3A%s", track_id, album_id);
	} else {
		snprintf(form, sizeof(form), "track-ids=%s", track_id);
	}
	char *body;
	json_doc_t doc;
	if (!api("POST", url, form, NULL, &body, &doc)) {
		return false;
	}
	api_done(body, &doc);
	return true;
}

bool yandex_like_track(const char *track_id, const char *album_id, bool like) {
	bool ok = likes_write("likes", like ? "add-multiple" : "remove", track_id, album_id);
	if (ok && liked_ids) {
		if (like && !yandex_is_liked(track_id) && liked_count < LIKED_MAX) {
			snprintf(liked_ids[liked_count++], sizeof(liked_ids[0]), "%s", track_id);
		} else if (!like) {
			for (int i = 0; i < liked_count; i++) {
				if (strcmp(liked_ids[i], track_id) == 0) {
					memmove(liked_ids[i], liked_ids[i + 1], (size_t)(liked_count - i - 1) * sizeof(liked_ids[0]));
					liked_count--;
					break;
				}
			}
		}
	}
	if (ok && wave_session[0]) {
		yandex_wave_feedback(like ? YANDEX_WAVE_LIKE : YANDEX_WAVE_SKIP, track_id, album_id, 0);
	}
	return ok;
}

bool yandex_dislike_track(const char *track_id, const char *album_id) {
	bool ok = likes_write("dislikes", "add-multiple", track_id, album_id);
	if (ok && wave_session[0]) {
		yandex_wave_feedback(YANDEX_WAVE_DISLIKE, track_id, album_id, 0);
	}
	return ok;
}

// ---------------------------------------------------------------------------
// My Wave
// ---------------------------------------------------------------------------

static int wave_take(const json_doc_t *doc, yandex_track_t *out, int max) {
	int result = json_get(doc, json_root(doc), "result");
	char id[96] = "", batch[96] = "";
	json_obj_str(doc, result, "radioSessionId", id, sizeof(id));
	json_obj_str(doc, result, "batchId", batch, sizeof(batch));
	if (id[0]) {
		snprintf(wave_session, sizeof(wave_session), "%s", id);
	}
	if (batch[0]) {
		snprintf(wave_batch, sizeof(wave_batch), "%s", batch);
	}
	return tracks_from_array(doc, json_get(doc, result, "sequence"), out, max);
}

int yandex_wave_start(yandex_track_t *out, int max) {
	char url[400];
	snprintf(url, sizeof(url), "%s/rotor/session/new", api_url);
	char *body;
	json_doc_t doc;
	wave_session[0] = wave_batch[0] = '\0';
	if (!api("POST", url, NULL, "{\"seeds\":[\"user:onyourwave\"],\"includeTracksInResponse\":true}", &body, &doc)) {
		return -1;
	}
	int got = wave_take(&doc, out, max);
	api_done(body, &doc);
	if (!wave_session[0]) {
		set_error("%s", tr("yandex_unreadable_reply"));
		return -1;
	}
	yandex_wave_feedback(YANDEX_WAVE_STARTED, NULL, NULL, 0);
	return got;
}

int yandex_wave_more(const char *const *played, int played_count, yandex_track_t *out, int max) {
	if (!wave_session[0]) {
		return yandex_wave_start(out, max);
	}
	char url[400];
	snprintf(url, sizeof(url), "%s/rotor/session/%s/tracks", api_url, wave_session);
	char json[2048];
	size_t used = (size_t)snprintf(json, sizeof(json), "{\"queue\":[");
	for (int i = 0; i < played_count && used + 64 < sizeof(json); i++) {
		used += (size_t)snprintf(json + used, sizeof(json) - used, "%s\"%s\"", i ? "," : "", played[i]);
	}
	snprintf(json + used, sizeof(json) - used, "]}");
	char *body;
	json_doc_t doc;
	if (!api("POST", url, NULL, json, &body, &doc)) {
		return -1;
	}
	int got = wave_take(&doc, out, max);
	api_done(body, &doc);
	return got;
}

bool yandex_wave_active(void) { return wave_session[0] != '\0'; }

bool yandex_wave_feedback(yandex_wave_event_t event, const char *track_id, const char *album_id, int played_secs) {
	if (!wave_session[0]) {
		return false;
	}
	static const char *const NAMES[] = {"radioStarted", "trackStarted", "trackFinished", "skip", "like", "dislike"};
	char stamp[40];
	time_t now = time(NULL);
	strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
	char track[96] = "";
	if (track_id && track_id[0]) {
		snprintf(track, sizeof(track), ",\"trackId\":\"%s%s%s\"", track_id, album_id && album_id[0] ? ":" : "", album_id ? album_id : "");
	}
	char played[48] = "";
	if (event == YANDEX_WAVE_SKIP || event == YANDEX_WAVE_TRACK_FINISHED) {
		snprintf(played, sizeof(played), ",\"totalPlayedSeconds\":%d", played_secs);
	}
	char json[512];
	snprintf(json, sizeof(json), "{\"event\":{\"type\":\"%s\",\"timestamp\":\"%s\"%s%s%s},\"batchId\":\"%s\"}", NAMES[event], stamp, track, played, event == YANDEX_WAVE_STARTED ? ",\"from\":\"sonix\"" : "", wave_batch);
	char url[400];
	snprintf(url, sizeof(url), "%s/rotor/session/%s/feedback", api_url, wave_session);
	char *body;
	json_doc_t doc;
	if (!api("POST", url, NULL, json, &body, &doc)) {
		return false;
	}
	api_done(body, &doc);
	return true;
}

// ---------------------------------------------------------------------------
// the file
// ---------------------------------------------------------------------------

// HMAC-SHA256 (RFC 2104) on top of core/sha256.c, which only hands out hex.
static void sha256_raw(sha256_ctx *c, unsigned char out[32]) {
	char hex[SHA256_HEX_LEN];
	sha256_final_hex(c, hex);
	for (int i = 0; i < 32; i++) {
		unsigned v = 0;
		sscanf(hex + 2 * i, "%2x", &v);
		out[i] = (unsigned char)v;
	}
}

static void hmac_sha256(const char *key, const char *msg, unsigned char out[32]) {
	unsigned char k[64] = {0};
	size_t klen = strlen(key);
	if (klen > 64) {
		sha256_ctx c;
		sha256_init(&c);
		sha256_update(&c, key, klen);
		sha256_raw(&c, k);
	} else {
		memcpy(k, key, klen);
	}
	unsigned char ipad[64], opad[64], inner[32];
	for (int i = 0; i < 64; i++) {
		ipad[i] = k[i] ^ 0x36;
		opad[i] = k[i] ^ 0x5c;
	}
	sha256_ctx c;
	sha256_init(&c);
	sha256_update(&c, ipad, 64);
	sha256_update(&c, msg, strlen(msg));
	sha256_raw(&c, inner);
	sha256_init(&c);
	sha256_update(&c, opad, 64);
	sha256_update(&c, inner, 32);
	sha256_raw(&c, out);
}

static void base64_nopad(const unsigned char *in, size_t len, char *out) {
	static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t o = 0;
	for (size_t i = 0; i < len; i += 3) {
		unsigned v = (unsigned)in[i] << 16 | (i + 1 < len ? (unsigned)in[i + 1] << 8 : 0) | (i + 2 < len ? in[i + 2] : 0);
		out[o++] = T[(v >> 18) & 63];
		out[o++] = T[(v >> 12) & 63];
		if (i + 1 < len) {
			out[o++] = T[(v >> 6) & 63];
		}
		if (i + 2 < len) {
			out[o++] = T[v & 63];
		}
	}
	out[o] = '\0';
}

bool yandex_file_info(const char *track_id, yandex_file_t *out) {
	memset(out, 0, sizeof(*out));
	const char *quality = "lossless";
	const char *transport = "raw";

	// The signature: ts + trackId + quality + the codecs without their commas
	// + transport, HMAC-SHA256 with the application's key, base64 without the
	// trailing '='. The codec list is sent with its commas, URL-encoded.
	char codecs_flat[64];
	size_t n = 0;
	for (const char *p = YANDEX_CODECS; *p && n + 1 < sizeof(codecs_flat); p++) {
		if (*p != ',') {
			codecs_flat[n++] = *p;
		}
	}
	codecs_flat[n] = '\0';
	char ts[24];
	snprintf(ts, sizeof(ts), "%ld", (long)time(NULL));
	char message[256];
	snprintf(message, sizeof(message), "%s%s%s%s%s", ts, track_id, quality, codecs_flat, transport);
	unsigned char mac[32];
	hmac_sha256(streamkeys_yandex_sign_key(), message, mac);
	char sign[48], sign_enc[96];
	base64_nopad(mac, sizeof(mac), sign);
	http_url_encode(sign, sign_enc, sizeof(sign_enc));

	char url[700];
	snprintf(url, sizeof(url), "%s/get-file-info?ts=%s&trackId=%s&quality=%s&codecs=%s&transports=%s&sign=%s", api_url, ts, track_id, quality, "flac%2Cflac-mp4%2Caac-mp4%2Che-aac-mp4%2Cmp3", transport, sign_enc);
	char *body;
	json_doc_t doc;
	if (!api(NULL, url, NULL, NULL, &body, &doc)) {
		return false;
	}
	int info = json_path(&doc, json_root(&doc), "result.downloadInfo");
	json_obj_str(&doc, info, "codec", out->codec, sizeof(out->codec));
	int urls = json_get(&doc, info, "urls");
	if (urls >= 0 && json_len(&doc, urls) > 0) {
		json_str(&doc, json_at(&doc, urls, 0), out->url, sizeof(out->url));
	} else {
		json_obj_str(&doc, info, "url", out->url, sizeof(out->url));
	}
	json_obj_str(&doc, info, "key", out->key, sizeof(out->key));
	out->bitrate = (int)json_obj_long(&doc, info, "bitrate", 0);
	api_done(body, &doc);
	if (!out->url[0] || !out->codec[0]) {
		set_error("%s", tr("yandex_no_file"));
		return false;
	}
	printf("yandex: track %s as %s%s\n", track_id, out->codec, out->key[0] ? " (encrypted)" : "");
	return true;
}

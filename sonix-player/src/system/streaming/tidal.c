#include "tidal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/system/core/base64.h"
#include "src/system/core/config.h"
#include "src/system/net/http.h"
#include "src/system/core/json.h"
#include "src/system/streaming/streamkeys.h"
#include "src/system/core/lang.h"

// The scopes asked for at sign-in: r_usr to read, w_usr to write favourites
// and playlists. Not w_sub.
//
// w_sub (WRITE_SUBSCRIPTION) modifies the subscription, which this player never
// does. Tidal grants scopes per application key, and a key that lacks one does
// not hand back a subset -- it refuses the whole authorization and the login
// page never opens:
//
//     One or more of the requested scopes are not allowed.
//     Requested scopes: [WRITE_SUBSCRIPTION, WRITE_USR, READ_USR_DATA],
//     allowed scopes: [WRITE_USR, READ_USR_DATA]
//
// So an unneeded scope is one more thing that can be refused.
//
// Configurable because keys change and so do the scopes they grant: a new key
// that needs others takes one line in device_config.ini instead of a rebuild.
#define TIDAL_SCOPE_DEFAULT "r_usr w_usr"

// How large a cover is requested.
//
// Tidal serves progressive JPEG, and that decides everything. A baseline JPEG
// goes through the scaled decoder (TJpgDec), which yields an eighth or a
// quarter of the original without ever holding it whole, so the source size
// hardly matters. A progressive one must be decoded entirely, with the
// coefficient plane in RAM, and cover.c budgets it at eleven bytes per pixel
// against five for baseline:
//
//     640x640 = 409,600 px x 11 = 4400 KB
//     320x320 = 102,400 px x 11 = 1100 KB
//
// The decode ceiling depends on free memory at that moment and in practice
// sits around four megabytes, which 640 exceeds: the cover downloads and the
// decoder then refuses it, leaving a grey placeholder on screen.
//
// The sizes the CDN serves are 80, 160, 320, 640 and 1280, and no others -- any
// other number in the URL answers 403, so 480 (what the player actually draws)
// is not available.
//
// Of the two usable sizes, 320 clears the budget by nearly four times so it
// always passes, while 640 sits above the ceiling unless free memory is over
// about seventeen megabytes. The same 320 file serves the 60 px list thumbnail
// and the 480 px player view: one download, two uses, at the cost of some
// upscaling.
//
// Configurable with `[tidal] cover_size` in device_config.ini. 640 is a valid
// URL and downloads fine, but the decoder usually refuses it and the grey
// placeholder stays.
#define TIDAL_COVER_PX_DEFAULT 320

// Favourites per request: the limit Tidal's endpoints accept.
#define TIDAL_FAVORITES_PAGE 50

// The base endpoints. Configurable for the one reason they exist: point them
// at a fake server and exercise the whole flow without a subscription, the way
// Qobuz does (see tools/fake_qobuz.py).
#define TIDAL_API_DEFAULT "https://api.tidal.com/v1"
#define TIDAL_AUTH_DEFAULT "https://auth.tidal.com/v1/oauth2"

// Where the images live. Not an API endpoint but a static-file CDN, whose URLs
// are built from a UUID (see tidal_cover_url).
#define TIDAL_IMAGES "https://resources.tidal.com/images"

#define TIDAL_TIMEOUT_SECS 15
#define TIDAL_BODY_LIMIT (2 * 1024 * 1024)

// The manifest arrives inside a JSON field and is never large: BTS is a
// two-hundred-byte JSON, a DASH MPD a few kilobytes. The cap keeps a runaway
// response from becoming a memory problem.
#define TIDAL_MANIFEST_LIMIT (256 * 1024)

// How far ahead of expiry the token is renewed. It lasts a week, so a minute
// of margin is ample; it only avoids being caught mid-request with a token
// that expired an instant ago.
#define TOKEN_MARGIN_SECS 60

static char api_url[256];
static char auth_url[256];
static char client_id[64];
static char client_secret[128];

// Percent-encoded once, ready to drop into a form. One copy for both login
// requests: device_authorization and token must ask for the same scopes, and
// two hand-written copies meant one of them would eventually be changed alone.
static char scope_encoded[128];
static int cover_px = TIDAL_COVER_PX_DEFAULT;

// Tidal's tokens are JWTs: six hundred to twelve hundred characters each, not
// Qobuz's sixty. Two thousand leaves room to spare.
#define TIDAL_TOKEN_MAX 2048

static char access_token[TIDAL_TOKEN_MAX];
static char refresh_token[TIDAL_TOKEN_MAX];
static long token_expires_at; // seconds since the epoch, 0 = unknown
static char country_code[8];
static char user_id[32];
static char display_name[TIDAL_NAME_MAX];

static int quality = TIDAL_QUALITY_LOSSLESS;

// Per-thread like http_last_error(), for the same reason: more than one worker
// can have a request in flight and they must not overwrite each other's answer.
static __thread char last_error[256];

const char *tidal_last_error(void) { return last_error; }

static void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void set_error(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vsnprintf(last_error, sizeof(last_error), fmt, args);
	va_end(args);
}

static void clear_error(void) { last_error[0] = '\0'; }

// ---------------------------------------------------------------------------
// quality
// ---------------------------------------------------------------------------

const char *tidal_quality_name(int q) {
	switch (q) {
	case TIDAL_QUALITY_LOW:
		return "LOW";
	case TIDAL_QUALITY_HIGH:
		return "HIGH";
	case TIDAL_QUALITY_HIRES:
		return "HI_RES_LOSSLESS";
	default:
		return "LOSSLESS";
	}
}

int tidal_get_quality(void) { return quality; }

void tidal_set_quality(int wanted) {
	if (wanted < TIDAL_QUALITY_LOW || wanted > TIDAL_QUALITY_HIRES) {
		return;
	}
	quality = wanted;
	config_set_int("tidal", "quality", quality);
	config_save();
}

// A guess, not a promise. The Qobuz equivalent is near certain -- ask for MP3
// and MP3 arrives, ask for FLAC and FLAC arrives -- but here asking for hi-res
// can yield a DASH manifest whose container is MP4 even with FLAC inside, and
// asking for LOSSLESS can still yield DASH when the application keys in use are
// ones Tidal has moved entirely to DASH.
//
// It earns its place anyway: it lets a track's filename be guessed before the
// track is requested, which is what makes queueing a whole album while the
// first track downloads possible. If the real type turns out different,
// tidalcache writes it under that type and the extension-based lookup
// (tidalcache_find) still finds it.
const char *tidal_expected_mime(void) {
	return (quality == TIDAL_QUALITY_LOW || quality == TIDAL_QUALITY_HIGH) ? "audio/mp4" : "audio/flac";
}

// ---------------------------------------------------------------------------
// session
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// The session lives in its own file, not in device_config.ini
//
// config.c holds every value in a 128-byte field and reads the file in 256-byte
// lines: the right sizes for "brightness = 70" and for Qobuz's short token. A
// Tidal token is an eight-to-nine-hundred-character JWT, which such a field
// truncates on the way in -- and a truncated refresh token cannot repair
// itself, so the session is only good until the next reboot.
//
// Widening config.c's limits for one case would mean two hundred and fifty-six
// two-kilobyte entries: half a megabyte of fixed memory for one string that
// needs it. A separate file costs forty lines, keeps the secrets out of the
// file users open to change the brightness, and imposes nothing on anyone
// else.
//
// The format is the simplest that works: `key=value`, one per line, in write
// order. A human can read it back without tools.

#define SESSION_FILE "tidal_session.ini"

// Alongside device_config.ini: that is the writable partition, and already the
// place where things that survive a reboot live.
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

static void session_put(FILE *f, const char *key, const char *value) {
	fprintf(f, "%s=%s\n", key, value ? value : "");
}

static void save_session(void) {
	char path[600];
	session_path(path, sizeof(path));
	if (!path[0]) {
		return;
	}

	// Atomic write: a temporary file first, then rename(). Opening the real file
	// for writing truncates it before it is filled, and a battery dying at that
	// instant would leave an empty session in place of a good one -- a sign-in to
	// redo for an avoidable reason.
	char tmp[640];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);

	FILE *f = fopen(tmp, "w");
	if (!f) {
		fprintf(stderr, "tidal: cannot save the session to %s\n", tmp);
		return;
	}
	session_put(f, "access_token", access_token);
	session_put(f, "refresh_token", refresh_token);
	fprintf(f, "expires_at=%ld\n", token_expires_at);
	session_put(f, "country", country_code);
	session_put(f, "user_id", user_id);
	session_put(f, "user", display_name);
	bool ok = fclose(f) == 0;

	if (!ok || rename(tmp, path) != 0) {
		fprintf(stderr, "tidal: the session was not saved\n");
		remove(tmp);
	}
}

// Reads the file back. Lines are as long as they need to be, which is the
// whole point.
static void load_session(void) {
	char path[600];
	session_path(path, sizeof(path));
	if (!path[0]) {
		return;
	}

	FILE *f = fopen(path, "r");
	if (!f) {
		return; // not there yet: nobody has signed in
	}

	static char line[TIDAL_TOKEN_MAX + 128];
	while (fgets(line, sizeof(line), f)) {
		char *nl = strchr(line, '\n');
		if (nl) {
			*nl = '\0';
		}
		char *eq = strchr(line, '=');
		if (!eq) {
			continue;
		}
		*eq = '\0';
		const char *key = line;
		const char *value = eq + 1;

		if (strcmp(key, "access_token") == 0) {
			snprintf(access_token, sizeof(access_token), "%s", value);
		} else if (strcmp(key, "refresh_token") == 0) {
			snprintf(refresh_token, sizeof(refresh_token), "%s", value);
		} else if (strcmp(key, "expires_at") == 0) {
			token_expires_at = strtol(value, NULL, 10);
		} else if (strcmp(key, "country") == 0) {
			snprintf(country_code, sizeof(country_code), "%s", value);
		} else if (strcmp(key, "user_id") == 0) {
			snprintf(user_id, sizeof(user_id), "%s", value);
		} else if (strcmp(key, "user") == 0) {
			snprintf(display_name, sizeof(display_name), "%s", value);
		}
	}
	fclose(f);
}

void tidal_init(void) {
	snprintf(api_url, sizeof(api_url), "%s", config_get("tidal", "base_url", TIDAL_API_DEFAULT));
	snprintf(auth_url, sizeof(auth_url), "%s", config_get("tidal", "auth_url", TIDAL_AUTH_DEFAULT));

	// Spaces become %20 here, once. Writing them as '+' in the form would be
	// shorter and wrong: the '+' would itself be percent-encoded and end up
	// inside the scope name.
	http_url_encode(config_get("tidal", "scope", TIDAL_SCOPE_DEFAULT), scope_encoded, sizeof(scope_encoded));

	cover_px = (int)config_get_int("tidal", "cover_size", TIDAL_COVER_PX_DEFAULT);
	// Only the sizes the CDN really serves: any other is a 403 on every cover.
	// Anything else falls back to the value known to work.
	static const int VALID[] = {80, 160, 320, 640, 1280};
	bool ok = false;
	for (size_t i = 0; i < sizeof(VALID) / sizeof(VALID[0]); i++) {
		ok = ok || cover_px == VALID[i];
	}
	if (!ok) {
		printf("tidal: cover_size %d is not a size Tidal serves, using %d\n", cover_px,
			   TIDAL_COVER_PX_DEFAULT);
		cover_px = TIDAL_COVER_PX_DEFAULT;
	}

	const char *id = streamkeys_tidal_client_id();
	const char *secret = streamkeys_tidal_client_secret();
	snprintf(client_id, sizeof(client_id), "%s", id ? id : "");
	snprintf(client_secret, sizeof(client_secret), "%s", secret ? secret : "");

	load_session();

	// A session left in device_config.ini is truncated and worthless, so it is
	// cleared out rather than left to linger. Only the country and the name are
	// kept, since those fit in 128 bytes.
	if (config_get("tidal", "access_token", NULL)) {
		printf("tidal: found an old truncated session, dropping it\n");
		config_set("tidal", "access_token", "");
		config_set("tidal", "refresh_token", "");
		if (!country_code[0]) {
			snprintf(country_code, sizeof(country_code), "%s", config_get("tidal", "country", ""));
		}
		if (!user_id[0]) {
			snprintf(user_id, sizeof(user_id), "%s", config_get("tidal", "user_id", ""));
		}
		if (!display_name[0]) {
			snprintf(display_name, sizeof(display_name), "%s", config_get("tidal", "user", ""));
		}
		config_save();
	}

	quality = (int)config_get_int("tidal", "quality", TIDAL_QUALITY_LOSSLESS);
	if (quality < TIDAL_QUALITY_LOW || quality > TIDAL_QUALITY_HIRES) {
		quality = TIDAL_QUALITY_LOSSLESS;
	}

	printf("tidal: %s, %s\n", tidal_configured() ? "configured" : "no application keys",
		   tidal_logged_in() ? "saved session" : "no session");
}

bool tidal_configured(void) { return client_id[0] && client_secret[0]; }

// The refresh token, not the access token: an expired access token is normal,
// since it renews itself on the first request, so checking that one would
// report a perfectly valid account as signed out.
bool tidal_logged_in(void) { return tidal_configured() && refresh_token[0]; }

const char *tidal_display_name(void) { return display_name; }

void tidal_logout(void) {
	access_token[0] = '\0';
	refresh_token[0] = '\0';
	token_expires_at = 0;
	country_code[0] = '\0';
	user_id[0] = '\0';
	display_name[0] = '\0';
	save_session();
	printf("tidal: session closed\n");
}

// ---------------------------------------------------------------------------
// how a request is made
// ---------------------------------------------------------------------------

// The country. Tidal wants it on every catalogue call, and it is not a
// formality: the catalogue really does differ by country, and without this
// parameter some responses come back empty rather than failing -- the worst way
// to fail, because it reads as "no results".
static const char *country(void) { return country_code[0] ? country_code : "US"; }

// The client version this player claims to be.
//
// Not documented as mandatory, and currently is not. Sent anyway because it is
// the header Tidal uses to tell requests apart, maintained clients keep it
// current, and a very old or missing value is the first suspect the day certain
// calls start refusing without explanation. It costs forty bytes.
#define TIDAL_CLIENT_VERSION "2025.7.16"

static void build_headers(char *out, size_t size) {
	// The token type is always "Bearer": Tidal returns it in the sign-in
	// response and it has never been anything else, so it is written literally.
	snprintf(out, size, "Authorization: Bearer %s\r\nx-tidal-client-version: " TIDAL_CLIENT_VERSION "\r\n",
			 access_token);
}

// Tidal reports errors as {"status":401,"subStatus":11003,
// "userMessage":"..."}. userMessage is written for a human and is far better
// than anything that could be invented here.
static void note_api_error(const json_doc_t *doc, int status) {
	int root = json_root(doc);
	char message[200] = "";
	if (json_obj_str(doc, root, "userMessage", message, sizeof(message)) && message[0]) {
		set_error("%s", message);
		return;
	}
	if (json_obj_str(doc, root, "error_description", message, sizeof(message)) && message[0]) {
		set_error("%s", message);
		return;
	}
	if (json_obj_str(doc, root, "error", message, sizeof(message)) && message[0]) {
		set_error("%s", message);
		return;
	}
	set_error(tr("tidal_answered_with"), status);
}

// A form POST to the auth endpoint, returning the parsed body. Requesting a
// code, polling for authorization and refreshing the token all go through here,
// because all three must behave the same way when something fails.
static bool auth_post(const char *path, const char *form, char **body, json_doc_t *doc, int *status_out) {
	char url[512];
	snprintf(url, sizeof(url), "%s/%s", auth_url, path);

	http_req_t req = {
		.method = "POST",
		.body = form,
		// All three endpoints expect a form body, which is also http_request's
		// default -- spelled out because here it is a choice, not an oversight.
		.content_type = "application/x-www-form-urlencoded",
		// "Not authorized yet" arrives as a 400 with the reason in the body:
		// without this, waiting for sign-in would be indistinguishable from a
		// real error and polling would stop after the first attempt.
		.want_error_body = true,
	};

	int status = 0;
	size_t len = 0;
	if (!http_request(url, &req, body, &len, TIDAL_BODY_LIMIT, TIDAL_TIMEOUT_SECS, &status)) {
		const char *why = http_last_error();
		set_error("%s", why && why[0] ? why : tr("tidal_no_reply"));
		return false;
	}
	if (status_out) {
		*status_out = status;
	}

	if (!json_parse(*body, doc)) {
		free(*body);
		*body = NULL;
		set_error("%s", tr("tidal_unreadable_tidal_reply"));
		return false;
	}
	return true;
}

static void api_done(char *body, json_doc_t *doc) {
	json_free(doc);
	free(body);
}

// Renews the access token from the refresh token. True when a good one is now
// in hand.
//
// The response does not carry a new refresh token: that one stays until sign
// out, and must not be overwritten with the empty string the response has in
// its place.
static bool token_refresh(void) {
	if (!refresh_token[0] || !tidal_configured()) {
		return false;
	}

	// The form must be as wide as the token: at 1024 bytes the refresh token --
	// a JWT -- was truncated into it and the refresh always failed, breaking the
	// one path that has to work by itself so nobody signs in again every week.
	char form[TIDAL_TOKEN_MAX + 512];
	snprintf(form, sizeof(form), "grant_type=refresh_token&refresh_token=%s&client_id=%s&client_secret=%s",
			 refresh_token, client_id, client_secret);

	char *body = NULL;
	json_doc_t doc;
	int status = 0;
	if (!auth_post("token", form, &body, &doc, &status)) {
		return false;
	}

	int root = json_root(&doc);
	char token[sizeof(access_token)] = "";
	json_obj_str(&doc, root, "access_token", token, sizeof(token));
	long expires_in = json_obj_long(&doc, root, "expires_in", 0);

	if (status != 200 || !token[0]) {
		note_api_error(&doc, status);
		api_done(body, &doc);

		// A rejected refresh is not a network error: the session is no longer
		// valid (password changed, access revoked from the phone, application
		// keys replaced). Keeping a dead refresh token means retrying on every
		// request forever; better to say plainly that signing in again is needed.
		if (status >= 400 && status < 500) {
			printf("tidal: the refresh was refused, session closed\n");
			tidal_logout();
		}
		return false;
	}

	snprintf(access_token, sizeof(access_token), "%s", token);
	token_expires_at = expires_in > 0 ? (long)time(NULL) + expires_in : 0;

	// The country can change (people travel) and arrives with the token.
	int user = json_get(&doc, root, "user");
	if (user >= 0) {
		char cc[sizeof(country_code)] = "";
		if (json_obj_str(&doc, user, "countryCode", cc, sizeof(cc)) && cc[0]) {
			snprintf(country_code, sizeof(country_code), "%s", cc);
		}
	}

	api_done(body, &doc);
	save_session();
	printf("tidal: token refreshed\n");
	return true;
}

// True when the access token is still good to use right now.
static bool token_is_fresh(void) {
	if (!access_token[0]) {
		return false;
	}
	if (token_expires_at <= 0) {
		return true; // expiry unknown: try it, and a 401 will sort it out
	}
	return (long)time(NULL) + TOKEN_MARGIN_SECS < token_expires_at;
}

// Makes the request, parses the response, and fills last_error on failure. On
// success the caller must free *body after it is done with *doc: the document
// points into the text rather than copying it.
//
// Token renewal lives here and not in the callers, which is the only workable
// place: twenty functions must not each remember to check expiry, and the case
// that matters is not "the token expired by the local clock" but "Tidal says
// no" -- this device's clock can be anything after a flat battery.
static bool api_call(const char *url, char **body, json_doc_t *doc) {
	clear_error();

	if (!tidal_configured()) {
		set_error("%s", tr("tidal_keys_missing"));
		return false;
	}
	if (!tidal_logged_in()) {
		set_error("%s", tr("tidal_not_signed_in"));
		return false;
	}

	if (!token_is_fresh() && !token_refresh()) {
		if (!last_error[0]) {
			set_error("%s", tr("tidal_session_renew_failed"));
		}
		return false;
	}

	for (int attempt = 0; attempt < 2; attempt++) {
		char headers[TIDAL_TOKEN_MAX + 128];
		build_headers(headers, sizeof(headers));

		// allow_empty_body is not about legitimate empty bodies: a 401 with an
		// empty body -- which Tidal does send -- otherwise left here as "no
		// response" and never reached the token refresh below. With a session
		// whose expiry is unrecorded (token_is_fresh() says yes when it does not
		// know) that path never recovered: every request failed with what looked
		// like a network error, forever, until the account was signed out by hand.
		http_req_t req = {
			.extra_headers = headers,
			.want_error_body = true,
			.allow_empty_body = true,
		};

		int status = 0;
		size_t len = 0;
		if (!http_request(url, &req, body, &len, TIDAL_BODY_LIMIT, TIDAL_TIMEOUT_SECS, &status)) {
			const char *why = http_last_error();
			set_error("%s", why && why[0] ? why : tr("tidal_no_reply"));
			return false;
		}

		// A 401 on the first pass means the token died before it said it would.
		// Renew once and retry: retrying twice would be a loop, and a loop on a
		// rejected token is one request per millisecond until someone pulls the
		// plug.
		if (status == 401 && attempt == 0) {
			free(*body);
			*body = NULL;
			if (token_refresh()) {
				continue;
			}
			if (!last_error[0]) {
				set_error("%s", tr("tidal_session_expired"));
			}
			return false;
		}

		if (!json_parse(*body, doc)) {
			free(*body);
			*body = NULL;
			set_error("%s", tr("tidal_unreadable_tidal_reply"));
			return false;
		}

		if (status != 200) {
			note_api_error(doc, status);
			json_free(doc);
			free(*body);
			*body = NULL;
			return false;
		}
		return true;
	}
	return false;
}

// A write: POST or DELETE with a form body, where success is read from the
// status code rather than the content (a successful DELETE answers 204 with no
// body).
static bool api_write(const char *method, const char *url, const char *form) {
	clear_error();

	if (!tidal_logged_in()) {
		set_error("%s", tr("tidal_not_signed_in"));
		return false;
	}
	if (!token_is_fresh() && !token_refresh()) {
		if (!last_error[0]) {
			set_error("%s", tr("tidal_session_renew_failed"));
		}
		return false;
	}

	for (int attempt = 0; attempt < 2; attempt++) {
		char headers[TIDAL_TOKEN_MAX + 128];
		build_headers(headers, sizeof(headers));

		http_req_t req = {
			.method = method,
			.extra_headers = headers,
			.body = form,
			.want_error_body = true,
			.allow_empty_body = true,
		};

		char *body = NULL;
		int status = 0;
		if (!http_request(url, &req, &body, NULL, TIDAL_BODY_LIMIT, TIDAL_TIMEOUT_SECS, &status)) {
			const char *why = http_last_error();
			set_error("%s", why && why[0] ? why : tr("tidal_no_reply"));
			return false;
		}

		if (status == 401 && attempt == 0) {
			free(body);
			if (token_refresh()) {
				continue;
			}
			set_error("%s", tr("tidal_session_expired"));
			return false;
		}

		bool ok = status >= 200 && status < 300;
		if (!ok) {
			json_doc_t doc;
			if (body && body[0] && json_parse(body, &doc)) {
				note_api_error(&doc, status);
				json_free(&doc);
			} else {
				set_error(tr("tidal_answered_with"), status);
			}
			// Also to the log, with method and URL: the toast is read by whoever
			// is there at the time, the log by whoever has to work out why a star
			// does not fill in.
			fprintf(stderr, "tidal: %s %s -> %d: %s\n", method, url, status, last_error);
		}
		free(body);
		return ok;
	}
	return false;
}

// ---------------------------------------------------------------------------
// device-code sign-in
// ---------------------------------------------------------------------------

bool tidal_login_begin(tidal_login_t *out) {
	clear_error();
	if (!out) {
		return false;
	}
	memset(out, 0, sizeof(*out));

	if (!tidal_configured()) {
		set_error("%s", tr("tidal_keys_missing"));
		return false;
	}

	char form[512];
	snprintf(form, sizeof(form), "client_id=%s&scope=%s", client_id, scope_encoded);

	char *body = NULL;
	json_doc_t doc;
	int status = 0;
	if (!auth_post("device_authorization", form, &body, &doc, &status)) {
		return false;
	}

	if (status != 200) {
		note_api_error(&doc, status);
		api_done(body, &doc);
		return false;
	}

	int root = json_root(&doc);
	json_obj_str(&doc, root, "deviceCode", out->device_code, sizeof(out->device_code));
	json_obj_str(&doc, root, "userCode", out->user_code, sizeof(out->user_code));

	// The URL arrives without a scheme ("link.tidal.com"). Showing it as-is is
	// fine on a sign, but this string also goes into a QR code or gets read
	// aloud, and a leading "https://" removes any doubt.
	char where[200] = "";
	if (!json_obj_str(&doc, root, "verificationUriComplete", where, sizeof(where)) || !where[0]) {
		json_obj_str(&doc, root, "verificationUri", where, sizeof(where));
	}
	if (where[0]) {
		if (strncmp(where, "http://", 7) == 0 || strncmp(where, "https://", 8) == 0) {
			snprintf(out->verification_url, sizeof(out->verification_url), "%s", where);
		} else {
			snprintf(out->verification_url, sizeof(out->verification_url), "https://%s", where);
		}
	}

	out->interval_secs = (int)json_obj_long(&doc, root, "interval", 2);
	out->expires_secs = (int)json_obj_long(&doc, root, "expiresIn", 300);
	if (out->interval_secs < 1) {
		out->interval_secs = 2;
	}

	api_done(body, &doc);

	if (!out->device_code[0] || !out->user_code[0]) {
		set_error("%s", tr("tidal_gave_no_login_code"));
		return false;
	}
	return true;
}

tidal_login_state_t tidal_login_poll(const tidal_login_t *login) {
	clear_error();
	if (!login || !login->device_code[0]) {
		return TIDAL_LOGIN_ERROR;
	}

	char form[1024];
	// The same scopes asked of device_authorization, from the same variable: if
	// the two lists differ Tidal refuses the exchange.
	snprintf(form, sizeof(form),
			 "client_id=%s&client_secret=%s&device_code=%s"
			 "&grant_type=urn:ietf:params:oauth:grant-type:device_code&scope=%s",
			 client_id, client_secret, login->device_code, scope_encoded);

	char *body = NULL;
	json_doc_t doc;
	int status = 0;
	if (!auth_post("token", form, &body, &doc, &status)) {
		return TIDAL_LOGIN_ERROR;
	}

	int root = json_root(&doc);

	if (status != 200) {
		char error[64] = "";
		json_obj_str(&doc, root, "error", error, sizeof(error));
		long sub = json_obj_long(&doc, root, "sub_status", 0);

		// "The code has not been entered yet" is the normal answer for the whole
		// wait, and arrives as a 400. Tidal signals it two ways and accepting
		// both costs nothing.
		if (strcmp(error, "authorization_pending") == 0 || sub == 1002) {
			api_done(body, &doc);
			return TIDAL_LOGIN_PENDING;
		}
		if (strcmp(error, "expired_token") == 0) {
			api_done(body, &doc);
			return TIDAL_LOGIN_EXPIRED;
		}

		note_api_error(&doc, status);
		api_done(body, &doc);
		return TIDAL_LOGIN_ERROR;
	}

	char access[sizeof(access_token)] = "";
	char refresh[sizeof(refresh_token)] = "";
	json_obj_str(&doc, root, "access_token", access, sizeof(access));
	json_obj_str(&doc, root, "refresh_token", refresh, sizeof(refresh));
	long expires_in = json_obj_long(&doc, root, "expires_in", 0);

	if (!access[0] || !refresh[0]) {
		api_done(body, &doc);
		set_error("%s", tr("tidal_gave_no_session_tokens"));
		return TIDAL_LOGIN_ERROR;
	}

	// Who the account is and where it is. Both come in this same response, which
	// saves the /sessions call other clients make.
	char cc[sizeof(country_code)] = "";
	char uid[sizeof(user_id)] = "";
	char name[TIDAL_NAME_MAX] = "";
	int user = json_get(&doc, root, "user");
	if (user >= 0) {
		json_obj_str(&doc, user, "countryCode", cc, sizeof(cc));
		long id = json_obj_long(&doc, user, "userId", 0);
		if (id > 0) {
			snprintf(uid, sizeof(uid), "%ld", id);
		}
		if (!json_obj_str(&doc, user, "username", name, sizeof(name)) || !name[0]) {
			if (!json_obj_str(&doc, user, "nickname", name, sizeof(name)) || !name[0]) {
				json_obj_str(&doc, user, "email", name, sizeof(name));
			}
		}
	}

	api_done(body, &doc);

	if (!uid[0]) {
		// Without the user id, favourites and playlists have no URL: better to
		// say so now than leave half a page broken with no explanation.
		set_error("%s", tr("tidal_account_unknown"));
		return TIDAL_LOGIN_ERROR;
	}

	snprintf(access_token, sizeof(access_token), "%s", access);
	snprintf(refresh_token, sizeof(refresh_token), "%s", refresh);
	token_expires_at = expires_in > 0 ? (long)time(NULL) + expires_in : 0;
	snprintf(country_code, sizeof(country_code), "%s", cc[0] ? cc : "US");
	snprintf(user_id, sizeof(user_id), "%s", uid);
	snprintf(display_name, sizeof(display_name), "%s", name[0] ? name : tr("tidal"));
	save_session();

	printf("tidal: logged in as %s (%s)\n", display_name, country_code);
	return TIDAL_LOGIN_OK;
}

// ---------------------------------------------------------------------------
// covers
//
// Tidal does not send URLs, it sends a UUID, and the URL comes from replacing
// the dashes with slashes:
//
//     c50c9146-3c7a-44e6-a1bc-55b20516eb29
//     -> .../images/c50c9146/3c7a/44e6/a1bc/55b20516eb29/320x320.jpg
//
// The available sizes differ by image type: 80 and 1280 exist for album covers
// but not for artist photos, which answer 404 at those sizes. Hence two
// functions rather than one with a parameter.
// ---------------------------------------------------------------------------

static void image_url(const char *uuid, int size_px, char *out, size_t out_size) {
	if (!out || out_size == 0) {
		return;
	}
	out[0] = '\0';
	if (!uuid || !*uuid) {
		return;
	}

	char slashed[64];
	size_t k = 0;
	for (const char *p = uuid; *p && k + 1 < sizeof(slashed); p++) {
		slashed[k++] = (*p == '-') ? '/' : *p;
	}
	slashed[k] = '\0';

	snprintf(out, out_size, "%s/%s/%dx%d.jpg", TIDAL_IMAGES, slashed, size_px, size_px);
}

// The size covers are fetched at. The player draws them 480 px across on this
// screen, so the default 320 is upscaled and looks soft. See
// TIDAL_COVER_PX_DEFAULT for why 640 is not the default anyway.
int tidal_cover_px(void) { return cover_px; }

void tidal_cover_url(const char *uuid, int size_px, char *out, size_t out_size) {
	image_url(uuid, size_px > 0 ? size_px : cover_px, out, out_size);
}

void tidal_artist_image_url(const char *uuid, int size_px, char *out, size_t out_size) {
	image_url(uuid, size_px > 0 ? size_px : 320, out, out_size);
}

// ---------------------------------------------------------------------------
// JSON into structures
// ---------------------------------------------------------------------------

// mediaMetadata.tags says whether hi-res exists, but only recent responses
// carry it; the rest fall back to audioQuality, which is always present.
static bool read_hires(const json_doc_t *doc, int obj) {
	int tags = json_path(doc, obj, "mediaMetadata.tags");
	int count = json_len(doc, tags);
	for (int i = 0; i < count; i++) {
		char tag[32] = "";
		json_str(doc, json_at(doc, tags, i), tag, sizeof(tag));
		if (strcmp(tag, "HIRES_LOSSLESS") == 0) {
			return true;
		}
	}

	char aq[32] = "";
	json_obj_str(doc, obj, "audioQuality", aq, sizeof(aq));
	return strcmp(aq, "HI_RES") == 0 || strcmp(aq, "HI_RES_LOSSLESS") == 0;
}

static void read_album(const json_doc_t *doc, int obj, tidal_album_t *a) {
	memset(a, 0, sizeof(*a));
	if (obj < 0) {
		return;
	}

	a->id = json_obj_long(doc, obj, "id", 0);
	snprintf(a->id_text, sizeof(a->id_text), "%ld", a->id);
	json_obj_str(doc, obj, "title", a->title, sizeof(a->title));
	a->track_count = (int)json_obj_long(doc, obj, "numberOfTracks", 0);
	a->hires = read_hires(doc, obj);

	// The artist: "artist" when present, otherwise the first of "artists". Tidal
	// sends one or the other depending on the endpoint, and on some albums
	// (compilations) the first is missing entirely.
	if (!json_obj_str(doc, json_get(doc, obj, "artist"), "name", a->artist, sizeof(a->artist)) || !a->artist[0]) {
		int artists = json_get(doc, obj, "artists");
		json_obj_str(doc, json_at(doc, artists, 0), "name", a->artist, sizeof(a->artist));
	}

	char cover[64] = "";
	if (json_obj_str(doc, obj, "cover", cover, sizeof(cover)) && cover[0]) {
		tidal_cover_url(cover, cover_px, a->cover, sizeof(a->cover));
	}

	// "2016-10-14": only the year is kept, which is all a list row shows.
	char released[32] = "";
	if (json_obj_str(doc, obj, "releaseDate", released, sizeof(released)) && released[0]) {
		snprintf(a->released, sizeof(a->released), "%.4s", released);
	}
}

static void read_track(const json_doc_t *doc, int obj, const tidal_album_t *album_hint, tidal_track_t *t) {
	memset(t, 0, sizeof(*t));
	if (obj < 0) {
		return;
	}

	t->id = json_obj_long(doc, obj, "id", 0);
	json_obj_str(doc, obj, "title", t->title, sizeof(t->title));
	t->duration = (int)json_obj_long(doc, obj, "duration", 0);
	t->track_number = (int)json_obj_long(doc, obj, "trackNumber", 0);
	t->hires = read_hires(doc, obj);

	// Both fields are needed: allowStreaming says the track may be played at
	// all, streamReady that it is ready now. An announced but unreleased track
	// has the first true and the second false.
	t->streamable = json_obj_bool(doc, obj, "allowStreaming", true) && json_obj_bool(doc, obj, "streamReady", true);

	// Bit depth and sample rate are not in the track record: they come with the
	// manifest, when playback is requested. Left at zero so the row shows
	// nothing, which beats showing an invented number.
	t->bit_depth = 0;
	t->sample_rate = 0;

	if (!json_obj_str(doc, json_get(doc, obj, "artist"), "name", t->artist, sizeof(t->artist)) || !t->artist[0]) {
		int artists = json_get(doc, obj, "artists");
		json_obj_str(doc, json_at(doc, artists, 0), "name", t->artist, sizeof(t->artist));
	}

	int album = json_get(doc, obj, "album");
	if (album >= 0) {
		json_obj_str(doc, album, "title", t->album, sizeof(t->album));
		long album_id = json_obj_long(doc, album, "id", 0);
		if (album_id > 0) {
			snprintf(t->album_id, sizeof(t->album_id), "%ld", album_id);
		}
		char cover[64] = "";
		if (json_obj_str(doc, album, "cover", cover, sizeof(cover)) && cover[0]) {
			tidal_cover_url(cover, cover_px, t->cover, sizeof(t->cover));
		}
	}

	// Inside /albums/{id}/tracks the tracks do not repeat the album's cover or
	// artist: fill them from the album already fetched, or the rows come out
	// bare.
	if (album_hint) {
		if (!t->album[0]) {
			snprintf(t->album, sizeof(t->album), "%s", album_hint->title);
		}
		if (!t->album_id[0]) {
			snprintf(t->album_id, sizeof(t->album_id), "%s", album_hint->id_text);
		}
		if (!t->cover[0]) {
			snprintf(t->cover, sizeof(t->cover), "%s", album_hint->cover);
		}
		if (!t->artist[0]) {
			snprintf(t->artist, sizeof(t->artist), "%s", album_hint->artist);
		}
	}
}

static void read_artist(const json_doc_t *doc, int obj, tidal_artist_t *a) {
	memset(a, 0, sizeof(*a));
	if (obj < 0) {
		return;
	}
	a->id = json_obj_long(doc, obj, "id", 0);
	json_obj_str(doc, obj, "name", a->name, sizeof(a->name));

	char picture[64] = "";
	if (json_obj_str(doc, obj, "picture", picture, sizeof(picture)) && picture[0]) {
		tidal_artist_image_url(picture, 320, a->image, sizeof(a->image));
	}
}

static void read_playlist(const json_doc_t *doc, int obj, tidal_playlist_t *p) {
	memset(p, 0, sizeof(*p));
	if (obj < 0) {
		return;
	}
	json_obj_str(doc, obj, "uuid", p->id, sizeof(p->id));
	json_obj_str(doc, obj, "title", p->name, sizeof(p->name));
	p->track_count = (int)json_obj_long(doc, obj, "numberOfTracks", 0);
	json_obj_str(doc, json_get(doc, obj, "creator"), "name", p->owner, sizeof(p->owner));

	// The square image when there is one, otherwise the wide one: the latter
	// always exists but is 3:2, and gets cropped in a square list row.
	char image[64] = "";
	if (json_obj_str(doc, obj, "squareImage", image, sizeof(image)) && image[0]) {
		image_url(image, 320, p->image, sizeof(p->image));
	} else if (json_obj_str(doc, obj, "image", image, sizeof(image)) && image[0]) {
		image_url(image, 320, p->image, sizeof(p->image));
	}
}

// ---------------------------------------------------------------------------
// walking a list
//
// Tidal has two list shapes and both must be handled, because which one arrives
// depends on the endpoint:
//
//   flat      {"items":[ <track>, ... ]}         searches, album tracks
//   wrapped   {"items":[{"item":<track>,...}]}   favourites, playlist rows
//
// Reading the second as if it were the first raises no error: it yields a list
// of empty entries, which on screen are blank rows. Both shapes are therefore
// checked for.
// ---------------------------------------------------------------------------

typedef void (*reader_fn)(const json_doc_t *doc, int obj, void *out, int index);

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
		int entry = json_at(doc, items, i);
		int wrapped = json_get(doc, entry, "item");
		read(doc, wrapped >= 0 ? wrapped : entry, out, i);
	}
	return count;
}

static void track_reader(const json_doc_t *doc, int obj, void *out, int index) {
	read_track(doc, obj, NULL, &((tidal_track_t *)out)[index]);
}
static void album_reader(const json_doc_t *doc, int obj, void *out, int index) {
	read_album(doc, obj, &((tidal_album_t *)out)[index]);
}
static void artist_reader(const json_doc_t *doc, int obj, void *out, int index) {
	read_artist(doc, obj, &((tidal_artist_t *)out)[index]);
}
static void playlist_reader(const json_doc_t *doc, int obj, void *out, int index) {
	read_playlist(doc, obj, &((tidal_playlist_t *)out)[index]);
}

// Any list: request, walk, release. Nearly every public function below is one
// URL line plus a call to this.
static int fetch_list(const char *url, reader_fn read, void *out, int max) {
	char *body = NULL;
	json_doc_t doc;
	if (!api_call(url, &body, &doc)) {
		return -1;
	}
	int count = read_items(&doc, json_root(&doc), read, out, max);
	api_done(body, &doc);
	return count;
}

// ---------------------------------------------------------------------------
// search
//
// There is also /search with types=, which returns five sub-lists at once. The
// per-type endpoints -- /search/tracks, /search/albums, /search/artists -- are
// used instead for two reasons: the response is the usual flat list rather than
// a shape of its own, and above all a fifth as much data arrives. On a device
// with thirty-two megabytes, downloading and parsing four lists that will never
// be shown is not free.
// ---------------------------------------------------------------------------

// Search has two possible response shapes and both must be handled.
//
// The per-type URL (/search/tracks) should answer with the usual flat envelope,
// {"items":[...]}, while the combined one (/search with types=) answers with an
// envelope per type: {"tracks":{"items":[...]}, "albums":{...}}. The first is
// not guaranteed, and guessing wrong raises no error: json_get(root, "items")
// simply finds nothing and the list comes out empty.
//
// So the type-named envelope is looked up first and the root read when it is
// absent. One key lookup, and the difference stops mattering.
static int search(const char *kind, const char *query, int offset, reader_fn read, void *out, int max) {
	if (!query || !*query || max <= 0) {
		return 0;
	}

	char encoded[512];
	http_url_encode(query, encoded, sizeof(encoded));

	char url[1024];
	snprintf(url, sizeof(url), "%s/search/%s?countryCode=%s&query=%s&limit=%d&offset=%d", api_url, kind, country(),
			 encoded, max, offset);

	char *body = NULL;
	json_doc_t doc;
	if (!api_call(url, &body, &doc)) {
		return -1;
	}

	int root = json_root(&doc);
	int bucket = json_get(&doc, root, kind); // "tracks", "albums", "artists"
	int count = read_items(&doc, bucket >= 0 ? bucket : root, read, out, max);

	// Zero results can mean "nothing matched" or "the response was not the shape
	// expected". From outside they look identical; in the log they do not.
	if (count == 0) {
		fprintf(stderr, "tidal: search %s '%s' -> no results (bucket '%s' %s)\n", kind, query, kind,
				bucket >= 0 ? "found" : "missing, read the root");
	}

	api_done(body, &doc);
	return count;
}

int tidal_search_tracks(const char *query, int offset, tidal_track_t *out, int max) {
	return search("tracks", query, offset, track_reader, out, max);
}

int tidal_search_albums(const char *query, int offset, tidal_album_t *out, int max) {
	return search("albums", query, offset, album_reader, out, max);
}

int tidal_search_artists(const char *query, int offset, tidal_artist_t *out, int max) {
	return search("artists", query, offset, artist_reader, out, max);
}

// ---------------------------------------------------------------------------
// catalogue contents
// ---------------------------------------------------------------------------

int tidal_album_tracks(const char *album_id, int offset, tidal_track_t *out, int max) {
	if (!album_id || !*album_id || max <= 0 || offset < 0) {
		return 0;
	}

	char encoded[64];
	http_url_encode(album_id, encoded, sizeof(encoded));

	// The album first, for the cover and artist to hand to the tracks: the rows
	// of /albums/{id}/tracks do not repeat them, and without this extra call an
	// album listing came out with no cover and no artist name.
	char url[768];
	snprintf(url, sizeof(url), "%s/albums/%s?countryCode=%s", api_url, encoded, country());

	tidal_album_t album;
	memset(&album, 0, sizeof(album));

	char *body = NULL;
	json_doc_t doc;
	if (api_call(url, &body, &doc)) {
		read_album(&doc, json_root(&doc), &album);
		api_done(body, &doc);
	}
	// If the album record does not arrive, carry on anyway: the tracks matter
	// more than the cover, and most of them carry one themselves.

	snprintf(url, sizeof(url), "%s/albums/%s/tracks?countryCode=%s&limit=%d&offset=%d", api_url, encoded, country(),
			 max, offset);

	body = NULL;
	if (!api_call(url, &body, &doc)) {
		return -1;
	}

	int items = json_get(&doc, json_root(&doc), "items");
	int count = json_len(&doc, items);
	if (count > max) {
		count = max;
	}
	for (int i = 0; i < count; i++) {
		int entry = json_at(&doc, items, i);
		int wrapped = json_get(&doc, entry, "item");
		read_track(&doc, wrapped >= 0 ? wrapped : entry, &album, &out[i]);
	}

	api_done(body, &doc);
	return count;
}

int tidal_artist_albums(long artist_id, int offset, tidal_album_t *out, int max) {
	if (max <= 0 || offset < 0) {
		return 0;
	}
	char url[768];
	snprintf(url, sizeof(url), "%s/artists/%ld/albums?countryCode=%s&limit=%d&offset=%d", api_url, artist_id,
			 country(), max, offset);
	return fetch_list(url, album_reader, out, max);
}

int tidal_playlist_tracks(const char *playlist_id, int offset, tidal_track_t *out, int max) {
	if (!playlist_id || !*playlist_id || max <= 0 || offset < 0) {
		return 0;
	}

	char encoded[64];
	http_url_encode(playlist_id, encoded, sizeof(encoded));

	// Tidal caps a request at a hundred: asking for more is not an error, it
	// just returns a hundred. More than that means paging, which is what the
	// list does by calling back with the next offset.
	int limit = max > 100 ? 100 : max;

	char url[768];
	snprintf(url, sizeof(url), "%s/playlists/%s/tracks?countryCode=%s&limit=%d&offset=%d", api_url, encoded, country(),
			 limit, offset);
	return fetch_list(url, track_reader, out, limit);
}

// ---------------------------------------------------------------------------
// the user's favourites and playlists
// ---------------------------------------------------------------------------

// Ordered by date added, newest first, which is the order they are wanted in.
// Tidal's default is ascending, putting whatever was favourited in 2013 on top.
static int favorites(const char *kind, int offset, reader_fn read, void *out, int max) {
	if (max <= 0) {
		return 0;
	}
	if (!tidal_logged_in()) {
		set_error("%s", tr("tidal_not_signed_in"));
		return -1;
	}

	char url[768];
	snprintf(url, sizeof(url), "%s/users/%s/favorites/%s?countryCode=%s&limit=%d&offset=%d&order=DATE&orderDirection=DESC",
			 api_url, user_id, kind, country(), max, offset);
	return fetch_list(url, read, out, max);
}

int tidal_favorite_tracks(int offset, tidal_track_t *out, int max) {
	return favorites("tracks", offset, track_reader, out, max);
}

int tidal_favorite_albums(int offset, tidal_album_t *out, int max) {
	return favorites("albums", offset, album_reader, out, max);
}

int tidal_favorite_artists(int offset, tidal_artist_t *out, int max) {
	return favorites("artists", offset, artist_reader, out, max);
}

int tidal_user_playlists(int offset, tidal_playlist_t *out, int max) {
	if (max <= 0) {
		return 0;
	}
	if (!tidal_logged_in()) {
		set_error("%s", tr("tidal_not_signed_in"));
		return -1;
	}

	char url[768];
	snprintf(url, sizeof(url), "%s/users/%s/playlists?countryCode=%s&limit=%d&offset=%d", api_url, user_id, country(),
			 max, offset);
	return fetch_list(url, playlist_reader, out, max);
}

// Tidal's own featured collections.
//
// This is the least solid part of the file. The other endpoints here are
// confirmed by open clients that use them daily; this one is not. Tidal moved
// its storefront to /v1/pages, which answers with nested "modules" meant to be
// drawn by an app rather than read by a program, and the old /v1/featured is
// still documented but no longer used by any client examined.
//
// So the old one is called, and the page is built so that a failure here hides
// the section instead of showing an error: new releases are a bonus, and an
// apology where a row of covers should be is worse than no row at all. If these
// ever come back empty, this is where to look, and the route is /v1/pages.
int tidal_featured_albums(const char *type, int offset, tidal_album_t *out, int max) {
	if (max <= 0) {
		return 0;
	}

	const char *group = "new";
	if (type && strcmp(type, "top") == 0) {
		group = "top";
	} else if (type && strcmp(type, "recommended") == 0) {
		group = "recommended";
	}

	char url[768];
	snprintf(url, sizeof(url), "%s/featured/%s/albums?countryCode=%s&limit=%d&offset=%d", api_url, group, country(),
			 max, offset);
	return fetch_list(url, album_reader, out, max);
}

// ---------------------------------------------------------------------------
// where the audio lives
// ---------------------------------------------------------------------------

// One XML attribute, with entities resolved.
//
// Not an XML parser and not trying to be: Tidal's MPD is machine-generated, has
// a fixed shape and is a few kilobytes. All that is needed is six attributes
// from three tags; a real parser would be four hundred lines doing the same job
// worse.
//
// Resolving entities matters: the URLs inside the MPD carry a signed query
// (?Policy=..&Signature=..&Key-Pair-Id=..) and every '&' arrives as "&amp;".
// Left alone, the CDN is asked for a URL with "amp;" between the parameters,
// the signature does not match, and every segment returns 403.
static bool xml_attr(const char *tag, const char *name, char *out, size_t size) {
	if (!out || size == 0) {
		return false;
	}
	out[0] = '\0';
	if (!tag) {
		return false;
	}

	char needle[48];
	snprintf(needle, sizeof(needle), " %s=\"", name);

	// Within this tag only: if the attribute is absent, searching past the '>'
	// would pick up the next tag's.
	const char *end = strchr(tag, '>');
	const char *at = strstr(tag, needle);
	if (!at || (end && at > end)) {
		return false;
	}
	at += strlen(needle);

	size_t k = 0;
	while (*at && *at != '"' && k + 1 < size) {
		if (*at == '&') {
			if (strncmp(at, "&amp;", 5) == 0) {
				out[k++] = '&';
				at += 5;
				continue;
			}
			if (strncmp(at, "&lt;", 4) == 0) {
				out[k++] = '<';
				at += 4;
				continue;
			}
			if (strncmp(at, "&gt;", 4) == 0) {
				out[k++] = '>';
				at += 4;
				continue;
			}
			if (strncmp(at, "&quot;", 6) == 0) {
				out[k++] = '"';
				at += 6;
				continue;
			}
			if (strncmp(at, "&apos;", 6) == 0) {
				out[k++] = '\'';
				at += 6;
				continue;
			}
		}
		out[k++] = *at++;
	}
	out[k] = '\0';

	// Half a value is worse than none: a truncated URL is still requested,
	// answers 403, and looks from outside like a permissions problem.
	if (*at != '"') {
		out[0] = '\0';
		return false;
	}
	return out[0] != '\0';
}

// How many segments there are. A SegmentTimeline is made of <S d="..." r="N"/>
// entries where r is how many times that duration repeats again, so an entry
// without r is one segment and one with r="39" is forty.
static int dash_count_segments(const char *mpd) {
	const char *timeline = strstr(mpd, "<SegmentTimeline");
	if (!timeline) {
		return 0;
	}
	const char *end = strstr(timeline, "</SegmentTimeline>");

	int total = 0;
	for (const char *s = strstr(timeline, "<S "); s && (!end || s < end); s = strstr(s + 1, "<S ")) {
		char repeat[16] = "";
		total += 1;
		if (xml_attr(s, "r", repeat, sizeof(repeat))) {
			long r = strtol(repeat, NULL, 10);
			// A negative r means "until the end of the period", normal on a live
			// stream. Tracks here are static so it should not appear; if it
			// does, count one rather than subtracting into an absurd total.
			if (r > 0) {
				total += (int)r;
			}
		}
	}
	return total;
}

static bool parse_dash(const char *mpd, tidal_stream_t *out) {
	out->kind = TIDAL_STREAM_DASH;

	const char *rep = strstr(mpd, "<Representation");
	const char *tmpl = strstr(mpd, "<SegmentTemplate");
	if (!rep || !tmpl) {
		set_error("%s", tr("tidal_manifest_unexpected"));
		return false;
	}

	// The codec decides everything that follows: FLAC has to be unwrapped from
	// the MP4 container, anything else is concatenated as-is. Carrying on
	// without it would write a fragmented MP4 named .m4a with FLAC inside -- a
	// file no decoder in this player opens, marked complete and cached forever.
	// So both places the standard allows it are checked, and its absence is a
	// hard failure.
	if (!xml_attr(rep, "codecs", out->codecs, sizeof(out->codecs))) {
		const char *set = strstr(mpd, "<AdaptationSet");
		if (!set || !xml_attr(set, "codecs", out->codecs, sizeof(out->codecs))) {
			set_error("%s", tr("tidal_manifest_no_format"));
			return false;
		}
	}

	char number[24] = "";
	if (xml_attr(rep, "audioSamplingRate", number, sizeof(number))) {
		out->sample_rate = (int)strtol(number, NULL, 10);
	}

	// The container is MP4 even with FLAC inside: that is what DASH is.
	const char *adaptation = strstr(mpd, "<AdaptationSet");
	if (!adaptation || !xml_attr(adaptation, "mimeType", out->mime, sizeof(out->mime))) {
		snprintf(out->mime, sizeof(out->mime), "audio/mp4");
	}

	if (!xml_attr(tmpl, "initialization", out->init_url, sizeof(out->init_url))) {
		set_error("%s", tr("tidal_manifest_no_start"));
		return false;
	}
	if (!xml_attr(tmpl, "media", out->media_template, sizeof(out->media_template))) {
		set_error("%s", tr("tidal_manifest_no_segment_urls"));
		return false;
	}

	out->start_number = 1;
	if (xml_attr(tmpl, "startNumber", number, sizeof(number))) {
		// Bounded, because the number comes off the network and added to the
		// segment count must stay inside an int: startNumber="2147483600"
		// overflowed that sum negative, the segment loop ended before it began,
		// and the track was marked complete containing only its header.
		long n = strtol(number, NULL, 10);
		if (n < 0 || n > 1000000L) {
			set_error("%s", tr("tidal_manifest_missing_first_segment"));
			return false;
		}
		out->start_number = (int)n;
	}

	out->segment_count = dash_count_segments(mpd);
	if (out->segment_count <= 0) {
		set_error("%s", tr("tidal_manifest_no_segment_count"));
		return false;
	}

	return true;
}

static bool parse_bts(const char *text, tidal_stream_t *out) {
	out->kind = TIDAL_STREAM_FILE;

	json_doc_t doc;
	if (!json_parse(text, &doc)) {
		set_error("%s", tr("tidal_manifest_unreadable"));
		return false;
	}

	int root = json_root(&doc);
	json_obj_str(&doc, root, "mimeType", out->mime, sizeof(out->mime));
	json_obj_str(&doc, root, "codecs", out->codecs, sizeof(out->codecs));

	// Encryption is "NONE" for ordinary tracks and no key is supplied. If
	// anything else ever arrives, better to say so than download a file the
	// decoder will see as noise.
	char encryption[32] = "";
	json_obj_str(&doc, root, "encryptionType", encryption, sizeof(encryption));
	if (encryption[0] && strcmp(encryption, "NONE") != 0) {
		json_free(&doc);
		set_error("%s", tr("tidal_track_encrypted"));
		return false;
	}

	int urls = json_get(&doc, root, "urls");
	json_str(&doc, json_at(&doc, urls, 0), out->url, sizeof(out->url));
	json_free(&doc);

	if (!out->url[0]) {
		set_error("%s", tr("tidal_no_track_address"));
		return false;
	}
	return true;
}

bool tidal_track_stream(long track_id, tidal_stream_t *out) {
	clear_error();
	if (!out) {
		return false;
	}
	memset(out, 0, sizeof(*out));

	char url[768];
	snprintf(url, sizeof(url),
			 "%s/tracks/%ld/playbackinfopostpaywall?countryCode=%s&audioquality=%s"
			 "&playbackmode=STREAM&assetpresentation=FULL",
			 api_url, track_id, country(), tidal_quality_name(quality));

	char *body = NULL;
	json_doc_t doc;
	if (!api_call(url, &body, &doc)) {
		return false;
	}

	int root = json_root(&doc);

	// What Tidal actually granted, which can be less than what was asked for:
	// the subscription, the country or the track itself may not reach that
	// level. Read from here and not from the request, or the player would show
	// "24 bit" on a 16-bit file.
	out->bit_depth = (int)json_obj_long(&doc, root, "bitDepth", 0);
	out->sample_rate = (int)json_obj_long(&doc, root, "sampleRate", 0);

	char mime_type[64] = "";
	json_obj_str(&doc, root, "manifestMimeType", mime_type, sizeof(mime_type));

	int manifest_tok = json_get(&doc, root, "manifest");
	if (manifest_tok < 0) {
		api_done(body, &doc);
		set_error("%s", tr("tidal_no_track_address"));
		return false;
	}

	// The field is base64 and can be long, so it is decoded straight out of the
	// document text rather than copied first into a fixed buffer whose right
	// size is unknown.
	const char *raw = doc.text + doc.toks[manifest_tok].start;
	size_t raw_len = (size_t)(doc.toks[manifest_tok].end - doc.toks[manifest_tok].start);

	size_t decoded_len = 0;
	uint8_t *decoded = base64_decode(raw, raw_len, TIDAL_MANIFEST_LIMIT, &decoded_len);
	api_done(body, &doc);

	if (!decoded) {
		set_error("%s", tr("tidal_manifest_undecodable"));
		return false;
	}

	bool ok;
	if (strstr(mime_type, "dash") != NULL) {
		ok = parse_dash((const char *)decoded, out);
	} else {
		// vnd.tidal.bts, and anything else that does not say "dash": the JSON
		// with a URL inside is the normal case, so trying it on an unknown shape
		// is likelier to work than giving up.
		ok = parse_bts((const char *)decoded, out);
	}

	free(decoded);
	return ok;
}

bool tidal_dash_segment_url(const tidal_stream_t *stream, int number, char *out, size_t size) {
	if (!stream || !out || size == 0) {
		return false;
	}
	out[0] = '\0';

	const char *tmpl = stream->media_template;
	const char *at = strstr(tmpl, "$Number$");
	if (!at) {
		return false;
	}

	int n = snprintf(out, size, "%.*s%d%s", (int)(at - tmpl), tmpl, number, at + 8);
	return n > 0 && (size_t)n < size;
}

// ---------------------------------------------------------------------------
// writing to the account
//
// Adding and removing are not symmetric, which is Tidal's quirk: adding is a
// POST whose body carries the id, removing a DELETE that carries it in the
// path. Written this way because that is what answers.
//
// The field name is singular ("trackId", not "trackIds") even when several
// comma-separated ids are sent. The plural returns 400.
// ---------------------------------------------------------------------------

static bool favorite_add(const char *kind, const char *field, const char *id) {
	if (!id || !*id) {
		return false;
	}

	char url[768];
	snprintf(url, sizeof(url), "%s/users/%s/favorites/%s?countryCode=%s", api_url, user_id, kind, country());

	char encoded[128];
	http_url_encode(id, encoded, sizeof(encoded));

	char form[256];
	snprintf(form, sizeof(form), "%s=%s", field, encoded);

	return api_write("POST", url, form);
}

static bool favorite_remove(const char *kind, const char *id) {
	if (!id || !*id) {
		return false;
	}

	char encoded[128];
	http_url_encode(id, encoded, sizeof(encoded));

	char url[768];
	snprintf(url, sizeof(url), "%s/users/%s/favorites/%s/%s?countryCode=%s", api_url, user_id, kind, encoded,
			 country());

	return api_write("DELETE", url, NULL);
}

bool tidal_favorite_add_track(long track_id) {
	char id[32];
	snprintf(id, sizeof(id), "%ld", track_id);
	return favorite_add("tracks", "trackId", id);
}

bool tidal_favorite_remove_track(long track_id) {
	char id[32];
	snprintf(id, sizeof(id), "%ld", track_id);
	return favorite_remove("tracks", id);
}

bool tidal_favorite_add_album(const char *album_id) { return favorite_add("albums", "albumId", album_id); }
bool tidal_favorite_remove_album(const char *album_id) { return favorite_remove("albums", album_id); }

int tidal_favorite_track_ids(long *out, int max) {
	if (!out || max <= 0) {
		return -1;
	}
	if (!tidal_logged_in()) {
		set_error("%s", tr("tidal_not_signed_in"));
		return -1;
	}

	// Paged, not all at once.
	//
	// Fifty is the limit all of Tidal's endpoints accept. Asking for more does
	// not get fifty back, it gets the request refused, so the mirror has to be
	// filled a page at a time.
	int count = 0;
	for (int offset = 0; count < max; offset += TIDAL_FAVORITES_PAGE) {
		char url[768];
		snprintf(url, sizeof(url), "%s/users/%s/favorites/tracks?countryCode=%s&limit=%d&offset=%d", api_url,
				 user_id, country(), TIDAL_FAVORITES_PAGE, offset);

		char *body = NULL;
		json_doc_t doc;
		if (!api_call(url, &body, &doc)) {
			// Keep whatever pages did arrive: a partial mirror draws most of the
			// stars correctly, which beats no stars at all.
			return count > 0 ? count : -1;
		}

		int items = json_get(&doc, json_root(&doc), "items");
		int got = json_len(&doc, items);
		for (int i = 0; i < got && count < max; i++) {
			int entry = json_at(&doc, items, i);
			int wrapped = json_get(&doc, entry, "item");
			long id = json_obj_long(&doc, wrapped >= 0 ? wrapped : entry, "id", 0);
			if (id > 0) {
				out[count++] = id;
			}
		}

		// A short page means the list is exhausted.
		bool last = got < TIDAL_FAVORITES_PAGE;
		api_done(body, &doc);
		if (last) {
			break;
		}
	}

	printf("tidal: %d favorite tracks in the mirror\n", count);
	return count;
}

bool tidal_playlist_delete(const char *playlist_id) {
	if (!playlist_id || !*playlist_id) {
		return false;
	}
	if (!tidal_logged_in()) {
		set_error("%s", tr("tidal_not_signed_in"));
		return false;
	}

	char encoded[64];
	http_url_encode(playlist_id, encoded, sizeof(encoded));

	char url[768];
	snprintf(url, sizeof(url), "%s/playlists/%s?countryCode=%s", api_url, encoded, country());
	return api_write("DELETE", url, NULL);
}

bool tidal_playlist_create(const char *name, char *out_id, size_t out_size) {
	clear_error();
	if (out_id && out_size) {
		out_id[0] = '\0';
	}
	if (!tidal_logged_in()) {
		set_error("%s", tr("tidal_not_signed_in"));
		return false;
	}
	if (!name || !*name) {
		set_error("%s", tr("the_playlist_needs_a_name"));
		return false;
	}
	if (!token_is_fresh() && !token_refresh()) {
		return false;
	}

	char encoded[256];
	http_url_encode(name, encoded, sizeof(encoded));

	char url[768];
	snprintf(url, sizeof(url), "%s/users/%s/playlists?countryCode=%s", api_url, user_id, country());

	char form[512];
	snprintf(form, sizeof(form), "title=%s&description=", encoded);

	char headers[TIDAL_TOKEN_MAX + 128];
	build_headers(headers, sizeof(headers));

	// allow_empty_body also means "any 2xx will do", which is needed here:
	// creating a playlist answers 201 Created, not 200. Without it the playlist
	// really was created on the account while the user was told it had failed --
	// with the uuid lost, so the tracks meant for it vanished silently.
	http_req_t req = {
		.method = "POST",
		.extra_headers = headers,
		.body = form,
		.want_error_body = true,
		.allow_empty_body = true,
	};

	char *body = NULL;
	int status = 0;
	if (!http_request(url, &req, &body, NULL, TIDAL_BODY_LIMIT, TIDAL_TIMEOUT_SECS, &status)) {
		const char *why = http_last_error();
		set_error("%s", why && why[0] ? why : tr("tidal_no_reply"));
		return false;
	}

	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		free(body);
		set_error("%s", tr("tidal_unreadable_tidal_reply"));
		return false;
	}

	if (status < 200 || status >= 300) {
		note_api_error(&doc, status);
		api_done(body, &doc);
		return false;
	}

	char uuid[TIDAL_ID_MAX] = "";
	json_obj_str(&doc, json_root(&doc), "uuid", uuid, sizeof(uuid));
	api_done(body, &doc);

	if (!uuid[0]) {
		set_error("%s", tr("tidal_playlist_unnamed"));
		return false;
	}
	if (out_id && out_size) {
		snprintf(out_id, out_size, "%s", uuid);
	}
	return true;
}

// Adding a track to a playlist requires that playlist's ETag.
//
// It is a concurrency check: Tidal wants proof that the version being modified
// is the one that was seen, not an older one. That means two requests, one to
// read the playlist and take its ETag header and one to write, with no way to
// do it in one.
//
// But http.c does not hand response headers back to callers, and adding that
// just for this would change a signature the radio, the covers and Qobuz all
// use. Instead "If-None-Match: *" is sent, meaning "any version will do" --
// exactly the semantics needed here, since appending to a playlist has no
// version it can conflict with.
bool tidal_playlist_add_track(const char *playlist_id, long track_id) {
	if (!playlist_id || !*playlist_id) {
		return false;
	}
	if (!tidal_logged_in()) {
		set_error("%s", tr("tidal_not_signed_in"));
		return false;
	}
	if (!token_is_fresh() && !token_refresh()) {
		return false;
	}

	char encoded[64];
	http_url_encode(playlist_id, encoded, sizeof(encoded));

	char url[768];
	snprintf(url, sizeof(url), "%s/playlists/%s/items?countryCode=%s", api_url, encoded, country());

	// onArtifactNotFound=SKIP: a track no longer in the catalogue must not fail
	// the whole add. onDupes=ADD: someone who adds it twice wanted it twice.
	char form[256];
	snprintf(form, sizeof(form), "trackIds=%ld&onArtifactNotFound=SKIP&onDupes=ADD", track_id);

	char headers[TIDAL_TOKEN_MAX + 160];
	int n = snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\nIf-None-Match: *\r\n", access_token);
	if (n <= 0 || (size_t)n >= sizeof(headers)) {
		set_error("%s", tr("tidal_request_failed"));
		return false;
	}

	http_req_t req = {
		.method = "POST",
		.extra_headers = headers,
		.body = form,
		.want_error_body = true,
		.allow_empty_body = true,
	};

	char *body = NULL;
	int status = 0;
	if (!http_request(url, &req, &body, NULL, TIDAL_BODY_LIMIT, TIDAL_TIMEOUT_SECS, &status)) {
		const char *why = http_last_error();
		set_error("%s", why && why[0] ? why : tr("tidal_no_reply"));
		return false;
	}

	bool ok = status >= 200 && status < 300;
	if (!ok) {
		json_doc_t doc;
		if (body && body[0] && json_parse(body, &doc)) {
			note_api_error(&doc, status);
			json_free(&doc);
		} else {
			set_error(tr("tidal_answered_with"), status);
		}
	}
	free(body);
	return ok;
}

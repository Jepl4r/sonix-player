#ifdef SONIX_ZVUK

#define _GNU_SOURCE 1 // strcasestr

#include "zvuk.h"

#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/core/json.h"
#include "src/system/core/lang.h"
#include "src/system/net/http.h"
#include "src/system/streaming/streamerr.h"
#include "src/system/streaming/zvuksync.h"

// See zvuk.h for what this is and the thread contract.

#define ZVUK_BASE_DEFAULT "https://zvuk.com"
// What the web player is: the anti-bot filter lets a browser through and
// little else. The one zvuk-music uses, which works today (ZVUK-API.md).
#define ZVUK_UA_DEFAULT                                                                                                \
	"Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 "         \
	"Safari/537.36"
#define ZVUK_BODY_LIMIT (4 * 1024 * 1024)
#define ZVUK_TIMEOUT_SECS 15
#define SESSION_FILE "zvuk_session.ini"
// A catalogue track for the connection check when the account has no liked
// one yet: the example in zvuk-grabber's documentation (ZVUK-API.md).
#define CHECK_TRACK_DEFAULT "61318527"

static char base_url[256];
static char user_agent[256];
// The headers every request carries, the token's apart (api_headers).
static char browser_headers[700];

static pthread_mutex_t session_lock = PTHREAD_MUTEX_INITIALIZER;
static char token[ZVUK_TOKEN_MAX];
static char user_name[ZVUK_NAME_MAX];
static char plan[64];
static long plan_expires;
static volatile bool expired;
static volatile bool needs_subscription;

static pthread_mutex_t format_lock = PTHREAD_MUTEX_INITIALIZER;
static char last_format[48];

static __thread char last_error[256];

static void set_error(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(last_error, sizeof(last_error), fmt, ap);
	va_end(ap);
}

const char *zvuk_last_error(void) { return last_error; }

bool zvuk_configured(void) { return true; }

bool zvuk_logged_in(void) { return token[0] != '\0'; }

bool zvuk_needs_subscription(void) { return needs_subscription; }

bool zvuk_session_expired(void) { return expired; }

void zvuk_account(char *name, size_t name_size, char *plan_out, size_t plan_size, long *expires) {
	pthread_mutex_lock(&session_lock);
	if (name) {
		snprintf(name, name_size, "%s", user_name);
	}
	if (plan_out) {
		snprintf(plan_out, plan_size, "%s", plan);
	}
	if (expires) {
		*expires = plan_expires;
	}
	pthread_mutex_unlock(&session_lock);
}

const char *zvuk_stream_headers(void) { return browser_headers; }

// ---------------------------------------------------------------------------
// the session file, beside device_config.ini, readable by its owner only
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

// Call with session_lock held. Written whole under another name, synced and
// renamed over the old one: a power cut leaves either session, never half of
// one. Mode 0600 from the moment it exists.
static void save_session(void) {
	char path[600], tmp[640];
	session_path(path, sizeof(path));
	if (!path[0]) {
		return;
	}
	if (!token[0]) {
		remove(path);
		return;
	}
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
	if (!f) {
		if (fd >= 0) {
			close(fd);
		}
		fprintf(stderr, "zvuk: cannot write %s\n", tmp);
		return;
	}
	fchmod(fd, 0600); // whatever the umask made of it
	fprintf(f, "token=%s\nname=%s\nplan=%s\nplan_expires=%ld\n", token, user_name, plan, plan_expires);
	bool ok = fflush(f) == 0 && fsync(fd) == 0;
	if (fclose(f) != 0 || !ok || rename(tmp, path) != 0) {
		remove(tmp);
		fprintf(stderr, "zvuk: cannot save the session\n");
	}
}

static void load_session(void) {
	char path[600];
	session_path(path, sizeof(path));
	FILE *f = path[0] ? fopen(path, "r") : NULL;
	if (!f) {
		return;
	}
	char line[ZVUK_TOKEN_MAX + 64];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		char *eq = strchr(line, '=');
		if (!eq) {
			continue;
		}
		*eq = '\0';
		const char *v = eq + 1;
		if (strcmp(line, "token") == 0) {
			snprintf(token, sizeof(token), "%s", v);
		} else if (strcmp(line, "name") == 0) {
			snprintf(user_name, sizeof(user_name), "%s", v);
		} else if (strcmp(line, "plan") == 0) {
			snprintf(plan, sizeof(plan), "%s", v);
		} else if (strcmp(line, "plan_expires") == 0) {
			plan_expires = strtol(v, NULL, 10);
		}
	}
	fclose(f);
}

// ---------------------------------------------------------------------------
// the mirrors of the collection: sorted ids, one array per kind
// ---------------------------------------------------------------------------

typedef struct {
	uint64_t *ids;
	int count, room;
	bool known;
} id_set_t;

static pthread_mutex_t liked_lock = PTHREAD_MUTEX_INITIALIZER;
static id_set_t liked[ZVUK_ITEM_KINDS];

static uint64_t id_num(const char *id) { return id && id[0] ? strtoull(id, NULL, 10) : 0; }

// Where `v` is or would go. Call with liked_lock held.
static int set_find(const id_set_t *s, uint64_t v, bool *found) {
	int lo = 0, hi = s->count;
	while (lo < hi) {
		int mid = (lo + hi) / 2;
		if (s->ids[mid] < v) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	*found = lo < s->count && s->ids[lo] == v;
	return lo;
}

static void set_put(id_set_t *s, uint64_t v, bool in) {
	if (!v) {
		return;
	}
	bool found;
	int at = set_find(s, v, &found);
	if (in && !found) {
		if (s->count == s->room) {
			int room = s->room ? s->room * 2 : 64;
			uint64_t *grown = realloc(s->ids, (size_t)room * sizeof(*grown));
			if (!grown) {
				return;
			}
			s->ids = grown;
			s->room = room;
		}
		memmove(s->ids + at + 1, s->ids + at, (size_t)(s->count - at) * sizeof(*s->ids));
		s->ids[at] = v;
		s->count++;
	} else if (!in && found) {
		memmove(s->ids + at, s->ids + at + 1, (size_t)(s->count - at - 1) * sizeof(*s->ids));
		s->count--;
	}
}

static int cmp_u64(const void *a, const void *b) {
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : x > y;
}

// Installs a whole list (unsorted, from an answer) as the mirror of `kind`.
static void set_install(zvuk_item_t kind, uint64_t *ids, int count) {
	qsort(ids, (size_t)count, sizeof(*ids), cmp_u64);
	pthread_mutex_lock(&liked_lock);
	free(liked[kind].ids);
	liked[kind].ids = ids;
	liked[kind].count = liked[kind].room = count;
	liked[kind].known = true;
	pthread_mutex_unlock(&liked_lock);
}

bool zvuk_is_liked(zvuk_item_t kind, const char *id) {
	if ((unsigned)kind >= ZVUK_ITEM_KINDS) {
		return false;
	}
	uint64_t v = id_num(id);
	pthread_mutex_lock(&liked_lock);
	bool found = false;
	if (v && liked[kind].count) {
		set_find(&liked[kind], v, &found);
	}
	pthread_mutex_unlock(&liked_lock);
	return found;
}

bool zvuk_liked_known(zvuk_item_t kind) { return (unsigned)kind < ZVUK_ITEM_KINDS && liked[kind].known; }

void zvuk_liked_mark(zvuk_item_t kind, const char *id, bool in) {
	if ((unsigned)kind >= ZVUK_ITEM_KINDS) {
		return;
	}
	pthread_mutex_lock(&liked_lock);
	set_put(&liked[kind], id_num(id), in);
	pthread_mutex_unlock(&liked_lock);
}

// ---------------------------------------------------------------------------
// start, stop
// ---------------------------------------------------------------------------

static bool session_fresh = true; // the next request is the session's first

static cardimport_result_t token_file_validate(const char lines[][CARDIMPORT_LINE_MAX], int count, char *message,
											   size_t message_size);

static void build_headers(void) {
	snprintf(browser_headers, sizeof(browser_headers),
			 "User-Agent: %s\r\n"
			 "Accept: application/json, text/plain, */*\r\n"
			 "Accept-Language: ru-RU,ru;q=0.9,en-US;q=0.8,en;q=0.7\r\n"
			 "Origin: https://zvuk.com\r\n"
			 "Referer: https://zvuk.com/\r\n",
			 user_agent);
}

void zvuk_init(void) {
	snprintf(base_url, sizeof(base_url), "%s", config_get("zvuk", "base_url", ZVUK_BASE_DEFAULT));
	size_t n = strlen(base_url);
	if (n > 0 && base_url[n - 1] == '/') {
		base_url[n - 1] = '\0';
	}
	snprintf(user_agent, sizeof(user_agent), "%s", config_get("zvuk", "user_agent", ZVUK_UA_DEFAULT));
	build_headers();
	pthread_mutex_lock(&session_lock);
	load_session();
	pthread_mutex_unlock(&session_lock);
	// The cache is the service's as soon as there is a card, signed in or not.
	zvuksync_register_provider();
	cardimport_register("zvuk", ZVUK_TOKEN_FILE, token_file_validate);
	printf("zvuk: %s, base %s\n", zvuk_logged_in() ? "saved session" : "no session", base_url);
}

static cardimport_result_t token_file_validate(const char lines[][CARDIMPORT_LINE_MAX], int count, char *message,
											   size_t message_size) {
	// The token is the first line that is not empty; a whole profile answer
	// pasted in is one line too.
	(void)count;
	return zvuk_sign_in(lines[0], message, message_size);
}

static void forget_account(void) {
	pthread_mutex_lock(&session_lock);
	memset(token, 0, sizeof(token));
	user_name[0] = plan[0] = '\0';
	plan_expires = 0;
	save_session();
	pthread_mutex_unlock(&session_lock);
	pthread_mutex_lock(&liked_lock);
	for (int k = 0; k < ZVUK_ITEM_KINDS; k++) {
		free(liked[k].ids);
		liked[k] = (id_set_t){0};
	}
	pthread_mutex_unlock(&liked_lock);
	needs_subscription = false;
}

void zvuk_logout(void) {
	expired = false;
	forget_account();
	printf("zvuk: signed out\n");
}

// The service refused the token in the middle of things: the session goes,
// and the page says why. What is saved on the card plays on (zvukdl.h).
static void session_refused(int status) {
	if (!zvuk_logged_in()) {
		return;
	}
	fprintf(stderr, "zvuk: the token was refused (HTTP %d): signing out\n", status);
	forget_account();
	expired = true;
}

// ---------------------------------------------------------------------------
// pacing: at least 250 ms between requests, and a pause after a refusal
// (15, 60, 120 s), as for Yandex. zvuk-music's own throttle is 5 a second.
// ---------------------------------------------------------------------------

static pthread_mutex_t pace_lock = PTHREAD_MUTEX_INITIALIZER;
static struct timespec last_request;
static time_t blocked_until;
static int strikes;

static void throttle(void) {
	pthread_mutex_lock(&pace_lock);
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	long since = (now.tv_sec - last_request.tv_sec) * 1000 + (now.tv_nsec - last_request.tv_nsec) / 1000000;
	if (last_request.tv_sec && since < 250) {
		usleep((useconds_t)(250 - since) * 1000);
	}
	clock_gettime(CLOCK_MONOTONIC, &last_request);
	pthread_mutex_unlock(&pace_lock);
}

static bool pace_allows(void) {
	pthread_mutex_lock(&pace_lock);
	bool ok = time(NULL) >= blocked_until;
	pthread_mutex_unlock(&pace_lock);
	if (!ok) {
		set_error("%s", tr("zvuk_too_many_requests"));
		streamerr_hint(STREAM_ERR_SERVER);
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
	fprintf(stderr, "zvuk: rate limited, waiting %d s\n", pause);
}

static void pace_ok(void) {
	pthread_mutex_lock(&pace_lock);
	strikes = 0;
	pthread_mutex_unlock(&pace_lock);
}

// ---------------------------------------------------------------------------
// diagnostics
//
// Nobody can try zvuk.com from where this was written; the first device log
// has to answer whether the firmware's TLS gets through ServicePipe at all.
// So the first request of a session -- and every request of a sign-in or of
// the developer options' check -- writes a block to the log: the request,
// the status, the content type, and the first 200 bytes when what came was
// not JSON. getStream adds which qualities came, the host of the address
// and how long it lives.
// ---------------------------------------------------------------------------

static __thread bool diag_thread; // a sign-in or a check: every request logged

static bool diag_wanted(void) {
	if (diag_thread) {
		return true;
	}
	if (session_fresh) {
		session_fresh = false;
		return true;
	}
	return false;
}

static void one_line(const char *in, char *out, size_t size, size_t max) {
	size_t n = 0;
	for (const char *p = in ? in : ""; *p && n + 1 < size && n < max; p++) {
		unsigned char c = (unsigned char)*p;
		out[n++] = c < 0x20 ? ' ' : (char)c;
	}
	out[n] = '\0';
}

static bool looks_like_json(const char *body) {
	const char *p = body ? body : "";
	while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') {
		p++;
	}
	return *p == '{' || *p == '[';
}

// An anti-bot answer: 418, or an HTML page where JSON was asked for. Not a 404
// page: that is an answer about the thing asked for.
static bool is_blocked(int status, const char *body) {
	if (status == 418) {
		return true;
	}
	if (status == 404 || !body) {
		return false;
	}
	char head[208];
	one_line(body, head, sizeof(head), 200);
	for (char *p = head; *p; p++) {
		*p = (char)tolower((unsigned char)*p);
	}
	return strstr(head, "<html") || strstr(head, "<!doctype") || strstr(head, "bot activity");
}

static void diag_answer(const char *what, int status, const char *body) {
	const char *ctype = http_last_content_type();
	bool json = looks_like_json(body);
	printf("zvuk: diag %s -> HTTP %d, content-type \"%s\", %zu bytes, %s\n", what, status, ctype ? ctype : "",
		   body ? strlen(body) : (size_t)0, json ? "JSON" : "NOT JSON");
	if (!json) {
		char head[208];
		one_line(body, head, sizeof(head), 200);
		printf("zvuk: diag first 200 bytes: %s\n", head);
	}
	if (is_blocked(status, body)) {
		printf("zvuk: diag BLOCKED by the anti-bot filter (ServicePipe): the firmware's request did not pass\n");
	}
}

static void diag_no_answer(const char *what) {
	http_outcome_t o;
	http_last_outcome(&o);
	static const char *const FAILS[] = {"none", "address", "dns", "connect", "tls", "timeout", "broken"};
	const char *why = http_last_error();
	printf("zvuk: diag %s -> no answer (%s%s%s)\n", what, (unsigned)o.fail < 7 ? FAILS[o.fail] : "?",
		   why && why[0] ? ": " : "", why ? why : "");
}

// ---------------------------------------------------------------------------
// requests
// ---------------------------------------------------------------------------

typedef enum {
	ANSWER_OK = 0,
	ANSWER_NONE,	// no answer at all
	ANSWER_REFUSED, // 401, 403, "unauthorized"
	ANSWER_BLOCKED, // the anti-bot filter
	ANSWER_LIMITED, // 429, or the pause after one
	ANSWER_FAILED,	// anything else: a status, unreadable JSON, a GraphQL error
} answer_t;

static __thread answer_t last_answer;
static __thread int last_status;

// One request. `json_body` NULL = GET. `referer` replaces zvuk.com/ (the
// Tiny API wants its own URL there). On ANSWER_OK the body is parsed into
// `doc`; the caller frees both with done().
static answer_t request(const char *what, const char *url, const char *json_body, const char *referer, const char *tok,
						char **body, json_doc_t *doc) {
	*body = NULL;
	last_status = 0;
	last_error[0] = '\0';
	if (!pace_allows()) {
		return last_answer = ANSWER_LIMITED;
	}
	char headers[1100];
	const char *headers_in = browser_headers;
	char replaced[800];
	if (referer) {
		// The same block with the Referer swapped.
		const char *at = strstr(browser_headers, "Referer: ");
		snprintf(replaced, sizeof(replaced), "%.*sReferer: %s\r\n", (int)(at ? at - browser_headers : 0),
				 browser_headers, referer);
		headers_in = replaced;
	}
	snprintf(headers, sizeof(headers), "%sX-Auth-Token: %s\r\n", headers_in, tok);
	http_req_t req = {
		.method = json_body ? "POST" : "GET",
		.extra_headers = headers,
		.body = json_body,
		.content_type = json_body ? "application/json" : NULL,
		.want_error_body = true,
		.allow_empty_body = true,
	};
	bool diag = diag_wanted();
	throttle();
	int status = 0;
	bool got = http_request(url, &req, body, NULL, ZVUK_BODY_LIMIT, ZVUK_TIMEOUT_SECS, &status);
	memset(headers, 0, sizeof(headers));
	if (!got) {
		if (diag) {
			diag_no_answer(what);
		}
		const char *why = http_last_error();
		set_error("%s", why && why[0] ? why : tr("zvuk_no_reply"));
		*body = NULL;
		return last_answer = ANSWER_NONE;
	}
	last_status = status;
	if (diag) {
		diag_answer(what, status, *body);
	}
	answer_t a = ANSWER_OK;
	if (is_blocked(status, *body)) {
		// Said once per occurrence in the log, whatever the diagnostics.
		fprintf(stderr, "zvuk: %s blocked by the anti-bot filter (HTTP %d)\n", what, status);
		set_error("%s", tr("zvuk_blocked"));
		streamerr_hint(STREAM_ERR_OTHER);
		a = ANSWER_BLOCKED;
	} else if (status == 429) {
		pace_refused();
		set_error("%s", tr("zvuk_too_many_requests"));
		streamerr_hint(STREAM_ERR_SERVER);
		a = ANSWER_LIMITED;
	} else if (status == 401 || status == 403) {
		set_error("%s", tr("zvuk_token_refused"));
		streamerr_hint(STREAM_ERR_SIGN_IN);
		a = ANSWER_REFUSED;
	} else if (status < 200 || status >= 300) {
		char head[120];
		one_line(*body, head, sizeof(head), 100);
		fprintf(stderr, "zvuk: %s -> HTTP %d %s\n", what, status, head);
		set_error(tr("zvuk_answered_with"), status);
		if (status == 404) {
			streamerr_hint(STREAM_ERR_NOT_FOUND);
		}
		a = ANSWER_FAILED;
	} else if (!json_parse(*body, doc)) {
		set_error("%s", tr("zvuk_unreadable_reply"));
		a = ANSWER_FAILED;
	}
	if (a != ANSWER_OK) {
		free(*body);
		*body = NULL;
	} else {
		pace_ok();
	}
	return last_answer = a;
}

static void done(char *body, json_doc_t *doc) {
	json_free(doc);
	free(body);
}

// Writes `in` as the inside of a JSON string.
static void json_escape(const char *in, char *out, size_t size) {
	size_t n = 0;
	for (const unsigned char *p = (const unsigned char *)in; *p && n + 7 < size; p++) {
		if (*p == '"' || *p == '\\') {
			out[n++] = '\\';
			out[n++] = (char)*p;
		} else if (*p < 0x20) {
			n += (size_t)snprintf(out + n, size - n, "\\u%04x", *p);
		} else {
			out[n++] = (char)*p;
		}
	}
	out[n] = '\0';
}

// A GraphQL call: the operation, its query (one line, no quotes in it) and
// its variables as a JSON object. On success `data` is the answer's "data".
// A 200 carrying only errors is a failure; one saying "unauthorized" is a
// refused token like a 401.
static bool gql(const char *op, const char *query, const char *variables, char **body, json_doc_t *doc, int *data) {
	if (!zvuk_logged_in()) {
		set_error("%s", tr("zvuk_not_signed_in"));
		streamerr_hint(STREAM_ERR_SIGN_IN);
		return false;
	}
	size_t len = strlen(query) + strlen(variables ? variables : "{}") + strlen(op) + 64;
	char *payload = malloc(len);
	if (!payload) {
		set_error("%s", tr("out_of_memory"));
		return false;
	}
	snprintf(payload, len, "{\"operationName\":\"%s\",\"query\":\"%s\",\"variables\":%s}", op, query,
			 variables ? variables : "{}");
	char url[300], what[96], tok[ZVUK_TOKEN_MAX];
	snprintf(url, sizeof(url), "%s/api/v1/graphql", base_url);
	snprintf(what, sizeof(what), "POST /api/v1/graphql %s", op);
	pthread_mutex_lock(&session_lock);
	snprintf(tok, sizeof(tok), "%s", token);
	pthread_mutex_unlock(&session_lock);
	answer_t a = request(what, url, payload, NULL, tok, body, doc);
	memset(tok, 0, sizeof(tok));
	free(payload);
	if (a == ANSWER_REFUSED) {
		session_refused(last_status);
		set_error("%s", tr("zvuk_token_expired"));
		return false;
	}
	if (a != ANSWER_OK) {
		return false;
	}
	int root = json_root(doc);
	int errors = json_get(doc, root, "errors");
	*data = json_get(doc, root, "data");
	if (errors >= 0 && json_len(doc, errors) > 0) {
		char message[200] = "";
		json_obj_str(doc, json_at(doc, errors, 0), "message", message, sizeof(message));
		fprintf(stderr, "zvuk: %s: GraphQL error: %s\n", op, message);
		if (strcasestr(message, "unauthori") || strcasestr(message, "unauthentic")) {
			done(*body, doc);
			*body = NULL;
			session_refused(401);
			set_error("%s", tr("zvuk_token_expired"));
			streamerr_hint(STREAM_ERR_SIGN_IN);
			return false;
		}
		// Partial data is still data: only a missing "data" fails the call.
		if (*data < 0 || doc->toks[*data].type != JSON_OBJECT) {
			set_error("%s", message[0] ? message : tr("zvuk_unreadable_reply"));
			done(*body, doc);
			*body = NULL;
			return false;
		}
	}
	if (*data < 0) {
		set_error("%s", tr("zvuk_unreadable_reply"));
		done(*body, doc);
		*body = NULL;
		return false;
	}
	return true;
}

// ["1","2"] for a list of ids, into `out`; false when it does not fit.
static bool ids_json(const char *const *ids, int count, char *out, size_t size) {
	size_t n = (size_t)snprintf(out, size, "[");
	for (int i = 0; i < count && n < size; i++) {
		n += (size_t)snprintf(out + n, size - n, "%s\"%.*s\"", i ? "," : "", ZVUK_ID_MAX - 1, ids[i]);
	}
	if (n + 2 > size) {
		return false;
	}
	snprintf(out + n, size - n, "]");
	return true;
}

// ---------------------------------------------------------------------------
// JSON to structs
// ---------------------------------------------------------------------------

void zvuk_cover_url(const char *src, int px, char *out, size_t size) {
	out[0] = '\0';
	if (!src || !src[0]) {
		return;
	}
	unsigned short side = (unsigned short)(px > 0 && px < 4096 ? px : 600);
	// Room left in `out` for the size parameter: a longer address is no
	// address the CDN would answer anyway.
	char abs[ZVUK_URL_MAX - 24];
	if (src[0] == '/') {
		snprintf(abs, sizeof(abs), "%s%s", base_url, src);
	} else {
		snprintf(abs, sizeof(abs), "%s", src);
	}
	const char *tpl = strstr(abs, "{size}");
	if (tpl) {
		snprintf(out, size, "%.*s%hux%hu%s", (int)(tpl - abs), abs, side, side, tpl + 6);
		return;
	}
	const char *q = strchr(abs, '?');
	const char *param = q ? strstr(q, "size=") : NULL;
	if (param && (param[-1] == '?' || param[-1] == '&')) {
		const char *end = param + 5 + strcspn(param + 5, "&");
		snprintf(out, size, "%.*ssize=%hux%hu%s", (int)(param - abs), abs, side, side, end);
		return;
	}
	snprintf(out, size, "%s%ssize=%hux%hu", abs, q ? "&" : "?", side, side);
}

// Ids come as strings, but a number is read too.
static void id_of(const json_doc_t *doc, int obj, const char *key, char *out, size_t size) {
	out[0] = '\0';
	int tok = obj >= 0 ? json_get(doc, obj, key) : -1;
	if (tok < 0) {
		return;
	}
	if (doc->toks[tok].type == JSON_NUMBER) {
		snprintf(out, size, "%lld", json_llong(doc, tok, 0));
	} else {
		json_str(doc, tok, out, size);
	}
}

static void artists_of(const json_doc_t *doc, int obj, char *out, size_t size, char *first_id, size_t id_size) {
	out[0] = '\0';
	if (first_id) {
		first_id[0] = '\0';
	}
	int arr = json_get(doc, obj, "artists");
	int n = arr >= 0 ? json_len(doc, arr) : 0;
	size_t used = 0;
	for (int i = 0; i < n && i < 3; i++) {
		int a = json_at(doc, arr, i);
		char name[96] = "";
		json_obj_str(doc, a, "title", name, sizeof(name));
		if (i == 0 && first_id) {
			id_of(doc, a, "id", first_id, id_size);
		}
		if (name[0] && used < size) {
			used += (size_t)snprintf(out + used, size - used, "%s%s", used ? ", " : "", name);
		}
	}
}

static int year_of(const json_doc_t *doc, int obj) {
	char date[32] = "";
	json_obj_str(doc, obj, "date", date, sizeof(date));
	int year = atoi(date);
	return year > 1800 && year < 3000 ? year : 0;
}

static void image_of(const json_doc_t *doc, int obj, int px, char *out, size_t size) {
	char src[ZVUK_URL_MAX] = "";
	int image = json_get(doc, obj, "image");
	if (image >= 0) {
		json_obj_str(doc, image, "src", src, sizeof(src));
	}
	zvuk_cover_url(src, px, out, size);
}

// A track object; `release` (may be -1) stands in for its own "release" when
// the track is listed inside one.
static void track_from(const json_doc_t *doc, int t, int release, zvuk_track_t *out) {
	memset(out, 0, sizeof(*out));
	id_of(doc, t, "id", out->id, sizeof(out->id));
	json_obj_str(doc, t, "title", out->title, sizeof(out->title));
	artists_of(doc, t, out->artist, sizeof(out->artist), out->artist_id, sizeof(out->artist_id));
	out->duration = (int)json_obj_long(doc, t, "duration", 0);
	int r = json_get(doc, t, "release");
	if (r < 0 || doc->toks[r].type != JSON_OBJECT) {
		r = release;
	}
	if (r >= 0) {
		id_of(doc, r, "id", out->album_id, sizeof(out->album_id));
		json_obj_str(doc, r, "title", out->album, sizeof(out->album));
		out->year = year_of(doc, r);
		image_of(doc, r, 600, out->cover, sizeof(out->cover));
	}
	out->available = out->id[0] != '\0';
}

static void album_from(const json_doc_t *doc, int a, zvuk_album_t *out) {
	memset(out, 0, sizeof(*out));
	id_of(doc, a, "id", out->id, sizeof(out->id));
	json_obj_str(doc, a, "title", out->title, sizeof(out->title));
	artists_of(doc, a, out->artist, sizeof(out->artist), NULL, 0);
	out->year = year_of(doc, a);
	image_of(doc, a, 600, out->cover, sizeof(out->cover));
	int tracks = json_get(doc, a, "tracks");
	out->track_count = tracks >= 0 ? json_len(doc, tracks) : 0;
}

static void artist_from(const json_doc_t *doc, int a, zvuk_artist_t *out) {
	memset(out, 0, sizeof(*out));
	id_of(doc, a, "id", out->id, sizeof(out->id));
	json_obj_str(doc, a, "title", out->name, sizeof(out->name));
}

static void playlist_from(const json_doc_t *doc, int p, zvuk_playlist_t *out) {
	memset(out, 0, sizeof(*out));
	id_of(doc, p, "id", out->id, sizeof(out->id));
	json_obj_str(doc, p, "title", out->title, sizeof(out->title));
	int tracks = json_get(doc, p, "tracks");
	out->track_count = tracks >= 0 ? json_len(doc, tracks) : 0;
}

// ---------------------------------------------------------------------------
// the queries
//
// Field for field the ones zvuk-music sends (its graphql/queries/*.graphql),
// cut to what the player shows: a field the schema does not have fails the
// whole query, so nothing here is invented. One line each, no quotes, so
// they go into the JSON body as they are.
// ---------------------------------------------------------------------------

#define TRACK_FIELDS "id title duration artists { id title } release { id title date image { src } }"
#define RELEASE_FIELDS "id title date image { src } artists { id title }"

static const char Q_STREAM[] = "query getStream($ids: [ID!]!) { mediaContents(ids: $ids) { __typename ... on Track { "
							   "stream { expire expireDelta flac flacdrm high mid } } } }";
static const char Q_TRACKS[] = "query getTracks($ids: [ID!]!) { getTracks(ids: $ids) { " TRACK_FIELDS " } }";
static const char Q_LIKED[] = "query userTracks($orderBy: TrackOrderByType, $orderDirection: OrderDirectionType) { "
							  "collection { tracks(orderBy: $orderBy, orderDirection: $orderDirection) { id } } }";
static const char Q_COLLECTION[] =
	"query userCollection { collection { artists { id } playlists { id } releases { id } tracks { id } } }";
static const char Q_PLAYLISTS_MINE[] = "query userPlaylists { collection { playlists { id userId } } }";
static const char Q_PLAYLISTS_SHORT[] = "query getShortPlaylist($ids: [ID!]!) { getPlaylists(ids: $ids) { id title } }";
static const char Q_PLAYLIST_IDS[] =
	"query getPlaylists($ids: [ID!]!) { getPlaylists(ids: $ids) { id title tracks { id } } }";
static const char Q_RELEASE[] = "query getReleases($ids: [ID!]!) { getReleases(ids: $ids) { " RELEASE_FIELDS
								" tracks { id title duration artists { id title } } } }";
static const char Q_RELEASES_SHORT[] =
	"query getReleases($ids: [ID!]!) { getReleases(ids: $ids) { " RELEASE_FIELDS " tracks { id } } }";
static const char Q_ARTISTS_SHORT[] = "query getArtists($ids: [ID!]!) { getArtists(ids: $ids) { id title } }";
static const char Q_ARTIST_PAGE[] =
	"query getArtists($ids: [ID!]!, $withReleases: Boolean = false, $withPopTracks: Boolean = false, "
	"$releasesLimit: Int = 100, $tracksLimit: Int = 100) { getArtists(ids: $ids) { id title "
	"releases(offset: 0, limit: $releasesLimit) @include(if: $withReleases) { " RELEASE_FIELDS " } "
	"popularTracks(offset: 0, limit: $tracksLimit) @include(if: $withPopTracks) { " TRACK_FIELDS " } } }";
static const char *const Q_SEARCH[ZVUK_SEARCH_KINDS] = {
	[ZVUK_SEARCH_TRACKS] = "query search($query: String, $limit: Int = 2) { search(query: $query) { tracks(limit: "
						   "$limit) { items { " TRACK_FIELDS " } } } }",
	[ZVUK_SEARCH_ALBUMS] = "query search($query: String, $limit: Int = 2) { search(query: $query) { releases(limit: "
						   "$limit) { items { " RELEASE_FIELDS " } } } }",
	[ZVUK_SEARCH_ARTISTS] = "query search($query: String, $limit: Int = 2) { search(query: $query) { artists(limit: "
							"$limit) { items { id title } } } }",
	[ZVUK_SEARCH_PLAYLISTS] = "query search($query: String, $limit: Int = 2) { search(query: $query) { "
							  "playlists(limit: $limit) { items { id title } } } }",
};
static const char Q_ADD[] = "mutation addItemToCollection($id: ID, $type: CollectionItemType) { collection { "
							"addItem(id: $id, type: $type) } }";
static const char Q_REMOVE[] = "mutation removeItemFromCollection($id: ID, $type: CollectionItemType) { collection { "
							   "removeItem(id: $id, type: $type) } }";

static const char *const ITEM_TYPES[ZVUK_ITEM_KINDS] = {"track", "release", "artist", "playlist"};

// ---------------------------------------------------------------------------
// the account
// ---------------------------------------------------------------------------

// Takes the profile's name and subscription out of a Tiny profile answer:
// {"result":{...}} (v1) or {"result":{"profile":{...},"subscription":{...}}}
// (v2). `anonymous` gets what it says about that.
static void take_profile(const json_doc_t *doc, bool *anonymous) {
	int result = json_get(doc, json_root(doc), "result");
	int profile = json_get(doc, result, "profile");
	if (profile < 0) {
		profile = result;
	}
	*anonymous = json_obj_bool(doc, profile, "is_anonymous", false);
	char name[ZVUK_NAME_MAX] = "";
	if (!json_obj_str(doc, profile, "name", name, sizeof(name)) || !name[0]) {
		json_obj_str(doc, profile, "username", name, sizeof(name));
	}
	int sub = json_get(doc, result, "subscription");
	char title[64] = "";
	long long ends = 0;
	if (sub >= 0 && doc->toks[sub].type == JSON_OBJECT) {
		json_obj_str(doc, sub, "title", title, sizeof(title));
		ends = json_obj_llong(doc, sub, "expiration", 0);
		if (ends > 100000000000LL) {
			ends /= 1000; // milliseconds
		}
	}
	pthread_mutex_lock(&session_lock);
	snprintf(user_name, sizeof(user_name), "%s", name);
	snprintf(plan, sizeof(plan), "%s", title);
	plan_expires = (long)ends;
	pthread_mutex_unlock(&session_lock);
}

// The profile with `tok`. ANSWER_OK with the name taken.
static answer_t read_profile(const char *tok, bool *anonymous) {
	char url[300];
	snprintf(url, sizeof(url), "%s/api/tiny/profile", base_url);
	char *body;
	json_doc_t doc;
	// The Tiny API answers 418 without a Referer; its own address will do
	// (zvuk-grabber).
	answer_t a = request("GET /api/tiny/profile", url, NULL, url, tok, &body, &doc);
	*anonymous = false;
	if (a == ANSWER_OK) {
		take_profile(&doc, anonymous);
		done(body, &doc);
	}
	return a;
}

bool zvuk_token_clean(const char *in, char *out, size_t size) {
	out[0] = '\0';
	if (!in) {
		return false;
	}
	// A whole profile answer: the value of "token".
	const char *key = strstr(in, "\"token\"");
	if (key) {
		const char *q = strchr(key + 7, '"');
		in = q ? q + 1 : key + 7;
	} else if (strncasecmp(in, "token", 5) == 0 && (in[5] == '=' || in[5] == ':')) {
		in += 6;
	}
	size_t n = 0;
	for (const char *p = in; *p && n + 1 < size; p++) {
		char c = *p;
		if (c == ' ' || c == '\t' || c == '\'' || (c == '"' && n == 0)) {
			continue;
		}
		if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.') {
			out[n++] = c;
		} else {
			break; // the closing quote, or anything a token is not made of
		}
	}
	out[n] = '\0';
	return n >= 16;
}

static void probe_after_sign_in(void);

cardimport_result_t zvuk_sign_in(const char *raw, char *message, size_t message_size) {
	char tok[ZVUK_TOKEN_MAX];
	message[0] = '\0';
	if (!zvuk_token_clean(raw, tok, sizeof(tok))) {
		snprintf(message, message_size, "%s", tr("zvuk_token_unreadable"));
		return CARDIMPORT_INVALID;
	}
	diag_thread = true;
	printf("zvuk: trying the token from the card (%zu characters), base %s\n", strlen(tok), base_url);
	bool anonymous = false;
	answer_t a = read_profile(tok, &anonymous);
	if (a == ANSWER_BLOCKED) {
		// The profile sits behind the strictest filter; GraphQL may still
		// pass (Music Assistant verifies tokens that way when it does not).
		printf("zvuk: the profile is blocked, trying the token through GraphQL\n");
		char url[300];
		snprintf(url, sizeof(url), "%s/api/v1/graphql", base_url);
		char payload[256];
		snprintf(payload, sizeof(payload), "{\"operationName\":\"userPlaylists\",\"query\":\"%s\",\"variables\":{}}",
				 Q_PLAYLISTS_MINE);
		char *body;
		json_doc_t doc;
		a = request("POST /api/v1/graphql userPlaylists (sign-in)", url, payload, NULL, tok, &body, &doc);
		if (a == ANSWER_OK) {
			bool data = json_get(&doc, json_root(&doc), "data") >= 0;
			int errors = json_get(&doc, json_root(&doc), "errors");
			if (!data || (errors >= 0 && json_len(&doc, errors) > 0)) {
				a = ANSWER_REFUSED;
			}
			done(body, &doc);
		}
	}
	diag_thread = false;
	cardimport_result_t result;
	switch (a) {
	case ANSWER_OK:
		if (anonymous) {
			snprintf(message, message_size, "%s", tr("zvuk_token_anonymous"));
			result = CARDIMPORT_REFUSED;
			break;
		}
		pthread_mutex_lock(&session_lock);
		snprintf(token, sizeof(token), "%s", tok);
		save_session();
		pthread_mutex_unlock(&session_lock);
		expired = false;
		needs_subscription = false;
		session_fresh = true;
		pthread_mutex_lock(&session_lock);
		snprintf(message, message_size, tr("zvuk_signed_in_as"), user_name[0] ? user_name : tr("zvuk"));
		pthread_mutex_unlock(&session_lock);
		printf("zvuk: signed in (%s)\n", plan[0] ? plan : "subscription unknown");
		diag_thread = true;
		probe_after_sign_in();
		diag_thread = false;
		result = CARDIMPORT_OK;
		break;
	case ANSWER_REFUSED:
		snprintf(message, message_size, "%s", tr("zvuk_token_refused"));
		result = CARDIMPORT_REFUSED;
		break;
	case ANSWER_BLOCKED:
		snprintf(message, message_size, "%s", tr("zvuk_blocked"));
		result = CARDIMPORT_BLOCKED;
		break;
	case ANSWER_NONE:
	case ANSWER_LIMITED:
		snprintf(message, message_size, "%s", last_error[0] ? last_error : tr("zvuk_no_reply"));
		result = CARDIMPORT_OFFLINE;
		break;
	default:
		// The service answered, but not with a profile: the token is not the
		// problem as far as anyone can tell. Kept, tried again later.
		snprintf(message, message_size, "%s", last_error[0] ? last_error : tr("zvuk_unreadable_reply"));
		result = CARDIMPORT_OFFLINE;
		break;
	}
	memset(tok, 0, sizeof(tok));
	return result;
}

bool zvuk_refresh_profile(void) {
	if (!zvuk_logged_in()) {
		return false;
	}
	char tok[ZVUK_TOKEN_MAX];
	pthread_mutex_lock(&session_lock);
	snprintf(tok, sizeof(tok), "%s", token);
	pthread_mutex_unlock(&session_lock);
	bool anonymous;
	answer_t a = read_profile(tok, &anonymous);
	memset(tok, 0, sizeof(tok));
	if (a == ANSWER_REFUSED) {
		session_refused(last_status);
		set_error("%s", tr("zvuk_token_expired"));
		return false;
	}
	if (a == ANSWER_OK) {
		pthread_mutex_lock(&session_lock);
		save_session();
		pthread_mutex_unlock(&session_lock);
	}
	return a == ANSWER_OK;
}

// ---------------------------------------------------------------------------
// the collection
// ---------------------------------------------------------------------------

// Every id of `arr` (objects with an "id") into `each` and, when `set` is
// given, into a fresh mirror. Returns how many.
static int ids_from(const json_doc_t *doc, int arr, zvuk_id_cb each, void *user, int set) {
	int n = arr >= 0 ? json_len(doc, arr) : 0;
	uint64_t *nums = set >= 0 ? malloc((size_t)(n > 0 ? n : 1) * sizeof(*nums)) : NULL;
	int count = 0;
	for (int i = 0; i < n; i++) {
		char id[ZVUK_ID_MAX];
		id_of(doc, json_at(doc, arr, i), "id", id, sizeof(id));
		if (!id[0]) {
			continue;
		}
		if (each) {
			each(id, user);
		}
		if (nums) {
			nums[count] = id_num(id);
		}
		count++;
	}
	if (nums) {
		set_install((zvuk_item_t)set, nums, count);
	}
	return count;
}

int zvuk_liked_ids(zvuk_id_cb each, void *user) {
	char *body;
	json_doc_t doc;
	int data;
	if (!gql("userTracks", Q_LIKED, "{\"orderBy\":\"dateAdded\",\"orderDirection\":\"desc\"}", &body, &doc, &data)) {
		return -1;
	}
	int n = ids_from(&doc, json_path(&doc, data, "collection.tracks"), each, user, ZVUK_ITEM_TRACK);
	done(body, &doc);
	printf("zvuk: %d liked tracks\n", n);
	return n;
}

// The collection's ids, every kind, into the mirrors; `want` (-1 for none)
// and `out` get one kind's ids as text for the caller, up to `max`.
static int collection(int want, char (*out)[ZVUK_ID_MAX], int max) {
	char *body;
	json_doc_t doc;
	int data;
	if (!gql("userCollection", Q_COLLECTION, "{}", &body, &doc, &data)) {
		return -1;
	}
	static const char *const KEYS[ZVUK_ITEM_KINDS] = {"collection.tracks", "collection.releases",
													  "collection.artists", "collection.playlists"};
	int got = 0;
	for (int k = 0; k < ZVUK_ITEM_KINDS; k++) {
		int arr = json_path(&doc, data, KEYS[k]);
		ids_from(&doc, arr, NULL, NULL, k);
		for (int i = 0; k == want && out && i < json_len(&doc, arr) && got < max; i++) {
			id_of(&doc, json_at(&doc, arr, i), "id", out[got], ZVUK_ID_MAX);
			got += out[got][0] != '\0';
		}
	}
	done(body, &doc);
	return got;
}

bool zvuk_collection_refresh(void) { return collection(-1, NULL, 0) >= 0; }

// Details of up to `max` items known by id: one request with all the ids.
static int details(const char *op, const char *query, char (*ids)[ZVUK_ID_MAX], int count, const char *field,
				   void (*each)(const json_doc_t *doc, int obj, void *out, int index), void *out, int max) {
	if (count <= 0) {
		return 0;
	}
	if (count > max) {
		count = max;
	}
	size_t room = (size_t)count * (ZVUK_ID_MAX + 3) + 32;
	char *vars = malloc(room + 16);
	const char **ptrs = malloc((size_t)count * sizeof(*ptrs));
	if (!vars || !ptrs) {
		free(vars);
		free(ptrs);
		set_error("%s", tr("out_of_memory"));
		return -1;
	}
	for (int i = 0; i < count; i++) {
		ptrs[i] = ids[i];
	}
	int n = (int)snprintf(vars, room, "{\"ids\":");
	ids_json(ptrs, count, vars + n, room - (size_t)n);
	strcat(vars, "}");
	free(ptrs);
	char *body;
	json_doc_t doc;
	int data;
	bool ok = gql(op, query, vars, &body, &doc, &data);
	free(vars);
	if (!ok) {
		return -1;
	}
	int arr = json_get(&doc, data, field);
	int got = 0;
	for (int i = 0; i < json_len(&doc, arr) && got < max; i++) {
		int obj = json_at(&doc, arr, i);
		if (obj >= 0 && doc.toks[obj].type == JSON_OBJECT) {
			each(&doc, obj, out, got++);
		}
	}
	done(body, &doc);
	return got;
}

static void each_album(const json_doc_t *doc, int obj, void *out, int index) {
	album_from(doc, obj, &((zvuk_album_t *)out)[index]);
}
static void each_artist(const json_doc_t *doc, int obj, void *out, int index) {
	artist_from(doc, obj, &((zvuk_artist_t *)out)[index]);
}
static void each_playlist(const json_doc_t *doc, int obj, void *out, int index) {
	playlist_from(doc, obj, &((zvuk_playlist_t *)out)[index]);
}
static void each_track(const json_doc_t *doc, int obj, void *out, int index) {
	track_from(doc, obj, -1, &((zvuk_track_t *)out)[index]);
}

static int liked_of(zvuk_item_t kind, const char *op, const char *query, const char *field,
					void (*each)(const json_doc_t *, int, void *, int), void *out, int max) {
	char(*ids)[ZVUK_ID_MAX] = calloc((size_t)(max > 0 ? max : 1), ZVUK_ID_MAX);
	if (!ids) {
		set_error("%s", tr("out_of_memory"));
		return -1;
	}
	int n = collection(kind, ids, max);
	int got = n > 0 ? details(op, query, ids, n, field, each, out, max) : n;
	free(ids);
	return got;
}

int zvuk_liked_albums(zvuk_album_t *out, int max) {
	return liked_of(ZVUK_ITEM_RELEASE, "getReleases", Q_RELEASES_SHORT, "getReleases", each_album, out, max);
}

int zvuk_liked_artists(zvuk_artist_t *out, int max) {
	return liked_of(ZVUK_ITEM_ARTIST, "getArtists", Q_ARTISTS_SHORT, "getArtists", each_artist, out, max);
}

int zvuk_playlists(zvuk_playlist_t *out, int max) {
	return liked_of(ZVUK_ITEM_PLAYLIST, "getShortPlaylist", Q_PLAYLISTS_SHORT, "getPlaylists", each_playlist, out,
					max);
}

int zvuk_playlist_ids(const char *playlist_id, zvuk_id_cb each, void *user) {
	char vars[96], *body;
	snprintf(vars, sizeof(vars), "{\"ids\":[\"%.*s\"]}", ZVUK_ID_MAX - 1, playlist_id);
	json_doc_t doc;
	int data;
	if (!gql("getPlaylists", Q_PLAYLIST_IDS, vars, &body, &doc, &data)) {
		return -1;
	}
	int n = ids_from(&doc, json_path(&doc, data, "getPlaylists.0.tracks"), each, user, -1);
	done(body, &doc);
	return n;
}

int zvuk_tracks(const char *const *ids, int count, zvuk_track_t *out, int max) {
	if (count > ZVUK_TRACKS_PER_CALL) {
		count = ZVUK_TRACKS_PER_CALL;
	}
	char(*copy)[ZVUK_ID_MAX] = calloc((size_t)(count > 0 ? count : 1), ZVUK_ID_MAX);
	if (!copy) {
		set_error("%s", tr("out_of_memory"));
		return -1;
	}
	for (int i = 0; i < count; i++) {
		snprintf(copy[i], ZVUK_ID_MAX, "%s", ids[i]);
	}
	int got = details("getTracks", Q_TRACKS, copy, count, "getTracks", each_track, out, max);
	free(copy);
	return got;
}

int zvuk_album_tracks(const char *album_id, zvuk_track_t *out, int max, zvuk_album_t *meta) {
	char vars[96], *body;
	snprintf(vars, sizeof(vars), "{\"ids\":[\"%.*s\"]}", ZVUK_ID_MAX - 1, album_id);
	json_doc_t doc;
	int data;
	if (!gql("getReleases", Q_RELEASE, vars, &body, &doc, &data)) {
		return -1;
	}
	int release = json_path(&doc, data, "getReleases.0");
	if (release < 0 || doc.toks[release].type != JSON_OBJECT) {
		done(body, &doc);
		set_error("%s", tr("zvuk_not_found"));
		streamerr_hint(STREAM_ERR_NOT_FOUND);
		return -1;
	}
	zvuk_album_t album;
	album_from(&doc, release, &album);
	if (meta) {
		*meta = album;
	}
	int arr = json_get(&doc, release, "tracks");
	int got = 0;
	for (int i = 0; i < json_len(&doc, arr) && got < max; i++) {
		int t = json_at(&doc, arr, i);
		if (t < 0 || doc.toks[t].type != JSON_OBJECT) {
			continue;
		}
		track_from(&doc, t, release, &out[got]);
		out[got].track_number = i + 1;
		got++;
	}
	done(body, &doc);
	return got;
}

int zvuk_artist_page(const char *artist_id, zvuk_track_t *tracks, int max_tracks, int *n_tracks, zvuk_album_t *albums,
					 int max_albums, int *n_albums) {
	*n_tracks = *n_albums = 0;
	char vars[200], *body;
	snprintf(vars, sizeof(vars),
			 "{\"ids\":[\"%.*s\"],\"withReleases\":true,\"withPopTracks\":true,\"releasesLimit\":%d,\"tracksLimit\":%d}",
			 ZVUK_ID_MAX - 1, artist_id, max_albums, max_tracks);
	json_doc_t doc;
	int data;
	if (!gql("getArtists", Q_ARTIST_PAGE, vars, &body, &doc, &data)) {
		return -1;
	}
	int artist = json_path(&doc, data, "getArtists.0");
	int arr = json_get(&doc, artist, "popularTracks");
	for (int i = 0; i < json_len(&doc, arr) && *n_tracks < max_tracks; i++) {
		track_from(&doc, json_at(&doc, arr, i), -1, &tracks[(*n_tracks)++]);
	}
	arr = json_get(&doc, artist, "releases");
	for (int i = 0; i < json_len(&doc, arr) && *n_albums < max_albums; i++) {
		album_from(&doc, json_at(&doc, arr, i), &albums[(*n_albums)++]);
	}
	done(body, &doc);
	return *n_tracks + *n_albums;
}

int zvuk_search(const char *text, zvuk_search_kind_t kind, void *out, int max) {
	if ((unsigned)kind >= ZVUK_SEARCH_KINDS) {
		return -1;
	}
	char escaped[400], vars[480], *body;
	json_escape(text, escaped, sizeof(escaped));
	snprintf(vars, sizeof(vars), "{\"query\":\"%s\",\"limit\":%d}", escaped, max);
	json_doc_t doc;
	int data;
	if (!gql("search", Q_SEARCH[kind], vars, &body, &doc, &data)) {
		return -1;
	}
	static const char *const PATHS[ZVUK_SEARCH_KINDS] = {"search.tracks.items", "search.releases.items",
														 "search.artists.items", "search.playlists.items"};
	int arr = json_path(&doc, data, PATHS[kind]);
	int got = 0;
	for (int i = 0; i < json_len(&doc, arr) && got < max; i++) {
		int obj = json_at(&doc, arr, i);
		if (obj < 0 || doc.toks[obj].type != JSON_OBJECT) {
			continue;
		}
		switch (kind) {
		case ZVUK_SEARCH_TRACKS:
			track_from(&doc, obj, -1, &((zvuk_track_t *)out)[got]);
			break;
		case ZVUK_SEARCH_ALBUMS:
			album_from(&doc, obj, &((zvuk_album_t *)out)[got]);
			break;
		case ZVUK_SEARCH_ARTISTS:
			artist_from(&doc, obj, &((zvuk_artist_t *)out)[got]);
			break;
		default:
			playlist_from(&doc, obj, &((zvuk_playlist_t *)out)[got]);
			break;
		}
		got++;
	}
	done(body, &doc);
	return got;
}

bool zvuk_like(zvuk_item_t kind, const char *id, bool like) {
	if ((unsigned)kind >= ZVUK_ITEM_KINDS || !id || !id[0]) {
		return false;
	}
	char vars[96], *body;
	snprintf(vars, sizeof(vars), "{\"id\":\"%.*s\",\"type\":\"%s\"}", ZVUK_ID_MAX - 1, id, ITEM_TYPES[kind]);
	json_doc_t doc;
	int data;
	bool ok = gql(like ? "addItemToCollection" : "removeItemFromCollection", like ? Q_ADD : Q_REMOVE, vars, &body,
				  &doc, &data);
	if (ok) {
		ok = json_path(&doc, data, like ? "collection.addItem" : "collection.removeItem") >= 0;
		done(body, &doc);
	}
	printf("zvuk: %s %s %s: %s\n", like ? "like" : "unlike", ITEM_TYPES[kind], id, ok ? "done" : last_error);
	if (ok) {
		zvuk_liked_mark(kind, id, like);
	}
	return ok;
}

// ---------------------------------------------------------------------------
// the file
// ---------------------------------------------------------------------------

static const char *const QUALITY_NAMES[ZVUK_QUALITY_COUNT] = {"flac", "high", "mid"};
static const char *const QUALITY_SHOWN[ZVUK_QUALITY_COUNT] = {"FLAC", "MP3 320 kbps", "MP3 128 kbps"};

zvuk_quality_t zvuk_get_quality(void) {
	const char *v = config_get("zvuk", "quality", "flac");
	for (int q = 0; q < ZVUK_QUALITY_COUNT; q++) {
		if (strcmp(v, QUALITY_NAMES[q]) == 0) {
			return (zvuk_quality_t)q;
		}
	}
	return ZVUK_QUALITY_FLAC;
}

void zvuk_set_quality(zvuk_quality_t quality) {
	if ((unsigned)quality < ZVUK_QUALITY_COUNT) {
		config_set("zvuk", "quality", QUALITY_NAMES[quality]);
		config_save();
	}
}

void zvuk_last_format(char *out, size_t size) {
	pthread_mutex_lock(&format_lock);
	snprintf(out, size, "%s", last_format);
	pthread_mutex_unlock(&format_lock);
}

static void host_of(const char *url, char *out, size_t size) {
	const char *p = strstr(url, "://");
	p = p ? p + 3 : url;
	snprintf(out, size, "%.*s", (int)strcspn(p, "/?:"), p);
}

bool zvuk_stream(const char *track_id, zvuk_stream_t *out) {
	memset(out, 0, sizeof(*out));
	out->asked = zvuk_get_quality();
	char vars[96], *body;
	snprintf(vars, sizeof(vars), "{\"ids\":[\"%.*s\"]}", ZVUK_ID_MAX - 1, track_id);
	json_doc_t doc;
	int data;
	bool diag = diag_thread || session_fresh;
	if (!gql("getStream", Q_STREAM, vars, &body, &doc, &data)) {
		return false;
	}
	int stream = json_path(&doc, data, "mediaContents.0.stream");
	char urls[ZVUK_QUALITY_COUNT][1024] = {"", "", ""};
	char drm[16] = "", expire[48] = "";
	if (stream >= 0) {
		json_obj_str(&doc, stream, "flac", urls[ZVUK_QUALITY_FLAC], sizeof(urls[0]));
		json_obj_str(&doc, stream, "high", urls[ZVUK_QUALITY_HIGH], sizeof(urls[0]));
		json_obj_str(&doc, stream, "mid", urls[ZVUK_QUALITY_MID], sizeof(urls[0]));
		json_obj_str(&doc, stream, "flacdrm", drm, sizeof(drm)); // only whether there is one
		json_obj_str(&doc, stream, "expire", expire, sizeof(expire));
		out->expires_in = json_obj_long(&doc, stream, "expireDelta", 0);
	}
	done(body, &doc);
	// The best at or below the ceiling; flacdrm is never one (DRM).
	out->got = ZVUK_QUALITY_COUNT;
	for (int q = out->asked; q < ZVUK_QUALITY_COUNT && out->got == ZVUK_QUALITY_COUNT; q++) {
		if (strncmp(urls[q], "http", 4) == 0) {
			out->got = (zvuk_quality_t)q;
		}
	}
	bool any = out->got != ZVUK_QUALITY_COUNT;
	char host[96] = "";
	if (any) {
		snprintf(out->url, sizeof(out->url), "%s", urls[out->got]);
		host_of(out->url, host, sizeof(host));
	}
	// Asked versus got, every time: the one line that says whether the
	// subscription and FLAC are there.
	printf("zvuk: stream %s: asked %s, got %s (flac %s, flacdrm %s, high %s, mid %s), host %s, expires in %ld s%s%s\n",
		   track_id, QUALITY_NAMES[out->asked], any ? QUALITY_NAMES[out->got] : "nothing",
		   urls[0][0] ? "yes" : "no", drm[0] ? "yes" : "no", urls[1][0] ? "yes" : "no", urls[2][0] ? "yes" : "no",
		   host[0] ? host : "-", out->expires_in, expire[0] ? ", expire " : "", expire);
	if (diag && !urls[0][0] && drm[0]) {
		printf("zvuk: diag only DRM FLAC (flacdrm) for %s: skipped, MP3 instead\n", track_id);
	}
	if (!any) {
		set_error("%s", tr("zvuk_no_file"));
		streamerr_hint(STREAM_ERR_OTHER); // the service's own sentence says it best
		return false;
	}
	if (out->got == ZVUK_QUALITY_MID && out->asked != ZVUK_QUALITY_MID) {
		needs_subscription = true;
	} else if (out->got != ZVUK_QUALITY_MID) {
		needs_subscription = false;
	}
	pthread_mutex_lock(&format_lock);
	snprintf(last_format, sizeof(last_format), "%s", QUALITY_SHOWN[out->got]);
	pthread_mutex_unlock(&format_lock);
	return true;
}

// ---------------------------------------------------------------------------
// the developer options' check, and the probe after a sign-in
// ---------------------------------------------------------------------------

// The first bytes of the file behind a stream address: whether the CDN lets
// the firmware through as well (Music Assistant reports the anti-bot filter
// refusing direct links at times), and what the file is -- "ftyp" is FLAC in
// MP4, "fLaC" a native FLAC, "ID3" or a frame sync an MP3. Logged; `out` gets
// a short form for the check's sheet. Worker thread.
static void probe_file(const char *url, char *out, size_t size) {
	out[0] = '\0';
	http_stream_t *st = malloc(sizeof(*st));
	if (!st) {
		return;
	}
	char host[96];
	host_of(url, host, sizeof(host));
	http_set_thread_stream_headers(browser_headers);
	bool open = http_stream_open_range(st, url, 0, ZVUK_TIMEOUT_SECS);
	http_set_thread_stream_headers(NULL);
	if (!open) {
		const char *why = http_last_error();
		printf("zvuk: diag file at %s -> refused (%s)\n", host, why && why[0] ? why : "no answer");
		snprintf(out, size, "%s", why && why[0] ? why : "-");
		free(st);
		return;
	}
	unsigned char head[16] = {0};
	int got = 0;
	while (got < (int)sizeof(head)) {
		int n = http_stream_read(st, head + got, (int)sizeof(head) - got);
		if (n <= 0) {
			break;
		}
		got += n;
	}
	long length = st->total_length ? st->total_length : st->content_length;
	http_stream_close(st);
	free(st);
	const char *kind = "unknown";
	if (got >= 8 && memcmp(head + 4, "ftyp", 4) == 0) {
		kind = "MP4 (FLAC in MP4)";
	} else if (got >= 4 && memcmp(head, "fLaC", 4) == 0) {
		kind = "native FLAC";
	} else if ((got >= 3 && memcmp(head, "ID3", 3) == 0) || (got >= 2 && head[0] == 0xFF && (head[1] & 0xE0) == 0xE0)) {
		kind = "MP3";
	} else if (got >= 1 && head[0] == '<') {
		kind = "HTML (blocked?)";
	}
	char hex[3 * sizeof(head) + 1] = "";
	for (int i = 0; i < got; i++) {
		snprintf(hex + 3 * i, sizeof(hex) - 3 * (size_t)i, "%02x ", head[i]);
	}
	printf("zvuk: diag file at %s -> %ld bytes, starts %s= %s\n", host, length, hex, kind);
	snprintf(out, size, "%s, %ld KB", kind, length / 1024);
}

// After a token is accepted: one getStream and the first bytes of its file,
// in the log only. What the owner's first device log must answer is whether
// the whole way -- profile, GraphQL, CDN -- passes the anti-bot filter with
// the firmware's TLS; a sign-in is when it is asked for the first time.
static void probe_after_sign_in(void) {
	const char *id = config_get("zvuk", "check_track", "");
	if (!id[0]) {
		id = CHECK_TRACK_DEFAULT;
	}
	zvuk_stream_t s;
	if (zvuk_stream(id, &s)) {
		char file[96];
		probe_file(s.url, file, sizeof(file));
	} else {
		printf("zvuk: diag getStream %s after the sign-in failed: %s\n", id, last_error);
	}
}

bool zvuk_check_connection(char *report, size_t size) {
	size_t used = 0;
#define SAY(...) used += (size_t)snprintf(report + used, used < size ? size - used : 0, __VA_ARGS__)
	report[0] = '\0';
	if (!zvuk_logged_in()) {
		SAY("%s", tr("zvuk_check_no_token"));
		return false;
	}
	diag_thread = true;
	printf("zvuk: connection check, base %s\n", base_url);
	bool ok = zvuk_refresh_profile();
	if (ok) {
		char name[ZVUK_NAME_MAX], p[64];
		zvuk_account(name, sizeof(name), p, sizeof(p), NULL);
		SAY(tr("zvuk_check_profile_ok"), name[0] ? name : "-", p[0] ? p : "-");
	} else {
		SAY(tr("zvuk_check_profile_failed"), last_error);
	}
	SAY("\n");
	// The profile may be blocked while GraphQL is not: the rest is tried
	// either way, while the token is still there. The collection first -- a
	// GraphQL read of the account's own, and a liked track to ask the stream
	// of, which the account can certainly play (a catalogue id may be
	// blocked in the region).
	if (zvuk_logged_in()) {
		if (zvuk_collection_refresh()) {
			pthread_mutex_lock(&liked_lock);
			int count = liked[ZVUK_ITEM_TRACK].count;
			pthread_mutex_unlock(&liked_lock);
			SAY(tr("zvuk_check_collection_ok"), count);
		} else {
			SAY(tr("zvuk_check_collection_failed"), last_error);
			ok = false;
		}
		SAY("\n");
	}
	if (zvuk_logged_in()) {
		const char *id = config_get("zvuk", "check_track", "");
		char liked_id[ZVUK_ID_MAX] = "";
		pthread_mutex_lock(&liked_lock);
		if (!id[0] && liked[ZVUK_ITEM_TRACK].count > 0) {
			snprintf(liked_id, sizeof(liked_id), "%llu", (unsigned long long)liked[ZVUK_ITEM_TRACK].ids[0]);
		}
		pthread_mutex_unlock(&liked_lock);
		if (!id[0]) {
			id = liked_id[0] ? liked_id : CHECK_TRACK_DEFAULT;
		}
		zvuk_stream_t s;
		if (zvuk_stream(id, &s)) {
			char host[96];
			host_of(s.url, host, sizeof(host));
			SAY(tr("zvuk_check_stream_ok"), id, QUALITY_SHOWN[s.got], host, s.expires_in);
			char file[96];
			probe_file(s.url, file, sizeof(file));
			SAY("\n");
			SAY(tr("zvuk_check_file"), file[0] ? file : "-");
		} else {
			SAY(tr("zvuk_check_stream_failed"), id, last_error);
			ok = false;
		}
	}
	diag_thread = false;
	printf("zvuk: connection check: %s\n", ok ? "passed" : "failed");
#undef SAY
	return ok;
}

#endif /* SONIX_ZVUK */

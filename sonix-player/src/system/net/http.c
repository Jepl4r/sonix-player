#define _GNU_SOURCE 1

#include "http.h"

#include "src/system/net/tls.h"
#include "src/system/core/lang.h"

#include <stdarg.h>

#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <resolv.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// The last failure, for the caller to show
// ---------------------------------------------------------------------------
//
// Per thread, because there are two independent users: the radio's directory
// worker and its playback thread, and one must not overwrite the other's
// reason. Only failures that a person could act on are recorded -- a wrong
// clock, a missing certificate bundle -- because the point is a message worth
// putting on the screen, not a log line.

static __thread char last_error[192];

// URL of the last hop of this thread's last successful request; see
// http_last_final_url() in http.h.
static __thread char last_final_url[2600];

const char *http_last_final_url(void) { return last_final_url; }

static void clear_last_error(void) {
	last_error[0] = '\0';
}

static void set_last_error(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(last_error, sizeof(last_error), fmt, ap);
	va_end(ap);
}

const char *http_last_error(void) {
	return last_error[0] ? last_error : NULL;
}

// ---------------------------------------------------------------------------
// URL handling
// ---------------------------------------------------------------------------

typedef struct {
	char host[256];
	char path[2048]; // always starts with '/'
	int port;
	bool secure; // https://
} url_t;

static bool url_parse(const char *url, url_t *out) {
	if (!url) {
		return false;
	}

	const char *p;
	if (strncasecmp(url, "http://", 7) == 0) {
		out->secure = false;
		out->port = 80;
		p = url + 7;
	} else if (strncasecmp(url, "https://", 8) == 0) {
		out->secure = true;
		out->port = 443;
		p = url + 8;
	} else {
		return false;
	}

	const char *slash = strchr(p, '/');
	const char *hostend = slash ? slash : p + strlen(p);

	// An authority may carry user:pass@ -- radio stations occasionally do.
	const char *at = memchr(p, '@', (size_t)(hostend - p));
	if (at) {
		p = at + 1;
	}

	const char *colon = memchr(p, ':', (size_t)(hostend - p));
	size_t host_len = (size_t)((colon ? colon : hostend) - p);
	if (host_len == 0 || host_len >= sizeof(out->host)) {
		return false;
	}
	memcpy(out->host, p, host_len);
	out->host[host_len] = '\0';

	if (colon) {
		out->port = atoi(colon + 1);
		if (out->port <= 0 || out->port > 65535) {
			return false;
		}
	}

	// Never request a truncated path. Tidal segment URLs carry a signed query
	// (Policy, Signature, Key-Pair-Id) well past five hundred characters, and
	// cutting the tail yields a URL the CDN rejects with 403 -- a failure that
	// looks like a subscription problem from outside rather than a short
	// buffer. Fail loudly instead.
	const char *path = slash ? slash : "/";
	if (strlen(path) >= sizeof(out->path)) {
		fprintf(stderr, "http: address too long (%zu characters of path)\n", strlen(path));
		return false;
	}
	snprintf(out->path, sizeof(out->path), "%s", path);
	return true;
}

char *http_url_encode(const char *in, char *out, size_t out_size) {
	static const char HEX[] = "0123456789ABCDEF";
	size_t o = 0;
	for (size_t i = 0; in && in[i] && o + 4 < out_size; i++) {
		unsigned char c = (unsigned char)in[i];
		bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
					c == '_' || c == '.' || c == '~';
		if (safe) {
			out[o++] = (char)c;
		} else {
			out[o++] = '%';
			out[o++] = HEX[c >> 4];
			out[o++] = HEX[c & 0x0f];
		}
	}
	if (out_size > 0) {
		out[o < out_size ? o : out_size - 1] = '\0';
	}
	return out;
}

// ---------------------------------------------------------------------------
// connecting
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// The resolver and a network that arrives late
//
// glibc reads /etc/resolv.conf once, on the process's first lookup, and keeps
// what it found for the life of the process. This one is 2.22, which predates
// the version that re-reads the file when its timestamp moves.
//
// The DHCP client writes that file when Wi-Fi associates, so any lookup made
// before the radio is up caches "no nameservers" permanently, and every later
// request fails to resolve. Watching the file closes that: when its timestamp
// or size has moved, res_init() drops the cached configuration and the next
// getaddrinfo() reads it again.
static void dns_follow_resolv_conf(void) {
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	static time_t seen_mtime;
	static off_t seen_size;
	static bool seen;

	struct stat st;
	if (stat("/etc/resolv.conf", &st) != 0) {
		return; // no file to follow; nothing cached from it either
	}

	pthread_mutex_lock(&lock);
	bool changed = !seen || st.st_mtime != seen_mtime || st.st_size != seen_size;
	if (changed) {
		seen = true;
		seen_mtime = st.st_mtime;
		seen_size = st.st_size;
	}
	pthread_mutex_unlock(&lock);

	if (changed) {
		// The first sight counts too: something else in the player -- the clock,
		// a service check -- may have resolved a name before this function ever
		// ran, and it is that lookup which pins the wrong configuration.
		res_init();
		fprintf(stderr, "http: /etc/resolv.conf has moved; the resolver reads it again\n");
	}
}

// A socket with both directions bounded by `timeout_secs`. The receive timeout
// is what keeps a station that goes quiet from holding the radio thread: the
// read comes back EAGAIN and the caller can give up.
static int connect_to(const char *host, int port, int timeout_secs) {
	char service[16];
	snprintf(service, sizeof(service), "%d", port);

	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET; // the player has no IPv6 route in practice
	hints.ai_socktype = SOCK_STREAM;

	dns_follow_resolv_conf();

	struct addrinfo *list = NULL;
	if (getaddrinfo(host, service, &hints, &list) != 0 || !list) {
		// Every failure path here records a host and a reason, so that broken
		// DNS, a downed network and a dead server do not all reach the caller
		// as the same generic "cannot reach X".
		set_last_error(tr("http_dns_failed"), host);
		return -1;
	}

	int fd = -1;
	for (struct addrinfo *a = list; a; a = a->ai_next) {
		fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
		if (fd < 0) {
			continue;
		}

		struct timeval tv = {.tv_sec = timeout_secs, .tv_usec = 0};
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

		if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) {
			break;
		}
		close(fd);
		fd = -1;
	}

	freeaddrinfo(list);
	if (fd < 0) {
		// The address resolved but the connection did not: a different fault
		// from DNS, and worth its own message.
		set_last_error(tr("http_connect_failed"), host, port);
	}
	return fd;
}

// ---------------------------------------------------------------------------
// One connection, plain or wrapped
// ---------------------------------------------------------------------------
//
// Everything below reads and writes through this instead of recv()/send()
// directly, so that the framing code (chunked, content-length, close-delimited,
// ICY) exists once and serves plain and TLS connections alike.

typedef struct {
	int fd;
	tls_conn_t *tls; // NULL on a plain http:// connection
} conn_t;

static int conn_read(conn_t *c, void *buf, size_t len) {
	if (c->tls) {
		return tls_read(c->tls, buf, (int)len);
	}
	ssize_t n = recv(c->fd, buf, len, 0);
	return n < 0 ? -1 : (int)n;
}

static bool conn_write_all(conn_t *c, const char *data, size_t len) {
	size_t sent = 0;
	while (sent < len) {
		int n;
		if (c->tls) {
			n = tls_write(c->tls, data + sent, (int)(len - sent));
		} else {
			ssize_t r = send(c->fd, data + sent, len - sent, MSG_NOSIGNAL);
			n = r <= 0 ? -1 : (int)r;
		}
		if (n <= 0) {
			return false;
		}
		sent += (size_t)n;
	}
	return true;
}

static void conn_close(conn_t *c) {
	if (c->tls) {
		tls_free(c->tls);
		c->tls = NULL;
	}
	if (c->fd >= 0) {
		close(c->fd);
		c->fd = -1;
	}
}

// Connects and, for an https:// URL, completes the TLS handshake. `err` gets
// the reason a handshake failed, which is worth carrying up: "expired
// certificate" almost always means the device clock is wrong, and that is
// something the user can fix.
static bool conn_open(conn_t *c, const url_t *u, int timeout_secs, char *err, size_t err_size) {
	c->fd = -1;
	c->tls = NULL;
	if (err && err_size) {
		err[0] = 0;
	}

	c->fd = connect_to(u->host, u->port, timeout_secs);
	if (c->fd < 0) {
		fprintf(stderr, "http: cannot reach %s:%d (%s)\n", u->host, u->port,
				http_last_error() ? http_last_error() : "?");
		return false;
	}

	if (!u->secure) {
		return true;
	}

	c->tls = tls_client(c->fd, u->host, err, err_size);
	if (!c->tls) {
		fprintf(stderr, "http: TLS to %s failed: %s\n", u->host, (err && err[0]) ? err : "?");
		if (err && err[0]) {
			set_last_error("%s", err);
		}
		close(c->fd);
		c->fd = -1;
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// response headers
// ---------------------------------------------------------------------------

// Reads until the blank line that ends the headers. Anything read past it is
// body and is handed back in `body`/`body_len` -- with a stream there is no
// way to put it back on the socket.
//
// Returns the status code, or -1.
static int read_headers(conn_t *c, char *headers, size_t headers_size, char *body, size_t body_size,
						int *body_len) {
	size_t used = 0;
	*body_len = 0;

	while (used + 1 < headers_size) {
		int n = conn_read(c, headers + used, headers_size - used - 1);
		if (n <= 0) {
			return -1;
		}
		used += (size_t)n;
		headers[used] = '\0';

		char *end = strstr(headers, "\r\n\r\n");
		size_t skip = 4;
		if (!end) {
			// Some shoutcast servers separate with bare newlines.
			end = strstr(headers, "\n\n");
			skip = 2;
		}
		if (!end) {
			continue;
		}

		size_t header_len = (size_t)(end - headers) + skip;
		size_t extra = used - header_len;
		if (extra > body_size) {
			// Silently dropping these would desynchronise everything that
			// follows. The caller's buffer is sized so this cannot happen;
			// if it ever does, say so rather than corrupt the stream.
			return -1;
		}
		memcpy(body, headers + header_len, extra);
		*body_len = (int)extra;
		headers[header_len] = '\0';
		break;
	}

	// "HTTP/1.1 200 OK", or shoutcast's own "ICY 200 OK".
	const char *space = strchr(headers, ' ');
	if (!space) {
		return -1;
	}
	return atoi(space + 1);
}

// Copies the value of `name` (case-insensitive, without the trailing CR) out
// of a header block. Returns false when the header is not there.
static bool header_value(const char *headers, const char *name, char *out, size_t out_size) {
	size_t name_len = strlen(name);
	const char *line = headers;

	while (line && *line) {
		if (strncasecmp(line, name, name_len) == 0 && line[name_len] == ':') {
			const char *v = line + name_len + 1;
			while (*v == ' ' || *v == '\t') {
				v++;
			}
			size_t len = strcspn(v, "\r\n");
			if (len >= out_size) {
				len = out_size - 1;
			}
			memcpy(out, v, len);
			out[len] = '\0';
			return true;
		}
		line = strchr(line, '\n');
		if (line) {
			line++;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// http_get
// ---------------------------------------------------------------------------

// A growing byte buffer for the response body.
typedef struct {
	char *data;
	size_t used;
	size_t cap;
	size_t limit;
} body_t;

static bool body_reserve(body_t *b, size_t extra) {
	if (b->used + extra + 1 <= b->cap) {
		return true;
	}
	if (b->cap >= b->limit) {
		return false; // at the ceiling: truncate rather than eat the heap
	}

	size_t want = b->cap ? b->cap : 16 * 1024;
	while (want < b->used + extra + 1) {
		want *= 2;
		if (want >= b->limit) {
			want = b->limit;
			break;
		}
	}

	char *grown = realloc(b->data, want + 1);
	if (!grown) {
		return false;
	}
	b->data = grown;
	b->cap = want;
	return true;
}

static bool body_append(body_t *b, const char *data, size_t len) {
	if (!body_reserve(b, len)) {
		return false;
	}
	size_t room = b->cap - b->used;
	if (len > room) {
		len = room;
	}
	memcpy(b->data + b->used, data, len);
	b->used += len;
	return true;
}

// Reads the rest of the body when the server framed it by closing the
// connection (HTTP/1.0, or Connection: close with no length).
static void read_until_close(conn_t *c, body_t *b) {
	char buf[8192];
	for (;;) {
		int got = conn_read(c, buf, sizeof(buf));
		if (got <= 0) {
			return;
		}
		if (!body_append(b, buf, (size_t)got)) {
			return;
		}
	}
}

// The same, for a body of a known length.
//
// Returns false when less arrived than the server promised. The distinction
// matters: a Tidal segment cut in half by a Wi-Fi hiccup and accepted as good
// becomes a gap mid-track, inside a file then marked complete and never
// re-downloaded.
static bool read_length(conn_t *c, body_t *b, size_t content_length) {
	char buf[8192];
	while (b->used < content_length) {
		size_t want = content_length - b->used;
		if (want > sizeof(buf)) {
			want = sizeof(buf);
		}
		int got = conn_read(c, buf, want);
		if (got <= 0) {
			return false;
		}
		if (!body_append(b, buf, (size_t)got)) {
			// Not a failure: body_append refuses once the caller's own limit
			// is reached, which is deliberate truncation, not a broken
			// response. The radio relies on it -- a playlist larger than its
			// limit is still usable because the URL it wants is at the top.
			// Only the case above is a failure: the server stopped short of
			// what it promised.
			return true;
		}
	}
	return true;
}

// A tiny reader over "whatever is already in the buffer, then the socket",
// used to unpack a chunked body without a second copy of the framing logic.
typedef struct {
	conn_t *conn;
	const char *lead;
	size_t lead_len;
	size_t lead_pos;
} source_t;

static int source_read(source_t *src, char *out, size_t len) {
	if (src->lead_pos < src->lead_len) {
		size_t take = src->lead_len - src->lead_pos;
		if (take > len) {
			take = len;
		}
		memcpy(out, src->lead + src->lead_pos, take);
		src->lead_pos += take;
		return (int)take;
	}
	int got = conn_read(src->conn, out, len);
	return got <= 0 ? 0 : got;
}

static bool source_read_exact(source_t *src, char *out, size_t len) {
	size_t got = 0;
	while (got < len) {
		int n = source_read(src, out + got, len - got);
		if (n <= 0) {
			return false;
		}
		got += (size_t)n;
	}
	return true;
}

// Reads one CRLF-terminated line (without the terminator) into `out`.
static bool source_read_line(source_t *src, char *out, size_t out_size) {
	size_t n = 0;
	for (;;) {
		char c;
		if (!source_read_exact(src, &c, 1)) {
			return false;
		}
		if (c == '\n') {
			while (n > 0 && (out[n - 1] == '\r')) {
				n--;
			}
			out[n] = '\0';
			return true;
		}
		if (n + 1 < out_size) {
			out[n++] = c;
		}
	}
}

// Transfer-Encoding: chunked. Each chunk is a hex length, CRLF, that many
// bytes, CRLF; a zero length ends the body.
//
// The API's own answers are not chunked, but a proxy in front of one of the
// mirrors may chunk a long reply -- and a long reply is exactly what a list of
// stations is.
static void read_chunked(source_t *src, body_t *b) {
	char line[64];
	for (;;) {
		if (!source_read_line(src, line, sizeof(line))) {
			return;
		}

		// The length may carry chunk extensions after a ';'.
		char *semi = strchr(line, ';');
		if (semi) {
			*semi = '\0';
		}
		long len = strtol(line, NULL, 16);
		if (len <= 0) {
			return; // the last chunk (trailers, if any, are of no interest)
		}

		char buf[8192];
		size_t left = (size_t)len;
		while (left > 0) {
			size_t want = left > sizeof(buf) ? sizeof(buf) : left;
			if (!source_read_exact(src, buf, want)) {
				return;
			}
			if (!body_append(b, buf, want)) {
				return;
			}
			left -= want;
		}

		char crlf[2];
		if (!source_read_exact(src, crlf, 2)) {
			return;
		}
	}
}

// The Host header must carry the port when it is not the scheme's default
// (RFC 7230 5.4). Without it a server on a non-standard port does not know
// which address it answers for, and anything that builds a URL from Host (an
// API returning a file link, an absolute redirect) builds it on port 80, where
// it is not listening.
static void host_header_value(const url_t *u, char *out, size_t size) {
	bool standard = (u->secure && u->port == 443) || (!u->secure && u->port == 80);
	if (standard) {
		snprintf(out, size, "%s", u->host);
	} else {
		snprintf(out, size, "%s:%d", u->host, u->port);
	}
}

// One request/response, no redirect handling. `location` is filled when the
// status is a redirect.
static bool get_once(const url_t *u, const http_req_t *req, char **out, size_t *out_len, size_t limit,
					 int timeout_secs, int *status_out, char *location, size_t location_size, char *err,
					 size_t err_size) {
	const char *method = req->method && req->method[0] ? req->method : "GET";
	const char *extra_headers = req->extra_headers;
	bool want_error_body = req->want_error_body;

	size_t body_len = 0;
	if (req->body) {
		body_len = req->body_len ? req->body_len : strlen(req->body);
	}

	conn_t conn;
	if (!conn_open(&conn, u, timeout_secs, err, err_size)) {
		return false;
	}

	// HTTP/1.0: a 1.0 request may not be answered with a chunked body, so the
	// common case stays simple. Chunked is handled below anyway, because
	// "may not" and "does not" are different things once a proxy is involved.
	// The extra headers are for Qobuz, which wants the app credentials there.
	// They arrive already CRLF-terminated, one per line.
	char host_header[300];
	host_header_value(u, host_header, sizeof(host_header));

	// A body needs two more headers. Content-Length always, even at zero:
	// without it a server expecting a body waits until its own timeout, and
	// this is HTTP/1.0 where there is no other way to say where the body ends.
	char body_headers[128];
	body_headers[0] = '\0';
	if (req->body) {
		snprintf(body_headers, sizeof(body_headers), "Content-Type: %s\r\nContent-Length: %zu\r\n",
				 req->content_type && req->content_type[0] ? req->content_type
														   : "application/x-www-form-urlencoded",
				 body_len);
	}

	char request[4096];
	int n = snprintf(request, sizeof(request),
					 "%s %s HTTP/1.0\r\n"
					 "Host: %s\r\n"
					 "User-Agent: " HTTP_USER_AGENT "\r\n"
					 "Accept: */*\r\n"
					 "Accept-Encoding: identity\r\n"
					 "Connection: close\r\n"
					 "%s"
					 "%s"
					 "\r\n",
					 method, u->path, host_header, body_headers, extra_headers ? extra_headers : "");
	// Compare the length against the buffer, not just against zero: snprintf
	// truncates but returns what it would have written, so an over-long
	// request would put bytes from past the end of the stack on the wire. It
	// fits with room today; this check keeps it fitting when someone adds a
	// longer header.
	if (n <= 0 || (size_t)n >= sizeof(request) || !conn_write_all(&conn, request, (size_t)n)) {
		conn_close(&conn);
		return false;
	}

	// The body goes out as a separate write rather than concatenated into
	// `request`: a long body would overflow that buffer silently, and snprintf
	// would truncate a request that has already declared its length.
	if (body_len > 0 && !conn_write_all(&conn, req->body, body_len)) {
		conn_close(&conn);
		return false;
	}

	char headers[HTTP_HEADER_BUFFER];
	char lead[HTTP_HEADER_BUFFER];
	int lead_len = 0;
	int status = read_headers(&conn, headers, sizeof(headers), lead, sizeof(lead), &lead_len);
	*status_out = status;

	if (status >= 300 && status < 400) {
		location[0] = '\0';
		header_value(headers, "Location", location, location_size);
		conn_close(&conn);
		return false; // the caller decides whether to follow
	}
	// An API's 4xx is not silence: the body carries the reason, and that is
	// what to show instead of "it did not work". Callers opt in.
	// Any 2xx is fine for callers that allow a missing body: a DELETE's
	// success is read from the status code, and that code is 204.
	bool ok_status = status == 200 || (req->allow_empty_body && status >= 200 && status < 300);
	if (!ok_status && !(want_error_body && status >= 400 && status < 500)) {
		fprintf(stderr, "http: %s%s -> status %d\n", u->host, u->path, status);
		// The status goes into last_error as well as the log, so that a server
		// that answered by saying no is not reported to the caller as silence.
		if (!last_error[0]) {
			set_last_error(tr("http_server_answered"), status);
		}
		conn_close(&conn);
		return false;
	}

	body_t body = {.limit = limit};

	char encoding[64];
	bool chunked = header_value(headers, "Transfer-Encoding", encoding, sizeof(encoding)) &&
				   strcasestr(encoding, "chunked") != NULL;

	if (chunked) {
		source_t src = {.conn = &conn, .lead = lead, .lead_len = (size_t)lead_len, .lead_pos = 0};
		read_chunked(&src, &body);
	} else {
		if (lead_len > 0 && !body_append(&body, lead, (size_t)lead_len)) {
			conn_close(&conn);
			free(body.data);
			return false;
		}

		char length_text[32];
		if (header_value(headers, "Content-Length", length_text, sizeof(length_text))) {
			if (!read_length(&conn, &body, (size_t)strtoul(length_text, NULL, 10))) {
				// Less than promised: the response is broken, not short.
				// Delivering it anyway means half a JPEG or, worse, an audio
				// segment with a hole in it, accepted as good and cached.
				fprintf(stderr, "http: %s%s -> body cut short\n", u->host, u->path);
				conn_close(&conn);
				free(body.data);
				return false;
			}
		} else {
			read_until_close(&conn, &body);
		}
	}

	conn_close(&conn);

	if (!body.data) {
		// An empty 200 is not an answer to anything asked here, except for
		// callers that allow it: those get an allocated empty string rather
		// than NULL, so the caller always free()s exactly once.
		if (req->allow_empty_body) {
			body.data = calloc(1, 1);
			if (!body.data) {
				return false;
			}
			body.used = 0;
		} else {
			fprintf(stderr, "http: %s%s -> %d with no body\n", u->host, u->path, status);
			return false;
		}
	}
	body.data[body.used] = '\0';

	*out = body.data;
	if (out_len) {
		*out_len = body.used;
	}
	return true;
}

bool http_get(const char *url, char **out, size_t *out_len, size_t limit, int timeout_secs) {
	return http_get_ex(url, NULL, false, out, out_len, limit, timeout_secs, NULL);
}

bool http_get_ex(const char *url, const char *extra_headers, bool want_error_body, char **out, size_t *out_len,
				 size_t limit, int timeout_secs, int *status_out) {
	http_req_t req = {.extra_headers = extra_headers, .want_error_body = want_error_body};
	return http_request(url, &req, out, out_len, limit, timeout_secs, status_out);
}

bool http_request(const char *url, const http_req_t *req, char **out, size_t *out_len, size_t limit, int timeout_secs,
				  int *status_out) {
	static const http_req_t plain_get;
	if (!req) {
		req = &plain_get;
	}
	if (!url || !out) {
		return false;
	}
	if (status_out) {
		*status_out = 0;
	}

	clear_last_error();

	char current[2600];
	snprintf(current, sizeof(current), "%s", url);

	// The request changes hop by hop: after a redirect that changes the
	// method, what gets re-sent is a GET with no body.
	http_req_t hop_req = *req;

	for (int hop = 0; hop <= HTTP_MAX_REDIRECTS; hop++) {
		url_t u;
		if (!url_parse(current, &u)) {
			set_last_error(tr("http_bad_address"), current);
			return false;
		}

		int status = 0;
		char location[2048] = "";
		char err[192] = "";
		bool ok = get_once(&u, &hop_req, out, out_len, limit, timeout_secs, &status, location, sizeof(location), err,
						   sizeof(err));
		if (status_out) {
			*status_out = status;
		}
		if (ok) {
			// The URL that actually answered, for callers that need it: after
			// a 302 it is no longer the one asked for, and an HLS manifest's
			// relative URLs resolve against this one.
			snprintf(last_final_url, sizeof(last_final_url), "%s", current);
			return true;
		}
		if (status < 300 || status >= 400 || !location[0]) {
			return false;
		}

		// Always on 303, and on 301/302 for anything that is not GET or HEAD:
		// the target is fetched with a GET and no body. That is what browsers
		// have done for twenty years and what servers assume -- re-sending a
		// POST with its body to an address that just said "look elsewhere"
		// can, at worst, write it twice. 307 and 308 exist to mean "repeat it
		// as-is", so they change nothing here.
		if (status == 303 || ((status == 301 || status == 302) && hop_req.method &&
							  strcmp(hop_req.method, "GET") != 0 && strcmp(hop_req.method, "HEAD") != 0)) {
			hop_req.method = "GET";
			hop_req.body = NULL;
			hop_req.body_len = 0;
			hop_req.content_type = NULL;
		}

		// A relative Location keeps the scheme and authority it came from; an
		// absolute one may switch between http and https, and plenty of hosts
		// answer port 80 with nothing but a redirect to their https self.
		if (location[0] == '/') {
			char base[512];
			snprintf(base, sizeof(base), "%s://%s:%d", u.secure ? "https" : "http", u.host, u.port);
			snprintf(current, sizeof(current), "%s%s", base, location);
		} else {
			snprintf(current, sizeof(current), "%s", location);
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// streaming
// ---------------------------------------------------------------------------

// A conn_t view of a stream, so the reading code below is the same code the
// rest of the file uses.
static conn_t stream_conn(http_stream_t *st) {
	conn_t c = {.fd = st->fd, .tls = (tls_conn_t *)st->tls};
	return c;
}

static bool stream_open_once(http_stream_t *st, const char *url, int timeout_secs, int *status_out,
							 char *location, size_t location_size) {
	url_t u;
	if (!url_parse(url, &u)) {
		return false;
	}

	conn_t conn;
	char err[192] = "";
	if (!conn_open(&conn, &u, timeout_secs, err, sizeof(err))) {
		return false;
	}

	// Nagle off: the request is one small write, and waiting to coalesce it
	// costs a round trip on a link that is already the slow part. (Under TLS
	// the handshake has already happened by now, so this only affects the
	// request itself -- which is exactly what it is for.)
	int one = 1;
	setsockopt(conn.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

	char host_header[300];
	host_header_value(&u, host_header, sizeof(host_header));

	char request[2600];
	int n = snprintf(request, sizeof(request),
					 "GET %s HTTP/1.0\r\n" // 1.0: no chunking, no keep-alive to argue about
					 "Host: %s\r\n"
					 "User-Agent: " HTTP_USER_AGENT "\r\n"
					 "Icy-MetaData: 1\r\n"
					 "Accept: */*\r\n"
					 "\r\n",
					 u.path, host_header);
	// As above: snprintf returns what it would have written, not what it did.
	if (n <= 0 || (size_t)n >= sizeof(request) || !conn_write_all(&conn, request, (size_t)n)) {
		conn_close(&conn);
		return false;
	}

	char headers[HTTP_HEADER_BUFFER];
	int status = read_headers(&conn, headers, sizeof(headers), st->lead, sizeof(st->lead), &st->lead_len);
	*status_out = status;

	if (status >= 300 && status < 400) {
		location[0] = '\0';
		header_value(headers, "Location", location, location_size);
		conn_close(&conn);
		return false;
	}
	if (status != 200) {
		conn_close(&conn);
		return false;
	}

	st->fd = conn.fd;
	st->tls = conn.tls;
	st->lead_pos = 0;

	header_value(headers, "Content-Type", st->content_type, sizeof(st->content_type));

	// The length, when the server states one. A radio station never does (it
	// does not end), a downloaded file does: this is what turns an endless
	// spinner into a progress bar.
	char length_text[32];
	st->content_length = header_value(headers, "Content-Length", length_text, sizeof(length_text))
							 ? strtol(length_text, NULL, 10)
							 : 0;

	header_value(headers, "icy-name", st->icy_name, sizeof(st->icy_name));
	header_value(headers, "icy-genre", st->icy_genre, sizeof(st->icy_genre));

	char metaint[32];
	if (header_value(headers, "icy-metaint", metaint, sizeof(metaint))) {
		st->icy_metaint = atoi(metaint);
		if (st->icy_metaint < 0 || st->icy_metaint > 1024 * 1024) {
			st->icy_metaint = 0;
		}
	}
	st->meta_countdown = st->icy_metaint;

	return true;
}

bool http_stream_open(http_stream_t *st, const char *url, int timeout_secs) {
	if (!st || !url) {
		return false;
	}
	memset(st, 0, sizeof(*st));
	st->fd = -1;
	clear_last_error();

	char current[2600];
	snprintf(current, sizeof(current), "%s", url);

	for (int hop = 0; hop <= HTTP_MAX_REDIRECTS; hop++) {
		int status = 0;
		char location[2048] = "";
		if (stream_open_once(st, current, timeout_secs, &status, location, sizeof(location))) {
			return true;
		}
		if (status < 300 || status >= 400 || !location[0]) {
			return false;
		}

		if (location[0] == '/') {
			url_t u;
			if (!url_parse(current, &u)) {
				return false;
			}
			char base[512];
			snprintf(base, sizeof(base), "%s://%s:%d", u.secure ? "https" : "http", u.host, u.port);
			snprintf(current, sizeof(current), "%s%s", base, location);
		} else {
			snprintf(current, sizeof(current), "%s", location);
		}
	}
	return false;
}

// Reads exactly `len` raw bytes (lead buffer first, then the socket).
// Returns false if the stream ended or errored partway.
static bool read_raw(http_stream_t *st, char *buf, int len) {
	int got = 0;
	while (got < len) {
		if (st->lead_pos < st->lead_len) {
			int take = st->lead_len - st->lead_pos;
			if (take > len - got) {
				take = len - got;
			}
			memcpy(buf + got, st->lead + st->lead_pos, (size_t)take);
			st->lead_pos += take;
			got += take;
			continue;
		}

		conn_t c = stream_conn(st);
		int n = conn_read(&c, buf + got, (size_t)(len - got));
		if (n <= 0) {
			return false;
		}
		got += n;
	}
	return true;
}

// One ICY metadata block: a length byte (in sixteens) then that many bytes of
// "StreamTitle='...';StreamUrl='...';", padded with NULs. A length of zero --
// which is the usual case, most blocks say nothing new -- means nothing
// follows.
static bool consume_metadata(http_stream_t *st) {
	char length_byte;
	if (!read_raw(st, &length_byte, 1)) {
		return false;
	}

	int len = (unsigned char)length_byte * 16;
	if (len == 0) {
		return true;
	}

	char block[16 * 255 + 1];
	if (!read_raw(st, block, len)) {
		return false;
	}
	block[len] = '\0';

	const char *title = strstr(block, "StreamTitle=");
	if (title) {
		title += 12;
		char quote = *title;
		if (quote == '\'' || quote == '"') {
			title++;

			// ICY has no escaping, so the end of the value has to be guessed.
			// The convention every player follows is that a field ends at
			// quote-then-semicolon: taking the first bare quote cuts
			// "Rock 'n' Roll" in half, and taking the last one swallows the
			// StreamUrl field that follows.
			const char *end = title;
			for (;;) {
				const char *q = strchr(end, quote);
				if (!q) {
					end = title + strlen(title);
					break;
				}
				if (q[1] == ';' || q[1] == '\0') {
					end = q;
					break;
				}
				end = q + 1;
			}

			size_t n = (size_t)(end - title);
			if (n >= sizeof(st->stream_title)) {
				n = sizeof(st->stream_title) - 1;
			}
			if (strncmp(st->stream_title, title, n) != 0 || st->stream_title[n] != '\0') {
				memcpy(st->stream_title, title, n);
				st->stream_title[n] = '\0';
				st->title_changed = true;
			}
		}
	}
	return true;
}

int http_stream_read(http_stream_t *st, void *buf, int len) {
	if (!st || st->fd < 0 || len <= 0) {
		return -1;
	}

	// Without ICY metadata every byte on the socket is audio.
	if (st->icy_metaint <= 0) {
		if (st->lead_pos < st->lead_len) {
			int take = st->lead_len - st->lead_pos;
			if (take > len) {
				take = len;
			}
			memcpy(buf, st->lead + st->lead_pos, (size_t)take);
			st->lead_pos += take;
			return take;
		}
		conn_t c = stream_conn(st);
		int n = conn_read(&c, buf, (size_t)len);
		return n < 0 ? -1 : n;
	}

	// With it, hand back at most what is left before the next metadata block.
	if (st->meta_countdown == 0) {
		if (!consume_metadata(st)) {
			return 0;
		}
		st->meta_countdown = st->icy_metaint;
	}

	int want = len < st->meta_countdown ? len : st->meta_countdown;

	int got = 0;
	if (st->lead_pos < st->lead_len) {
		got = st->lead_len - st->lead_pos;
		if (got > want) {
			got = want;
		}
		memcpy(buf, st->lead + st->lead_pos, (size_t)got);
		st->lead_pos += got;
	} else {
		conn_t c = stream_conn(st);
		int n = conn_read(&c, buf, (size_t)want);
		if (n <= 0) {
			return n == 0 ? 0 : -1;
		}
		got = n;
	}

	st->meta_countdown -= got;
	return got;
}

void http_stream_wake(http_stream_t *st) {
	if (st && st->fd >= 0) {
		shutdown(st->fd, SHUT_RDWR);
	}
}

void http_stream_close(http_stream_t *st) {
	if (!st) {
		return;
	}
	// The TLS session first: it wants to write a close-notify, and it can only
	// do that while the socket is still open.
	if (st->tls) {
		tls_free((tls_conn_t *)st->tls);
		st->tls = NULL;
	}
	if (st->fd >= 0) {
		close(st->fd);
		st->fd = -1;
	}
}

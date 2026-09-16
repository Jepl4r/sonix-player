#ifndef HTTP_H
#define HTTP_H

#include <stdbool.h>
#include <stddef.h>

// A very small HTTP/1.1 client.
//
// http:// goes over a plain socket. https:// goes over the OpenSSL the device
// already ships, opened at run time -- see tls.h for why it is loaded that way
// and where the certificates come from. When OpenSSL or the certificate bundle
// is missing an https:// URL fails with a reason the user can act on, and
// everything http:// carries on working.
//
// A growing share of the stations in the radio directory are https-only.
//
// Two shapes of request:
//
//   http_get()         a whole small response into memory -- the API's JSON.
//   http_stream_open() headers read, body left on the socket -- a radio
//                      stream, which never ends and must not be buffered.
//
// Everything blocks, so nothing here may be called from the UI thread. Both
// calls take a timeout in seconds that bounds the connect and every read, so a
// server that accepts the connection and then says nothing cannot wedge the
// caller's thread forever -- which is exactly what a dead radio station does.

// What the server is told this client is. radio-browser.info asks for a
// descriptive agent of the form appname/version.
#define HTTP_USER_AGENT "SonixPlayer/1.0"

// Fetches `url` into a freshly malloc'd, NUL-terminated buffer. The caller
// frees it. `out_len` may be NULL. Redirects (301/302/303/307/308) are
// followed, up to HTTP_MAX_REDIRECTS, and may cross between http and https in
// either direction -- which real redirects do constantly.
//
// Returns false on any failure, leaving *out untouched. `limit` caps how much
// body is kept; a longer response is truncated rather than allowed to eat the
// heap.
#define HTTP_MAX_REDIRECTS 5
bool http_get(const char *url, char **out, size_t *out_len, size_t limit, int timeout_secs);

// The same, for callers talking to an API rather than a radio station.
//
//   extra_headers   lines already terminated with "\r\n", one per header, or
//                   NULL. Qobuz wants the app credentials in there.
//   want_error_body true keeps the body of a 4xx instead of discarding it: that
//                   is where an API says why it refused, which is what should
//                   be shown instead of a generic failure. 5xx and empty bodies
//                   remain failures.
//   status_out      the status code, or NULL.
bool http_get_ex(const char *url, const char *extra_headers, bool want_error_body, char **out, size_t *out_len,
				 size_t limit, int timeout_secs, int *status_out);

// Where the request ended up, which is not always where it started: radio CDNs
// answer an HLS manifest with a 302 to an edge (msvdn.net does it every
// session), and the relative URLs inside the manifest resolve against the edge,
// not the requested host. After a successful http_get()/http_request() this
// holds the URL of the last hop -- per thread, like http_last_error(), and
// overwritten by the next request on the same thread, so copy it immediately.
const char *http_last_final_url(void);

// ---------------------------------------------------------------------------
// Requests that are not GET
//
// Qobuz gets by with GET alone -- even adding a favourite is a GET with query
// parameters, which is their choice. Tidal does not: login is a POST with a
// form body, favourites are written with POST and removed with DELETE, and a
// successful DELETE answers 204 with no body.
//
// That last point is why a dedicated flag exists. Everywhere else in the player
// a 200 with no body is a failure -- a radio station that accepts the
// connection and sends nothing is broken, and treating it as success would hand
// the decoder an empty buffer. But 204 is the right answer to removing a
// favourite, and the caller must be able to say so without changing behaviour
// for everyone else.
typedef struct {
	// "GET", "POST", "DELETE". NULL means GET.
	const char *method;

	// Lines already terminated with "\r\n", one per header, or NULL.
	const char *extra_headers;

	// The body, or NULL. `body_len` 0 with a non-NULL body means strlen(),
	// which is the normal case since these bodies are text.
	const char *body;
	size_t body_len;

	// The body's type. NULL with a body present means
	// "application/x-www-form-urlencoded", which is what both Tidal's login
	// endpoint and its writes expect.
	const char *content_type;

	// Keep the body of a 4xx instead of discarding it: that is where the API
	// says why it refused. See http_get_ex().
	bool want_error_body;

	// Read success from the status code rather than the content. Two effects,
	// both needed by the same kind of caller:
	//
	//   * any 2xx counts as success, not just 200. Creating something answers
	//     201, which without this would read as a failure;
	//   * an empty body counts as a response. A successful DELETE answers 204
	//     and sends nothing; and a 4xx with no body -- which happens -- would
	//     otherwise surface as "no response" instead of its code, hiding a 401
	//     from a caller waiting for it to refresh the token.
	//
	// With this set *out is still allocated and terminated: an empty string,
	// never NULL, so the caller always frees the same way.
	bool allow_empty_body;

} http_req_t;

// Like http_get_ex(), but with the method and body chosen by the caller.
// Redirects are followed as usual; a 303, and a 301/302 on a POST, become a GET
// to the new destination, which is what browsers do and what servers expect.
bool http_request(const char *url, const http_req_t *req, char **out, size_t *out_len, size_t limit, int timeout_secs,
				  int *status_out);

// How much is read at once while looking for the end of the headers.
#define HTTP_HEADER_BUFFER 4096

// A stream whose body is read a piece at a time.
typedef struct {
	int fd;

	// The TLS session riding on that fd, or NULL for a plain http:// stream.
	// Opaque here on purpose: http.h is included in places that have no
	// business knowing OpenSSL exists.
	void *tls;

	// From the response headers, when the server sent them.
	char content_type[64]; // e.g. "audio/mpeg"
	char icy_name[128];	   // the station's own name for itself
	char icy_genre[128];
	int icy_metaint; // bytes of audio between metadata blocks, 0 when none

	// Content-Length, or 0 when the server does not send it -- always the case
	// for a radio station, and almost never for a downloaded file.
	long content_length;

	// Body bytes already read while parsing the headers. The first read has
	// to hand these back before touching the socket again.
	//
	// This MUST be at least as big as the buffer the header parser reads
	// into: one TCP segment can carry the headers and several kilobytes of
	// body, and a lead buffer too small to hold the overshoot loses those
	// bytes. On a radio stream that loses an ICY block boundary -- the
	// metadata counter desynchronises and the decoder is handed metadata as if
	// it were audio.
	char lead[HTTP_HEADER_BUFFER];
	int lead_len;
	int lead_pos;

	// Where the ICY metadata counter is within the current block. Only
	// meaningful when icy_metaint > 0.
	int meta_countdown;

	// The last "StreamTitle" the station sent, or empty. Updated in place by
	// http_stream_read(); the caller polls it.
	char stream_title[256];
	bool title_changed;
} http_stream_t;

// Connects, sends the request (asking for ICY metadata), reads the response
// headers and stops there. Returns false if the URL is neither http:// nor
// https://, the host does not resolve, the connection or the TLS handshake
// fails, or the status is not 200.
bool http_stream_open(http_stream_t *st, const char *url, int timeout_secs);

// Reads up to `len` bytes of *audio* into buf: any ICY metadata blocks in the
// way are consumed and parsed out, never handed to the caller. Returns the
// number of bytes read, 0 at end of stream, -1 on error.
int http_stream_read(http_stream_t *st, void *buf, int len);

void http_stream_close(http_stream_t *st);

// Wakes a reader blocked in http_stream_read() from another thread: shuts the
// socket down so the pending recv() returns at once instead of sitting out the
// receive timeout. The stream stays valid (its owner still has to close it);
// every read from here on simply fails.
//
// It works the same under TLS, and for the same reason: SSL_read is sitting in
// a recv() on that fd like everything else, and a shut-down socket ends it.
//
// This is what makes stopping a station immediate: without it a station whose
// server has gone quiet holds its thread for the whole receive timeout.
void http_stream_wake(http_stream_t *st);

// Why the last http_get() or http_stream_open() on THIS thread failed, when
// the reason is one a person could act on -- a device clock so wrong that
// every certificate looks expired, a missing certificate bundle, a name that
// does not match. NULL when there is nothing useful to say, which is the usual
// case for an ordinary network failure.
//
// Per thread on purpose: the radio's directory worker and its playback thread
// both use this file and must not overwrite each other's answer.
const char *http_last_error(void);

// Percent-encodes `in` into `out` for use inside a URL path segment or query
// value. Returns out.
char *http_url_encode(const char *in, char *out, size_t out_size);

#endif /* HTTP_H */

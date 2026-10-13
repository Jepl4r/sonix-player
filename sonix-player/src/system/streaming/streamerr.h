#ifndef STREAMERR_H
#define STREAMERR_H

#include <stdbool.h>
#include <stddef.h>

// Why a streaming request failed, in words a person can act on.
//
// Every service used to hand its page whatever its last error string was, and
// the page put it up as it came: "Server answered 503", "Subsonic answered
// 401", or the reason a TLS handshake failed. True, and no help -- the user
// wants to know whether to check the Wi-Fi, sign in again, or wait. The causes
// worth telling apart are few and the same for every service, so they are
// worked out here, once, from what the request itself recorded (http.h's
// outcome) plus what only the service can know (a Subsonic error code, a
// Yandex account without Plus, application keys the firmware lacks), and
// turned into one sentence per class. The technical detail -- status, host,
// path without its query -- goes to the log instead, which is where it is
// useful.
//
// Thread contract: the record is per thread, like http_last_error(). A page's
// worker calls streamerr_begin() before a job and streamerr_describe() after
// one that failed, on the same thread, with the service's calls in between;
// the service calls streamerr_hint() from inside them. The resulting class and
// text are plain values, handed to the GUI thread like any other result.

// How long a request the interface waits on may take to connect, and then
// between any two pieces of its answer: the cap a streaming page's worker puts
// on its thread (http_set_thread_timeout_cap). Long enough for a slow home
// server on a weak link, short enough that a dead one is said in seconds.
// Background work -- the sync threads, downloads -- keeps its own patience.
#define STREAM_UI_TIMEOUT_SECS 8

typedef enum {
	STREAM_ERR_NONE = 0,
	STREAM_ERR_NO_WIFI,			// no network at all
	STREAM_ERR_NO_RESPONSE,		// the server could not be reached, or went quiet
	STREAM_ERR_SIGN_IN,			// the session was refused: sign in again
	STREAM_ERR_NO_SUBSCRIPTION, // the account's plan does not cover it
	STREAM_ERR_KEYS,			// the firmware's application keys are missing or out of date
	STREAM_ERR_SERVER,			// the service is failing (5xx, or asking us to slow down)
	STREAM_ERR_NOT_FOUND,		// what was asked for is not there (any more)
	STREAM_ERR_OTHER,			// anything else; the service's own words are kept
} stream_err_t;

// A job is starting: forget the hint and the last request's outcome, so that a
// failure before any request is made is not read as an older request's.
void streamerr_begin(void);

// From inside a service: it knows better than the status code. A Subsonic
// "wrong credentials" arrives as HTTP 200 with code 40 in the body; Qobuz's
// "invalid app_id" is a 400. The last hint before streamerr_describe() wins.
void streamerr_hint(stream_err_t cls);

// The class of the failure that just happened on this thread, without logging
// or clearing anything.
stream_err_t streamerr_classify(void);

// The failure that just happened on this thread: classifies it, logs one line
// ("stream: subsonic: no_response (timeout, http 0) at host/rest/getAlbum.view:
// <detail>"), clears the hint, and writes the sentence to show into `out`.
// `detail` is the service's own message (its last_error), kept for the log and
// shown as the sentence only for STREAM_ERR_OTHER -- and not even then when it
// is merely the HTTP status in words. Returns the class.
stream_err_t streamerr_describe(const char *who, const char *detail, char *out, size_t size);

// The sentence for a class: the translation key, and the text with `detail`
// standing in for STREAM_ERR_OTHER when there is one.
const char *streamerr_key(stream_err_t cls);
void streamerr_text(stream_err_t cls, const char *detail, char *out, size_t size);

// For a class the user can do nothing about but wait or check the network:
// a sign-in page shows these as they are and keeps the service's own words
// (a wrong password) for the rest.
bool streamerr_is_transport(stream_err_t cls);

// Whether there is a network to try at all: Wi-Fi associated and with an
// address. Never blocks. The host build has no Wi-Fi and answers true, unless
// the file named by SONIX_FAKE_OFFLINE exists -- how the emulator plays at
// having lost it.
bool streamerr_online(void);

#endif /* STREAMERR_H */

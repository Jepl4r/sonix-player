#include "streamerr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/system/core/lang.h"
#include "src/system/net/http.h"
#include "src/system/net/wifi.h"

// See streamerr.h.

static __thread stream_err_t hint;

void streamerr_begin(void) {
	hint = STREAM_ERR_NONE;
	http_forget_outcome();
}

void streamerr_hint(stream_err_t cls) { hint = cls; }

bool streamerr_online(void) {
#ifdef HOST_BUILD
	// The PC is always online; a flag file lets the emulator pretend not to
	// be, which is the only way to see the "no Wi-Fi" message there.
	const char *flag = getenv("SONIX_FAKE_OFFLINE");
	return !(flag && flag[0] && access(flag, F_OK) == 0);
#else
	wifi_status_t status;
	wifi_get_status(&status);
	return status.state == WIFI_STATE_CONNECTED && status.ip[0] != '\0';
#endif
}

stream_err_t streamerr_classify(void) {
	if (hint != STREAM_ERR_NONE) {
		return hint;
	}
	http_outcome_t o;
	http_last_outcome(&o);
	switch (o.fail) {
	case HTTP_FAIL_DNS:
	case HTTP_FAIL_CONNECT:
	case HTTP_FAIL_TIMEOUT:
	case HTTP_FAIL_BROKEN:
		// No answer. Whether that is the network or the server is a question
		// for the radio, not the socket: a name that does not resolve looks
		// the same either way.
		return streamerr_online() ? STREAM_ERR_NO_RESPONSE : STREAM_ERR_NO_WIFI;
	case HTTP_FAIL_TLS:
	case HTTP_FAIL_ADDRESS:
		// The service's message carries http_last_error()'s reason (a wrong
		// clock, a missing certificate bundle), which the user can act on.
		return STREAM_ERR_OTHER;
	case HTTP_FAIL_NONE:
		break;
	}
	if (o.status == 401 || o.status == 403) {
		return STREAM_ERR_SIGN_IN;
	}
	if (o.status == 404 || o.status == 410) {
		return STREAM_ERR_NOT_FOUND;
	}
	if (o.status == 429 || o.status >= 500) {
		return STREAM_ERR_SERVER;
	}
	if (o.status == 0 && !streamerr_online()) {
		// Nothing was even asked: the service checked the network first.
		return STREAM_ERR_NO_WIFI;
	}
	return STREAM_ERR_OTHER;
}

static const char *class_name(stream_err_t cls) {
	static const char *const NAMES[] = {
		[STREAM_ERR_NONE] = "none",
		[STREAM_ERR_NO_WIFI] = "no_wifi",
		[STREAM_ERR_NO_RESPONSE] = "no_response",
		[STREAM_ERR_SIGN_IN] = "sign_in",
		[STREAM_ERR_NO_SUBSCRIPTION] = "no_subscription",
		[STREAM_ERR_KEYS] = "keys",
		[STREAM_ERR_SERVER] = "server",
		[STREAM_ERR_NOT_FOUND] = "not_found",
		[STREAM_ERR_OTHER] = "other",
	};
	return (unsigned)cls < sizeof(NAMES) / sizeof(NAMES[0]) ? NAMES[cls] : "?";
}

static const char *fail_name(http_fail_t fail) {
	static const char *const NAMES[] = {
		[HTTP_FAIL_NONE] = "answered",	   [HTTP_FAIL_ADDRESS] = "bad address", [HTTP_FAIL_DNS] = "dns",
		[HTTP_FAIL_CONNECT] = "connect",   [HTTP_FAIL_TLS] = "tls",				[HTTP_FAIL_TIMEOUT] = "timeout",
		[HTTP_FAIL_BROKEN] = "cut short",
	};
	return (unsigned)fail < sizeof(NAMES) / sizeof(NAMES[0]) ? NAMES[fail] : "?";
}

const char *streamerr_key(stream_err_t cls) {
	switch (cls) {
	case STREAM_ERR_NO_WIFI:
		return "stream_err_no_wifi";
	case STREAM_ERR_NO_RESPONSE:
		return "stream_err_no_response";
	case STREAM_ERR_SIGN_IN:
		return "stream_err_sign_in";
	case STREAM_ERR_NO_SUBSCRIPTION:
		return "stream_err_no_subscription";
	case STREAM_ERR_KEYS:
		return "stream_err_keys";
	case STREAM_ERR_SERVER:
		return "stream_err_server";
	case STREAM_ERR_NOT_FOUND:
		return "stream_err_not_found";
	default:
		return "stream_err_other";
	}
}

void streamerr_text(stream_err_t cls, const char *detail, char *out, size_t size) {
	if (!out || size == 0) {
		return;
	}
	if (cls == STREAM_ERR_OTHER && detail && detail[0]) {
		snprintf(out, size, "%s", detail);
		return;
	}
	snprintf(out, size, "%s", tr(streamerr_key(cls)));
}

bool streamerr_is_transport(stream_err_t cls) {
	return cls == STREAM_ERR_NO_WIFI || cls == STREAM_ERR_NO_RESPONSE || cls == STREAM_ERR_SERVER;
}

stream_err_t streamerr_describe(const char *who, const char *detail, char *out, size_t size) {
	stream_err_t cls = streamerr_classify();
	http_outcome_t o;
	http_last_outcome(&o);

	// The service's message is kept for "other" -- it may be a reason worth
	// reading, Qobuz's and Tidal's are written for people -- but not when all
	// it says is the status code in words ("Qobuz answered 400", http.c's
	// "The server answered 400"): a number is exactly what this module exists
	// to keep off the screen. Every such sentence carries the number, and a
	// reason the service wrote almost never does.
	const char *shown = detail;
	if (cls == STREAM_ERR_OTHER && o.fail == HTTP_FAIL_NONE && o.status > 0 && shown) {
		char code[16];
		snprintf(code, sizeof(code), "%d", o.status);
		if (strstr(shown, code)) {
			shown = NULL;
		}
	}

	fprintf(stderr, "stream: %s: %s (%s, http %d) at %s%s: %s\n", who ? who : "?", class_name(cls), fail_name(o.fail),
			o.status, o.host[0] ? o.host : "-", o.path, detail && detail[0] ? detail : "-");

	streamerr_text(cls, shown, out, size);
	hint = STREAM_ERR_NONE;
	return cls;
}

#include "subsonicimport.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "src/system/core/lang.h"
#include "src/system/streaming/cardimport.h"
#include "src/system/streaming/streamerr.h"
#include "src/system/streaming/subsonic.h"

// See subsonicimport.h.

// "key = value", or NULL when the line is not one. `key` gets the name, lower
// case (the value is the line's own, past the '=' and its spaces).
static const char *key_value(const char *line, char *key, size_t size) {
	const char *eq = strchr(line, '=');
	// A password may well hold an '=': only a known name before it makes the
	// line a pair.
	if (!eq || eq == line) {
		return NULL;
	}
	size_t len = (size_t)(eq - line);
	while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t')) {
		len--;
	}
	if (len == 0 || len >= size) {
		return NULL;
	}
	for (size_t i = 0; i < len; i++) {
		char c = line[i];
		key[i] = (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
	}
	key[len] = '\0';
	static const char *const KNOWN[] = {"server", "url", "user", "login", "username", "password", "pass"};
	bool known = false;
	for (size_t i = 0; i < sizeof(KNOWN) / sizeof(KNOWN[0]) && !known; i++) {
		known = strcmp(key, KNOWN[i]) == 0;
	}
	if (!known) {
		return NULL;
	}
	eq++;
	while (*eq == ' ' || *eq == '\t') {
		eq++;
	}
	return eq;
}

static cardimport_result_t validate(const char lines[][CARDIMPORT_LINE_MAX], int count, char *message,
									size_t message_size) {
	const char *server = NULL, *user = NULL, *password = NULL;
	int pairs = 0;
	for (int i = 0; i < count; i++) {
		char key[16];
		const char *value = key_value(lines[i], key, sizeof(key));
		if (!value) {
			continue;
		}
		pairs++;
		if (strcmp(key, "server") == 0 || strcmp(key, "url") == 0) {
			server = value;
		} else if (strcmp(key, "password") == 0 || strcmp(key, "pass") == 0) {
			password = value;
		} else {
			user = value;
		}
	}
	if (pairs == 0 && count >= 3) {
		server = lines[0];
		user = lines[1];
		password = lines[2];
	}
	if (!server || !server[0] || !user || !user[0] || !password || !password[0]) {
		snprintf(message, message_size, "%s", tr("subsonic_import_invalid"));
		return CARDIMPORT_INVALID;
	}
	streamerr_begin();
	if (subsonic_login(server, user, password)) {
		char shown[SUBSONIC_SERVER_MAX];
		subsonic_server(shown, sizeof(shown));
		const char *host = strstr(shown, "://");
		snprintf(message, message_size, tr("subsonic_import_ok"), user, host ? host + 3 : shown);
		return CARDIMPORT_OK;
	}
	char why[256];
	stream_err_t cls = streamerr_describe("subsonic", subsonic_last_error(), why, sizeof(why));
	if (streamerr_is_transport(cls)) {
		snprintf(message, message_size, "%s", why);
		return CARDIMPORT_OFFLINE;
	}
	// The server's own words when it gave some ("Wrong username or
	// password"): the class's sentence speaks of a session that ended, which
	// is not what happened to a file never signed in with.
	const char *said = subsonic_last_error();
	snprintf(message, message_size, tr("subsonic_import_refused"), said && said[0] ? said : why);
	return CARDIMPORT_REFUSED;
}

void subsonicimport_init(void) { cardimport_register("subsonic", SUBSONICIMPORT_FILE, validate); }

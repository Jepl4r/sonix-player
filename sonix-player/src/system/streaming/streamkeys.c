#include "streamkeys.h"

#include <stdio.h>
#include <string.h>

#include "src/system/core/config.h"

// The values stay here for the life of the program: short strings, read once,
// and every caller expects a pointer that does not die under it.
static char qobuz_app_id[64];
static char qobuz_app_secret[128];
static char tidal_client_id[64];
static char tidal_client_secret[128];
static char podcast_key[64];
static char podcast_secret[128];
static char source_path[256];
static bool loaded_any;

static void trim(char *s) {
	char *start = s;
	while (*start == ' ' || *start == '\t') {
		start++;
	}
	if (start != s) {
		memmove(s, start, strlen(start) + 1);
	}
	size_t len = strlen(s);
	while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) {
		s[--len] = '\0';
	}
}

// A tiny INI reader rather than config.c, for two reasons: this file is
// read-only and must not join what the player rewrites, and a failure here must
// not be able to corrupt device_config.ini.
void streamkeys_init(void) {
	const char *path = config_get("streaming", "keys_file", STREAMKEYS_PATH);

	FILE *f = fopen(path, "r");
	if (!f) {
		printf("streamkeys: no %s, Tidal, Qobuz and podcasts stay off\n", path);
		return;
	}

	char section[32] = "";
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		trim(line);
		if (line[0] == '\0' || line[0] == '#' || line[0] == ';') {
			continue;
		}

		if (line[0] == '[') {
			char *close = strchr(line, ']');
			if (close) {
				*close = '\0';
				// Clamped to the buffer width: a section name half a kilobyte
				// long is not a section of interest, and the truncation is
				// written out rather than left to happen.
				snprintf(section, sizeof(section), "%.31s", line + 1);
				trim(section);
			}
			continue;
		}

		char *eq = strchr(line, '=');
		if (!eq) {
			continue;
		}
		*eq = '\0';
		char *key = line;
		char *value = eq + 1;
		trim(key);
		trim(value);
		if (value[0] == '\0') {
			continue;
		}

		char *dest = NULL;
		size_t dest_size = 0;
		if (strcmp(section, "qobuz") == 0) {
			if (strcmp(key, "app_id") == 0) {
				dest = qobuz_app_id;
				dest_size = sizeof(qobuz_app_id);
			} else if (strcmp(key, "app_secret") == 0) {
				dest = qobuz_app_secret;
				dest_size = sizeof(qobuz_app_secret);
			}
		} else if (strcmp(section, "podcast") == 0) {
			if (strcmp(key, "api_key") == 0) {
				dest = podcast_key;
				dest_size = sizeof(podcast_key);
			} else if (strcmp(key, "api_secret") == 0) {
				dest = podcast_secret;
				dest_size = sizeof(podcast_secret);
			}
		} else if (strcmp(section, "tidal") == 0) {
			if (strcmp(key, "client_id") == 0) {
				dest = tidal_client_id;
				dest_size = sizeof(tidal_client_id);
			} else if (strcmp(key, "client_secret") == 0) {
				dest = tidal_client_secret;
				dest_size = sizeof(tidal_client_secret);
			}
		}

		if (dest) {
			snprintf(dest, dest_size, "%s", value);
			loaded_any = true;
		}
	}
	fclose(f);

	snprintf(source_path, sizeof(source_path), "%s", path);

	// The log records that the keys are present, never their values: the log
	// lands on the card and from there goes to whoever asks for it about any
	// problem at all.
	printf("streamkeys: from %s -- Qobuz %s, Tidal %s, Podcast %s\n", path,
		   qobuz_app_id[0] && qobuz_app_secret[0] ? "yes" : "no",
		   tidal_client_id[0] && tidal_client_secret[0] ? "yes" : "no",
		   podcast_key[0] && podcast_secret[0] ? "yes" : "no");
}

static const char *or_null(const char *s) { return s[0] ? s : NULL; }

const char *streamkeys_qobuz_app_id(void) { return or_null(qobuz_app_id); }
const char *streamkeys_qobuz_app_secret(void) { return or_null(qobuz_app_secret); }
const char *streamkeys_tidal_client_id(void) { return or_null(tidal_client_id); }
const char *streamkeys_tidal_client_secret(void) { return or_null(tidal_client_secret); }

const char *streamkeys_podcast_key(void) { return or_null(podcast_key); }
const char *streamkeys_podcast_secret(void) { return or_null(podcast_secret); }

const char *streamkeys_source(void) { return loaded_any ? source_path : NULL; }

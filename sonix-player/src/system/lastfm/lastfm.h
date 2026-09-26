#ifndef LASTFM_H
#define LASTFM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
	bool enabled;
	bool logged_in;
	bool logging_in;
	bool api_key_configured;
	bool api_secret_configured;
	char username[256];
	char status[192];
	char last_message[192];
	uint64_t message_serial;
} lastfm_snapshot_t;

// Starts the native Last.fm background service. `sd_root` is the same card root
// used by the rest of the player; the offline queue and session state live in
// its .local directory, falling back to /tmp on the host or a missing card.
void lastfm_init(const char *sd_root);

void lastfm_set_enabled(bool enabled);
void lastfm_set_api_key(const char *api_key);
void lastfm_set_api_secret(const char *api_secret);

void lastfm_login(const char *username, const char *password);
void lastfm_logout(void);

void lastfm_get_api_key(char *out, size_t out_size);
void lastfm_get_api_secret(char *out, size_t out_size);

// True while Last.fm has a logged-in session or an authentication request
// is in progress. The power manager uses this to prevent idle Wi-Fi parking
// while Last.fm needs the network, including with the screen off.
bool lastfm_network_wanted(void);

void lastfm_get_snapshot(lastfm_snapshot_t *out);

#endif /* LASTFM_H */

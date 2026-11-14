#ifndef SUBSONICCACHE_H
#define SUBSONICCACHE_H

#include <stdbool.h>
#include <stddef.h>

// The songs fetched from the server, kept on the card while they play.
//
// The model is the one qobuzcache.c and tidalcache.c already use, and for the
// same reasons: a file named after a hash of the identity, a sidecar that
// proves whose file it is, a marker that says the body is incomplete, and a
// path prefix that answers "is this one of ours" without a table.
//
//   <card>/.local/subsonic-cache/<md5(server,user,song_id)><suffix>
//   <card>/.local/subsonic-cache/<same>.identity   server, user, song id
//   <card>/.local/subsonic-cache/<same>.incompleto while the body is arriving
//
// Nothing here blocks except subsoniccache_fetch(), which is meant for a
// worker thread.

// Where the cache lives. NULL or "" turns it off: every call then reports
// "no card" instead of writing somewhere it should not.
void subsoniccache_set_root(const char *card_root);

// True when there is a root, the directory exists or could be created, and the
// cache is not full beyond its limit.
bool subsoniccache_ready(void);

// The path a song would have, whether or not it is there. `suffix` is the
// extension with its dot ("flac"); empty means ".bin". False when there is no
// root or the arguments do not fit.
bool subsoniccache_path(const char *song_id, const char *suffix, char *out, size_t size);

// True when the song is fully here: the file exists and the marker does not.
bool subsoniccache_has(const char *song_id);

// True when `path` is inside the cache directory. This is what the power code
// asks to decide whether the radio must stay up while this file plays.
bool subsoniccache_owns(const char *path);

// Fetches the song and publishes it atomically. Blocking; worker thread only.
// False on any failure, with subsonic_last_error() carrying the reason.
bool subsoniccache_fetch(const char *song_id, const char *suffix);

// True while a fetch is in flight. The power code keeps the radio parked off
// while this is set.
bool subsoniccache_fetching(void);

// Set by the page before asking for something over the network and cleared
// when it has everything: between a tap and the first byte of a download
// nothing is "fetching" yet, and without this the radio would be switched off
// under the request.
void subsoniccache_set_network_wanted(bool on);
bool subsoniccache_network_wanted(void);

// Drops the least recently used files until the cache fits in `limit_bytes`.
// 0 or a limit above what is there does nothing.
void subsoniccache_prune(unsigned long long limit_bytes);

// Removes everything the cache owns. Called on shutdown, like the other two.
void subsoniccache_clear_on_exit(void);

#endif /* SUBSONICCACHE_H */

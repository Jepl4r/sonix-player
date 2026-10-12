#ifndef SUBSONICCACHE_H
#define SUBSONICCACHE_H

#include <stdbool.h>
#include <stddef.h>

#include "src/system/streaming/streamturn.h"

// Subsonic tracks downloaded to the card and then played like any other file.
//
// The same machine as qobuzcache.h, whose comments explain why a track is
// downloaded rather than decoded off the socket, how the prebuffer is measured
// and why the cache empties itself. Read that file first; only the differences
// are written down here.
//
// The one real difference is the identity. Qobuz and Tidal ids are numbers, so
// their file name is the id and a path maps back to the track with no table.
// Subsonic ids are strings the server makes up (Navidrome: 22 random
// characters, Gonic: "tr-1234", Airsonic: a number) and they are only unique on
// one server. So the file is named after a key -- 16 hexadecimal digits of a
// 64-bit FNV-1a hash of server URL and song id, see subsonic_track_key() -- and
// the song id itself is written into the `.tags` sidecar, where the star and the
// scrobbler read it back. A file name is never built from anything a server
// sent.
//
// Every function taking a key rejects anything that is not 16 lowercase
// hexadecimal digits.

#define SUBSONICCACHE_DIR ".local/subsonic-cache"

// Characters in a key, terminator included.
#define SUBSONICCACHE_KEY_LEN 17

// The ceiling: the same gigabyte as Qobuz. Unlike Qobuz, this cache is not
// swept every few tracks nor emptied at power-off -- it is the user's own music
// and doubles as an offline copy of what was listened to lately. Past the
// ceiling the least recently played files go.
#define SUBSONICCACHE_MAX_BYTES (1024L * 1024L * 1024L)
#define SUBSONICCACHE_PROTECTED_MAX 64

void subsoniccache_set_root(const char *sd_root);
bool subsoniccache_ready(void);
const char *subsoniccache_dir(void);
bool subsoniccache_owns(const char *path);

// The path the track would have, from its key and the server's suffix
// ("flac"). Says nothing about whether it exists.
void subsoniccache_path(const char *key, const char *suffix, char *out, size_t size);

// True if downloaded in full (under that suffix, or under any, for _find).
bool subsoniccache_has(const char *key, const char *suffix, char *out, size_t size);
bool subsoniccache_find(const char *key, char *out, size_t size);

// The key back from a path, without touching the card. False for anything that
// is not a track of this cache.
bool subsoniccache_path_key(const char *path, char *out, size_t size);

// Starts downloading `url` into the cache; returns once the decoder can start
// (see qobuzcache_start, same contract: blocks for the prebuffer, worker
// threads only).
bool subsoniccache_start(const char *key, const char *url, const char *suffix, int duration_secs, char *out,
						 size_t size);

// Writes beside the track what the server knows: title, artist, album, album
// id, track number and the song id (in `<file>.tags`), and the cover (in
// `<stem>.jpg`, downloaded from `cover_url` when given). Blocks on the cover
// download: worker threads only when `cover_url` is set.
void subsoniccache_write_sidecars(const char *key, const char *suffix, const char *song_id, const char *title,
								  const char *artist, const char *album, const char *album_id, int track_number,
								  const char *cover_url);

// One key of the `.tags` sidecar ("id", "album_id"...). False when the path is
// not from this cache or the sidecar does not say.
bool subsoniccache_tag(const char *path, const char *key, char *out, size_t size);
bool subsoniccache_album_id(const char *path, char *out, size_t size);
bool subsoniccache_song_id(const char *path, char *out, size_t size);

// A download is running (any track), or this key's is.
bool subsoniccache_busy(void);
bool subsoniccache_is_downloading(const char *key);

void subsoniccache_wait_idle(void);
void subsoniccache_set_slow_start_cb(void (*cb)(int seconds));
void subsoniccache_abandon_all(void);

// See qobuzcache_set_network_wanted(): the gap between one track's end and the
// next one's first bytes, during which Wi-Fi must not be parked.
void subsoniccache_set_network_wanted(bool wanted);
bool subsoniccache_network_wanted(void);

const char *subsoniccache_last_error(void);

// Every track that enters the cache: keeps it safe from a prune in the instant
// before the queue knows about it.
void subsoniccache_note_played(const char *path);
void subsoniccache_set_protected(const char *const *paths, int count);
// Empties the cache and the list covers: factory reset only.
void subsoniccache_wipe(void);
long subsoniccache_bytes(void);
void subsoniccache_clear(void);

#endif /* SUBSONICCACHE_H */

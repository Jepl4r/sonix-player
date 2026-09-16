#ifndef QOBUZCACHE_H
#define QOBUZCACHE_H

#include <stdbool.h>
#include <stddef.h>

#include "src/system/streaming/streamturn.h"

// Qobuz tracks downloaded to the card and then played like any other file.
//
// Why download instead of decoding as it arrives: radio does the latter (reads
// from the socket and decodes on the fly), but a radio stream has no end, is
// never seeked, and has a single format. A track is different:
//
//   * decoder_open() wants a path, and behind it are seven different decoders
//     (FLAC, ALAC, Opus, WavPack...) that open the file themselves. Making them
//     read from a socket would mean an abstraction layer inside each one;
//   * seeking inside a track requires going backwards, and a socket cannot;
//   * with the file on disk, the queue, the equalizer, ReplayGain, chapters and
//     the progress bar all work with no extra code.
//
// It is also what the original does: it keeps its tracks in
// `a:\.hiby\<...>\cache\....flac`.
//
// The track is not waited for: playback starts while it is still downloading.
// As soon as there is enough for the decoder to start, playback begins and the
// rest arrives underneath -- the reader that knows how to wait is in
// decode/growfile.c. Same behaviour as streaming, except the file remains at
// the end.

#define QOBUZCACHE_DIR ".local/qobuz-cache"

// How large the cache may grow before the oldest tracks are dropped. One GB is
// about thirty 16/44 albums or ten hi-res ones: enough not to re-download what
// is being listened to these days, small enough not to silently fill a 32 GB
// card.
#define QOBUZCACHE_MAX_BYTES (1024L * 1024L * 1024L)

// Where the card is. Called once at startup and on every card change; NULL or
// empty disables the cache (and with it Qobuz, which cannot play anything
// without it).
void qobuzcache_set_root(const char *sd_root);
bool qobuzcache_ready(void);

// The path this track would have. Says nothing about whether it exists.
void qobuzcache_path(long track_id, const char *mime, char *out, size_t size);

// True if the track has been downloaded in full.
bool qobuzcache_has(long track_id, const char *mime, char *out, size_t size);

// Like qobuzcache_has, but without knowing the format: tries every extension
// this cache may have used. Needed by callers that queue a track before asking
// Qobuz for it -- the name is guessed from the selected quality, and if Qobuz
// sent a different container the file is still there under another name.
bool qobuzcache_find(long track_id, char *out, size_t size);

// Download progress for the bar: `done` and `total` in bytes (total 0 when the
// server does not say). Called from the download thread.
typedef void (*qobuzcache_progress_cb)(long done, long total, void *user);

// Starts downloading `url` into the cache and puts the local path in `out`.
//
// Returns as soon as there is enough material for the decoder to open the file
// (STREAM_PREBUFFER bytes, or all of it if the track is shorter); the rest
// keeps downloading on its own thread. The caller can hand the path to playback
// immediately.
//
// Blocks for the duration of the prebuffer -- a few tenths of a second -- so it
// must be called from a worker thread, never from the GUI thread.
//
// A track already in the cache returns immediately without touching the
// network. `duration_secs` is the track duration according to the API: it gives
// the bytes per second playback will consume, and hence how much must be in
// hand before starting. 0 when unknown (a fixed prebuffer is used).
bool qobuzcache_start(long track_id, const char *url, const char *mime, int duration_secs, char *out, size_t size);

// Waits until no download is in progress. Only one runs at a time: a caller
// prefetching the next track calls this before starting, otherwise it competes
// for network and card with the track being listened to right now.
void qobuzcache_wait_idle(void);

// Called when the prebuffer will take more than a moment, with the estimate in
// seconds. On a network slower than the track there is no choice -- 24/176.4
// needs half a megabyte per second, and if the network gives half of that, more
// than half the track must be in hand before starting -- but staring at a
// frozen screen without knowing why is a different matter.
//
// Called from the download thread: the receiver must bounce to the GUI thread
// itself.
void qobuzcache_set_slow_start_cb(void (*cb)(int seconds));

// The prebuffer -- how long to wait before starting the decoder, and how it is
// worked out -- is identical for every service and lives in streamturn.h, so
// that the numbers cannot drift apart.

// Abandons downloads in progress. Call when switching to another track: the
// previous one is no longer needed and the bandwidth belongs to this one.
void qobuzcache_abandon_all(void);

// The id of the track downloading right now, 0 if none. This is the robust way
// to ask "is this track already downloading?": by id, not by path.
long qobuzcache_downloading_id(void);

// "Qobuz needs the network", even when nothing is downloading at this instant.
//
// Needed by the Wi-Fi parking logic (power.c), which powers the radio down
// after ninety seconds of blank screen when nothing is playing and nobody is
// using the network. Between "the track ended and the next one is not there"
// and "the next one's bytes started arriving" sit the playback stop and an
// HTTPS request for the file URL: in that gap nothing plays and
// qobuzcache_downloading_id() is still zero, so without this flag parking reads
// a clear path and takes Wi-Fi down under the download about to start.
void qobuzcache_set_network_wanted(bool wanted);
bool qobuzcache_network_wanted(void);

// True if `path` is inside the Qobuz cache. Needed by the details page, which
// must not show a cache file path: it is not somewhere the user put anything
// and it tells them nothing.
bool qobuzcache_owns(const char *path);

// The cache directory, or NULL with no card. For callers that must clean up
// elsewhere whatever pointed into it.
const char *qobuzcache_dir(void);

// Writes beside the track what the API knows and the file does not: title,
// artist, album (in `<file>.tags`) and the cover (in `<stem>.jpg`).
//
// FLACs from Qobuz often arrive untagged, and without this the player would
// show "12345678.flac". A sidecar rather than rewriting the tags inside the
// FLAC because it is reversible, does not touch the audio, and is read
// unaided by both metadata.c and the cover loader (which already finds an image
// named after the track).
void qobuzcache_write_sidecars(long track_id, const char *mime, const char *title, const char *artist,
							   const char *album, const char *album_id, int track_number, const char *cover_url);

// The Qobuz album id the track belongs to, read back from the sidecar. False if
// the path is not from this cache or the sidecar does not say. Used by "Show
// album" in the player.
bool qobuzcache_album_id(const char *path, char *out, size_t size);

// Why the last qobuzcache_start() failed, as a sentence fit to display. Empty
// on success. Per thread, like http_last_error().
const char *qobuzcache_last_error(void);

// The Qobuz track id from the local path, 0 if that path is not a track of this
// cache. The file name is the id ("12345678.flac"), so no table is needed: a
// playing file always maps back to the Qobuz track, which is what lets the
// player's favourite star and playlists reach the account instead of the local
// database.
long qobuzcache_track_id(const char *path);

// How many tracks between automatic cache sweeps. Ten is about three quarters
// of an hour of listening: enough not to re-download constantly, small enough
// not to leave half the card occupied by transient files.
#define QOBUZCACHE_CLEAR_EVERY 10

// Call for every track that enters the cache, with its path. Sweeps every
// QOBUZCACHE_CLEAR_EVERY calls.
//
// The path serves two purposes: counting, and keeping a ring of recently
// downloaded files. Those are never dropped, because an instant passes between
// the download and the track appearing in the queue, and in that instant a
// sweep would have deleted them, stopping playback at the following track.
void qobuzcache_note_played(const char *path);

// How many paths can be protected from the automatic sweep.
#define QOBUZCACHE_PROTECTED_MAX 64

// The tracks the queue expects to play: the current one and those after it. The
// automatic sweep skips them.
//
// Without this the sweep would delete tracks already queued -- downloaded and
// waiting their turn -- and playback would stop mid-album with the next file
// gone.
//
// Call from the GUI thread, the only owner of the queue.
void qobuzcache_set_protected(const char *const *paths, int count);

// Call on the way out (shutdown, reboot): the cache does not survive a power
// cycle.
void qobuzcache_clear_on_exit(void);

// Current size, and how to empty it.
long qobuzcache_bytes(void);
void qobuzcache_clear(void);

#endif /* QOBUZCACHE_H */

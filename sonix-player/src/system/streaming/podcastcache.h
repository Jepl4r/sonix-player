#ifndef PODCASTCACHE_H
#define PODCASTCACHE_H

#include <stdbool.h>
#include <stddef.h>

// Podcast episodes downloaded to the card and then played like any other file.
//
// The third twin of qobuzcache.h and tidalcache.h, and the reasons for
// downloading instead of decoding on the fly are the same: decoder_open() wants
// a path, seeking inside an episode requires going backwards, and with the file
// on disk the queue, the equalizer, ReplayGain and the progress bar work with
// no extra code. See qobuzcache.h for the full reasoning.
//
// What differs from the other two:
//
// Little, which is good news: an episode has a direct URL and only needs
// downloading. No per-track signature as on Qobuz, no segments to reassemble as
// on Tidal. The differences are elsewhere:
//
//   * an episode lasts an hour, not three minutes. The cache ceiling is higher
//     accordingly, and pruning by last listen matters more: three episodes fill
//     what on Qobuz would be an album;
//   * the sidecar keeps the podcast id instead of the album id. That is what
//     makes "Show podcast" work in the track menu.
//
// Separate directory, as for the other two and for the same reason: the three
// catalogues number independently, so in a single directory episode 12345678
// and Tidal track 12345678 would collide on one file. Distinct directories make
// that impossible by construction and give *_owns() a clean answer, which is
// how the player knows which badge to draw.

#define PODCASTCACHE_DIR ".local/podcast-cache"

// How large the cache may grow before the oldest episodes are dropped. Two GB
// rather than the other two caches' one: a 128 kbps episode runs an hour and
// weighs about 60 MB, so one GB is roughly fifteen episodes, too few for anyone
// following four or five podcasts.
//
// `LL` and not `L`. Two GB is 2,147,483,648, one more than the maximum of a
// signed 32-bit `long`, which is what this device has. Written
// `2048L * 1024L * 1024L` the compiler evaluates in `long` and the result wraps
// to -2,147,483,648, a negative ceiling that pruning can never satisfy, so
// every download empties the whole directory. A 64-bit host does not show it.
//
// Every byte count in this cache is therefore 64-bit: two GB does not fit in a
// `long` here, not even as a sum.
#define PODCASTCACHE_MAX_BYTES (2048LL * 1024LL * 1024LL)

// Where the card is. Once at startup and on every card change; NULL or empty
// disables the cache (and with it podcasts, which cannot play without it).
void podcastcache_set_root(const char *sd_root);
bool podcastcache_ready(void);

// The path this episode would have. Says nothing about whether it exists.
void podcastcache_path(long long episode_id, const char *mime, char *out, size_t size);

// True if the episode has been downloaded in full.
bool podcastcache_has(long long episode_id, const char *mime, char *out, size_t size);

// Like podcastcache_has, but without knowing the format: tries every extension
// this cache may have used. Needed by callers that queue an episode before
// knowing what the server will actually send.
bool podcastcache_find(long long episode_id, char *out, size_t size);

// Download progress for the bar, in bytes.
typedef void (*podcastcache_progress_cb)(long done, long total, void *user);

// Starts downloading `url` into the cache and puts the local path in `out`.
//
// Returns as soon as there is enough material for the decoder to open the file;
// the rest keeps downloading on its own thread, and the caller can hand the
// path to playback immediately. An episode already in the cache returns
// immediately without touching the network.
//
// Blocks for the duration of the prebuffer, so call it from a worker thread,
// never from the GUI thread.
//
// `duration_secs` is the duration according to the catalogue: it gives the bytes
// per second playback will consume, and hence how much must be in hand before
// starting. 0 when unknown.
bool podcastcache_start(long long episode_id, const char *url, const char *mime, int duration_secs, char *out, size_t size);

// Waits until no download is in progress.
void podcastcache_wait_idle(void);

// The network is slow: the prebuffer will take `seconds`. Lets the page say so
// instead of looking frozen.
void podcastcache_set_slow_start_cb(void (*cb)(int seconds));

// Drops the download in progress. Callable from another thread.
void podcastcache_abandon_all(void);

// The id of the episode downloading right now (0 = none).
long long podcastcache_downloading_id(void);

// "Podcasts need the network", even when nothing is downloading at this
// instant: between one episode and the next there is a window with no download
// in flight, and without this flag Wi-Fi parking saw a clear path and powered
// the radio down underneath the download.
void podcastcache_set_network_wanted(bool wanted);
bool podcastcache_network_wanted(void);

// True if `path` is inside this cache. That is how the player knows it is
// playing a podcast: which badge to draw, which buttons to show, and which
// entry to put in the track menu.
bool podcastcache_owns(const char *path);

const char *podcastcache_dir(void);

// The sidecars: what the catalogue knows and the file does not. A podcast
// enclosure is often an MP3 carrying another episode's tags, or no tags at all,
// so these are the real metadata.
//
// `feed_id` is the podcast id: without it the playing file could not lead back
// to the other episodes, which is what "Show podcast" does. `feed_author` and
// `feed_image` are not for displaying the episode: they serve the player's
// follow star, which must be able to follow the podcast being listened to
// without asking the catalogue -- and when playing an already downloaded
// episode there may well be no network.
//
// `cover_url` is the episode cover: it is always written to the tags (which is
// what lets podcastcache_ensure_cover fetch it later), but it is downloaded
// immediately only with `fetch_cover_now` -- the queue writes the tags of sixty
// episodes at once, and sixty downloads on the thread that has to start
// playback do not fit. Other episodes' covers are fetched when their turn comes
// (ensure_cover, from the loader).
void podcastcache_write_sidecars(long long episode_id, const char *mime, const char *title, const char *feed_title,
								 long long feed_id, const char *feed_author, const char *feed_image,
								 const char *cover_url, bool fetch_cover_now);

// The episode cover, fetched when the episode's turn comes: if the jpg beside
// the file is not there yet, it is downloaded now from the tags' cover_url (or
// feed_image). True if the cover is present after the call. Meant for the cover
// loader thread, which can afford to wait on a network request.
bool podcastcache_ensure_cover(const char *path);

// The id of the podcast the episode belongs to, read back from the sidecar.
// False when absent.
bool podcastcache_feed_id(const char *path, long long *out);

// Any sidecar field, by name. Needed by the follow star, which wants three
// fields rather than one.
bool podcastcache_tag(const char *path, const char *key, char *out, size_t size);

// Why the last podcastcache_start() failed.
const char *podcastcache_last_error(void);

// The episode id from the local path, 0 when the path is not this cache's.
// Named this way
// rather than track_id as in the other two caches because here there are two
// ids -- the episode and the podcast -- and "track" would not say which.
long long podcastcache_episode_id(const char *path);

// How many played episodes between sweeps.
#define PODCASTCACHE_CLEAR_EVERY 10
void podcastcache_note_played(const char *path);

// Queued episodes the pruning must not touch.
#define PODCASTCACHE_PROTECTED_MAX 64
void podcastcache_set_protected(const char *const *paths, int count);

// Emptied at shutdown, like the other two.
void podcastcache_clear_on_exit(void);

long long podcastcache_bytes(void);
void podcastcache_clear(void);

#endif /* PODCASTCACHE_H */

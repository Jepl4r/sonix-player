#include "podcastcache.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <pthread.h>

#include "src/system/decode/growfile.h"
#include "src/system/net/http.h"
#include "src/system/streaming/streamturn.h"

// The smallest head worth starting on, whatever the arithmetic says: a decoder
// still has to find its headers in it.
#define PODCAST_MIN_HEAD (32 * 1024)
#include "src/system/core/utils.h"
#include "src/system/core/lang.h"

#define FETCH_TIMEOUT_SECS 20
#define FETCH_CHUNK 32768

static char cache_dir[512];

// Every error exit records what went wrong, and the page shows that instead of
// the generic phrase. Before, any failure -- no card, no network, card full --
// produced the same "download failed" message, which led nowhere.
static __thread char last_error[192];

const char *podcastcache_last_error(void) { return last_error; }

static void set_error(const char *what) { snprintf(last_error, sizeof(last_error), "%s", what ? what : ""); }

void podcastcache_abandon_all(void);

// Registers with the shared turn once, at the first opportunity: from then on a
// track requested from Tidal can make a podcast download let go, and the other
// way round.
static void register_abandon_once(void) {
	static bool done;
	if (!done) {
		done = true;
		streamturn_register_abandon(podcastcache_abandon_all);
	}
}

void podcastcache_set_root(const char *sd_root) {
	register_abandon_once();
	if (!sd_root || !*sd_root) {
		cache_dir[0] = '\0';
		return;
	}
	snprintf(cache_dir, sizeof(cache_dir), "%s/%s", sd_root, PODCASTCACHE_DIR);
}

bool podcastcache_ready(void) { return cache_dir[0] != '\0'; }

// The extension from the MIME type the feed declares. The decoder picks its
// path from the extension, so a FLAC named .bin never opens. The fallback is
// MP3, not FLAC as elsewhere: podcasts are overwhelmingly MP3, and a feed that
// declares nothing is almost always one of them.
static const char *extension_for(const char *mime) {
	if (!mime || !*mime) {
		return "mp3";
	}
	if (strstr(mime, "mpeg") || strstr(mime, "mp3")) {
		return "mp3";
	}
	if (strstr(mime, "mp4") || strstr(mime, "m4a") || strstr(mime, "aac")) {
		return "m4a";
	}
	if (strstr(mime, "ogg") || strstr(mime, "opus")) {
		return "opus";
	}
	if (strstr(mime, "wav")) {
		return "wav";
	}
	if (strstr(mime, "flac")) {
		return "flac";
	}
	return "mp3";
}

void podcastcache_path(long long episode_id, const char *mime, char *out, size_t size) {
	if (!out || size == 0) {
		return;
	}
	if (!podcastcache_ready()) {
		out[0] = '\0';
		return;
	}
	// The directory part is capped at what fits alongside the file name: a
	// path truncated halfway would silently open (or delete) something
	// elsewhere.
	snprintf(out, size, "%.400s/%lld.%s", cache_dir, episode_id, extension_for(mime));
}

// A present file may still be an interrupted download: while it grows an empty
// marker sits beside it, removed only once the download completes. The episode
// is written straight to its final name -- the decoder opens it while it grows,
// and renaming halfway would change the file under it -- so completeness has to
// be recorded somewhere else.
static void marker_path(const char *path, char *out, size_t size) {
	snprintf(out, size, "%.500s.incompleto", path);
}

bool podcastcache_has(long long episode_id, const char *mime, char *out, size_t size) {
	char path[512];
	podcastcache_path(episode_id, mime, path, sizeof(path));
	if (!path[0]) {
		return false;
	}

	char marker[544];
	marker_path(path, marker, sizeof(marker));
	struct stat unused;
	if (stat(marker, &unused) == 0) {
		return false; // interrupted: download it again
	}

	struct stat st;
	if (stat(path, &st) != 0 || st.st_size == 0) {
		return false;
	}
	if (out && size) {
		snprintf(out, size, "%s", path);
	}
	return true;
}

bool podcastcache_find(long long episode_id, char *out, size_t size) {
	static const char *const MIMES[] = {"audio/mpeg", "audio/mp4", "audio/opus", "audio/wav", "audio/flac"};
	for (size_t i = 0; i < sizeof(MIMES) / sizeof(MIMES[0]); i++) {
		if (podcastcache_has(episode_id, MIMES[i], out, size)) {
			return true;
		}
	}
	if (out && size) {
		out[0] = '\0';
	}
	return false;
}

// mkdir -p over the two levels needed (.local, then the cache directory).
static bool ensure_dir(void) {
	if (!podcastcache_ready()) {
		return false;
	}

	char path[512];
	snprintf(path, sizeof(path), "%s", cache_dir);
	for (char *p = path + 1; *p; p++) {
		if (*p != '/') {
			continue;
		}
		*p = '\0';
		mkdir(path, 0777);
		*p = '/';
	}
	if (mkdir(path, 0777) != 0 && errno != EEXIST) {
		fprintf(stderr, "podcastcache: cannot create %s: %s\n", path, strerror(errno));
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// measuring and pruning
// ---------------------------------------------------------------------------

typedef struct {
	char name[64];
	long long size;
	long atime; // last access: what tells which episodes are being listened to
} entry_t;

static int scan(entry_t *out, int max, long long *total_out) {
	long long total = 0;
	int count = 0;

	DIR *d = opendir(cache_dir);
	if (!d) {
		if (total_out) {
			*total_out = 0;
		}
		return 0;
	}

	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') {
			continue;
		}
		char path[640];
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", cache_dir, e->d_name) >= sizeof(path)) {
			continue;
		}
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
			continue;
		}
		total += st.st_size;

		// Whatever is downloading right now cannot be deleted, nor can its
		// sidecars: the comparison is on the name without extension, so
		// "101.flac", "101.flac.tags" and "101.jpg" are all protected while
		// "101.flac" grows.
		//
		// Without this, high-resolution material -- two or three episodes and
		// the cache ceiling is already reached -- had a new download delete the
		// file that was playing.
		char stem[640];
		snprintf(stem, sizeof(stem), "%.500s/%.63s", cache_dir, e->d_name);
		char *dot = strrchr(stem + strlen(cache_dir) + 1, '.');
		if (dot) {
			*dot = '\0';
		}
		if (growfile_prefix_is_growing(stem)) {
			continue;
		}
		if (out && count < max && strlen(e->d_name) < sizeof(out[0].name)) {
			snprintf(out[count].name, sizeof(out[count].name), "%.63s", e->d_name);
			out[count].size = (long long)st.st_size;
			out[count].atime = (long)st.st_atime;
			count++;
		}
	}
	closedir(d);

	if (total_out) {
		*total_out = total;
	}
	return count;
}

long long podcastcache_bytes(void) {
	if (!podcastcache_ready()) {
		return 0;
	}
	long long total = 0;
	scan(NULL, 0, &total);
	return total;
}

#define PRUNE_MAX_ENTRIES 512

static bool is_protected_stem(const char *stem);
static bool is_recent_stem(const char *stem);

// Drops the least-listened entries until the cache is back under its ceiling.
// By last access rather than creation: something downloaded a month ago but
// played yesterday stays, while something downloaded yesterday and never
// resumed can go.
static void prune(long long keep_room_for) {
	// Borrowed for the length of the pass and given straight back: this runs
	// only when a download is about to overflow the cache, so forty kilobytes
	// are not worth holding for the life of the player.
	entry_t *entries = malloc(sizeof(*entries) * PRUNE_MAX_ENTRIES);
	if (!entries) {
		return; // nothing is deleted, and the ceiling is checked again next time
	}

	long long total = 0;
	int count = scan(entries, PRUNE_MAX_ENTRIES, &total);
	if (total + keep_room_for <= PODCASTCACHE_MAX_BYTES) {
		free(entries);
		return;
	}

	// Selection sort: at most 512 entries, and only occasionally.
	for (int i = 0; i < count; i++) {
		int oldest = i;
		for (int k = i + 1; k < count; k++) {
			if (entries[k].atime < entries[oldest].atime) {
				oldest = k;
			}
		}
		entry_t tmp = entries[i];
		entries[i] = entries[oldest];
		entries[oldest] = tmp;
	}

	for (int i = 0; i < count && total + keep_room_for > PODCASTCACHE_MAX_BYTES; i++) {
		char path[640];
		snprintf(path, sizeof(path), "%.500s/%.63s", cache_dir, entries[i].name);

		// Episodes still ahead in the queue are off limits, and so are their
		// sidecars. Pruning is meant to make room, not to dismantle the queue
		// being listened to: without this check, being near the ceiling was
		// enough for the next download to take away the following episode --
		// and its tags, which is what the Queue page reads to draw titles. The
		// periodic clear below already made this check; pruning did not.
		char stem[640];
		snprintf(stem, sizeof(stem), "%s", path);
		char *dot = strrchr(stem + strlen(cache_dir) + 1, '.');
		if (dot) {
			*dot = '\0';
		}
		if (is_protected_stem(stem) || is_recent_stem(stem)) {
			continue;
		}

		if (remove(path) == 0) {
			total -= entries[i].size;
			printf("podcastcache: removed %s (%lld KB)\n", entries[i].name, entries[i].size / 1024);
		}
	}

	free(entries);
}

static bool is_recent_stem(const char *stem);

// Episodes still ahead in the queue, which the periodic clear must not touch.
static pthread_mutex_t protected_lock = PTHREAD_MUTEX_INITIALIZER;
static char protected_paths[PODCASTCACHE_PROTECTED_MAX][512];
static int protected_count;

void podcastcache_set_protected(const char *const *paths, int count) {
	pthread_mutex_lock(&protected_lock);
	protected_count = 0;
	for (int i = 0; i < count && protected_count < PODCASTCACHE_PROTECTED_MAX; i++) {
		if (paths[i] && paths[i][0]) {
			snprintf(protected_paths[protected_count++], sizeof(protected_paths[0]), "%s", paths[i]);
		}
	}
	pthread_mutex_unlock(&protected_lock);
}

// True when `stem` (the name without extension) belongs to a protected episode
// or one of its sidecars.
static bool is_protected_stem(const char *stem) {
	size_t len = strlen(stem);
	pthread_mutex_lock(&protected_lock);
	bool match = false;
	for (int i = 0; i < protected_count && !match; i++) {
		match = strncmp(protected_paths[i], stem, len) == 0;
	}
	pthread_mutex_unlock(&protected_lock);
	return match;
}

// `keep_playing` true spares what is downloading right now, plus its sidecars.
// The periodic clear needs it because it runs while something is playing; a
// clear the user asked for takes everything.
static void clear_files(bool keep_playing) {
	if (!podcastcache_ready()) {
		return;
	}
	DIR *d = opendir(cache_dir);
	if (!d) {
		return;
	}
	int removed = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') {
			continue;
		}
		char path[640];
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", cache_dir, e->d_name) >= sizeof(path)) {
			continue;
		}
		if (keep_playing) {
			char stem[640];
			snprintf(stem, sizeof(stem), "%.500s/%.63s", cache_dir, e->d_name);
			char *dot = strrchr(stem + strlen(cache_dir) + 1, '.');
			if (dot) {
				*dot = '\0';
			}
			if (growfile_prefix_is_growing(stem) || is_protected_stem(stem) || is_recent_stem(stem)) {
				continue;
			}
		}
		if (remove(path) == 0) {
			removed++;
		}
	}
	closedir(d);
	printf("podcastcache: cleared (%d files)\n", removed);
}

void podcastcache_clear(void) { clear_files(false); }

// ---------------------------------------------------------------------------
// the periodic clear
//
// The cache is a staging area, not a library. Keeping everything ever listened
// to fills the user's card with files they did not put there and will never see
// (it lives in a hidden directory). The one-gigabyte ceiling alone is not
// enough: it gets reached and stays reached.
//
// So the clear runs at two moments: every PODCASTCACHE_CLEAR_EVERY episodes
// started, and at program exit (which on this device means shutdown or reboot,
// since /usr/bin/hiby_player.sh restarts as soon as the process leaves).
// Whatever is downloading at that moment is spared, otherwise the episode being
// listened to would be deleted.
// ---------------------------------------------------------------------------

static int played_since_clear;

// Ring of the most recently downloaded. Deliberately small: a safety net for
// the window between "the file exists" and "the queue knows about it", not a
// second cache.
#define RECENT_MAX 8
static pthread_mutex_t recent_lock = PTHREAD_MUTEX_INITIALIZER;
static char recent_paths[RECENT_MAX][512];
static int recent_next;

static bool is_recent_stem(const char *stem) {
	size_t len = strlen(stem);
	pthread_mutex_lock(&recent_lock);
	bool match = false;
	for (int i = 0; i < RECENT_MAX && !match; i++) {
		match = recent_paths[i][0] && strncmp(recent_paths[i], stem, len) == 0;
	}
	pthread_mutex_unlock(&recent_lock);
	return match;
}

void podcastcache_note_played(const char *path) {
	if (!podcastcache_ready()) {
		return;
	}

	if (path && path[0]) {
		pthread_mutex_lock(&recent_lock);
		snprintf(recent_paths[recent_next], sizeof(recent_paths[0]), "%s", path);
		recent_next = (recent_next + 1) % RECENT_MAX;
		pthread_mutex_unlock(&recent_lock);
	}

	played_since_clear++;
	if (played_since_clear < PODCASTCACHE_CLEAR_EVERY) {
		return;
	}
	played_since_clear = 0;
	printf("podcastcache: %d episodes since the last sweep, cleaning up\n", PODCASTCACHE_CLEAR_EVERY);
	clear_files(true);
}

// Cover art lives in a sibling directory of the episodes (.local/podcast-art
// beside .local/podcast-cache) and has the same lifetime: transient files that
// must not survive a power cycle. The path is derived from the episode
// directory rather than asked of the GUI, so every exit route (shutdown,
// reboot, update, factory reset) clears both with one call.
static void clear_art_files(void) {
	char art[sizeof(cache_dir) + 16];
	snprintf(art, sizeof(art), "%s", cache_dir);
	char *slash = strrchr(art, '/');
	if (!slash) {
		return;
	}
	snprintf(slash + 1, sizeof(art) - (size_t)(slash + 1 - art), "podcast-art");

	DIR *d = opendir(art);
	if (!d) {
		return;
	}
	int removed = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') {
			continue;
		}
		char path[sizeof(art) + 300];
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", art, e->d_name) >= sizeof(path)) {
			continue;
		}
		if (remove(path) == 0) {
			removed++;
		}
	}
	closedir(d);
	printf("podcastcache: covers cleared (%d files)\n", removed);
}

void podcastcache_clear_on_exit(void) {
	if (!podcastcache_ready()) {
		return;
	}
	printf("podcastcache: shutting down, clearing the cache\n");
	clear_files(false);
	clear_art_files();
}

// Reads one key back from the tags sidecar. False when it is not there.
static bool read_tag(const char *path, const char *key, char *out, size_t size) {
	if (!out || size == 0) {
		return false;
	}
	out[0] = '\0';

	char tags[544];
	snprintf(tags, sizeof(tags), "%.500s.tags", path);
	FILE *f = fopen(tags, "r");
	if (!f) {
		return false;
	}

	size_t key_len = strlen(key);
	char line[512];
	bool found = false;
	while (!found && fgets(line, sizeof(line), f)) {
		if (strncmp(line, key, key_len) != 0 || line[key_len] != '=') {
			continue;
		}
		char *value = line + key_len + 1;
		char *end = value + strlen(value);
		while (end > value && (end[-1] == '\n' || end[-1] == '\r')) {
			*--end = '\0';
		}
		snprintf(out, size, "%s", value);
		found = out[0] != '\0';
	}
	fclose(f);
	return found;
}

bool podcastcache_feed_id(const char *path, long long *out) {
	if (!podcastcache_owns(path) || !out) {
		return false;
	}
	char text[32];
	if (!read_tag(path, "feed_id", text, sizeof(text)) || !text[0]) {
		return false;
	}
	*out = strtoll(text, NULL, 10);
	return *out > 0;
}

bool podcastcache_tag(const char *path, const char *key, char *out, size_t size) {
	if (!podcastcache_owns(path) || !key || !out || size == 0) {
		return false;
	}
	out[0] = '\0';
	return read_tag(path, key, out, size);
}

// The episode id from the local path: the file name is the id and nothing else
// ("12345678.flac"), so it can be recovered without keeping any state. Returns
// 0 when the path is not in this cache or the name is not a number.
long long podcastcache_episode_id(const char *path) {
	if (!podcastcache_owns(path)) {
		return 0;
	}
	const char *name = strrchr(path, '/');
	name = name ? name + 1 : path;
	if (*name < '0' || *name > '9') {
		return 0;
	}
	char *end = NULL;
	long long id = strtoll(name, &end, 10);
	// The digits must be followed by the extension dot: "12.flac" yes,
	// "12abc.flac" no.
	if (!end || *end != '.' || id <= 0) {
		return 0;
	}
	return id;
}

// ---------------------------------------------------------------------------
// downloading
// ---------------------------------------------------------------------------

// The episode is written straight to its final name, not to a temporary file
// renamed at the end: the decoder opens it while it grows, and a rename halfway
// would swap the file under it. Completeness is recorded by an empty marker
// beside it, removed only when everything has arrived.

typedef struct {
	long long episode_id;
	char path[512];
	char marker[544];
	http_stream_t stream;
	FILE *f;
	long done;
	long total;
	unsigned generation;
} download_t;

// Changing episode raises this number; threads downloading something else
// notice at the next chunk and stop, so the bandwidth goes to what is being
// listened to now.
static unsigned generation = 1;
static pthread_mutex_t gen_lock = PTHREAD_MUTEX_INITIALIZER;

// ---------------------------------------------------------------------------
// one download at a time
//
// The turn does not belong to podcasts but to the device, which has one network
// and one card to share with Qobuz and Tidal as well, so it lives in
// streamturn.c. The full reasoning is there.
// ---------------------------------------------------------------------------

static void busy_acquire(void) { streamturn_acquire(); }
static void busy_release(void) { streamturn_release(); }

static void (*slow_start_cb)(int seconds);

void podcastcache_set_slow_start_cb(void (*cb)(int seconds)) { slow_start_cb = cb; }

void podcastcache_wait_idle(void) { streamturn_wait_idle(); }

// The stream of the download in progress, so another thread can shake it loose.
// Only one runs at a time (the rule above), so a single pointer is enough.
// Guarded by its own lock and cleared before download_end frees the memory, so
// the wake never lands on a dead object.
static pthread_mutex_t active_lock = PTHREAD_MUTEX_INITIALIZER;
static http_stream_t *active_stream;

// The id of the episode downloading right now (0 = none). "Is this episode
// already downloading?" is answered from this, by id, rather than from growfile
// by path: an id is exact, while two paths for the same episode can differ.
static long long active_episode_id;

long long podcastcache_downloading_id(void) {
	pthread_mutex_lock(&active_lock);
	long long id = active_episode_id;
	pthread_mutex_unlock(&active_lock);
	return id;
}

// See podcastcache.h. A single bool, written by the interface thread and read
// by the power loop: neither can observe half of it.
static volatile bool network_wanted;

void podcastcache_set_network_wanted(bool wanted) { network_wanted = wanted; }
bool podcastcache_network_wanted(void) { return network_wanted; }

void podcastcache_abandon_all(void) {
	pthread_mutex_lock(&gen_lock);
	generation++;
	pthread_mutex_unlock(&gen_lock);

	// On a real network raising the number is not enough on its own: the
	// downloader only notices at the next chunk, and if the socket is stalled
	// -- Wi-Fi breathing, slow server -- the next chunk arrives after the
	// receive timeout, twenty seconds. For all that time the abandoned download
	// holds the turn and the episode the user just asked for cannot start.
	//
	// The wake is the same trick as the radio: close the socket under the
	// blocked recv(), which returns at once, and the download loop sees the
	// changed generation and releases the turn immediately.
	pthread_mutex_lock(&active_lock);
	if (active_stream) {
		http_stream_wake(active_stream);
	}
	pthread_mutex_unlock(&active_lock);
}

static unsigned current_generation(void) {
	pthread_mutex_lock(&gen_lock);
	unsigned value = generation;
	pthread_mutex_unlock(&gen_lock);
	return value;
}

// Stops advertising this episode as downloading. Required on every path that
// leaves without downloading anything: the id is published as soon as the turn
// is taken (see podcastcache_start), so a failed start would leave it lying,
// and whoever believes it waits for a file that is not coming.
static void clear_active(long long episode_id) {
	pthread_mutex_lock(&active_lock);
	if (active_episode_id == episode_id) {
		active_episode_id = 0;
	}
	pthread_mutex_unlock(&active_lock);
}

// Closes everything and records how it ended.
static void download_end(download_t *d, bool ok) {
	// The stream is about to die: take it out of sight of anything that could
	// wake it first, then close it. The other order is a wake on freed memory.
	pthread_mutex_lock(&active_lock);
	if (active_stream == &d->stream) {
		active_stream = NULL;
	}
	if (active_episode_id == d->episode_id) {
		active_episode_id = 0;
	}
	pthread_mutex_unlock(&active_lock);

	if (d->f) {
		if (fclose(d->f) != 0) {
			ok = false;
		}
		d->f = NULL;
	}
	http_stream_close(&d->stream);

	// Fewer bytes than the server promised: not a usable file.
	if (ok && d->total > 0 && d->done != d->total) {
		fprintf(stderr, "podcastcache: %lld cut short at %ld of %ld bytes\n", d->episode_id, d->done, d->total);
		ok = false;
	}
	if (ok && d->done == 0) {
		ok = false;
	}

	growfile_finish(d->path, ok);

	if (ok) {
		remove(d->marker); // complete now
		printf("podcastcache: %lld complete (%ld KB)\n", d->episode_id, d->done / 1024);
	} else {
		// The marker stays: next time podcastcache_has() says no and the
		// episode is downloaded again instead of playing half of it.
		printf("podcastcache: %lld not completed\n", d->episode_id);
	}

	// The turn is released last, not as soon as the stream is closed: while the
	// marker is still there the episode counts as incomplete, and whoever was
	// waiting for the turn would start downloading what had just finished
	// downloading. Releasing above the remove() failed the cache test in one
	// place, and on the device would mean an episode occasionally fetched twice
	// for no reason.
	busy_release();

	free(d);
}

// Writes as long as it can. False when it went wrong.
static bool download_pump(download_t *d, long until, bool *finished) {
	char buf[FETCH_CHUNK];
	*finished = false;

	while (d->done < until || until == 0) {
		if (current_generation() != d->generation) {
			return false; // something else is being listened to now
		}

		int n = http_stream_read(&d->stream, buf, (int)sizeof(buf));
		if (n < 0) {
			return false;
		}
		if (n == 0) {
			*finished = true;
			return true;
		}
		if (fwrite(buf, 1, (size_t)n, d->f) != (size_t)n) {
			fprintf(stderr, "podcastcache: write failed (card full?)\n");
			return false;
		}

		// Flush before announcing the new length: without it the reader would
		// go looking on disk for bytes still sitting in the stdio buffer.
		fflush(d->f);
		d->done += n;
		growfile_progress(d->path, d->done);

		if (until == 0 && *finished) {
			break;
		}
	}
	return true;
}

// The rest of the download, once playback has started.
static void *download_thread(void *arg) {
	// Background, like every other worker: there is one core, and a thread
	// writing to the card at full priority takes it away from the interface.
	// That was half the reason the interface stuttered.
	thread_be_background("podcast download");

	download_t *d = arg;
	bool finished = false;
	bool ok = download_pump(d, 0, &finished);
	download_end(d, ok && finished);
	return NULL;
}

bool podcastcache_start(long long episode_id, const char *url, const char *mime, int duration_secs, char *out, size_t size) {
	if (!out || size == 0) {
		return false;
	}
	out[0] = '\0';
	set_error("");

	if (!podcastcache_ready()) {
		fprintf(stderr, "podcastcache: no card to write to\n");
		set_error(tr("download_no_card"));
		return false;
	}
	if (!url || !*url) {
		set_error(tr("podcast_this_episode_has_no_download_address"));
		return false;
	}

	// Already downloaded in full: no network, no wait.
	if (podcastcache_has(episode_id, mime, out, size)) {
		return true;
	}

	if (!ensure_dir()) {
		set_error(tr("cache_folder_failed"));
		return false;
	}

	download_t *d = calloc(1, sizeof(*d));
	if (!d) {
		set_error(tr("out_of_memory"));
		return false;
	}
	d->episode_id = episode_id;
	podcastcache_path(episode_id, mime, d->path, sizeof(d->path));
	marker_path(d->path, d->marker, sizeof(d->marker));

	// This episode is already downloading, and it is the one being listened to.
	// Starting again would reopen the file for writing -- that is, truncate it
	// -- under the decoder reading it. Tapping the same episode twice means
	// wanting to hear it, not to restart it, so the existing path is returned.
	if (growfile_is_growing(d->path)) {
		snprintf(out, size, "%.500s", d->path);
		free(d);
		return true;
	}

	// One at a time: if another download is running, wait for the turn.
	//
	// The running one is deliberately not abandoned here. Prefetch also calls
	// this function, for the episode after the one playing, so abandoning here
	// would mean reading ahead interrupts the download of the episode being
	// played -- the opposite of what is wanted. Abandoning belongs to whoever
	// received a user command, which is the page: see streamturn_abandon_all().
	busy_acquire();

	// The generation is taken AFTER the wait: a value read before it would be
	// stale by the time the turn arrives, and an episode change that happened
	// during the wait would cancel this download the moment it starts.
	d->generation = current_generation();

	// The id is published here and not at the second publication point below,
	// which sits after http_stream_open() -- a full round of DNS, TCP, TLS and
	// request, easily a couple of seconds. Until the id is out,
	// podcastcache_downloading_id() answers zero for an episode that is very
	// much downloading, and callers act on that: the player reports the file as
	// missing, and prefetch abandons the connection being opened for the very
	// episode it wants.
	//
	// The socket, in contrast, stays NULL until it really exists: an abandon
	// reads it to close it, and the generation alone is enough to stop a
	// download in the meantime.
	pthread_mutex_lock(&active_lock);
	active_episode_id = d->episode_id;
	pthread_mutex_unlock(&active_lock);

	if (!http_stream_open(&d->stream, url, FETCH_TIMEOUT_SECS)) {
		const char *why = http_last_error();
		fprintf(stderr, "podcastcache: %lld does not open%s%s\n", episode_id, why && why[0] ? ": " : "",
				why && why[0] ? why : "");
		set_error(why && why[0] ? why : tr("podcast_cannot_reach_the_podcast_server"));
		clear_active(d->episode_id);
		busy_release();
		free(d);
		return false;
	}

	d->total = d->stream.content_length;
	prune(d->total > 0 ? d->total : 0);

	// The marker before the file: if power is lost between the two lines, an
	// orphan marker (which does nothing) is better than an incomplete file that
	// looks good.
	FILE *marker = fopen(d->marker, "wb");
	if (marker) {
		fclose(marker);
	}

	d->f = fopen(d->path, "wb");
	if (!d->f) {
		fprintf(stderr, "podcastcache: %s does not open for writing: %s\n", d->path, strerror(errno));
		set_error(errno == ENOSPC ? tr("card_full") : tr("card_write_failed"));
		http_stream_close(&d->stream);
		clear_active(d->episode_id);
		busy_release();
		free(d);
		return false;
	}

	// From here on this is the download in progress, and an abandon has to be
	// able to close its socket (see podcastcache_abandon_all).
	pthread_mutex_lock(&active_lock);
	active_stream = &d->stream;
	active_episode_id = d->episode_id;
	pthread_mutex_unlock(&active_lock);

	growfile_announce(d->path, d->total);

	// --- the prebuffer, measured instead of guessed ---------------------------
	//
	// A fixed number cannot work: half a megabyte is twelve seconds of an MP3
	// and not even one of a 24/176.4 stream. What matters is not how much has
	// been downloaded but whether the download keeps up with playback.
	//
	// So a first chunk is downloaded against the clock, which gives the network
	// rate. With R the rate the episode consumes (bytes per second) and M the
	// measured rate, starting with H bytes in hand the reserve at time t is
	// H + (M - R)t, and the download finishes at t = (total - H)/M. For the
	// reserve not to run out first:
	//
	//     H >= total * (1 - M/R)
	//
	// If the network is faster than the episode (M >= R) a token amount is
	// enough. If it is slower, the formula says exactly how much is needed
	// before starting -- and if it is far slower it says "everything", which is
	// the truth.
	// The floor, in seconds of audio rather than bytes.
	//
	// STREAM_PREBUFFER is half a megabyte, a number sized for music: at
	// 24/176.4 it is a second and a half, but a talk show at 96 kbps is twelve
	// kilobytes a second, so the same half megabyte is forty seconds of audio
	// to wait through. The catalogue gives the duration and the server gives
	// the length, so the rate the episode consumes is known before a byte is
	// fetched, and STREAM_HEAD_SECS of it is the honest floor.
	//
	// Starting with less is not starting blind: the measurement below raises it
	// when the network is slower than the episode, and audio.c's refill path
	// stops, stocks up and resumes when even that is not enough.
	long want = STREAM_PREBUFFER;
	double required_now = (d->total > 0 && duration_secs > 0) ? (double)d->total / duration_secs : 0.0;
	if (required_now > 0) {
		want = (long)(required_now * STREAM_HEAD_SECS);
		if (want < PODCAST_MIN_HEAD) {
			want = PODCAST_MIN_HEAD; // enough for any decoder to open the file
		}
	}
	if (d->total > 0 && d->total < want) {
		want = d->total;
	}

	// The probe follows the floor rather than the other way round: 256 KB timed
	// against the clock is itself two seconds on a player's Wi-Fi, and timing
	// the network is not worth more than the wait it is meant to avoid. Under a
	// third of a second it simply does not count as a measurement, and the
	// branch below already says so.
	long probe = want < STREAM_PROBE ? want : STREAM_PROBE;

	printf("podcastcache: %lld runs %d s for %ld KB -> starting with %ld KB\n", episode_id, duration_secs,
		   d->total / 1024, want / 1024);
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	bool finished = false;
	if (!download_pump(d, probe, &finished)) {
		set_error(current_generation() != d->generation ? tr("download_cancelled") : tr("download_interrupted"));
		download_end(d, false);
		return false;
	}

	if (!finished && d->total > 0 && duration_secs > 0) {
		struct timespec t1;
		clock_gettime(CLOCK_MONOTONIC, &t1);
		double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

		// Under a third of a second the measurement is not a measurement: a
		// first chunk can arrive all at once out of the socket buffer and make
		// the network look ten times faster than it is. Knowing nothing beats
		// knowing something false -- the floor below holds on its own.
		double measured = elapsed > 0.3 ? d->done / elapsed : 0.0;
		double required = (double)d->total / duration_secs;

		// The floor: a few seconds of audio in hand regardless, even when the
		// network looks very fast. It keeps the first hiccup from stopping
		// playback that has only just started.
		if (required > 0) {
			long floor_bytes = (long)(required * STREAM_HEAD_SECS);
			if (floor_bytes > want) {
				want = floor_bytes;
			}
			if (want > d->total) {
				want = d->total;
			}
		}

		if (measured > 0 && required > 0) {
			// A fifth of margin: a single-chunk measurement is noisy, and a
			// wobbling network must not start an episode that will stall right
			// away.
			double head = (double)d->total * (1.0 - measured / (required * 1.2));

			// Capped. Without the cap, on a network slower than the episode
			// this formula asks for nearly the whole file -- minutes of waiting
			// before the first note. The refill path in audio.c makes "never
			// stall" unnecessary: start with little and, if the network cannot
			// keep up, pause briefly to build a reserve. See podcastcache.h.
			double cap = required * STREAM_HEAD_MAX_SECS;
			if (head > cap) {
				head = cap;
			}
			if (head > want) {
				want = (long)head;
			}
			if (want > d->total) {
				want = d->total;
			}
			printf("podcastcache: %lld needs %.0f KB/s, the network gives %.0f -> starting with %ld KB of %ld\n", episode_id,
				   required / 1024, measured / 1024, want / 1024, d->total / 1024);

			// A wait measured in seconds is worth telling the user about.
			int wait_secs = (int)((want - d->done) / measured);
			if (wait_secs >= 2 && slow_start_cb) {
				slow_start_cb(wait_secs);
			}
		}
	}

	if (!finished && d->done < want && !download_pump(d, want, &finished)) {
		set_error(current_generation() != d->generation ? tr("download_cancelled") : tr("download_interrupted"));
		download_end(d, false);
		return false;
	}

	if (finished) {
		// Short episode: it is all here already.
		snprintf(out, size, "%.500s", d->path);
		download_end(d, true);
		return true;
	}

	snprintf(out, size, "%.500s", d->path);

	// Both numbers are read before starting the thread: from that moment `d`
	// belongs to the thread, and a short file can finish downloading and free
	// it before the printf below gets to read it. Valgrind caught exactly that
	// -- a read of freed memory inside the "starts at N KB" printf. On the
	// device it would be a random number in the log, or worse.
	long started_done = d->done;
	long started_total = d->total;

	pthread_t thread;
	if (pthread_create(&thread, NULL, download_thread, d) != 0) {
		set_error(tr("cannot_start_the_download"));
		download_end(d, false); // releases the turn as well
		out[0] = '\0';
		return false;
	}
	pthread_detach(thread);

	printf("podcastcache: %lld starts at %ld KB of %ld KB\n", episode_id, started_done / 1024,
		   started_total > 0 ? started_total / 1024 : 0);
	return true;
}

// ---------------------------------------------------------------------------
// sidecars: what the API knows and the audio file does not
// ---------------------------------------------------------------------------

const char *podcastcache_dir(void) { return cache_dir[0] ? cache_dir : NULL; }

bool podcastcache_owns(const char *path) {
	return path && cache_dir[0] && strncmp(path, cache_dir, strlen(cache_dir)) == 0;
}

static bool fetch_cover_to(const char *path, const char *url); // defined below

void podcastcache_write_sidecars(long long episode_id, const char *mime, const char *title, const char *feed_title,
								 long long feed_id, const char *feed_author, const char *feed_image,
								 const char *cover_url, bool fetch_cover_now) {
	if (!podcastcache_ready()) {
		return;
	}

	char path[512];
	podcastcache_path(episode_id, mime, path, sizeof(path));
	if (!path[0]) {
		return;
	}

	char tags[544];
	snprintf(tags, sizeof(tags), "%.500s.tags", path);

	// Atomic write: a temporary file first, then rename(). Opening .tags for
	// writing truncates it before it is refilled, and whoever reads the
	// metadata at that instant -- device_state at episode start, from another
	// thread -- found an empty file and the player showed "12345678.mp3"
	// instead of the title.
	char tags_tmp[560];
	snprintf(tags_tmp, sizeof(tags_tmp), "%s.tmp", tags);
	FILE *f = fopen(tags_tmp, "w");
	if (f) {
		// The episode title as title, and the podcast name as both artist and
		// album. The player and the queue have three fields and no notion of a
		// podcast, and the line wanted under the title is the podcast name.
		// Writing it in both places makes it appear everywhere without
		// teaching either of them a new concept.
		fprintf(f, "title=%s\n", title ? title : "");
		fprintf(f, "artist=%s\n", feed_title ? feed_title : "");
		fprintf(f, "album=%s\n", feed_title ? feed_title : "");
		// The feed id, which is not a tag and is never displayed: it is what
		// makes "Mostra podcast" in the episode menu work, since without it
		// there is no way to know which podcast to reopen.
		fprintf(f, "feed_id=%lld\n", feed_id);
		// Two more fields nobody displays: they let the player's star follow
		// the podcast without asking the catalogue anything.
		fprintf(f, "feed_author=%s\n", feed_author ? feed_author : "");
		fprintf(f, "feed_image=%s\n", feed_image ? feed_image : "");
		// The episode cover as a URL, so podcastcache_ensure_cover() can fetch
		// it later, when the episode's turn comes, even if it is not fetched
		// now.
		fprintf(f, "cover_url=%s\n", cover_url ? cover_url : "");
		fclose(f);
		if (rename(tags_tmp, tags) != 0) {
			remove(tags_tmp);
		}
	}

	if (!fetch_cover_now || !cover_url || !*cover_url) {
		return;
	}
	fetch_cover_to(path, cover_url);
}

// Downloads `url` into "<file name>.jpg" beside the audio: the cover loader
// already looks for that name, so nothing has to be taught to it. True when the
// file exists after the call.
static bool fetch_cover_to(const char *path, const char *url) {
	char cover[544];
	const char *dot = strrchr(path, '.');
	snprintf(cover, sizeof(cover), "%.*s.jpg", dot ? (int)(dot - path) : (int)strlen(path), path);

	struct stat st;
	if (stat(cover, &st) == 0 && st.st_size > 0) {
		return true; // already downloaded
	}
	if (!url || !*url) {
		return false;
	}

	char *body = NULL;
	size_t len = 0;
	// 8 MB: real podcast covers reach 3000x3000.
	if (!http_get(url, &body, &len, 8 * 1024 * 1024, FETCH_TIMEOUT_SECS) || !body || len == 0) {
		free(body);
		return false;
	}

	bool ok = false;
	FILE *img = fopen(cover, "wb");
	if (img) {
		ok = fwrite(body, 1, len, img) == len;
		fclose(img);
		if (!ok) {
			remove(cover);
		}
	}
	free(body);
	return ok;
}

bool podcastcache_ensure_cover(const char *path) {
	if (!path || !podcastcache_owns(path)) {
		return false;
	}
	// Already there: no network needed.
	char cover[544];
	const char *dot = strrchr(path, '.');
	snprintf(cover, sizeof(cover), "%.*s.jpg", dot ? (int)(dot - path) : (int)strlen(path), path);
	struct stat st;
	if (stat(cover, &st) == 0 && st.st_size > 0) {
		return true;
	}
	// The URL is in the tags: the episode cover, falling back to the podcast
	// cover, which is better than nothing.
	char url[512];
	if (podcastcache_tag(path, "cover_url", url, sizeof(url)) && url[0]) {
		if (fetch_cover_to(path, url)) {
			return true;
		}
	}
	if (podcastcache_tag(path, "feed_image", url, sizeof(url)) && url[0]) {
		return fetch_cover_to(path, url);
	}
	return false;
}

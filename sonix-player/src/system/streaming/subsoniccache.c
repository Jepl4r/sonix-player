#include "subsoniccache.h"

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
#include "src/system/core/utils.h"
#include "src/system/core/lang.h"

#define FETCH_TIMEOUT_SECS 20
#define FETCH_CHUNK 32768

static char cache_dir[512];

// The reason a download failed, for the page to show.
//
// Every error exit here records what happened, so that no card, no network and
// a full card do not all reach the page as one "download failed".
static __thread char last_error[192];

const char *subsoniccache_last_error(void) { return last_error; }

static void set_error(const char *what) { snprintf(last_error, sizeof(last_error), "%s", what ? what : ""); }

void subsoniccache_abandon_all(void);

// Registers with the shared turn once, at the first opportunity: from then on
// a track requested from Tidal knows how to make a Subsonic download let go, and
// vice versa.
static void register_abandon_once(void) {
	static bool done;
	if (!done) {
		done = true;
		streamturn_register_abandon(subsoniccache_abandon_all);
	}
}

void subsoniccache_set_root(const char *sd_root) {
	register_abandon_once();
	if (!sd_root || !*sd_root) {
		cache_dir[0] = '\0';
		return;
	}
	snprintf(cache_dir, sizeof(cache_dir), "%s/%s", sd_root, SUBSONICCACHE_DIR);
}

bool subsoniccache_ready(void) { return cache_dir[0] != '\0'; }

// The file extension from the server's `suffix` ("flac", "mp3", "opus"). The
// decoder picks its path from the extension, so it must be the real one; with
// format=raw it is the original file's, with a transcode the target format's.
// Anything odd (empty, too long, not alphanumeric) becomes "flac" rather than a
// name nobody can open -- or a path component.
static void extension_for(const char *suffix, char *out, size_t size) {
	size_t n = 0;
	for (const char *p = suffix; p && *p && n + 1 < size; p++) {
		char c = *p;
		if (c >= 'A' && c <= 'Z') {
			c = (char)(c - 'A' + 'a');
		}
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) {
			n = 0;
			break;
		}
		out[n++] = c;
	}
	if (n == 0 || n > 5) {
		snprintf(out, size, "flac");
		return;
	}
	out[n] = '\0';
}

// A key is the 16 hexadecimal digits subsonic_track_key() makes; nothing else is
// ever turned into a file name.
static bool key_valid(const char *key) {
	if (!key || strlen(key) != SUBSONICCACHE_KEY_LEN - 1) {
		return false;
	}
	for (const char *p = key; *p; p++) {
		if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) {
			return false;
		}
	}
	return true;
}

void subsoniccache_path(const char *key, const char *suffix, char *out, size_t size) {
	if (!out || size == 0) {
		return;
	}
	if (!subsoniccache_ready() || !key_valid(key)) {
		out[0] = '\0';
		return;
	}
	// The directory part is bounded so the file name always fits: a path
	// truncated halfway would silently open (or delete) something elsewhere.
	char ext[8];
	extension_for(suffix, ext, sizeof(ext));
	snprintf(out, size, "%.400s/%s.%s", cache_dir, key, ext);
}

// Path of the marker that says a download is incomplete. While a track is
// coming down an empty marker sits next to it, removed only when the download
// finishes. The track is written straight to its final name (the decoder opens
// it while it grows, and a rename partway would swap the file under it), so
// completeness has to be recorded somewhere else.
static void marker_path(const char *path, char *out, size_t size) {
	snprintf(out, size, "%.500s.incompleto", path);
}

bool subsoniccache_has(const char *key, const char *suffix, char *out, size_t size) {
	char path[512];
	subsoniccache_path(key, suffix, path, sizeof(path));
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

bool subsoniccache_find(const char *key, char *out, size_t size) {
	// The suffixes a Subsonic server realistically sends. A transcode setting
	// changed between queueing and playing is what makes the guess miss.
	static const char *const SUFFIXES[] = {"flac", "mp3", "opus", "ogg", "m4a", "aac", "wav", "wv", "ape", "dsf"};
	for (size_t i = 0; i < sizeof(SUFFIXES) / sizeof(SUFFIXES[0]); i++) {
		if (subsoniccache_has(key, SUFFIXES[i], out, size)) {
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
	if (!subsoniccache_ready()) {
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
		fprintf(stderr, "subsoniccache: cannot create %s: %s\n", path, strerror(errno));
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// measuring and pruning
// ---------------------------------------------------------------------------

typedef struct {
	char name[64];
	long size;
	long atime; // last access: what tells which tracks are actually listened to
} entry_t;

static int scan(entry_t *out, int max, long *total_out) {
	long total = 0;
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

		// What is coming down right now is not prunable, nor are its sidecar
		// files: the comparison is on the name without extension, so
		// "101.flac", "101.flac.tags" and "101.jpg" are all protected while
		// "101.flac" grows.
		//
		// Without this, a hi-res album (two or three tracks and the cache
		// ceiling is already reached) had each new download delete the file
		// that was playing.
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
			out[count].size = (long)st.st_size;
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

long subsoniccache_bytes(void) {
	if (!subsoniccache_ready()) {
		return 0;
	}
	long total = 0;
	scan(NULL, 0, &total);
	return total;
}

#define PRUNE_MAX_ENTRIES 512

// Drops the least listened to until the cache fits again. Ordered by last
// access, not creation: an album downloaded a month ago but replayed yesterday
// should stay, one downloaded yesterday and never replayed can go.
static void prune(long keep_room_for) {
	// Borrowed for the length of the pass and given straight back: this runs
	// only when a download is about to overflow the cache, so forty kilobytes
	// are not worth holding for the life of the player.
	entry_t *entries = malloc(sizeof(*entries) * PRUNE_MAX_ENTRIES);
	if (!entries) {
		return; // nothing is deleted, and the ceiling is checked again next time
	}

	long total = 0;
	int count = scan(entries, PRUNE_MAX_ENTRIES, &total);
	if (total + keep_room_for <= SUBSONICCACHE_MAX_BYTES) {
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

	for (int i = 0; i < count && total + keep_room_for > SUBSONICCACHE_MAX_BYTES; i++) {
		char path[640];
		snprintf(path, sizeof(path), "%.500s/%.63s", cache_dir, entries[i].name);
		if (remove(path) == 0) {
			total -= entries[i].size;
			printf("subsoniccache: removed %s (%ld KB)\n", entries[i].name, entries[i].size / 1024);
		}
	}

	free(entries);
}

static bool is_recent_stem(const char *stem);

// The tracks the queue still has ahead, which the periodic clear must not
// touch.
static pthread_mutex_t protected_lock = PTHREAD_MUTEX_INITIALIZER;
static char protected_paths[SUBSONICCACHE_PROTECTED_MAX][512];
static int protected_count;

void subsoniccache_set_protected(const char *const *paths, int count) {
	pthread_mutex_lock(&protected_lock);
	protected_count = 0;
	for (int i = 0; i < count && protected_count < SUBSONICCACHE_PROTECTED_MAX; i++) {
		if (paths[i] && paths[i][0]) {
			snprintf(protected_paths[protected_count++], sizeof(protected_paths[0]), "%s", paths[i]);
		}
	}
	pthread_mutex_unlock(&protected_lock);
}

// True when `stem` (the name without extension) belongs to one of the
// protected tracks or to one of its sidecar files.
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

// `keep_playing` spares what is coming down right now (and its sidecar files).
// The periodic clear needs it because it happens while something is playing; a
// clear the user asked for takes everything.
static void clear_files(bool keep_playing) {
	if (!subsoniccache_ready()) {
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
	printf("subsoniccache: cleared (%d files)\n", removed);
}

void subsoniccache_clear(void) { clear_files(false); }

// ---------------------------------------------------------------------------
// what stays
//
// Unlike Qobuz and Tidal, this cache is NOT swept every few tracks nor emptied
// at power-off. The music is the user's own, on their own server, and the
// cache is what lets an album played at home play again on the bus with no
// network. What bounds it is the ceiling alone: when a download would pass
// SUBSONICCACHE_MAX_BYTES, the least recently listened to files go (prune()).
// The protected list and the recent ring below still matter for that prune
// and for an explicit clear.
//
// A factory reset does empty it (subsoniccache_wipe): it must leave nothing of
// the old owner's behind.
// ---------------------------------------------------------------------------


// Ring of the most recently downloaded tracks. Deliberately small: it is a
// safety net for the window between "the file exists" and "the queue knows
// about it", not a second cache.
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

void subsoniccache_note_played(const char *path) {
	if (!subsoniccache_ready()) {
		return;
	}

	if (path && path[0]) {
		pthread_mutex_lock(&recent_lock);
		snprintf(recent_paths[recent_next], sizeof(recent_paths[0]), "%s", path);
		recent_next = (recent_next + 1) % RECENT_MAX;
		pthread_mutex_unlock(&recent_lock);
	}
}

// The lists' cover art lives in a sibling directory of the tracks
// (.local/qobuz-art, shared by every service: see qobuzart.h) and has the same
// lifetime:
// transient data that must not survive a power cycle. The path is derived from
// the track directory rather than asked of the GUI, so every exit path
// (shutdown, restart, update, factory reset) clears both with one call.
static void clear_art_files(void) {
	char art[sizeof(cache_dir) + 16];
	snprintf(art, sizeof(art), "%s", cache_dir);
	char *slash = strrchr(art, '/');
	if (!slash) {
		return;
	}
	snprintf(slash + 1, sizeof(art) - (size_t)(slash + 1 - art), "qobuz-art");

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
	printf("subsoniccache: covers cleared (%d files)\n", removed);
}

void subsoniccache_wipe(void) {
	if (!subsoniccache_ready()) {
		return;
	}
	printf("subsoniccache: wiping the cache\n");
	clear_files(false);
	clear_art_files();
}

// Reads one key back out of the tags sidecar. Returns false when it is absent.
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

bool subsoniccache_tag(const char *path, const char *key, char *out, size_t size) {
	if (!subsoniccache_owns(path)) {
		if (out && size) {
			out[0] = '\0';
		}
		return false;
	}
	return read_tag(path, key, out, size);
}

bool subsoniccache_album_id(const char *path, char *out, size_t size) {
	return subsoniccache_tag(path, "album_id", out, size);
}

bool subsoniccache_song_id(const char *path, char *out, size_t size) {
	return subsoniccache_tag(path, "id", out, size);
}

// The key from the local path: the file name is nothing but the key
// ("0123456789abcdef.flac"), so it maps back without touching the card. False
// when the path is not a track of this cache.
bool subsoniccache_path_key(const char *path, char *out, size_t size) {
	if (out && size) {
		out[0] = '\0';
	}
	if (!subsoniccache_owns(path) || !out || size < SUBSONICCACHE_KEY_LEN) {
		return false;
	}
	const char *name = strrchr(path, '/');
	name = name ? name + 1 : path;
	const char *dot = strchr(name, '.');
	if (!dot || dot - name != SUBSONICCACHE_KEY_LEN - 1) {
		return false;
	}
	char key[SUBSONICCACHE_KEY_LEN];
	memcpy(key, name, SUBSONICCACHE_KEY_LEN - 1);
	key[SUBSONICCACHE_KEY_LEN - 1] = '\0';
	if (!key_valid(key)) {
		return false;
	}
	snprintf(out, size, "%s", key);
	return true;
}

// ---------------------------------------------------------------------------
// downloading
//
// A track is written straight to its final name, not to a temporary renamed at
// the end: the decoder opens it while it grows, and a rename partway would
// swap the file under it. Completeness is recorded by an empty marker
// alongside, removed only once everything has arrived.
// ---------------------------------------------------------------------------

typedef struct {
	char key[SUBSONICCACHE_KEY_LEN];
	char path[512];
	char marker[544];
	http_stream_t stream;
	FILE *f;
	long done;
	long total;
	unsigned generation;

	// Where it came from, for picking it up after a dropped connection
	// (streamturn_resume); empty when the URL was too long to keep.
	char url[2600];
	int resumes;
	long resumed_at;
} download_t;

// Changing track bumps this counter; threads downloading something else notice
// at the next chunk and stop, so the bandwidth goes to what is playing now.
static unsigned generation = 1;
static pthread_mutex_t gen_lock = PTHREAD_MUTEX_INITIALIZER;

// ---------------------------------------------------------------------------
// one download at a time
//
// The turn is not Subsonic's, it belongs to the device, which has one network and
// one card to share with Tidal and podcasts as well, so it lives in
// streamturn.c. The full reasoning is there.
// ---------------------------------------------------------------------------

static void busy_acquire(void) { streamturn_acquire(); }
static void busy_release(void) { streamturn_release(); }

static void (*slow_start_cb)(int seconds);

void subsoniccache_set_slow_start_cb(void (*cb)(int seconds)) { slow_start_cb = cb; }

void subsoniccache_wait_idle(void) { streamturn_wait_idle(); }

// The in-progress download's stream, so another thread can jolt it awake. Only
// one runs at a time (the rule above), so a single pointer suffices. Guarded by
// its own lock and cleared before download_end frees the memory, so the wake
// never lands on a dead object.
static pthread_mutex_t active_lock = PTHREAD_MUTEX_INITIALIZER;
static http_stream_t *active_stream;

// Id of the track coming down right now (0 = none). "Is this track already
// downloading?" is answered from here, by id, not from growfile by path: the
// string comparison between the queue's path and the announced one broke on
// the device in a way the logs have not yet explained, and the id cannot be
// wrong.
static char active_key[SUBSONICCACHE_KEY_LEN];

bool subsoniccache_busy(void) {
	pthread_mutex_lock(&active_lock);
	bool busy = active_key[0] != '\0';
	pthread_mutex_unlock(&active_lock);
	return busy;
}

bool subsoniccache_is_downloading(const char *key) {
	if (!key || !key[0]) {
		return false;
	}
	pthread_mutex_lock(&active_lock);
	bool same = strcmp(active_key, key) == 0;
	pthread_mutex_unlock(&active_lock);
	return same;
}

// See subsoniccache.h. A single bool, written by the UI thread and read by the
// power loop: neither can observe half of it.
static volatile bool network_wanted;

void subsoniccache_set_network_wanted(bool wanted) { network_wanted = wanted; }
bool subsoniccache_network_wanted(void) { return network_wanted; }

void subsoniccache_abandon_all(void) {
	pthread_mutex_lock(&gen_lock);
	generation++;
	pthread_mutex_unlock(&gen_lock);

	// On a real network the counter bump alone is not enough: the downloader
	// only notices at the next chunk, and if the socket is idle (Wi-Fi
	// breathing, a slow server) the next chunk arrives after the receive
	// timeout, twenty seconds. For all that time the abandoned download holds
	// the turn and the track the user just asked for cannot start.
	//
	// The wake is the same one the radio uses: shut the socket down under the
	// blocked recv(), which returns at once, and the download loop sees the
	// changed counter and releases the turn immediately.
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

// Stops advertising this track as downloading. Call on every path that exits
// without having downloaded anything: the id is published as soon as the turn
// is taken (see subsoniccache_start), so a failed start would leave it lying, and
// whoever believes it waits for a file that is not coming.
static void clear_active(const char *key) {
	pthread_mutex_lock(&active_lock);
	if (strcmp(active_key, key) == 0) {
		active_key[0] = '\0';
	}
	pthread_mutex_unlock(&active_lock);
}

// Closes everything and records how it ended for readers of the file.
static void download_end(download_t *d, bool ok) {
	// The stream is about to die: take it out of view of whoever might jolt it
	// first, close it second. The other order is a wake on freed memory.
	pthread_mutex_lock(&active_lock);
	if (active_stream == &d->stream) {
		active_stream = NULL;
	}
	if (strcmp(active_key, d->key) == 0) {
		active_key[0] = '\0';
	}
	pthread_mutex_unlock(&active_lock);

	if (d->f) {
		if (fclose(d->f) != 0) {
			ok = false;
		}
		d->f = NULL;
	}
	http_stream_close(&d->stream);

	// Less arrived than the server promised: this is not a usable file.
	if (ok && d->total > 0 && d->done != d->total) {
		fprintf(stderr, "subsoniccache: %s cut short at %ld of %ld bytes\n", d->key, d->done, d->total);
		ok = false;
	}
	if (ok && d->done == 0) {
		ok = false;
	}

	growfile_finish(d->path, ok);

	if (ok) {
		remove(d->marker); // complete now
		printf("subsoniccache: %s complete (%ld KB)\n", d->key, d->done / 1024);
	} else {
		// The marker stays: next time subsoniccache_has() says no and the track
		// is downloaded again instead of playing half of it.
		printf("subsoniccache: %s not completed\n", d->key);
	}

	// Release the turn last, not as soon as the stream is closed: while the
	// marker is still there the track counts as incomplete, so anyone waiting
	// for the turn would start re-downloading what has just finished. This call
	// must stay below the remove().
	busy_release();

	free(d);
}

// The connection dropped mid-download: pick it up where it stopped, in the
// same file. See streamturn_resume() for the care taken with the stream an
// abandon may be waking at the same moment.
static bool download_resume(download_t *d) {
	char who[64];
	snprintf(who, sizeof(who), "subsoniccache: %s", d->key);
	streamturn_resume_t r = {
		.stream = &d->stream,
		.active_lock = &active_lock,
		.active_stream = &active_stream,
		.generation = current_generation,
		.started_generation = d->generation,
		.url = d->url,
		.have = d->done,
		.total = d->total,
		.timeout_secs = FETCH_TIMEOUT_SECS,
		.who = who,
	};
	if (!streamturn_resume(&r, &d->resumes)) {
		return false;
	}
	d->resumed_at = d->done;
	return true;
}

// Writes until `until` bytes are on disk (or, with until == 0, to the end of
// the stream). Returns false when it went wrong.
static bool download_pump(download_t *d, long until, bool *finished) {
	char buf[FETCH_CHUNK];
	*finished = false;

	while (d->done < until || until == 0) {
		if (current_generation() != d->generation) {
			return false; // something else is playing now
		}

		int n = http_stream_read(&d->stream, buf, (int)sizeof(buf));
		// An error, or an end before the promised length: the connection
		// dropped. Reconnect for the rest rather than lose what is here.
		if (n < 0 || (n == 0 && d->total > 0 && d->done < d->total)) {
			if (download_resume(d)) {
				continue;
			}
			return false;
		}
		if (n == 0) {
			*finished = true;
			return true;
		}
		if (fwrite(buf, 1, (size_t)n, d->f) != (size_t)n) {
			fprintf(stderr, "subsoniccache: write failed (card full?)\n");
			return false;
		}

		// Flush before announcing the progress: without it a reader would go
		// looking on disk for bytes still sitting in the stdio buffer.
		fflush(d->f);
		d->done += n;
		growfile_progress(d->path, d->done);
		if (d->resumes && d->done - d->resumed_at >= STREAM_RESUME_GOOD_BYTES) {
			d->resumes = 0;
		}

		if (until == 0 && *finished) {
			break;
		}
	}
	return true;
}

// The rest of the download, after playback has started.
static void *download_thread(void *arg) {
	// Below the interface and the audio threads, but not idle class: playback
	// reads this file as it grows, and an idle-class thread gets almost
	// nothing while music plays (see qobuzcache.c, and yandexcache.c since
	// 1.2.4).
	thread_be_low_priority("subsonic dl");

	download_t *d = arg;
	bool finished = false;
	bool ok = download_pump(d, 0, &finished);
	download_end(d, ok && finished);
	return NULL;
}

bool subsoniccache_start(const char *key, const char *url, const char *suffix, int duration_secs, char *out,
						  size_t size) {
	if (!out || size == 0) {
		return false;
	}
	out[0] = '\0';
	set_error("");

	if (!subsoniccache_ready()) {
		fprintf(stderr, "subsoniccache: no card to write to\n");
		set_error(tr("download_no_card"));
		return false;
	}
	if (!url || !*url) {
		set_error(tr("subsonic_no_track_address"));
		return false;
	}

	// Already fully downloaded: no network, no wait.
	if (!key_valid(key)) {
		set_error(tr("subsonic_no_track_address"));
		return false;
	}
	if (subsoniccache_has(key, suffix, out, size)) {
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
	snprintf(d->key, sizeof(d->key), "%s", key);
	if (url && strlen(url) < sizeof(d->url)) {
		snprintf(d->url, sizeof(d->url), "%s", url);
	}
	subsoniccache_path(key, suffix, d->path, sizeof(d->path));
	marker_path(d->path, d->marker, sizeof(d->marker));

	// This track is already coming down, and it is the one playing.
	//
	// Starting again would reopen the file for writing -- that is, truncate it
	// -- under the decoder reading it. Tapping the same track twice means
	// wanting to hear it, not to restart it, so return the path already in use.
	if (growfile_is_growing(d->path)) {
		snprintf(out, size, "%.500s", d->path);
		free(d);
		return true;
	}

	// One at a time: if another download is running, wait for the turn.
	//
	// Do not abandon the running one from here, tempting as it is: prefetch
	// also calls this function, for the track after the one playing.
	// Abandoning here would mean reading ahead interrupts the download of the
	// track being played -- the opposite of what is wanted. Abandoning is done
	// by whoever received a user command, which is the page: see
	// streamturn_abandon_all().
	busy_acquire();

	// Take the generation AFTER the wait: a value read before it would be stale
	// by the time the turn arrives, and a track change that happened during the
	// wait would cancel this download the moment it starts.
	d->generation = current_generation();

	// Publish the track id immediately, not once the connection is up.
	//
	// The other publication point further down sits after http_stream_open(),
	// that is after the full round of DNS, TCP, TLS and request -- easily a
	// couple of seconds. Until the id is out, subsoniccache_downloading_id()
	// answers zero for a track that is very much downloading, and callers act
	// on that: the player reports the file as missing, and prefetch abandons
	// the connection being opened for the very track it wants.
	//
	// The stream pointer stays NULL until it really exists: abandoners read it
	// to shut the socket down, and the generation counter alone is enough to
	// stop a download in the meantime.
	pthread_mutex_lock(&active_lock);
	snprintf(active_key, sizeof(active_key), "%s", d->key);
	pthread_mutex_unlock(&active_lock);

	if (!http_stream_open(&d->stream, url, FETCH_TIMEOUT_SECS)) {
		const char *why = http_last_error();
		fprintf(stderr, "subsoniccache: %s does not open%s%s\n", key, why && why[0] ? ": " : "",
				why && why[0] ? why : "");
		set_error(why && why[0] ? why : tr("subsonic_cannot_reach_server"));
		clear_active(d->key);
		busy_release();
		free(d);
		return false;
	}

	d->total = d->stream.content_length;
	prune(d->total > 0 ? d->total : 0);

	// The marker before the file: if power is lost between the two lines, an
	// orphan marker (which does nothing) beats an incomplete file that looks
	// good.
	FILE *marker = fopen(d->marker, "wb");
	if (marker) {
		fclose(marker);
	}

	d->f = fopen(d->path, "wb");
	if (!d->f) {
		fprintf(stderr, "subsoniccache: %s does not open for writing: %s\n", d->path, strerror(errno));
		set_error(errno == ENOSPC ? tr("card_full") : tr("card_write_failed"));
		http_stream_close(&d->stream);
		clear_active(d->key);
		busy_release();
		free(d);
		return false;
	}

	// From here on this is the download in progress, and an abandon must be
	// able to shut its socket down (see subsoniccache_abandon_all).
	pthread_mutex_lock(&active_lock);
	active_stream = &d->stream;
	snprintf(active_key, sizeof(active_key), "%s", d->key);
	pthread_mutex_unlock(&active_lock);

	growfile_announce(d->path, d->total);

	// --- prebuffer, measured rather than guessed -----------------------------
	//
	// A fixed number cannot work: half a megabyte is twelve seconds of an MP3
	// and not even one of a 24/176.4 track. What matters is not how much has
	// been downloaded but whether the download keeps up with playback.
	//
	// So a first chunk is downloaded and timed, which gives the network rate.
	// With R the rate the track consumes (bytes per second) and M the measured
	// one, starting with H bytes in hand the reserve at time t is
	// H + (M - R)t, and the download finishes at t = (total - H)/M. For the
	// reserve not to run out first:
	//
	//     H >= total * (1 - M/R)
	//
	// If the network is faster than the track (M >= R) a sliver is enough. If
	// it is slower, the formula says exactly how much must be in hand before
	// starting -- and if it is much slower it says "all of it", which is true.
	long want = STREAM_PREBUFFER;
	if (d->total > 0 && d->total < want) {
		want = d->total;
	}

	long probe = want < STREAM_PROBE ? want : STREAM_PROBE;
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

		// Under a third of a second the measurement is not a measurement: the
		// first chunk can arrive all at once out of the socket buffer and make
		// the network look ten times faster than it is. Better no figure than
		// a false one; the seconds-based floor below stands on its own.
		double measured = elapsed > 0.3 ? d->done / elapsed : 0.0;
		double required = (double)d->total / duration_secs;

		// The floor: a few seconds of music in hand regardless, even when the
		// network looks very fast. It keeps the network's first hiccup from
		// stopping playback right after it starts.
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
			// A fifth of margin: a measurement over one chunk is noisy, and a
			// wobbling network must not start a track that will stall at once.
			double head = (double)d->total * (1.0 - measured / (required * 1.2));

			// Capped, though. Without a cap, on a network slower than the
			// track this formula asks for nearly the whole file -- minutes of
			// waiting before the first note. The refill in audio.c makes
			// "never stall" unnecessary: start with little and, if the network
			// cannot keep up, pause briefly to build a reserve. See
			// subsoniccache.h.
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
			printf("subsoniccache: %s needs %.0f KB/s, the network gives %.0f -> starting with %ld KB of %ld\n", key,
				   required / 1024, measured / 1024, want / 1024, d->total / 1024);

			// If the wait is measured in seconds, tell the user.
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
		// Short track: all of it is already here.
		snprintf(out, size, "%.500s", d->path);
		download_end(d, true);
		return true;
	}

	snprintf(out, size, "%.500s", d->path);

	// Both numbers are taken before starting the thread: from that moment `d`
	// belongs to it, and a short file can finish downloading and free it before
	// any line below could read them.
	long started_done = d->done;
	long started_total = d->total;

	pthread_t thread;
	if (pthread_create(&thread, NULL, download_thread, d) != 0) {
		set_error(tr("cannot_start_the_download"));
		download_end(d, false); // also releases the turn
		out[0] = '\0';
		return false;
	}
	pthread_detach(thread);

	printf("subsoniccache: %s starts at %ld KB of %ld KB\n", key, started_done / 1024,
		   started_total > 0 ? started_total / 1024 : 0);
	return true;
}

// ---------------------------------------------------------------------------
// sidecar files: what the API knows and the audio file does not
// ---------------------------------------------------------------------------

const char *subsoniccache_dir(void) { return cache_dir[0] ? cache_dir : NULL; }

bool subsoniccache_owns(const char *path) {
	return path && cache_dir[0] && strncmp(path, cache_dir, strlen(cache_dir)) == 0;
}

void subsoniccache_write_sidecars(const char *key, const char *suffix, const char *song_id, const char *title,
								  const char *artist, const char *album, const char *album_id, int track_number,
								  const char *cover_url) {
	if (!subsoniccache_ready()) {
		return;
	}

	char path[512];
	subsoniccache_path(key, suffix, path, sizeof(path));
	if (!path[0]) {
		return;
	}

	char tags[544];
	snprintf(tags, sizeof(tags), "%.500s.tags", path);

	// Atomic write: a temporary file first, then rename(). fopen(.., "w")
	// straight onto the .tags truncates it to zero before writing, and anyone
	// reading the metadata at that instant -- device_state at track start,
	// from another thread -- sees an empty file and falls back to the file
	// name. A rename within the same filesystem is indivisible: either the old
	// file or the new one is seen, never a half-written one.
	char tags_tmp[560];
	snprintf(tags_tmp, sizeof(tags_tmp), "%s.tmp", tags);
	FILE *f = fopen(tags_tmp, "w");
	if (f) {
		// Deliberately dumb format: one line per field, key and value
		// separated by an equals sign. metadata.c reads it back in thirty
		// lines and a human can read it without tools.
		fprintf(f, "title=%s\n", title ? title : "");
		fprintf(f, "artist=%s\n", artist ? artist : "");
		fprintf(f, "album=%s\n", album ? album : "");
		// The album id is not a tag and is never displayed: it is what "show
		// album" in the player needs to know which Subsonic album to open. The
		// name is not enough, since different albums share a title.
		fprintf(f, "album_id=%s\n", album_id ? album_id : "");
		// And the server's song id: the file name is a hash of it, which
		// cannot be turned back, and the star and scrobbles need the id itself.
		fprintf(f, "id=%s\n", song_id ? song_id : "");
		fprintf(f, "track=%d\n", track_number);
		fclose(f);
		if (rename(tags_tmp, tags) != 0) {
			remove(tags_tmp);
		}
	}

	if (!cover_url || !*cover_url) {
		return;
	}

	// The cover takes the track's name: the cover loader already looks for
	// "<track name>.jpg" next to the file, so it needs no extra rule.
	char cover[544];
	const char *dot = strrchr(path, '.');
	snprintf(cover, sizeof(cover), "%.*s.jpg", dot ? (int)(dot - path) : (int)strlen(path), path);

	struct stat st;
	if (stat(cover, &st) == 0 && st.st_size > 0) {
		return; // already downloaded
	}

	char *body = NULL;
	size_t len = 0;
	if (!http_get(cover_url, &body, &len, 4 * 1024 * 1024, FETCH_TIMEOUT_SECS) || !body) {
		return;
	}

	FILE *img = fopen(cover, "wb");
	if (img) {
		fwrite(body, 1, len, img);
		fclose(img);
	}
	free(body);
}

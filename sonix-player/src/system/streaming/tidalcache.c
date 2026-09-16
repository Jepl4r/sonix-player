#include "tidalcache.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <pthread.h>

#include "src/system/decode/growfile.h"
#include "src/system/net/http.h"
#include "src/system/library/mp4flac.h"
#include "src/system/streaming/streamturn.h"
#include "src/system/core/utils.h"
#include "src/system/core/lang.h"

#define FETCH_TIMEOUT_SECS 20
#define FETCH_CHUNK 32768

// A DASH segment is a few hundred kilobytes -- four seconds of audio -- and is
// fetched into memory whole before being unwrapped. The ceiling is deliberately
// generous: it only stops a runaway response from taking all the RAM there is.
#define SEGMENT_LIMIT (8 * 1024 * 1024)

// How many segments are fetched before playback starts. Each is worth about
// four seconds, so three put roughly a dozen seconds in hand: the same order of
// magnitude as the Qobuz prebuffer, arrived at by a different route.
#define DASH_HEAD_SEGMENTS 3

static char cache_dir[512];

static __thread char last_error[192];

const char *tidalcache_last_error(void) { return last_error; }

static void set_error(const char *what) { snprintf(last_error, sizeof(last_error), "%s", what ? what : ""); }

void tidalcache_abandon_all(void);

static void register_abandon_once(void) {
	static bool done;
	if (!done) {
		done = true;
		streamturn_register_abandon(tidalcache_abandon_all);
	}
}

void tidalcache_set_root(const char *sd_root) {
	register_abandon_once();
	if (!sd_root || !*sd_root) {
		cache_dir[0] = '\0';
		return;
	}
	snprintf(cache_dir, sizeof(cache_dir), "%s/%s", sd_root, TIDALCACHE_DIR);
}

bool tidalcache_ready(void) { return cache_dir[0] != '\0'; }

// The extension from the MIME type. The decoder picks by extension: nothing
// will open a FLAC called .bin.
static const char *extension_for(const char *mime) {
	if (!mime || !*mime) {
		return "flac";
	}
	if (strstr(mime, "mpeg") || strstr(mime, "mp3")) {
		return "mp3";
	}
	if (strstr(mime, "flac")) {
		return "flac";
	}
	if (strstr(mime, "mp4") || strstr(mime, "m4a") || strstr(mime, "aac")) {
		return "m4a";
	}
	return "flac";
}

// For a real stream the codec is checked before the container, which is where
// Tidal differs from Qobuz: a DASH manifest declares mimeType "audio/mp4" even
// when FLAC is inside, because MP4 is only the wrapper. After mp4flac the
// wrapper is gone and the file is a .flac for all purposes -- calling it .m4a
// would hand it to the wrong decoder.
const char *tidalcache_extension(const tidal_stream_t *stream) {
	if (!stream) {
		return "flac";
	}
	if (stream->kind == TIDAL_STREAM_DASH) {
		return mp4flac_extension_for(stream->codecs);
	}
	if (stream->codecs[0] && (strncmp(stream->codecs, "flac", 4) == 0 || strncmp(stream->codecs, "fLaC", 4) == 0)) {
		return "flac";
	}
	return extension_for(stream->mime);
}

void tidalcache_path(long track_id, const char *mime, char *out, size_t size) {
	if (!out || size == 0) {
		return;
	}
	if (!tidalcache_ready()) {
		out[0] = '\0';
		return;
	}
	// The folder is clamped to what fits alongside the file name: a path
	// truncated halfway would open, or delete, something else entirely, and
	// do it silently.
	snprintf(out, size, "%.400s/%ld.%s", cache_dir, track_id, extension_for(mime));
}

static void path_with_ext(long track_id, const char *ext, char *out, size_t size) {
	snprintf(out, size, "%.400s/%ld.%s", cache_dir, track_id, ext);
}

// The file may exist and still be an interrupted download: while it grows an
// empty marker sits beside it, removed only once the download is complete.
static void marker_path(const char *path, char *out, size_t size) {
	snprintf(out, size, "%.500s.incompleto", path);
}

static bool have_file(const char *path, char *out, size_t size) {
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

bool tidalcache_has(long track_id, const char *mime, char *out, size_t size) {
	char path[512];
	tidalcache_path(track_id, mime, path, sizeof(path));
	if (!path[0]) {
		return false;
	}
	return have_file(path, out, size);
}

bool tidalcache_find(long track_id, char *out, size_t size) {
	if (!tidalcache_ready()) {
		if (out && size) {
			out[0] = '\0';
		}
		return false;
	}
	static const char *const EXTS[] = {"flac", "m4a", "mp3"};
	for (size_t i = 0; i < sizeof(EXTS) / sizeof(EXTS[0]); i++) {
		char path[512];
		path_with_ext(track_id, EXTS[i], path, sizeof(path));
		if (have_file(path, out, size)) {
			return true;
		}
	}
	if (out && size) {
		out[0] = '\0';
	}
	return false;
}

// mkdir -p over the two levels needed: .local, then the cache folder.
static bool ensure_dir(void) {
	if (!tidalcache_ready()) {
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
		fprintf(stderr, "tidalcache: cannot create %s: %s\n", path, strerror(errno));
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
	long atime; // last access: what tells which tracks are still listened to
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

		// What is downloading right now cannot be deleted, nor can its
		// sidecars: the name is compared without its extension, so
		// "101.flac", "101.flac.tags" and "101.jpg" are all protected while
		// "101.flac" is still growing.
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

long tidalcache_bytes(void) {
	if (!tidalcache_ready()) {
		return 0;
	}
	long total = 0;
	scan(NULL, 0, &total);
	return total;
}

#define PRUNE_MAX_ENTRIES 512

// Drops the least recently played until the cache fits again. By last access
// and not creation date: an album downloaded a month ago but played again
// yesterday has to stay.
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
	if (total + keep_room_for <= TIDALCACHE_MAX_BYTES) {
		free(entries);
		return;
	}

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

	for (int i = 0; i < count && total + keep_room_for > TIDALCACHE_MAX_BYTES; i++) {
		char path[640];
		snprintf(path, sizeof(path), "%.500s/%.63s", cache_dir, entries[i].name);
		if (remove(path) == 0) {
			total -= entries[i].size;
			printf("tidalcache: removed %s (%ld KB)\n", entries[i].name, entries[i].size / 1024);
		}
	}

	free(entries);
}

static bool is_recent_stem(const char *stem);

// The tracks the queue still has ahead of it, which the automatic clear must
// not touch.
static pthread_mutex_t protected_lock = PTHREAD_MUTEX_INITIALIZER;
static char protected_paths[TIDALCACHE_PROTECTED_MAX][512];
static int protected_count;

void tidalcache_set_protected(const char *const *paths, int count) {
	pthread_mutex_lock(&protected_lock);
	protected_count = 0;
	for (int i = 0; i < count && protected_count < TIDALCACHE_PROTECTED_MAX; i++) {
		if (paths[i] && paths[i][0]) {
			snprintf(protected_paths[protected_count++], sizeof(protected_paths[0]), "%s", paths[i]);
		}
	}
	pthread_mutex_unlock(&protected_lock);
}

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

static void clear_files(bool keep_playing) {
	if (!tidalcache_ready()) {
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
	printf("tidalcache: cleared (%d files)\n", removed);
}

void tidalcache_clear(void) { clear_files(false); }

// ---------------------------------------------------------------------------
// the periodic clear
// ---------------------------------------------------------------------------

static int played_since_clear;

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

void tidalcache_note_played(const char *path) {
	if (!tidalcache_ready()) {
		return;
	}

	if (path && path[0]) {
		pthread_mutex_lock(&recent_lock);
		snprintf(recent_paths[recent_next], sizeof(recent_paths[0]), "%s", path);
		recent_next = (recent_next + 1) % RECENT_MAX;
		pthread_mutex_unlock(&recent_lock);
	}

	played_since_clear++;
	if (played_since_clear < TIDALCACHE_CLEAR_EVERY) {
		return;
	}
	played_since_clear = 0;
	printf("tidalcache: %d tracks since the last sweep, cleaning up\n", TIDALCACHE_CLEAR_EVERY);
	clear_files(true);
}

// Cover art lives in a sibling folder of the tracks (.local/tidal-art next to
// .local/tidal-cache) and has the same lifetime: transient data that must not
// survive a power cycle.
static void clear_art_files(void) {
	char art[sizeof(cache_dir) + 16];
	snprintf(art, sizeof(art), "%s", cache_dir);
	char *slash = strrchr(art, '/');
	if (!slash) {
		return;
	}
	snprintf(slash + 1, sizeof(art) - (size_t)(slash + 1 - art), "tidal-art");

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
	printf("tidalcache: covers cleared (%d files)\n", removed);
}

void tidalcache_clear_on_exit(void) {
	if (!tidalcache_ready()) {
		return;
	}
	printf("tidalcache: shutting down, clearing the cache\n");
	clear_files(false);
	clear_art_files();
}

// ---------------------------------------------------------------------------
// the sidecar files
// ---------------------------------------------------------------------------

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

const char *tidalcache_dir(void) { return cache_dir[0] ? cache_dir : NULL; }

bool tidalcache_owns(const char *path) {
	return path && cache_dir[0] && strncmp(path, cache_dir, strlen(cache_dir)) == 0;
}

bool tidalcache_album_id(const char *path, char *out, size_t size) {
	if (!tidalcache_owns(path)) {
		if (out && size) {
			out[0] = '\0';
		}
		return false;
	}
	return read_tag(path, "album_id", out, size);
}

long tidalcache_track_id(const char *path) {
	if (!tidalcache_owns(path)) {
		return 0;
	}
	const char *name = strrchr(path, '/');
	name = name ? name + 1 : path;
	if (*name < '0' || *name > '9') {
		return 0;
	}
	char *end = NULL;
	long id = strtol(name, &end, 10);
	// The digits must be followed by the extension's dot: "12.flac" counts,
	// "12abc.flac" does not.
	if (!end || *end != '.' || id <= 0) {
		return 0;
	}
	return id;
}

void tidalcache_write_sidecars(long track_id, const char *mime, const char *title, const char *artist,
							   const char *album, const char *album_id, int track_number, const char *cover_url) {
	if (!tidalcache_ready()) {
		return;
	}

	// The real file, not the one the MIME type would suggest.
	//
	// For a hi-res track the two differ: the DASH manifest declares
	// "audio/mp4" (the wrapper), but after mp4flac the wrapper is gone and the
	// file on the card is named .flac. Trusting the MIME type would put the
	// tags in "<id>.m4a.tags", which nothing reads.
	char path[512];
	if (!tidalcache_find(track_id, path, sizeof(path))) {
		tidalcache_path(track_id, mime, path, sizeof(path));
	}
	if (!path[0]) {
		return;
	}

	char tags[544];
	snprintf(tags, sizeof(tags), "%.500s.tags", path);

	// Atomic write: a temporary file first, then rename(). fopen(.., "w")
	// straight onto the .tags truncates it to zero before writing, and a
	// reader of the metadata at that instant -- device_state at track start,
	// on another thread -- would find an empty file.
	char tags_tmp[560];
	snprintf(tags_tmp, sizeof(tags_tmp), "%s.tmp", tags);
	FILE *f = fopen(tags_tmp, "w");
	if (f) {
		fprintf(f, "title=%s\n", title ? title : "");
		fprintf(f, "artist=%s\n", artist ? artist : "");
		fprintf(f, "album=%s\n", album ? album : "");
		fprintf(f, "album_id=%s\n", album_id ? album_id : "");
		fprintf(f, "track=%d\n", track_number);
		// The size the cover beside it was fetched at: this is how a changed
		// setting is noticed and the cover re-fetched.
		fprintf(f, "cover_px=%d\n", tidal_cover_px());
		fclose(f);
		if (rename(tags_tmp, tags) != 0) {
			remove(tags_tmp);
		}
	}

	if (!cover_url || !*cover_url) {
		return;
	}

	// The name stays "<id>.jpg", and has to.
	//
	// Putting the size in the name -- "<id>-480.jpg" -- would make a size
	// change a new file, but the cover is looked up by albumart.c, which next
	// to the track looks for an image with the track's own name minus the
	// extension: "<id>.flac" -> "<id>.jpg". With the size glued to the name
	// that match fails and no cover is found at all, trading "a bit soft" for
	// "nothing".
	//
	// The size is recorded in the tags file next door instead. If the recorded
	// size is not the one being asked for now, the cover is fetched again, so
	// changing `[tidal] cover_size` also applies to tracks already cached.
	char cover[544];
	const char *dot = strrchr(path, '.');
	snprintf(cover, sizeof(cover), "%.*s.jpg", dot ? (int)(dot - path) : (int)strlen(path), path);

	int want_px = tidal_cover_px();
	char had[16] = "";
	bool same_size = read_tag(path, "cover_px", had, sizeof(had)) && atoi(had) == want_px;

	struct stat st;
	if (same_size && stat(cover, &st) == 0 && st.st_size > 0) {
		return; // already fetched, and at this size
	}

	char *body = NULL;
	size_t len = 0;
	if (!http_get(cover_url, &body, &len, 4 * 1024 * 1024, FETCH_TIMEOUT_SECS) || !body) {
		const char *why = http_last_error();
		fprintf(stderr, "tidalcache: cover '%s' not downloaded: %s\n", cover_url,
				why && why[0] ? why : "no reason given");
		return;
	}

	FILE *img = fopen(cover, "wb");
	if (img) {
		fwrite(body, 1, len, img);
		fclose(img);
	}
	free(body);
}

// ---------------------------------------------------------------------------
// downloading
//
// Two shapes, and they really are different.
//
//   FILE   one socket, opened and held open, read in chunks. Same as Qobuz.
//   DASH   one request per segment, each complete, the content handed to
//          mp4flac, which unwraps it into the file.
//
// What does not change is what the rest of the player sees: a growing file,
// announced to growfile, openable by the decoder before it is finished.
// ---------------------------------------------------------------------------

typedef struct {
	long track_id;
	char path[512];
	char marker[544];
	FILE *f;
	long done;	// bytes written to the file
	long total; // bytes expected, 0 when unknown
	unsigned generation;

	tidal_stream_t stream;

	// FILE
	http_stream_t http;
	bool http_open;

	// DASH
	mp4flac_t unwrap;
	int next_segment; // the next one to request, in Tidal's numbering
} download_t;

static unsigned generation = 1;
static pthread_mutex_t gen_lock = PTHREAD_MUTEX_INITIALIZER;

static void (*slow_start_cb)(int seconds);

void tidalcache_set_slow_start_cb(void (*cb)(int seconds)) { slow_start_cb = cb; }

void tidalcache_wait_idle(void) { streamturn_wait_idle(); }

static pthread_mutex_t active_lock = PTHREAD_MUTEX_INITIALIZER;
static http_stream_t *active_stream;
static long active_track_id;

long tidalcache_downloading_id(void) {
	pthread_mutex_lock(&active_lock);
	long id = active_track_id;
	pthread_mutex_unlock(&active_lock);
	return id;
}

static volatile bool network_wanted;

void tidalcache_set_network_wanted(bool wanted) { network_wanted = wanted; }
bool tidalcache_network_wanted(void) { return network_wanted; }

void tidalcache_abandon_all(void) {
	pthread_mutex_lock(&gen_lock);
	generation++;
	pthread_mutex_unlock(&gen_lock);

	// The tug on the socket, as in qobuzcache: bumping the generation alone is
	// not enough, because the downloader only notices at the next chunk and a
	// stalled socket can keep it waiting twenty seconds.
	//
	// FILE only. DASH has no socket to tug -- each segment is its own request
	// -- and needs none: a segment is four seconds of audio, a few hundred
	// kilobytes, so the check between segments comes round fast enough not to
	// be noticed.
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

static void download_end(download_t *d, bool ok) {
	// The stream is about to die: take it out of sight of anything that might
	// tug it first, and only then close it.
	pthread_mutex_lock(&active_lock);
	if (active_stream == &d->http) {
		active_stream = NULL;
	}
	if (active_track_id == d->track_id) {
		active_track_id = 0;
	}
	pthread_mutex_unlock(&active_lock);

	if (d->f) {
		if (fclose(d->f) != 0) {
			ok = false;
		}
		d->f = NULL;
	}
	if (d->http_open) {
		http_stream_close(&d->http);
		d->http_open = false;
	}

	if (ok && d->stream.kind == TIDAL_STREAM_FILE && d->total > 0 && d->done != d->total) {
		fprintf(stderr, "tidalcache: %ld cut short at %ld of %ld bytes\n", d->track_id, d->done, d->total);
		ok = false;
	}
	// On DASH the file starts with a locally written header, so it is never
	// empty even when not one sample arrived. The right comparison is against
	// that header: a file still exactly its length got no music, and marking
	// it complete would keep a silent track in the cache forever, never
	// downloaded again.
	if (ok && d->stream.kind == TIDAL_STREAM_DASH && d->done <= d->unwrap.header_bytes) {
		fprintf(stderr, "tidalcache: %ld brought no samples\n", d->track_id);
		ok = false;
	}
	if (ok && d->done == 0) {
		ok = false;
	}

	growfile_finish(d->path, ok);

	if (ok) {
		remove(d->marker); // complete now
		printf("tidalcache: %ld complete (%ld KB)\n", d->track_id, d->done / 1024);
	} else {
		// The marker is left in place: next time round the track counts as
		// incomplete and is downloaded again instead of playing half of it.
		printf("tidalcache: %ld not completed\n", d->track_id);
	}

	// The turn is released last, not as soon as the stream is closed: while
	// the marker is still there the track counts as incomplete, and whoever
	// was waiting for the turn would start downloading what has just finished
	// downloading.
	streamturn_release();

	free(d);
}

// --- the FILE shape --------------------------------------------------------

static bool pump_file(download_t *d, long until, bool *finished) {
	char buf[FETCH_CHUNK];
	*finished = false;

	while (d->done < until || until == 0) {
		if (current_generation() != d->generation) {
			return false; // something else is playing now
		}

		int n = http_stream_read(&d->http, buf, (int)sizeof(buf));
		if (n < 0) {
			return false;
		}
		if (n == 0) {
			*finished = true;
			return true;
		}
		if (fwrite(buf, 1, (size_t)n, d->f) != (size_t)n) {
			fprintf(stderr, "tidalcache: write failed (card full?)\n");
			return false;
		}

		// Push out what was written before announcing it: without the fflush a
		// reader would go looking on the card for bytes still sitting in the
		// stdio buffer.
		fflush(d->f);
		d->done += n;
		growfile_progress(d->path, d->done);
	}
	return true;
}

// --- the DASH shape --------------------------------------------------------

// One segment: request, unwrap, write. `count` of them, or to the end when
// count is 0.
static bool pump_dash(download_t *d, int count, bool *finished) {
	*finished = false;

	// In long: the sum of two ints coming off the network can overflow, and a
	// negative limit ended the track before it began.
	long last = (long)d->stream.start_number + (long)d->stream.segment_count;

	int done_now = 0;
	while (count == 0 || done_now < count) {
		if ((long)d->next_segment >= last) {
			*finished = true;
			return true;
		}
		if (current_generation() != d->generation) {
			return false;
		}

		char url[TIDAL_URL_MAX];
		if (!tidal_dash_segment_url(&d->stream, d->next_segment, url, sizeof(url))) {
			fprintf(stderr, "tidalcache: cannot build segment %d\n", d->next_segment);
			return false;
		}

		char *body = NULL;
		size_t len = 0;
		if (!http_get(url, &body, &len, SEGMENT_LIMIT, FETCH_TIMEOUT_SECS) || !body) {
			fprintf(stderr, "tidalcache: segment %d did not arrive\n", d->next_segment);
			free(body);
			return false;
		}
		if (len == 0) {
			// An empty segment is not a segment. Carrying on would leave a
			// silent gap in the middle of the track, in a file then marked
			// complete and never downloaded again.
			fprintf(stderr, "tidalcache: segment %d empty\n", d->next_segment);
			free(body);
			return false;
		}

		bool ok = mp4flac_segment(&d->unwrap, (const uint8_t *)body, len);
		free(body);
		if (!ok) {
			return false;
		}

		fflush(d->f);
		d->done = d->unwrap.written;
		growfile_progress(d->path, d->done);

		d->next_segment++;
		done_now++;
	}
	return true;
}

static void *download_thread(void *arg) {
	// In the background like every other worker: there is only one core, and a
	// thread writing to the card at full priority takes it from the interface.
	thread_be_background("tidal download");

	download_t *d = arg;
	bool finished = false;
	bool ok = d->stream.kind == TIDAL_STREAM_DASH ? pump_dash(d, 0, &finished) : pump_file(d, 0, &finished);
	download_end(d, ok && finished);
	return NULL;
}

// --- starting a download ---------------------------------------------------

// What both shapes have in common: the marker and the output file.
static bool open_output(download_t *d) {
	// The marker before the file: if power is lost between the two lines, an
	// orphan marker (which does nothing) is better than an incomplete file
	// that looks good.
	FILE *marker = fopen(d->marker, "wb");
	if (marker) {
		fclose(marker);
	}

	d->f = fopen(d->path, "wb");
	if (!d->f) {
		fprintf(stderr, "tidalcache: %s does not open for writing: %s\n", d->path, strerror(errno));
		set_error(errno == ENOSPC ? tr("card_full") : tr("card_write_failed"));
		return false;
	}
	return true;
}

// The two start_* functions have a precise contract with their caller, spelled
// out because getting it wrong here is a use-after-free.
//
// `*consumed` comes out true when download_end() has already run: the track
// finished during prebuffering, or it failed after the file was open. In both
// cases `d` no longer exists, and the caller must not touch it nor release the
// turn (download_end already did).
//
// False means `d` is still alive: either it went well and the rest downloads
// on a thread, or it failed before opening anything and must be freed by hand.
static bool start_dash(download_t *d, bool *consumed, char *out, size_t size) {
	// The init segment. Not audio: it carries the STREAMINFO, without which
	// the file is not a FLAC. If it does not arrive there is nothing to be
	// done, and saying so now beats discovering it at the first frame.
	char *init = NULL;
	size_t init_len = 0;
	if (!http_get(d->stream.init_url, &init, &init_len, SEGMENT_LIMIT, FETCH_TIMEOUT_SECS) || !init) {
		const char *why = http_last_error();
		set_error(why && why[0] ? why : tr("tidal_cannot_reach_tidal"));
		free(init);
		return false;
	}

	// Make room before starting, which needs an idea of how much the track
	// will take. A single file has Content-Length; DASH does not, so it is
	// estimated: a segment is about four seconds, and 24-bit FLAC runs about
	// half a megabyte per second. The estimate is coarse and deliberately high
	// -- pruning a little more than necessary costs a cover re-fetch, pruning
	// too little fills the user's card. prune(0) here would prune nothing at
	// all and let the cache grow past its ceiling.
	prune((long)d->stream.segment_count * 2L * 1024L * 1024L);

	if (!open_output(d)) {
		free(init);
		return false;
	}

	bool ok = mp4flac_begin(&d->unwrap, d->f, d->stream.codecs, (const uint8_t *)init, init_len);
	free(init);
	if (!ok) {
		set_error(tr("tidal_format_unsupported"));
		fclose(d->f);
		d->f = NULL;
		return false;
	}
	d->done = d->unwrap.written;
	d->next_segment = d->stream.start_number;

	growfile_announce(d->path, 0); // total unknown: it emerges as the file grows
	snprintf(out, size, "%.500s", d->path);

	// Prebuffer: a few segments in hand before the decoder starts. Each is
	// worth about four seconds.
	int head = DASH_HEAD_SEGMENTS;
	if (head > d->stream.segment_count) {
		head = d->stream.segment_count;
	}

	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	bool finished = false;
	if (!pump_dash(d, head, &finished)) {
		set_error(current_generation() != d->generation ? tr("download_cancelled") : tr("download_interrupted"));
		download_end(d, false);
		*consumed = true;
		out[0] = '\0';
		return false;
	}

	// If the first segments took more than a couple of seconds, the rest will
	// take as long per piece, which is worth telling the user.
	if (!finished && slow_start_cb) {
		struct timespec t1;
		clock_gettime(CLOCK_MONOTONIC, &t1);
		double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
		if (elapsed >= 2.0) {
			slow_start_cb((int)elapsed);
		}
	}

	if (finished) {
		download_end(d, true); // short track: it is all here already
		*consumed = true;
	}
	return true;
}

static bool start_file(download_t *d, bool *consumed, int duration_secs, char *out, size_t size) {
	if (!http_stream_open(&d->http, d->stream.url, FETCH_TIMEOUT_SECS)) {
		const char *why = http_last_error();
		fprintf(stderr, "tidalcache: %ld does not open%s%s\n", d->track_id, why && why[0] ? ": " : "",
				why && why[0] ? why : "");
		set_error(why && why[0] ? why : tr("tidal_cannot_reach_tidal"));
		return false;
	}
	d->http_open = true;
	d->total = d->http.content_length;
	prune(d->total > 0 ? d->total : 0);

	if (!open_output(d)) {
		return false;
	}

	pthread_mutex_lock(&active_lock);
	active_stream = &d->http;
	pthread_mutex_unlock(&active_lock);

	growfile_announce(d->path, d->total);

	// The prebuffer, identical to Qobuz's: a first chunk is downloaded against
	// the clock, which gives the network's speed, and from that how much must
	// be in hand for playback not to catch up with the download. The reasoning
	// in full is in qobuzcache.c and qobuzcache.h: same formula, same numbers,
	// and a change has to be made in both places.
	long want = STREAM_PREBUFFER;
	if (d->total > 0 && d->total < want) {
		want = d->total;
	}

	long probe = want < STREAM_PROBE ? want : STREAM_PROBE;
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	bool finished = false;
	if (!pump_file(d, probe, &finished)) {
		set_error(current_generation() != d->generation ? tr("download_cancelled") : tr("download_interrupted"));
		download_end(d, false);
		*consumed = true;
		out[0] = '\0';
		return false;
	}

	if (!finished && d->total > 0 && duration_secs > 0) {
		struct timespec t1;
		clock_gettime(CLOCK_MONOTONIC, &t1);
		double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

		double measured = elapsed > 0.3 ? d->done / elapsed : 0.0;
		double required = (double)d->total / duration_secs;

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
			double head = (double)d->total * (1.0 - measured / (required * 1.2));
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
			printf("tidalcache: %ld needs %.0f KB/s, the network gives %.0f -> starting with %ld KB of %ld\n", d->track_id,
				   required / 1024, measured / 1024, want / 1024, d->total / 1024);

			int wait_secs = (int)((want - d->done) / measured);
			if (wait_secs >= 2 && slow_start_cb) {
				slow_start_cb(wait_secs);
			}
		}
	}

	if (!finished && d->done < want && !pump_file(d, want, &finished)) {
		set_error(current_generation() != d->generation ? tr("download_cancelled") : tr("download_interrupted"));
		download_end(d, false);
		*consumed = true;
		out[0] = '\0';
		return false;
	}

	snprintf(out, size, "%.500s", d->path);

	if (finished) {
		download_end(d, true); // short track: it is all here already
		*consumed = true;
	}
	return true;
}

bool tidalcache_start(long track_id, const tidal_stream_t *stream, int duration_secs, char *out, size_t size) {
	if (!out || size == 0) {
		return false;
	}
	out[0] = '\0';
	set_error("");

	if (!tidalcache_ready()) {
		fprintf(stderr, "tidalcache: no card to write to\n");
		set_error(tr("download_no_card"));
		return false;
	}
	if (!stream) {
		set_error(tr("tidal_no_track_address"));
		return false;
	}
	if (stream->kind == TIDAL_STREAM_FILE && !stream->url[0]) {
		set_error(tr("tidal_no_track_address"));
		return false;
	}
	if (stream->kind == TIDAL_STREAM_DASH && (!stream->init_url[0] || stream->segment_count <= 0)) {
		set_error(tr("tidal_manifest_incomplete"));
		return false;
	}

	const char *ext = tidalcache_extension(stream);

	char path[512];
	path_with_ext(track_id, ext, path, sizeof(path));

	// Already downloaded in full: no network, no wait.
	if (have_file(path, out, size)) {
		return true;
	}

	if (!ensure_dir()) {
		set_error(tr("cache_folder_failed"));
		return false;
	}

	// This track is already downloading, and it is the one being listened to.
	// Starting over would mean reopening the file for writing -- truncating it
	// -- under the nose of the decoder reading it.
	if (growfile_is_growing(path)) {
		snprintf(out, size, "%.500s", path);
		return true;
	}

	download_t *d = calloc(1, sizeof(*d));
	if (!d) {
		set_error(tr("out_of_memory"));
		return false;
	}
	d->track_id = track_id;
	d->stream = *stream;
	snprintf(d->path, sizeof(d->path), "%s", path);
	marker_path(d->path, d->marker, sizeof(d->marker));

	// One at a time, and the turn is shared with Qobuz: there is only one
	// network.
	//
	// The download in progress is not abandoned from here: this function is
	// also called by the prefetch, and abandoning would cut off the track
	// being listened to in order to get ahead with the next one. Abandoning is
	// done by whoever received a user command -- the page, via
	// streamturn_abandon_all().
	streamturn_acquire();

	// The generation is taken AFTER the wait: a value read before it would be
	// stale by the time the turn arrives, and a track change that happened
	// during the wait would cancel this download the moment it starts.
	d->generation = current_generation();

	pthread_mutex_lock(&active_lock);
	active_track_id = track_id;
	pthread_mutex_unlock(&active_lock);

	bool consumed = false;
	bool started = d->stream.kind == TIDAL_STREAM_DASH ? start_dash(d, &consumed, out, size)
													   : start_file(d, &consumed, duration_secs, out, size);

	// From here on `d` may be touched only while consumed is false: when true,
	// download_end() has already freed it, turn included.
	if (consumed) {
		return started;
	}

	if (!started) {
		// Failed before opening anything: nothing to close here except the
		// turn, which download_end never got the chance to release.
		if (d->http_open) {
			http_stream_close(&d->http);
		}
		pthread_mutex_lock(&active_lock);
		if (active_stream == &d->http) {
			active_stream = NULL;
		}
		if (active_track_id == track_id) {
			active_track_id = 0;
		}
		pthread_mutex_unlock(&active_lock);
		streamturn_release();
		free(d);
		return false;
	}

	// Both values are read before the thread starts: from that moment `d`
	// belongs to the thread, and a short file can download in full and free it
	// before any line below could read it.
	long started_done = d->done;
	bool started_dash = d->stream.kind == TIDAL_STREAM_DASH;

	pthread_t thread;
	if (pthread_create(&thread, NULL, download_thread, d) != 0) {
		set_error(tr("cannot_start_the_download"));
		download_end(d, false); // releases the turn as well
		out[0] = '\0';
		return false;
	}
	pthread_detach(thread);

	printf("tidalcache: %ld starts at %ld KB%s\n", track_id, started_done / 1024,
		   started_dash ? " (segments)" : "");
	return true;
}

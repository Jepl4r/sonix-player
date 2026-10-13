#include "yandexcache.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/decode/growfile.h"
#include "src/system/library/mp4flac.h"
#include "src/system/net/crypto.h"
#include "src/system/net/http.h"
#include "src/system/streaming/provider.h"
#include "src/system/streaming/streamturn.h"

// See yandexcache.h.

#define PREBUFFER_BYTES (384L * 1024)
#define MAX_BOX (32L * 1024 * 1024) // a flac-mp4 fragment larger than this is refused
#define MAX_INIT (1024L * 1024)

static char cache_dir[400];
static __thread char last_error[256];

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;

typedef struct {
	char track_id[YANDEX_ID_MAX];
	char path[512];
	char part[520];
	yandex_file_t file;
	bool unwrap; // flac-mp4
	bool whole;	 // m4a: hand over only when complete
	FILE *f;
	long written;  // bytes in the output file
	bool finished; // the thread is done, `ok` says how
	bool ok;
	volatile bool abandon;
	char error[256];

	// the source: ranged windows over the HTTP stream
	http_stream_t st;
	bool st_open;
	long src_pos;
	long window_left;
	long src_total;
	bool refreshed;
	uint8_t key[CRYPTO_AES256_KEY_SIZE];
	size_t key_len; // 16 or 32
	bool encrypted;

	// A download for somebody else (yandexcache_fetch): written to their file,
	// not the cache's, with no growfile, and giving way to playback.
	bool detached;
	void (*progress)(long written, long total, void *user);
	void *progress_user;
	uint64_t samples; // flac-mp4: the length counted while unwrapping
} download_t;

static download_t *current;	 // the one download in flight, or NULL
static download_t *detached; // the one yandexcache_fetch() in flight, or NULL

static void set_error(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(last_error, sizeof(last_error), fmt, ap);
	va_end(ap);
}

const char *yandexcache_last_error(void) { return last_error; }

void yandexcache_set_root(const char *sd_root) {
	cache_dir[0] = '\0';
	if (!sd_root || !sd_root[0]) {
		return;
	}
	char local[400];
	snprintf(local, sizeof(local), "%s/.local", sd_root);
	mkdir(local, 0755);
	snprintf(cache_dir, sizeof(cache_dir), "%s/%s", sd_root, YANDEXCACHE_DIR);
	mkdir(cache_dir, 0755);
}

bool yandexcache_ready(void) { return cache_dir[0] != '\0'; }

const char *yandexcache_dir(void) { return cache_dir[0] ? cache_dir : NULL; }

const char *yandexcache_extension(const char *codec) {
	if (strncmp(codec, "flac", 4) == 0) {
		return "flac"; // flac-mp4 too: it is unwrapped on the way in
	}
	if (strstr(codec, "mp4")) {
		return "m4a";
	}
	if (strstr(codec, "aac")) {
		return "aac";
	}
	return "mp3";
}

void yandexcache_path(const char *track_id, const char *ext, char *out, size_t size) { snprintf(out, size, "%s/%s.%s", cache_dir, track_id, ext); }

bool yandexcache_find(const char *track_id, char *out, size_t size) {
	static const char *const EXTS[] = {"flac", "mp3", "m4a", "aac"};
	for (size_t i = 0; i < sizeof(EXTS) / sizeof(EXTS[0]); i++) {
		char path[512];
		yandexcache_path(track_id, EXTS[i], path, sizeof(path));
		struct stat st;
		if (stat(path, &st) == 0 && st.st_size > 0 && !growfile_is_growing(path)) {
			if (out) {
				snprintf(out, size, "%s", path);
			}
			return true;
		}
	}
	return false;
}

bool yandexcache_owns(const char *path) {
	size_t n = strlen(cache_dir);
	return n && path && strncmp(path, cache_dir, n) == 0 && path[n] == '/';
}

void yandexcache_track_id(const char *path, char *out, size_t size) {
	out[0] = '\0';
	if (!yandexcache_owns(path)) {
		return;
	}
	const char *name = path + strlen(cache_dir) + 1;
	size_t len = strcspn(name, ".");
	snprintf(out, size, "%.*s", (int)len, name);
}

void yandexcache_downloading_id(char *out, size_t size) {
	pthread_mutex_lock(&lock);
	snprintf(out, size, "%s", current && !current->finished ? current->track_id : "");
	pthread_mutex_unlock(&lock);
}

bool yandexcache_busy(void) {
	pthread_mutex_lock(&lock);
	bool busy = current && !current->finished;
	pthread_mutex_unlock(&lock);
	return busy;
}

static volatile bool network_wanted;

void yandexcache_set_network_wanted(bool wanted) { network_wanted = wanted; }
bool yandexcache_network_wanted(void) { return network_wanted; }

// The audio files of the cache; everything else in it (".tags", ".jpg",
// ".part") is a track's companion, not a track.
static bool is_audio_name(const char *name) {
	const char *dot = strrchr(name, '.');
	if (!dot || dot == name) {
		return false;
	}
	static const char *const EXTS[] = {".flac", ".mp3", ".m4a", ".aac"};
	for (size_t i = 0; i < sizeof(EXTS) / sizeof(EXTS[0]); i++) {
		if (strcmp(dot, EXTS[i]) == 0) {
			return true;
		}
	}
	return false;
}

bool yandexcache_is_track(const char *path) {
	if (!yandexcache_owns(path)) {
		return false;
	}
	const char *name = path + strlen(cache_dir) + 1;
	return !strchr(name, '/') && strcspn(name, ".") > 0 && is_audio_name(name);
}

bool yandexcache_album_id(const char *path, char *out, size_t size) {
	if (!out || size == 0) {
		return false;
	}
	out[0] = '\0';
	if (!yandexcache_is_track(path)) {
		return false;
	}
	char tags[600];
	snprintf(tags, sizeof(tags), "%.560s.tags", path);
	FILE *f = fopen(tags, "r");
	if (!f) {
		return false;
	}
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "album_id=", 9) == 0) {
			line[strcspn(line, "\r\n")] = '\0';
			snprintf(out, size, "%s", line + 9);
			break;
		}
	}
	fclose(f);
	return out[0] != '\0';
}

void yandexcache_abandon_all(void) {
	pthread_mutex_lock(&lock);
	if (current) {
		current->abandon = true;
		if (current->st_open) {
			http_stream_wake(&current->st);
		}
	}
	pthread_mutex_unlock(&lock);
}

// ---------------------------------------------------------------------------
// the source
// ---------------------------------------------------------------------------

// The encraw key: 32 hex digits for AES-128 (older answers, the mock's), 64
// for AES-256 (what the service sends since 2025). Anything else is refused.
static bool hex_key(const char *hex, uint8_t out[CRYPTO_AES256_KEY_SIZE], size_t *len) {
	size_t digits = strlen(hex);
	if (digits != 2 * CRYPTO_AES128_KEY_SIZE && digits != 2 * CRYPTO_AES256_KEY_SIZE) {
		return false;
	}
	for (size_t i = 0; i < digits / 2; i++) {
		unsigned v;
		if (sscanf(hex + 2 * i, "%2x", &v) != 1) {
			return false;
		}
		out[i] = (uint8_t)v;
	}
	*len = digits / 2;
	return true;
}

static bool src_open(download_t *d) {
	for (int attempt = 0; attempt < 3 && !d->abandon; attempt++) {
		if (http_stream_open_range(&d->st, d->file.url, d->src_pos, 20)) {
			d->st_open = true;
			d->window_left = YANDEXCACHE_WINDOW;
			if (d->src_total <= 0 && d->st.total_length > 0) {
				d->src_total = d->st.total_length;
			}
			return true;
		}
		// The signed address lives a limited time: an open refused once is
		// retried with a fresh one, which costs one get-file-info.
		if (!d->refreshed) {
			d->refreshed = true;
			yandex_file_t fresh;
			// Only the same file under a new address will do: a different
			// codec, or plain bytes where encrypted ones were coming (or the
			// other way round), would splice two files into one.
			if (yandex_file_info(d->track_id, &fresh) && strcmp(fresh.codec, d->file.codec) == 0 && !fresh.key[0] == !d->file.key[0]) {
				snprintf(d->file.url, sizeof(d->file.url), "%s", fresh.url);
				if (fresh.key[0] && strcmp(fresh.key, d->file.key) != 0) {
					size_t len = 0;
					if (!hex_key(fresh.key, d->key, &len) || len != d->key_len) {
						break;
					}
					snprintf(d->file.key, sizeof(d->file.key), "%s", fresh.key);
				}
				continue;
			}
		}
		sleep(1);
	}
	snprintf(d->error, sizeof(d->error), "%s", http_last_error());
	return false;
}

// A detached download (yandexcache_fetch: a track saved to the library) never
// competes with what is being listened to: while the cache or any other
// service is downloading, it lets go of its connection and waits, then picks
// up where it was with a ranged request. The playing track gets the whole of
// the Wi-Fi and the card, and the saved file loses nothing but time.
static bool others_downloading(void) {
	if (yandexcache_busy()) {
		return true;
	}
	for (int i = 0; i < stream_provider_count(); i++) {
		const stream_provider_t *p = stream_provider_at(i);
		if (p && strcmp(p->id, "yandex") != 0 && p->downloading && p->downloading()) {
			return true;
		}
	}
	return false;
}

static void give_way(download_t *d) {
	if (!d->detached || !others_downloading()) {
		return;
	}
	if (d->st_open) {
		http_stream_close(&d->st);
		d->st_open = false;
	}
	printf("yandex: saving %s waits for playback's download (at %ld bytes)\n", d->track_id, d->src_pos);
	while (!d->abandon && others_downloading()) {
		usleep(500 * 1000);
	}
	printf("yandex: saving %s goes on\n", d->track_id);
}

// Reads up to `n` bytes at the source position, decrypted. 0 at the end of
// the file, -1 on failure.
static long src_read(download_t *d, uint8_t *buf, long n) {
	if (d->src_total > 0 && d->src_pos >= d->src_total) {
		return 0;
	}
	give_way(d);
	for (int attempt = 0; attempt < 3; attempt++) {
		if (d->abandon) {
			return -1;
		}
		if (!d->st_open || d->window_left <= 0) {
			if (d->st_open) {
				http_stream_close(&d->st);
				d->st_open = false;
			}
			if (!src_open(d)) {
				return -1;
			}
		}
		long want = n < d->window_left ? n : d->window_left;
		int got = http_stream_read(&d->st, buf, (int)want);
		if (got > 0) {
			if (d->encrypted) {
				static const uint8_t ZERO_IV[16] = {0};
				if (!crypto_aes_ctr(d->key, d->key_len, ZERO_IV, buf, buf, (size_t)got, (uint64_t)d->src_pos)) {
					snprintf(d->error, sizeof(d->error), "%s", tr("yandex_cannot_decrypt"));
					return -1;
				}
			}
			d->src_pos += got;
			d->window_left -= got;
			return got;
		}
		if (got == 0 && (d->src_total <= 0 || d->src_pos >= d->src_total)) {
			return 0; // the end, unknown length or reached
		}
		// A dropped connection in the middle: reopen at the same place.
		http_stream_close(&d->st);
		d->st_open = false;
	}
	return -1;
}

static bool src_read_full(download_t *d, uint8_t *buf, long n) {
	long done = 0;
	while (done < n) {
		long got = src_read(d, buf + done, n - done);
		if (got <= 0) {
			return false;
		}
		done += got;
	}
	return true;
}

// ---------------------------------------------------------------------------
// the download thread
// ---------------------------------------------------------------------------

static void note_written(download_t *d, long written) {
	if (d->detached) {
		d->written = written;
		if (d->progress) {
			d->progress(written, d->src_total, d->progress_user);
		}
		return;
	}
	pthread_mutex_lock(&lock);
	d->written = written;
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&lock);
	growfile_progress(d->part[0] && d->whole ? d->part : d->path, written);
}

static bool copy_raw(download_t *d) {
	uint8_t *buf = malloc(64 * 1024);
	if (!buf) {
		return false;
	}
	long written = 0;
	bool ok = true;
	for (;;) {
		long got = src_read(d, buf, 64 * 1024);
		if (got == 0) {
			break;
		}
		if (got < 0 || fwrite(buf, 1, (size_t)got, d->f) != (size_t)got || fflush(d->f) != 0) {
			ok = false;
			break;
		}
		written += got;
		note_written(d, written);
	}
	free(buf);
	return ok;
}

// Copies `n` bytes of the source through the unwrapper (`u` set) or drops
// them (`u` NULL), a buffer at a time; -1 for "to the end of the file".
static bool pass_through(download_t *d, mp4flac_t *u, long n) {
	uint8_t *buf = malloc(64 * 1024);
	if (!buf) {
		return false;
	}
	bool ok = true;
	while (ok && n != 0) {
		long want = n < 0 || n > 64 * 1024 ? 64 * 1024 : n;
		long got = src_read(d, buf, want);
		if (got == 0 && n < 0) {
			break;
		}
		if (got <= 0) {
			ok = false;
			break;
		}
		if (u) {
			ok = mp4flac_payload(u, buf, (size_t)got) && fflush(d->f) == 0;
			note_written(d, u->written);
		}
		if (n > 0) {
			n -= got;
		}
	}
	free(buf);
	return ok;
}

// flac-mp4: everything before the first moof is the init segment; after that
// the file is moof+mdat pairs, each handed to mp4flac whole. One fragment in
// RAM at a time.
//
// A file that is not fragmented -- moov, then a single mdat holding every
// frame -- is unwrapped too: its mdat is FLAC frames back to back, streamed
// through as it arrives rather than held. Yandex's own files are fragmented
// (its web player feeds them to Media Source Extensions, which wants that),
// but nothing on the service's side promises it.
static bool unwrap_mp4(download_t *d) {
	mp4flac_t u;
	memset(&u, 0, sizeof(u));
	uint8_t *init = NULL, *seg = NULL;
	long init_len = 0, seg_len = 0;
	bool begun = false, whole = false, ok = false;

	for (;;) {
		uint8_t head[16];
		long got = src_read(d, head, 1);
		if (got == 0) {
			ok = begun && u.written > u.header_bytes;
			break;
		}
		if (got < 0 || !src_read_full(d, head + 1, 7)) {
			break;
		}
		uint64_t size = (uint64_t)head[0] << 24 | (uint64_t)head[1] << 16 | (uint64_t)head[2] << 8 | head[3];
		long hlen = 8;
		if (size == 1) {
			if (!src_read_full(d, head + 8, 8)) {
				break;
			}
			size = 0;
			for (int i = 8; i < 16; i++) {
				size = size << 8 | head[i];
			}
			hlen = 16;
		}
		bool is_moof = memcmp(head + 4, "moof", 4) == 0;
		bool is_mdat = memcmp(head + 4, "mdat", 4) == 0;
		bool to_end = size == 0; // "the rest of the file"
		if (is_mdat && !begun && init_len > 0) {
			// Not fragmented: the header is all there is before the audio.
			if (!mp4flac_begin(&u, d->f, "flac", init, (size_t)init_len)) {
				snprintf(d->error, sizeof(d->error), "%s", tr("yandex_unsupported_file"));
				break;
			}
			begun = whole = true;
			printf("yandex: %s is a whole MP4, its mdat streamed through\n", d->track_id);
		}
		if (whole) {
			// The mdat goes through; whatever follows it (an index, padding)
			// is read past.
			if ((!to_end && size < (uint64_t)hlen) || !pass_through(d, is_mdat ? &u : NULL, to_end ? -1 : (long)(size - (uint64_t)hlen))) {
				break;
			}
			if (to_end) {
				ok = u.written > u.header_bytes;
				break;
			}
			continue;
		}
		if (size < (uint64_t)hlen || size > (uint64_t)MAX_BOX) {
			snprintf(d->error, sizeof(d->error), "%s", tr("yandex_unsupported_file"));
			break;
		}
		if (!begun && !is_moof) {
			if (init_len + (long)size > MAX_INIT) {
				snprintf(d->error, sizeof(d->error), "%s", tr("yandex_unsupported_file"));
				break;
			}
			uint8_t *grown = realloc(init, (size_t)(init_len + (long)size));
			if (!grown) {
				break;
			}
			init = grown;
			memcpy(init + init_len, head, (size_t)hlen);
			if (!src_read_full(d, init + init_len + hlen, (long)size - hlen)) {
				break;
			}
			init_len += (long)size;
			continue;
		}
		if (!begun) {
			if (!mp4flac_begin(&u, d->f, "flac", init, (size_t)init_len)) {
				snprintf(d->error, sizeof(d->error), "%s", tr("yandex_unsupported_file"));
				break;
			}
			begun = true;
		}
		// Collect moof, then hand it over with the mdat that follows.
		uint8_t *grown = realloc(seg, (size_t)(seg_len + (long)size));
		if (!grown) {
			break;
		}
		seg = grown;
		memcpy(seg + seg_len, head, (size_t)hlen);
		if (!src_read_full(d, seg + seg_len + hlen, (long)size - hlen)) {
			break;
		}
		seg_len += (long)size;
		if (is_mdat) {
			if (!mp4flac_segment(&u, seg, (size_t)seg_len)) {
				break;
			}
			fflush(d->f);
			note_written(d, u.written);
			seg_len = 0;
		}
	}
	free(init);
	free(seg);
	d->samples = mp4flac_total_samples(&u);
	return ok;
}

static void *download_main(void *arg) {
	download_t *d = arg;
	// Below the interface and the audio threads, so the TLS and the card
	// writes of a download give way to them on this single core; but not idle
	// class like a sync thread, because playback is reading this file as it
	// grows and an idle thread gets almost nothing while music plays.
	thread_be_low_priority("yandex download");
	streamturn_acquire();
	bool ok = !d->abandon && (d->unwrap ? unwrap_mp4(d) : copy_raw(d));
	streamturn_release();
	if (d->st_open) {
		http_stream_close(&d->st);
	}
	ok = (fclose(d->f) == 0) && ok && !d->abandon;
	const char *growing = d->whole ? d->part : d->path;
	if (ok && d->whole && rename(d->part, d->path) != 0) {
		ok = false;
	}
	if (!ok) {
		remove(growing);
		fprintf(stderr, "yandex: download of %s failed: %s\n", d->track_id, d->abandon ? "abandoned" : d->error);
	} else {
		// The STREAMINFO a fragmented file leaves at "unknown length": known
		// now, written in, so seeking and the progress bar work next time.
		if (d->unwrap && d->samples && mp4flac_patch_total_samples(d->path, d->samples)) {
			printf("yandex: %s is %llu samples long\n", d->track_id, (unsigned long long)d->samples);
		}
		printf("yandex: %s complete (%ld bytes)\n", d->track_id, d->written);
	}
	growfile_finish(growing, ok);

	pthread_mutex_lock(&lock);
	d->finished = true;
	d->ok = ok;
	pthread_cond_broadcast(&changed);
	if (current == d) {
		current = NULL;
	}
	pthread_mutex_unlock(&lock);
	free(d);
	return NULL;
}

bool yandexcache_fetch(const char *track_id, const yandex_file_t *file, FILE *out, void (*progress)(long written, long total, void *user), void *user, uint64_t *samples, char *err, size_t err_size) {
	if (samples) {
		*samples = 0;
	}
	if (err && err_size) {
		err[0] = '\0';
	}
	download_t *d = calloc(1, sizeof(*d));
	if (!d) {
		return false;
	}
	snprintf(d->track_id, sizeof(d->track_id), "%s", track_id);
	d->file = *file;
	d->unwrap = strcmp(file->codec, "flac-mp4") == 0;
	d->f = out;
	d->detached = true;
	d->progress = progress;
	d->progress_user = user;
	if (file->key[0]) {
		if (!hex_key(file->key, d->key, &d->key_len) || !crypto_aes_ctr_available(d->key_len)) {
			if (err && err_size) {
				snprintf(err, err_size, "%s", tr("yandex_cannot_decrypt"));
			}
			free(d);
			return false;
		}
		d->encrypted = true;
	}
	pthread_mutex_lock(&lock);
	detached = d;
	pthread_mutex_unlock(&lock);

	bool ok = d->unwrap ? unwrap_mp4(d) : copy_raw(d);
	ok = ok && !d->abandon && fflush(out) == 0;

	pthread_mutex_lock(&lock);
	detached = NULL;
	if (d->st_open) {
		http_stream_close(&d->st);
		d->st_open = false;
	}
	pthread_mutex_unlock(&lock);
	if (!ok && err && err_size) {
		snprintf(err, err_size, "%s", d->abandon ? tr("download_cancelled") : d->error[0] ? d->error : tr("download_failed"));
	}
	if (ok && samples) {
		*samples = d->samples;
	}
	free(d);
	return ok;
}

void yandexcache_fetch_cancel(void) {
	pthread_mutex_lock(&lock);
	if (detached) {
		detached->abandon = true;
		if (detached->st_open) {
			http_stream_wake(&detached->st);
		}
	}
	pthread_mutex_unlock(&lock);
}

static void streamturn_hook(void) { yandexcache_abandon_all(); }

static void prune(long room); // below, with the rest of the cache's bounds

bool yandexcache_start(const yandex_track_t *track, const yandex_file_t *file, char *out, size_t size) {
	last_error[0] = '\0';
	static bool registered;
	if (!registered) {
		registered = true;
		streamturn_register_abandon(streamturn_hook);
	}
	if (!yandexcache_ready()) {
		set_error("%s", tr("download_failed"));
		return false;
	}
	if (yandexcache_find(track->id, out, size)) {
		return true;
	}
	// Already downloading (prefetch got there first): wait for that one
	// rather than abandon it and start again.
	pthread_mutex_lock(&lock);
	if (current && !current->finished && strcmp(current->track_id, track->id) == 0) {
		download_t *d = current;
		snprintf(out, size, "%s", d->path);
		while (current == d && (d->whole || d->written < PREBUFFER_BYTES)) {
			pthread_cond_wait(&changed, &lock);
		}
		bool ok = current == d || yandexcache_find(track->id, NULL, 0);
		pthread_mutex_unlock(&lock);
		return ok;
	}
	pthread_mutex_unlock(&lock);

	download_t *d = calloc(1, sizeof(*d));
	if (!d) {
		return false;
	}
	snprintf(d->track_id, sizeof(d->track_id), "%s", track->id);
	d->file = *file;
	d->unwrap = strcmp(file->codec, "flac-mp4") == 0;
	d->whole = strcmp(yandexcache_extension(file->codec), "m4a") == 0;
	yandexcache_path(track->id, yandexcache_extension(file->codec), d->path, sizeof(d->path));
	snprintf(d->part, sizeof(d->part), "%s.part", d->path);
	if (file->key[0]) {
		if (!hex_key(file->key, d->key, &d->key_len) || !crypto_aes_ctr_available(d->key_len)) {
			set_error("%s", tr("yandex_cannot_decrypt"));
			free(d);
			return false;
		}
		d->encrypted = true;
	}
	// Another track still coming down is NOT abandoned from here, tempting as
	// it is: prefetch calls this too, for the track after the one playing, and
	// the download in flight is then usually the very file being played.
	// Abandoning belongs to whoever received a user command -- the page, see
	// streamturn_abandon_all() -- exactly as in qobuzcache.c.
	//
	// What is done instead is wait for it to end. The download thread would
	// wait for the turn anyway (streamturn_acquire), so this costs no time,
	// and it keeps `current` the one download in flight: replacing it while
	// the other still runs would put that one out of reach of every abandon.
	pthread_mutex_lock(&lock);
	while (current && !current->finished) {
		pthread_cond_wait(&changed, &lock);
	}
	pthread_mutex_unlock(&lock);

	// Room for this one before it starts. Its size is only known once the
	// first window answers, so the headroom is a generous hi-res track.
	prune(YANDEXCACHE_PRUNE_HEADROOM);
	const char *target = d->whole ? d->part : d->path;
	d->f = fopen(target, "wb");
	if (!d->f) {
		set_error("%s", tr("download_failed"));
		free(d);
		return false;
	}

	pthread_mutex_lock(&lock);
	current = d;
	pthread_mutex_unlock(&lock);
	growfile_announce(target, 0);

	pthread_t th;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, 128 * 1024);
	if (pthread_create(&th, &attr, download_main, d) != 0) {
		pthread_attr_destroy(&attr);
		growfile_finish(target, false);
		fclose(d->f);
		remove(target);
		pthread_mutex_lock(&lock);
		current = NULL;
		pthread_mutex_unlock(&lock);
		free(d);
		return false;
	}
	pthread_attr_destroy(&attr);

	// The prebuffer: enough to start (a FLAC or MP3 plays while the rest
	// arrives), or all of it for an MP4 container, whose index may sit at the
	// end of the file.
	snprintf(out, size, "%s", d->path);
	pthread_mutex_lock(&lock);
	bool ok;
	for (;;) {
		if (current != d) { // finished (and freed) or replaced
			ok = yandexcache_find(track->id, NULL, 0);
			break;
		}
		if (!d->whole && d->written >= PREBUFFER_BYTES) {
			ok = true;
			break;
		}
		pthread_cond_wait(&changed, &lock);
	}
	pthread_mutex_unlock(&lock);
	if (!ok) {
		set_error("%s", tr("download_failed"));
	}
	return ok;
}

// ---------------------------------------------------------------------------
// keeping the cache in bounds
//
// The Qobuz policy (qobuzcache.c), for the same reasons: the cache is a
// waypoint on the user's card, in a directory they never see, so it is swept
// every YANDEXCACHE_CLEAR_EVERY tracks and on the way out, and held under
// YANDEXCACHE_MAX_BYTES in between. What the queue still expects (the track
// playing and those after it), what was fetched a moment ago and is about to
// be queued, and what is coming down right now are never touched by the
// automatic passes.
// ---------------------------------------------------------------------------

static pthread_mutex_t keep_lock = PTHREAD_MUTEX_INITIALIZER;
static char protected_paths[YANDEXCACHE_PROTECTED_MAX][512];
static int protected_count;

// Ring of the most recently fetched tracks: the safety net for the moment
// between "the file exists" and "the queue knows about it".
#define RECENT_MAX 8
static char recent_paths[RECENT_MAX][512];
static int recent_next;
static int played_since_clear;

void yandexcache_set_protected(const char *const *paths, int count) {
	pthread_mutex_lock(&keep_lock);
	protected_count = 0;
	for (int i = 0; i < count && protected_count < YANDEXCACHE_PROTECTED_MAX; i++) {
		if (paths[i] && paths[i][0]) {
			snprintf(protected_paths[protected_count++], sizeof(protected_paths[0]), "%s", paths[i]);
		}
	}
	pthread_mutex_unlock(&keep_lock);
}

// True when `stem` (a path with its last extension cut) is the start of `path`
// up to one of its dots: "…/101" and "…/101.flac" both belong to "…/101.flac",
// "…/10" does not.
static bool stem_of(const char *stem, const char *path) {
	size_t len = strlen(stem);
	return path[0] && strncmp(path, stem, len) == 0 && (path[len] == '.' || path[len] == '\0');
}

static bool is_kept_stem(const char *stem) {
	if (growfile_prefix_is_growing(stem)) {
		return true;
	}
	bool keep = false;
	pthread_mutex_lock(&keep_lock);
	for (int i = 0; i < protected_count && !keep; i++) {
		keep = stem_of(stem, protected_paths[i]);
	}
	for (int i = 0; i < RECENT_MAX && !keep; i++) {
		keep = stem_of(stem, recent_paths[i]);
	}
	pthread_mutex_unlock(&keep_lock);
	return keep;
}

typedef struct {
	char name[64];
	long size;
	long atime; // last access: what is actually being listened to
} entry_t;

#define PRUNE_MAX_ENTRIES 512

// Visits every file of the cache. `keep` (may be NULL) spares those whose
// stem it accepts. Fills `out` (up to `max`) with what may go and returns how
// many; `total` gets the size of everything.
static int scan(entry_t *out, int max, long *total, bool (*keep)(const char *stem)) {
	*total = 0;
	int count = 0;
	DIR *d = opendir(cache_dir);
	if (!d) {
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
		*total += (long)st.st_size;
		char *dot = strrchr(path + strlen(cache_dir) + 1, '.');
		if (dot) {
			*dot = '\0';
		}
		if (keep && keep(path)) {
			continue;
		}
		if (out && count < max && strlen(e->d_name) < sizeof(out[0].name)) {
			snprintf(out[count].name, sizeof(out[count].name), "%.*s", (int)sizeof(out[count].name) - 1, e->d_name); // fits: checked above
			out[count].size = (long)st.st_size;
			out[count].atime = (long)st.st_atime;
			count++;
		}
	}
	closedir(d);
	return count;
}

// Drops the least recently listened to until `room` more bytes fit under the
// ceiling. Borrowed memory, given straight back: this runs once per download.
static void prune(long room) {
	entry_t *entries = malloc(sizeof(*entries) * PRUNE_MAX_ENTRIES);
	if (!entries) {
		return;
	}
	long total;
	int count = scan(entries, PRUNE_MAX_ENTRIES, &total, is_kept_stem);
	for (int i = 0; i < count && total + room > YANDEXCACHE_MAX_BYTES; i++) {
		int oldest = i;
		for (int k = i + 1; k < count; k++) {
			if (entries[k].atime < entries[oldest].atime) {
				oldest = k;
			}
		}
		entry_t tmp = entries[i];
		entries[i] = entries[oldest];
		entries[oldest] = tmp;
		char path[640];
		snprintf(path, sizeof(path), "%s/%s", cache_dir, entries[i].name);
		if (remove(path) == 0) {
			total -= entries[i].size;
			printf("yandexcache: removed %s (%ld KB)\n", entries[i].name, entries[i].size / 1024);
		}
	}
	free(entries);
}

static void clear_files(bool (*keep)(const char *stem)) {
	if (!yandexcache_ready()) {
		return;
	}
	entry_t *entries = malloc(sizeof(*entries) * PRUNE_MAX_ENTRIES);
	if (!entries) {
		return;
	}
	long total;
	int count = scan(entries, PRUNE_MAX_ENTRIES, &total, keep);
	int removed = 0;
	for (int i = 0; i < count; i++) {
		char path[640];
		snprintf(path, sizeof(path), "%s/%s", cache_dir, entries[i].name);
		removed += remove(path) == 0;
	}
	free(entries);
	printf("yandexcache: cleared (%d files)\n", removed);
}

void yandexcache_note_played(const char *path) {
	if (!yandexcache_ready() || !path || !path[0]) {
		return;
	}
	pthread_mutex_lock(&keep_lock);
	snprintf(recent_paths[recent_next], sizeof(recent_paths[0]), "%s", path);
	recent_next = (recent_next + 1) % RECENT_MAX;
	bool sweep = ++played_since_clear >= YANDEXCACHE_CLEAR_EVERY;
	if (sweep) {
		played_since_clear = 0;
	}
	pthread_mutex_unlock(&keep_lock);
	if (sweep) {
		printf("yandexcache: %d tracks since the last sweep, cleaning up\n", YANDEXCACHE_CLEAR_EVERY);
		clear_files(is_kept_stem);
	}
}

// What a clear the user asks for spares: only the file coming down right now.
// Unlike the other services' caches, a Yandex AAC file grows under ".part" and
// is renamed when whole, so removing it mid-download fails the rename and
// with it the track being listened to; everything else goes.
static bool is_growing_stem(const char *stem) { return growfile_prefix_is_growing(stem); }

void yandexcache_clear(void) {
	if (!yandexcache_ready()) {
		return;
	}
	printf("yandexcache: clearing the cache on request\n");
	clear_files(is_growing_stem);
}

void yandexcache_clear_on_exit(void) {
	if (!yandexcache_ready()) {
		return;
	}
	printf("yandexcache: shutting down, clearing the cache\n");
	yandexcache_abandon_all();
	clear_files(NULL);
}

// ---------------------------------------------------------------------------
// sidecars
// ---------------------------------------------------------------------------

void yandexcache_write_sidecars(const yandex_track_t *track, const char *path) {
	if (!path || !path[0]) {
		return;
	}
	char tags[600], tmp[620];
	snprintf(tags, sizeof(tags), "%.560s.tags", path);
	snprintf(tmp, sizeof(tmp), "%s.tmp", tags);
	FILE *f = fopen(tmp, "w");
	if (f) {
		fprintf(f, "title=%s\nartist=%s\nalbum=%s\nalbum_id=%s\ntrack=%d\n", track->title, track->artist, track->album, track->album_id, track->track_number);
		fclose(f);
		if (rename(tmp, tags) != 0) {
			remove(tmp);
		}
	}
	if (!track->cover[0]) {
		return;
	}
	char jpg[600];
	const char *dot = strrchr(path, '.');
	snprintf(jpg, sizeof(jpg), "%.*s.jpg", (int)(dot ? dot - path : (long)strlen(path)), path);
	struct stat st;
	if (stat(jpg, &st) == 0 && st.st_size > 0) {
		return;
	}
	char *body = NULL;
	size_t len = 0;
	if (http_get(track->cover, &body, &len, 2 * 1024 * 1024, 15) && len > 0) {
		FILE *c = fopen(jpg, "wb");
		if (c) {
			fwrite(body, 1, len, c);
			fclose(c);
		}
	}
	free(body);
}

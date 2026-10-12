#include "yandexcache.h"

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
#include "src/system/decode/growfile.h"
#include "src/system/library/mp4flac.h"
#include "src/system/net/crypto.h"
#include "src/system/net/http.h"
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
	uint8_t key[16];
	bool encrypted;
} download_t;

static download_t *current; // the one download in flight, or NULL

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
			if (yandex_file_info(d->track_id, &fresh)) {
				snprintf(d->file.url, sizeof(d->file.url), "%s", fresh.url);
				continue;
			}
		}
		sleep(1);
	}
	snprintf(d->error, sizeof(d->error), "%s", http_last_error());
	return false;
}

// Reads up to `n` bytes at the source position, decrypted. 0 at the end of
// the file, -1 on failure.
static long src_read(download_t *d, uint8_t *buf, long n) {
	if (d->src_total > 0 && d->src_pos >= d->src_total) {
		return 0;
	}
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
				if (!crypto_aes128_ctr(d->key, ZERO_IV, buf, buf, (size_t)got, (uint64_t)d->src_pos)) {
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

// flac-mp4: everything before the first moof is the init segment; after that
// the file is moof+mdat pairs, each handed to mp4flac whole. One fragment in
// RAM at a time.
static bool unwrap_mp4(download_t *d) {
	mp4flac_t u;
	memset(&u, 0, sizeof(u));
	uint8_t *init = NULL, *seg = NULL;
	long init_len = 0, seg_len = 0;
	bool begun = false, ok = false;

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
	return ok;
}

static void *download_main(void *arg) {
	download_t *d = arg;
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

static bool hex_key(const char *hex, uint8_t out[16]) {
	if (strlen(hex) < 32) {
		return false;
	}
	for (int i = 0; i < 16; i++) {
		unsigned v;
		if (sscanf(hex + 2 * i, "%2x", &v) != 1) {
			return false;
		}
		out[i] = (uint8_t)v;
	}
	return true;
}

static void streamturn_hook(void) { yandexcache_abandon_all(); }

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
		if (!crypto_available() || !hex_key(file->key, d->key)) {
			set_error("%s", tr("yandex_cannot_decrypt"));
			free(d);
			return false;
		}
		d->encrypted = true;
	}
	const char *target = d->whole ? d->part : d->path;
	d->f = fopen(target, "wb");
	if (!d->f) {
		set_error("%s", tr("download_failed"));
		free(d);
		return false;
	}

	yandexcache_abandon_all();
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

#ifdef SONIX_ZVUK

#include "zvukcache.h"

#include <dirent.h>
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
#include "src/system/net/http.h"
#include "src/system/streaming/provider.h"
#include "src/system/streaming/streamturn.h"

// See zvukcache.h.

#define PREBUFFER_BYTES (384L * 1024)
#define MAX_HEAD (8L * 1024 * 1024) // the boxes before the audio, moov included
#define BUF_SIZE (64 * 1024)
#define MARKER ".incompleto"

static char cache_dir[400];
static __thread char last_error[256];

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;

typedef struct {
	char track_id[ZVUK_ID_MAX];
	char path[512];
	char marker[540];
	zvuk_stream_t stream;
	FILE *f;
	long written;
	bool finished, ok;
	volatile bool abandon;
	char error[256];

	http_stream_t st;
	bool st_open;
	long src_pos;
	long src_total;
	int refreshes;

	bool detached; // zvukcache_fetch: the caller's file, no growfile, gives way
	void (*progress)(long written, long total, void *user);
	void *progress_user;
	uint64_t samples;
} download_t;

static download_t *current;
static download_t *detached;

static void set_error(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(last_error, sizeof(last_error), fmt, ap);
	va_end(ap);
}

const char *zvukcache_last_error(void) { return last_error; }

void zvukcache_set_root(const char *sd_root) {
	cache_dir[0] = '\0';
	if (!sd_root || !sd_root[0]) {
		return;
	}
	char local[420];
	snprintf(local, sizeof(local), "%s/.local", sd_root);
	mkdir(local, 0755);
	snprintf(cache_dir, sizeof(cache_dir), "%s/%s", sd_root, ZVUKCACHE_DIR);
	mkdir(cache_dir, 0755);
}

bool zvukcache_ready(void) { return cache_dir[0] != '\0'; }

const char *zvukcache_dir(void) { return cache_dir[0] ? cache_dir : NULL; }

const char *zvukcache_extension(zvuk_quality_t got) { return got == ZVUK_QUALITY_FLAC ? "flac" : "mp3"; }

void zvukcache_path(const char *track_id, const char *ext, char *out, size_t size) {
	snprintf(out, size, "%s/%s.%s", cache_dir, track_id, ext);
}

static bool complete(const char *path) {
	struct stat st;
	char marker[560];
	snprintf(marker, sizeof(marker), "%s" MARKER, path);
	return stat(path, &st) == 0 && st.st_size > 0 && !growfile_is_growing(path) && access(marker, F_OK) != 0;
}

bool zvukcache_find(const char *track_id, char *out, size_t size) {
	static const char *const EXTS[] = {"flac", "mp3"};
	for (size_t i = 0; cache_dir[0] && i < sizeof(EXTS) / sizeof(EXTS[0]); i++) {
		char path[512];
		zvukcache_path(track_id, EXTS[i], path, sizeof(path));
		if (complete(path)) {
			if (out) {
				snprintf(out, size, "%s", path);
			}
			return true;
		}
	}
	return false;
}

bool zvukcache_owns(const char *path) {
	size_t n = strlen(cache_dir);
	return n && path && strncmp(path, cache_dir, n) == 0 && path[n] == '/';
}

bool zvukcache_is_track(const char *path) {
	if (!zvukcache_owns(path)) {
		return false;
	}
	const char *name = path + strlen(cache_dir) + 1;
	const char *dot = strrchr(name, '.');
	return !strchr(name, '/') && dot && dot > name && (strcmp(dot, ".flac") == 0 || strcmp(dot, ".mp3") == 0);
}

void zvukcache_track_id(const char *path, char *out, size_t size) {
	out[0] = '\0';
	if (!zvukcache_owns(path)) {
		return;
	}
	const char *name = path + strlen(cache_dir) + 1;
	snprintf(out, size, "%.*s", (int)strcspn(name, "."), name);
}

bool zvukcache_album_id(const char *path, char *out, size_t size) {
	out[0] = '\0';
	if (!zvukcache_is_track(path)) {
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

void zvukcache_downloading_id(char *out, size_t size) {
	pthread_mutex_lock(&lock);
	snprintf(out, size, "%s", current && !current->finished ? current->track_id : "");
	pthread_mutex_unlock(&lock);
}

bool zvukcache_busy(void) {
	pthread_mutex_lock(&lock);
	bool busy = current && !current->finished;
	pthread_mutex_unlock(&lock);
	return busy;
}

static volatile bool network_wanted;
void zvukcache_set_network_wanted(bool wanted) { network_wanted = wanted; }
bool zvukcache_network_wanted(void) { return network_wanted; }

void zvukcache_abandon_all(void) {
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
// the source: one GET, resumed with a Range, its address renewed when refused
// ---------------------------------------------------------------------------

static void src_close(download_t *d) {
	if (d->st_open) {
		http_stream_close(&d->st);
		d->st_open = false;
	}
}

// Opens at src_pos. The address is asked for again (getStream) when the CDN
// refuses it -- they expire -- but only the same quality will do.
static bool src_open(download_t *d) {
	for (int attempt = 0; attempt < 4 && !d->abandon; attempt++) {
		bool ok = d->src_pos > 0 ? http_stream_resume(&d->st, d->stream.url, d->src_pos, 20)
								 : http_stream_open_range(&d->st, d->stream.url, 0, 20);
		if (ok) {
			d->st_open = true;
			http_stream_set_read_timeout(&d->st, 15);
			if (d->src_total <= 0 && d->st.total_length > 0) {
				d->src_total = d->st.total_length;
			}
			return true;
		}
		http_outcome_t o;
		http_last_outcome(&o);
		if (o.status >= 400 && o.status < 500 && d->refreshes < 2) {
			d->refreshes++;
			printf("zvuk: %s: the CDN answered %d at %ld bytes, asking for a new address\n", d->track_id, o.status,
				   d->src_pos);
			zvuk_stream_t fresh;
			if (zvuk_stream(d->track_id, &fresh) && fresh.got == d->stream.got) {
				snprintf(d->stream.url, sizeof(d->stream.url), "%s", fresh.url);
				continue;
			}
			snprintf(d->error, sizeof(d->error), "%s", tr("zvuk_link_expired"));
			return false;
		}
		sleep(1);
	}
	const char *why = http_last_error();
	snprintf(d->error, sizeof(d->error), "%s", why && why[0] ? why : tr("download_failed"));
	return false;
}

static bool others_downloading(void) {
	if (zvukcache_busy()) {
		return true;
	}
	for (int i = 0; i < stream_provider_count(); i++) {
		const stream_provider_t *p = stream_provider_at(i);
		if (p && strcmp(p->id, "zvuk") != 0 && p->downloading && p->downloading()) {
			return true;
		}
	}
	return false;
}

// A save to the library (detached) lets go of its connection while anything
// playback needs is coming down, and resumes with a Range after.
static void give_way(download_t *d) {
	if (!d->detached || !others_downloading()) {
		return;
	}
	src_close(d);
	printf("zvuk: saving %s waits for playback's download (at %ld bytes)\n", d->track_id, d->src_pos);
	while (!d->abandon && others_downloading()) {
		usleep(500 * 1000);
	}
}

static long src_read(download_t *d, uint8_t *buf, long n) {
	if (d->src_total > 0 && d->src_pos >= d->src_total) {
		return 0;
	}
	give_way(d);
	for (int attempt = 0; attempt < 4; attempt++) {
		if (d->abandon) {
			return -1;
		}
		if (!d->st_open && !src_open(d)) {
			return -1;
		}
		int got = http_stream_read(&d->st, buf, (int)(n > BUF_SIZE ? BUF_SIZE : n));
		if (got > 0) {
			d->src_pos += got;
			return got;
		}
		if (got == 0 && (d->src_total <= 0 || d->src_pos >= d->src_total)) {
			return 0;
		}
		// Dropped in the middle: resumed where it stopped.
		printf("zvuk: %s: connection lost at %ld bytes, resuming\n", d->track_id, d->src_pos);
		src_close(d);
	}
	return -1;
}

static bool src_read_full(download_t *d, uint8_t *buf, long n) {
	for (long done = 0; done < n;) {
		long got = src_read(d, buf + done, n - done);
		if (got <= 0) {
			return false;
		}
		done += got;
	}
	return true;
}

// Moves the source to `pos`: a short way forward is read past, a long one
// (or backwards) is a new ranged request.
static bool src_seek(download_t *d, long pos, uint8_t *buf) {
	if (pos >= d->src_pos && pos - d->src_pos <= BUF_SIZE) {
		return src_read_full(d, buf, pos - d->src_pos);
	}
	src_close(d);
	d->src_pos = pos;
	return true;
}

// ---------------------------------------------------------------------------
// writing
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
	growfile_progress(d->path, written);
}

// Everything from the source as it is, after `lead` bytes already read.
static bool copy_raw(download_t *d, uint8_t *buf, const uint8_t *lead, long lead_len) {
	long written = 0;
	if (lead_len > 0) {
		if (fwrite(lead, 1, (size_t)lead_len, d->f) != (size_t)lead_len) {
			return false;
		}
		written = lead_len;
	}
	for (;;) {
		long got = src_read(d, buf, BUF_SIZE);
		if (got == 0) {
			return written > 0;
		}
		if (got < 0 || fwrite(buf, 1, (size_t)got, d->f) != (size_t)got || fflush(d->f) != 0) {
			return false;
		}
		written += got;
		note_written(d, written);
	}
}

// `n` bytes of FLAC frames through the unwrapper; -1 = to the end.
static bool frames_through(download_t *d, mp4flac_t *u, uint8_t *buf, long n) {
	while (n != 0) {
		long want = n < 0 || n > BUF_SIZE ? BUF_SIZE : n;
		long got = src_read(d, buf, want);
		if (got == 0 && n < 0) {
			return true;
		}
		if (got <= 0 || !mp4flac_payload(u, buf, (size_t)got) || fflush(d->f) != 0) {
			return false;
		}
		note_written(d, u->written);
		if (n > 0) {
			n -= got;
		}
	}
	return true;
}

static bool unsupported(download_t *d) {
	snprintf(d->error, sizeof(d->error), "%s", tr("zvuk_unsupported_file"));
	return false;
}

// FLAC in a plain MP4: the boxes before the audio are kept (ftyp, moov...),
// the mdat's payload -- FLAC frames back to back -- is the audio. With the
// moov after the mdat, the mdat is stepped over (one ranged request) to read
// the moov, and the source comes back to the frames (another).
static bool unwrap_mp4(download_t *d, uint8_t *buf, const uint8_t *lead) {
	uint8_t *head = NULL;
	long head_len = 0;
	long mdat_at = -1, mdat_len = -1; // the payload, once the mdat was passed
	bool ok = false;
	mp4flac_t u;
	memset(&u, 0, sizeof(u));
	uint8_t box[16];
	memcpy(box, lead, 8); // the first box's header was read to tell what this is
	bool have_box = true;
	for (;;) {
		if (!have_box) {
			long got = src_read(d, box, 1);
			if (got == 0) {
				ok = unsupported(d); // the end, and no audio met
				break;
			}
			if (got < 0 || !src_read_full(d, box + 1, 7)) {
				break;
			}
		}
		have_box = false;
		long box_at = d->src_pos - 8;
		uint64_t size = (uint64_t)box[0] << 24 | (uint64_t)box[1] << 16 | (uint64_t)box[2] << 8 | box[3];
		long hlen = 8;
		if (size == 1) {
			if (!src_read_full(d, box + 8, 8)) {
				break;
			}
			size = 0;
			for (int i = 8; i < 16; i++) {
				size = size << 8 | box[i];
			}
			hlen = 16;
		}
		bool to_end = size == 0;
		if (!to_end && size < (uint64_t)hlen) {
			ok = unsupported(d);
			break;
		}
		bool is_mdat = memcmp(box + 4, "mdat", 4) == 0;
		if (is_mdat && mdat_at < 0) {
			bool moov_known = false;
			for (long at = 0; at + 8 <= head_len && !moov_known;) {
				uint32_t s = (uint32_t)head[at] << 24 | (uint32_t)head[at + 1] << 16 | (uint32_t)head[at + 2] << 8 |
							 head[at + 3];
				moov_known = memcmp(head + at + 4, "moov", 4) == 0;
				at += s >= 8 ? (long)s : head_len;
			}
			if (moov_known) {
				// The usual order: the header is all here, the frames follow.
				if (!mp4flac_begin(&u, d->f, "flac", head, (size_t)head_len)) {
					ok = unsupported(d);
					break;
				}
				note_written(d, u.written);
				ok = frames_through(d, &u, buf, to_end ? -1 : (long)(size - (uint64_t)hlen));
				ok = ok && u.written > u.header_bytes;
				break;
			}
			if (to_end) {
				ok = unsupported(d); // audio to the end, and no moov before it
				break;
			}
			// The moov comes after: step over the audio to find it.
			mdat_at = box_at + hlen;
			mdat_len = (long)(size - (uint64_t)hlen);
			printf("zvuk: %s: the moov comes after the audio, reading it first\n", d->track_id);
			if (!src_seek(d, box_at + (long)size, buf)) {
				break;
			}
			continue;
		}
		if (to_end || head_len + (long)size > MAX_HEAD) {
			ok = unsupported(d);
			break;
		}
		bool is_moov = memcmp(box + 4, "moov", 4) == 0;
		if (mdat_at >= 0 && !is_moov) {
			// Past the audio, before the moov: nothing wanted.
			if (!src_seek(d, box_at + (long)size, buf)) {
				break;
			}
			continue;
		}
		uint8_t *grown = realloc(head, (size_t)(head_len + (long)size));
		if (!grown) {
			break;
		}
		head = grown;
		memcpy(head + head_len, box, (size_t)hlen);
		if (!src_read_full(d, head + head_len + hlen, (long)size - hlen)) {
			break;
		}
		head_len += (long)size;
		if (is_moov && mdat_at >= 0) {
			if (!mp4flac_begin(&u, d->f, "flac", head, (size_t)head_len)) {
				ok = unsupported(d);
				break;
			}
			note_written(d, u.written);
			src_close(d);
			d->src_pos = mdat_at;
			ok = frames_through(d, &u, buf, mdat_len) && u.written > u.header_bytes;
			break;
		}
	}
	free(head);
	d->samples = mp4flac_total_samples(&u);
	return ok;
}

// What the file is decides how it is written: native FLAC and MP3 as they
// come, FLAC in MP4 unwrapped.
static bool transfer(download_t *d) {
	uint8_t *buf = malloc(BUF_SIZE);
	if (!buf) {
		return false;
	}
	uint8_t lead[8];
	bool ok = src_read_full(d, lead, sizeof(lead));
	if (ok && memcmp(lead + 4, "ftyp", 4) == 0) {
		ok = unwrap_mp4(d, buf, lead);
	} else if (ok) {
		if (d->stream.got == ZVUK_QUALITY_FLAC && memcmp(lead, "fLaC", 4) != 0) {
			printf("zvuk: %s: the FLAC address sent neither FLAC nor MP4, kept as it came\n", d->track_id);
		}
		ok = copy_raw(d, buf, lead, sizeof(lead));
	}
	free(buf);
	return ok;
}

static void *download_main(void *arg) {
	download_t *d = arg;
	thread_be_low_priority("zvuk download");
	http_set_thread_stream_headers(zvuk_stream_headers());
	streamturn_acquire();
	bool ok = !d->abandon && transfer(d);
	streamturn_release();
	src_close(d);
	ok = (fclose(d->f) == 0) && ok && !d->abandon;
	if (!ok) {
		remove(d->path);
		fprintf(stderr, "zvuk: download of %s failed: %s\n", d->track_id, d->abandon ? "abandoned" : d->error);
	} else {
		if (d->samples && mp4flac_patch_total_samples(d->path, d->samples)) {
			printf("zvuk: %s is %llu samples long\n", d->track_id, (unsigned long long)d->samples);
		}
		printf("zvuk: %s complete (%ld bytes from %ld)\n", d->track_id, d->written, d->src_pos);
	}
	remove(d->marker);
	growfile_finish(d->path, ok);
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

bool zvukcache_fetch(const char *track_id, const zvuk_stream_t *stream, FILE *out,
					 void (*progress)(long written, long total, void *user), void *user, uint64_t *samples, char *err,
					 size_t err_size) {
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
	d->stream = *stream;
	d->f = out;
	d->detached = true;
	d->progress = progress;
	d->progress_user = user;
	http_set_thread_stream_headers(zvuk_stream_headers());
	pthread_mutex_lock(&lock);
	detached = d;
	pthread_mutex_unlock(&lock);
	bool ok = transfer(d) && !d->abandon && fflush(out) == 0;
	pthread_mutex_lock(&lock);
	detached = NULL;
	src_close(d);
	pthread_mutex_unlock(&lock);
	if (ok && samples) {
		// The caller's file may be a sink that cannot seek: the length goes
		// in afterwards, by its path (zvukdl.c).
		*samples = d->samples;
	}
	if (!ok && err && err_size) {
		snprintf(err, err_size, "%s", d->abandon ? tr("download_cancelled") : d->error[0] ? d->error
																						  : tr("download_failed"));
	}
	free(d);
	return ok;
}

void zvukcache_fetch_cancel(void) {
	pthread_mutex_lock(&lock);
	if (detached) {
		detached->abandon = true;
		if (detached->st_open) {
			http_stream_wake(&detached->st);
		}
	}
	pthread_mutex_unlock(&lock);
}

static void streamturn_hook(void) { zvukcache_abandon_all(); }

static void prune(long room);

bool zvukcache_start(const zvuk_track_t *track, const zvuk_stream_t *stream, char *out, size_t size) {
	last_error[0] = '\0';
	static bool registered;
	if (!registered) {
		registered = true;
		streamturn_register_abandon(streamturn_hook);
	}
	if (!zvukcache_ready()) {
		set_error("%s", tr("download_failed"));
		return false;
	}
	if (zvukcache_find(track->id, out, size)) {
		return true;
	}
	// The same track coming down already (the prefetch got there first).
	pthread_mutex_lock(&lock);
	if (current && !current->finished && strcmp(current->track_id, track->id) == 0) {
		download_t *d = current;
		snprintf(out, size, "%s", d->path);
		while (current == d && d->written < PREBUFFER_BYTES) {
			pthread_cond_wait(&changed, &lock);
		}
		bool ok = current == d || zvukcache_find(track->id, NULL, 0);
		pthread_mutex_unlock(&lock);
		return ok;
	}
	// Another one: waited for, never abandoned from here (see yandexcache.c).
	while (current && !current->finished) {
		pthread_cond_wait(&changed, &lock);
	}
	pthread_mutex_unlock(&lock);

	download_t *d = calloc(1, sizeof(*d));
	if (!d) {
		return false;
	}
	snprintf(d->track_id, sizeof(d->track_id), "%s", track->id);
	d->stream = *stream;
	zvukcache_path(track->id, zvukcache_extension(stream->got), d->path, sizeof(d->path));
	snprintf(d->marker, sizeof(d->marker), "%s" MARKER, d->path);
	prune(ZVUKCACHE_PRUNE_HEADROOM);
	FILE *marker = fopen(d->marker, "w");
	if (marker) {
		fclose(marker);
	}
	d->f = fopen(d->path, "wb");
	if (!d->f) {
		remove(d->marker);
		set_error("%s", tr("download_failed"));
		free(d);
		return false;
	}
	pthread_mutex_lock(&lock);
	current = d;
	pthread_mutex_unlock(&lock);
	growfile_announce(d->path, 0);

	pthread_t th;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, 192 * 1024); // TLS through the reconnects
	if (pthread_create(&th, &attr, download_main, d) != 0) {
		pthread_attr_destroy(&attr);
		growfile_finish(d->path, false);
		fclose(d->f);
		remove(d->path);
		remove(d->marker);
		pthread_mutex_lock(&lock);
		current = NULL;
		pthread_mutex_unlock(&lock);
		free(d);
		return false;
	}
	pthread_attr_destroy(&attr);

	snprintf(out, size, "%s", d->path);
	pthread_mutex_lock(&lock);
	bool ok;
	for (;;) {
		if (current != d) {
			ok = zvukcache_find(track->id, NULL, 0);
			break;
		}
		if (d->written >= PREBUFFER_BYTES) {
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
// keeping the cache in bounds (the Qobuz policy, as yandexcache.c)
// ---------------------------------------------------------------------------

static pthread_mutex_t keep_lock = PTHREAD_MUTEX_INITIALIZER;
static char protected_paths[ZVUKCACHE_PROTECTED_MAX][512];
static int protected_count;
#define RECENT_MAX 8
static char recent_paths[RECENT_MAX][512];
static int recent_next;
static int played_since_clear;

void zvukcache_set_protected(const char *const *paths, int count) {
	pthread_mutex_lock(&keep_lock);
	protected_count = 0;
	for (int i = 0; i < count && protected_count < ZVUKCACHE_PROTECTED_MAX; i++) {
		if (paths[i] && paths[i][0]) {
			snprintf(protected_paths[protected_count++], sizeof(protected_paths[0]), "%s", paths[i]);
		}
	}
	pthread_mutex_unlock(&keep_lock);
}

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
	long atime;
} entry_t;

#define PRUNE_MAX_ENTRIES 512

static int scan(entry_t *out, int max, long *total, bool (*keep)(const char *stem)) {
	*total = 0;
	int count = 0;
	DIR *dir = cache_dir[0] ? opendir(cache_dir) : NULL;
	if (!dir) {
		return 0;
	}
	struct dirent *e;
	while ((e = readdir(dir)) != NULL) {
		if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof(out[0].name)) {
			continue;
		}
		char path[640];
		// The name is short (checked above): the precision only tells the
		// compiler so.
		snprintf(path, sizeof(path), "%s/%.*s", cache_dir, (int)sizeof(out[0].name), e->d_name);
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
			continue;
		}
		*total += (long)st.st_size;
		// The stem: the name up to its first dot ("123.flac.tags" -> "123").
		char *dot = strchr(path + strlen(cache_dir) + 1, '.');
		if (dot) {
			*dot = '\0';
		}
		if (keep && keep(path)) {
			continue;
		}
		if (out && count < max) {
			snprintf(out[count].name, sizeof(out[count].name), "%.*s", (int)sizeof(out[count].name) - 1, e->d_name);
			out[count].size = (long)st.st_size;
			out[count].atime = (long)st.st_atime;
			count++;
		}
	}
	closedir(dir);
	return count;
}

static void prune(long room) {
	entry_t *entries = malloc(sizeof(*entries) * PRUNE_MAX_ENTRIES);
	if (!entries) {
		return;
	}
	long total;
	int count = scan(entries, PRUNE_MAX_ENTRIES, &total, is_kept_stem);
	for (int i = 0; i < count && total + room > ZVUKCACHE_MAX_BYTES; i++) {
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
			printf("zvukcache: removed %s (%ld KB)\n", entries[i].name, entries[i].size / 1024);
		}
	}
	free(entries);
}

static void clear_files(bool (*keep)(const char *stem)) {
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
	printf("zvukcache: cleared (%d files)\n", removed);
}

void zvukcache_note_played(const char *path) {
	if (!zvukcache_ready() || !path || !path[0]) {
		return;
	}
	pthread_mutex_lock(&keep_lock);
	snprintf(recent_paths[recent_next], sizeof(recent_paths[0]), "%s", path);
	recent_next = (recent_next + 1) % RECENT_MAX;
	bool sweep = ++played_since_clear >= ZVUKCACHE_CLEAR_EVERY;
	if (sweep) {
		played_since_clear = 0;
	}
	pthread_mutex_unlock(&keep_lock);
	if (sweep) {
		clear_files(is_kept_stem);
	}
}

static bool is_growing_stem(const char *stem) { return growfile_prefix_is_growing(stem); }

void zvukcache_clear(void) {
	if (zvukcache_ready()) {
		printf("zvukcache: clearing the cache on request\n");
		clear_files(is_growing_stem);
	}
}

void zvukcache_clear_on_exit(void) {
	if (zvukcache_ready()) {
		printf("zvukcache: shutting down, clearing the cache\n");
		zvukcache_abandon_all();
		clear_files(NULL);
	}
}

// ---------------------------------------------------------------------------
// sidecars
// ---------------------------------------------------------------------------

void zvukcache_write_sidecars(const zvuk_track_t *track, const char *path) {
	if (!path || !path[0]) {
		return;
	}
	char tags[600], tmp[620];
	snprintf(tags, sizeof(tags), "%.560s.tags", path);
	snprintf(tmp, sizeof(tmp), "%s.tmp", tags);
	FILE *f = fopen(tmp, "w");
	if (f) {
		fprintf(f, "title=%s\nartist=%s\nalbum=%s\nalbum_id=%s\ntrack=%d\n", track->title, track->artist, track->album,
				track->album_id, track->track_number);
		if (track->year > 0) {
			fprintf(f, "year=%d\n", track->year);
		}
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
	if (http_get_ex(track->cover, zvuk_stream_headers(), false, &body, &len, 2 * 1024 * 1024, 15, NULL) && len > 0) {
		FILE *c = fopen(jpg, "wb");
		if (c) {
			fwrite(body, 1, len, c);
			fclose(c);
		}
	}
	free(body);
}

#endif /* SONIX_ZVUK */

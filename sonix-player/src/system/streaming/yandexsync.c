#include "yandexsync.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "src/system/core/utils.h"
#include "src/system/streaming/provider.h"
#include "src/system/streaming/yandex.h"
#include "src/system/streaming/yandexcache.h"
#include "src/system/streaming/yandexdl.h"
#include "src/system/streaming/yandexsaved.h"

// See yandexsync.h.

static long monotonic_secs(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long)now.tv_sec;
}

// ---------------------------------------------------------------------------
// the worker and its queue
//
// A queue of a few jobs, not one slot, for the reason subsonicsync.c gives: a
// like tapped while the mirror is still loading must not replace the load, or
// the other way round. Started on first use, so a player never signed in to
// Yandex never has the thread.
// ---------------------------------------------------------------------------

typedef enum { JOB_LOAD, JOB_LIKE } job_kind_t;

typedef struct {
	job_kind_t kind;
	char track_id[YANDEX_ID_MAX];
	char album_id[YANDEX_ID_MAX];
	bool want;
	yandexsync_done_cb done;
	void (*changed)(void);
	void *user;
} job_t;

#define QUEUE_MAX 8

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static job_t queue[QUEUE_MAX];
static int queue_head;
static int queue_len;
static bool worker_started;
static bool load_queued; // the star asks on every redraw: one load in the queue is enough
// A load that failed is not asked for again before this (monotonic seconds):
// the star asks on every redraw, and a list that cannot be had right now
// (no network, the rate limit) would otherwise be asked for on every one.
static long load_retry_after;
#define LOAD_RETRY_SECS 60

static void *worker_main(void *arg);

static void submit(const job_t *job) {
	pthread_mutex_lock(&lock);
	if (!worker_started) {
		pthread_t thread;
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setstacksize(&attr, 256 * 1024); // TLS: as the page's worker
		if (pthread_create(&thread, &attr, worker_main, NULL) == 0) {
			pthread_detach(thread);
			worker_started = true;
		}
		pthread_attr_destroy(&attr);
	}
	if (queue_len == QUEUE_MAX) {
		// Full: the oldest goes. Eight writes waiting means Yandex has stopped
		// answering (or is making us wait, see throttle() in yandex.c), and the
		// newest tap is the one that matters.
		if (queue[queue_head].kind == JOB_LOAD) {
			load_queued = false;
		}
		queue_head = (queue_head + 1) % QUEUE_MAX;
		queue_len--;
	}
	if (job->kind == JOB_LOAD) {
		load_queued = true;
	}
	queue[(queue_head + queue_len) % QUEUE_MAX] = *job;
	queue_len++;
	pthread_cond_signal(&ready);
	pthread_mutex_unlock(&lock);
}

static void run_job(const job_t *job) {
	switch (job->kind) {
	case JOB_LOAD:
		if (yandex_liked_known() || !yandex_logged_in()) {
			break;
		}
		if (yandex_liked_refresh()) {
			// A star tapped while the list was on its way is in the queue,
			// not on the account yet: the list just installed does not have
			// it, and showing that would flip the star back under the
			// user's finger. Their taps win until they are sent.
			pthread_mutex_lock(&lock);
			for (int i = 0; i < queue_len; i++) {
				const job_t *q = &queue[(queue_head + i) % QUEUE_MAX];
				if (q->kind == JOB_LIKE) {
					yandex_liked_mark(q->track_id, q->want);
				}
			}
			pthread_mutex_unlock(&lock);
			if (job->changed) {
				job->changed();
			}
		} else {
			// Without the list the star reads every track as not liked, and
			// a tap on a liked one adds it again instead of removing it.
			fprintf(stderr, "yandexsync: the liked tracks did not load (%s); next try in %d s\n", yandex_last_error(), LOAD_RETRY_SECS);
			pthread_mutex_lock(&lock);
			load_retry_after = monotonic_secs() + LOAD_RETRY_SECS;
			pthread_mutex_unlock(&lock);
		}
		break;
	case JOB_LIKE: {
		printf("yandexsync: %s %s (album %s)\n", job->want ? "like" : "unlike", job->track_id, job->album_id[0] ? job->album_id : "none");
		bool ok = yandex_like_track(job->track_id, job->album_id, job->want);
		if (!ok) {
			yandex_liked_mark(job->track_id, !job->want); // refused: put it back
			fprintf(stderr, "yandexsync: like of %s failed: %s\n", job->track_id, yandex_last_error());
		}
		if (job->done) {
			job->done(ok, ok ? "" : yandex_last_error(), job->user);
		}
		break;
	}
	}
}

static void *worker_main(void *arg) {
	(void)arg;
	// Nice ten, not SCHED_IDLE (which the other services' sync threads use).
	// The likes are the one thing here, and a like is a tap waiting for its
	// answer. With Bluetooth LDAC encoding and a track decoding, the device
	// log shows the core 0 to 20 % idle; an idle-class thread then gets well
	// under one percent of it (see thread_be_low_priority), and the TLS
	// handshake plus the liked list a like may queue behind took minutes --
	// long enough for the star to flip back, for the player to sleep, for
	// the like never to leave. The lists, on the page's own worker, were at
	// normal priority all along, which is why everything but the likes
	// worked.
	thread_be_low_priority("yandex sync");
	for (;;) {
		pthread_mutex_lock(&lock);
		while (queue_len == 0) {
			pthread_cond_wait(&ready, &lock);
		}
		job_t job = queue[queue_head];
		queue_head = (queue_head + 1) % QUEUE_MAX;
		queue_len--;
		if (job.kind == JOB_LOAD) {
			load_queued = false;
		}
		pthread_mutex_unlock(&lock);
		run_job(&job);
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// the calls
// ---------------------------------------------------------------------------

void yandexsync_like(const char *track_id, const char *album_id, bool want, yandexsync_done_cb done, void *user) {
	if (!track_id || !track_id[0]) {
		return;
	}
	yandex_liked_mark(track_id, want);
	job_t job = {.kind = JOB_LIKE, .want = want, .done = done, .user = user};
	snprintf(job.track_id, sizeof(job.track_id), "%s", track_id);
	snprintf(job.album_id, sizeof(job.album_id), "%s", album_id ? album_id : "");
	submit(&job);
}

void yandexsync_likes_refresh(void (*done)(void)) {
	// Called on every redraw of the star: the common case -- already known,
	// or not signed in -- costs two flag reads and no lock held for long.
	if (yandex_liked_known() || !yandex_logged_in()) {
		return;
	}
	pthread_mutex_lock(&lock);
	bool queued = load_queued || monotonic_secs() < load_retry_after;
	pthread_mutex_unlock(&lock);
	if (queued) {
		return;
	}
	job_t job = {.kind = JOB_LOAD, .changed = done};
	submit(&job);
}

// ---------------------------------------------------------------------------
// the service, as the rest of the program sees it (provider.h)
//
// The file name is the track id ("12345.flac"), so path -> id is string work
// and costs nothing on every redraw. The album id, which the like wants beside
// the track's, is in the sidecar: read only when the star is tapped.
// ---------------------------------------------------------------------------

// A track being saved to the library counts as a download too: the Wi-Fi
// must stay up and the player awake for it, as for the cache's own.
static bool provider_downloading(void) { return yandexcache_busy() || yandexdl_busy(); }

static bool provider_network_wanted(void) { return yandexcache_network_wanted() || yandexdl_busy(); }

// The card: the cache follows it, and a save queued for the old one stops.
static void provider_set_root(const char *sd_root) {
	yandexcache_set_root(sd_root);
	yandexdl_set_root(sd_root);
	yandexsaved_set_root(sd_root);
}

// Shutdown, restart, update: the save in progress stops (its .part goes) and
// the cache is swept as before.
static void provider_clear_on_exit(void) {
	yandexdl_cancel_all();
	yandexcache_clear_on_exit();
}

// A saved file played from the Yandex pages is a Yandex track too -- its like
// goes to the account, "Show album" opens the service's album -- but only
// while that queue plays it (yandexsaved_claim): the same file played from
// the Library is the library's, and its queue, position and star are kept as
// for any other file.
static bool provider_owns(const char *path) { return yandexcache_owns(path) || yandexsaved_claimed(path); }

static bool provider_is_track(const char *path) { return yandexcache_is_track(path) || yandexsaved_claimed(path); }

// The track id behind a path: the cache's file name, or the index's entry.
static void track_ids(const char *path, char *id, size_t id_size, char *album, size_t album_size) {
	yandexcache_track_id(path, id, id_size);
	if (id[0]) {
		if (album && !yandexcache_album_id(path, album, album_size)) {
			album[0] = '\0';
		}
		return;
	}
	yandexsaved_track_for_path(path, id, id_size, album, album ? album_size : 0);
}

static bool provider_fav_get(const char *path) {
	char id[YANDEX_ID_MAX];
	track_ids(path, id, sizeof(id), NULL, 0);
	return id[0] && yandex_is_liked(id);
}

static void provider_fav_toggle(const char *path, bool want, stream_provider_done_cb done, void *user) {
	char id[YANDEX_ID_MAX], album[YANDEX_ID_MAX];
	track_ids(path, id, sizeof(id), album, sizeof(album));
	yandexsync_like(id, album, want, done, user);
}

static bool provider_album_id(const char *path, char *out, size_t size) {
	if (!out || size == 0) {
		return false;
	}
	char id[YANDEX_ID_MAX];
	track_ids(path, id, sizeof(id), out, size);
	return out[0] != '\0';
}

static stream_provider_t provider = {
	.id = "yandex",
	.set_root = provider_set_root,
	.cache_dir = yandexcache_dir,
	.owns_path = provider_owns,
	.downloading = provider_downloading,
	.network_wanted = provider_network_wanted,
	// Transient, like Qobuz's: swept on the way out (yandexcache.h). What was
	// saved to the library is the user's and is never touched.
	.clear_on_exit = provider_clear_on_exit,
	.is_track = provider_is_track,
	.fav_get = provider_fav_get,
	.fav_refresh = yandexsync_likes_refresh,
	.fav_toggle = provider_fav_toggle,
	.fav_failed_key = "yandex_like_failed",
	.album_id = provider_album_id,
	.album_failed_key = "yandex_album_failed",
	// No playlists: "Add to playlist" offers the card's for a Yandex track.
	.playlist_title_key = "yandex",
	.change_refused_key = "yandex_like_failed",
};

void yandexsync_register_provider(void) { stream_provider_register(&provider); }

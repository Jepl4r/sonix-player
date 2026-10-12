#include "yandexsync.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "src/system/core/utils.h"
#include "src/system/streaming/provider.h"
#include "src/system/streaming/yandex.h"
#include "src/system/streaming/yandexcache.h"

// See yandexsync.h.

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
		if (!yandex_liked_known() && yandex_logged_in() && yandex_liked_refresh() && job->changed) {
			job->changed();
		}
		break;
	case JOB_LIKE: {
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
	thread_be_background("yandex sync");
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
	bool queued = load_queued;
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

static bool provider_downloading(void) { return yandexcache_busy(); }

static bool provider_is_track(const char *path) { return yandexcache_is_track(path); }

static bool provider_fav_get(const char *path) {
	char id[YANDEX_ID_MAX];
	yandexcache_track_id(path, id, sizeof(id));
	return id[0] && yandex_is_liked(id);
}

static void provider_fav_toggle(const char *path, bool want, stream_provider_done_cb done, void *user) {
	char id[YANDEX_ID_MAX], album[YANDEX_ID_MAX];
	yandexcache_track_id(path, id, sizeof(id));
	if (!yandexcache_album_id(path, album, sizeof(album))) {
		album[0] = '\0';
	}
	yandexsync_like(id, album, want, done, user);
}

static stream_provider_t provider = {
	.id = "yandex",
	.set_root = yandexcache_set_root,
	.cache_dir = yandexcache_dir,
	.owns_path = yandexcache_owns,
	.downloading = provider_downloading,
	.network_wanted = yandexcache_network_wanted,
	// Transient, like Qobuz's: swept on the way out (yandexcache.h).
	.clear_on_exit = yandexcache_clear_on_exit,
	.is_track = provider_is_track,
	.fav_get = provider_fav_get,
	.fav_refresh = yandexsync_likes_refresh,
	.fav_toggle = provider_fav_toggle,
	.fav_failed_key = "yandex_like_failed",
	.album_id = yandexcache_album_id,
	.album_failed_key = "yandex_album_failed",
	// No playlists: "Add to playlist" offers the card's for a Yandex track.
	.playlist_title_key = "yandex",
	.change_refused_key = "yandex_like_failed",
};

void yandexsync_register_provider(void) { stream_provider_register(&provider); }

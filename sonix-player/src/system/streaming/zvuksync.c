#ifdef SONIX_ZVUK

#include "zvuksync.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "src/system/core/utils.h"
#include "src/system/streaming/provider.h"
#include "src/system/streaming/zvukcache.h"
#include "src/system/streaming/zvukdl.h"

// See zvuksync.h.

typedef enum { JOB_LOAD, JOB_LIKE } job_kind_t;

typedef struct {
	job_kind_t kind;
	zvuk_item_t item;
	char id[ZVUK_ID_MAX];
	bool want;
	zvuksync_done_cb done;
	void (*changed)(void);
	void *user;
} job_t;

#define QUEUE_MAX 8
#define LOAD_RETRY_SECS 60

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static job_t queue[QUEUE_MAX];
static int queue_head, queue_len;
static bool worker_started, load_queued;
static long load_retry_after;

static long monotonic_secs(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long)now.tv_sec;
}

static void *worker_main(void *arg);

static void submit(const job_t *job) {
	pthread_mutex_lock(&lock);
	if (!worker_started) {
		pthread_t thread;
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setstacksize(&attr, 256 * 1024);
		if (pthread_create(&thread, &attr, worker_main, NULL) == 0) {
			pthread_detach(thread);
			worker_started = true;
		}
		pthread_attr_destroy(&attr);
	}
	if (queue_len == QUEUE_MAX) {
		// Eight writes waiting: the service is not answering, and the newest
		// tap is the one that matters.
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
	if (job->kind == JOB_LOAD) {
		if (zvuk_liked_known(ZVUK_ITEM_TRACK) || !zvuk_logged_in()) {
			return;
		}
		if (zvuk_collection_refresh()) {
			// Taps still in the queue win over the list just read (see
			// yandexsync.c).
			pthread_mutex_lock(&lock);
			for (int i = 0; i < queue_len; i++) {
				const job_t *q = &queue[(queue_head + i) % QUEUE_MAX];
				if (q->kind == JOB_LIKE) {
					zvuk_liked_mark(q->item, q->id, q->want);
				}
			}
			pthread_mutex_unlock(&lock);
			if (job->changed) {
				job->changed();
			}
		} else {
			fprintf(stderr, "zvuksync: the collection did not load (%s); next try in %d s\n", zvuk_last_error(),
					LOAD_RETRY_SECS);
			pthread_mutex_lock(&lock);
			load_retry_after = monotonic_secs() + LOAD_RETRY_SECS;
			pthread_mutex_unlock(&lock);
		}
		return;
	}
	bool ok = zvuk_like(job->item, job->id, job->want);
	if (!ok) {
		zvuk_liked_mark(job->item, job->id, !job->want); // refused: put back
	}
	if (job->done) {
		job->done(ok, ok ? "" : zvuk_last_error(), job->user);
	}
}

static void *worker_main(void *arg) {
	(void)arg;
	// Nice ten, as yandexsync.c explains: a like is a tap waiting.
	thread_be_low_priority("zvuk sync");
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

void zvuksync_like(zvuk_item_t kind, const char *id, bool want, zvuksync_done_cb done, void *user) {
	if (!id || !id[0]) {
		return;
	}
	zvuk_liked_mark(kind, id, want);
	job_t job = {.kind = JOB_LIKE, .item = kind, .want = want, .done = done, .user = user};
	snprintf(job.id, sizeof(job.id), "%s", id);
	submit(&job);
}

void zvuksync_likes_refresh(void (*done)(void)) {
	if (zvuk_liked_known(ZVUK_ITEM_TRACK) || !zvuk_logged_in()) {
		return;
	}
	pthread_mutex_lock(&lock);
	bool queued = load_queued || monotonic_secs() < load_retry_after;
	pthread_mutex_unlock(&lock);
	if (!queued) {
		job_t job = {.kind = JOB_LOAD, .changed = done};
		submit(&job);
	}
}

// ---------------------------------------------------------------------------
// the service, as the rest of the program sees it (provider.h)
// ---------------------------------------------------------------------------

static bool provider_downloading(void) { return zvukcache_busy() || zvukdl_busy(); }

static bool provider_network_wanted(void) { return zvukcache_network_wanted() || zvukdl_busy(); }

static void provider_set_root(const char *sd_root) {
	zvukcache_set_root(sd_root);
	zvukdl_set_root(sd_root);
}

static void provider_clear_on_exit(void) {
	zvukdl_cancel_all();
	zvukcache_clear_on_exit();
}

static bool provider_owns(const char *path) { return zvukcache_owns(path) || zvukdl_claimed(path); }

static bool provider_is_track(const char *path) { return zvukcache_is_track(path) || zvukdl_claimed(path); }

static void track_ids(const char *path, char *id, size_t id_size, char *album, size_t album_size) {
	zvukcache_track_id(path, id, id_size);
	if (id[0]) {
		if (album && !zvukcache_album_id(path, album, album_size)) {
			album[0] = '\0';
		}
		return;
	}
	zvukdl_track_for_path(path, id, id_size, album, album ? album_size : 0);
}

static bool provider_fav_get(const char *path) {
	char id[ZVUK_ID_MAX];
	track_ids(path, id, sizeof(id), NULL, 0);
	return id[0] && zvuk_is_liked(ZVUK_ITEM_TRACK, id);
}

static void provider_fav_toggle(const char *path, bool want, stream_provider_done_cb done, void *user) {
	char id[ZVUK_ID_MAX];
	track_ids(path, id, sizeof(id), NULL, 0);
	zvuksync_like(ZVUK_ITEM_TRACK, id, want, done, user);
}

static bool provider_album_id(const char *path, char *out, size_t size) {
	if (!out || size == 0) {
		return false;
	}
	char id[ZVUK_ID_MAX];
	track_ids(path, id, sizeof(id), out, size);
	return out[0] != '\0';
}

static stream_provider_t provider = {
	.id = "zvuk",
	.set_root = provider_set_root,
	.cache_dir = zvukcache_dir,
	.owns_path = provider_owns,
	.downloading = provider_downloading,
	.network_wanted = provider_network_wanted,
	// The cache is transient, swept on the way out; what was saved to the
	// library is the user's and never touched.
	.clear_on_exit = provider_clear_on_exit,
	.is_track = provider_is_track,
	.fav_get = provider_fav_get,
	.fav_refresh = zvuksync_likes_refresh,
	.fav_toggle = provider_fav_toggle,
	.fav_failed_key = "zvuk_like_failed",
	.album_id = provider_album_id,
	.album_failed_key = "zvuk_album_failed",
	.playlist_title_key = "zvuk",
	.change_refused_key = "zvuk_like_failed",
};

void zvuksync_register_provider(void) { stream_provider_register(&provider); }

#endif /* SONIX_ZVUK */

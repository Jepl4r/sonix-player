#include "subsonicsync.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/streaming/provider.h"
#include "src/system/streaming/subsoniccache.h"

// How many starred songs the mirror holds. Past this the rest show an empty
// star until touched -- the Qobuz trade-off. Allocated on first load, so a
// player never signed in to a Subsonic server spends nothing on it.
#define FAVORITES_MAX 1000

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char (*favorites)[SUBSONIC_ID_MAX];
static int favorites_count;
static bool favorites_loaded;

static subsonic_playlist_t *playlists;
static int playlists_count;
static bool playlists_loaded;

// When a failed load of the starred songs may be retried (see
// subsonicsync_favorites_refresh). Under `lock`.
static long load_retry_after;

// ---------------------------------------------------------------------------
// the mirror
// ---------------------------------------------------------------------------

bool subsonicsync_is_favorite(const char *song_id) {
	if (!song_id || !song_id[0]) {
		return false;
	}
	pthread_mutex_lock(&lock);
	bool found = false;
	for (int i = 0; i < favorites_count && !found; i++) {
		found = strcmp(favorites[i], song_id) == 0;
	}
	pthread_mutex_unlock(&lock);
	return found;
}

// Call with the lock held.
static void mirror_set_locked(const char *song_id, bool on) {
	for (int i = 0; i < favorites_count; i++) {
		if (strcmp(favorites[i], song_id) != 0) {
			continue;
		}
		if (!on) {
			memcpy(favorites[i], favorites[--favorites_count], SUBSONIC_ID_MAX);
		}
		return;
	}
	if (!on) {
		return;
	}
	if (!favorites) {
		favorites = calloc(FAVORITES_MAX, SUBSONIC_ID_MAX);
		if (!favorites) {
			return;
		}
	}
	if (favorites_count < FAVORITES_MAX) {
		snprintf(favorites[favorites_count++], SUBSONIC_ID_MAX, "%s", song_id);
	}
}

void subsonicsync_forget(void) {
	pthread_mutex_lock(&lock);
	free(favorites);
	favorites = NULL;
	favorites_count = 0;
	favorites_loaded = false;
	load_retry_after = 0; // another account, another server: ask at once
	free(playlists);
	playlists = NULL;
	playlists_count = 0;
	playlists_loaded = false;
	pthread_mutex_unlock(&lock);
}

// ---------------------------------------------------------------------------
// the worker and its queue
// ---------------------------------------------------------------------------

typedef enum {
	JOB_FAV_LOAD,
	JOB_STAR,
	JOB_STAR_ALBUM,
	JOB_SCROBBLE,
	JOB_PLAYLISTS_LOAD,
	JOB_PLAYLIST_ADD,
	JOB_PLAYLIST_CREATE,
} job_kind_t;

typedef struct {
	job_kind_t kind;
	char id[SUBSONIC_ID_MAX];
	char id2[SUBSONIC_TITLE_MAX]; // playlist id, or a new playlist's name
	bool flag;					  // star: want; scrobble: submission
	subsonicsync_done_cb done;
	void (*changed)(void);
	void *user;
} job_t;

#define QUEUE_MAX 8

static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static job_t queue[QUEUE_MAX];
static int queue_head;
static int queue_len;
static bool worker_started;

// The star asks for the mirror on every redraw until it is there: one load,
// waiting or running, is enough. Without this a server that does not answer filled the
// queue with loads, and a full queue drops its oldest job -- which could be
// the star the user had just tapped.
static bool load_queued;

// A load that failed is not asked for again before this (monotonic seconds),
// for the same reason: every redraw would otherwise ask again at once.
#define LOAD_RETRY_SECS 60

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
		if (pthread_create(&thread, NULL, worker_main, NULL) == 0) {
			pthread_detach(thread);
			worker_started = true;
		}
	}
	if (queue_len == QUEUE_MAX) {
		// Full: the oldest job goes. Eight pending writes means the server has
		// stopped answering, and the newest tap is the one that matters.
		if (queue[queue_head].kind == JOB_FAV_LOAD) {
			load_queued = false;
		}
		queue_head = (queue_head + 1) % QUEUE_MAX;
		queue_len--;
	}
	if (job->kind == JOB_FAV_LOAD) {
		load_queued = true;
	}
	queue[(queue_head + queue_len) % QUEUE_MAX] = *job;
	queue_len++;
	pthread_cond_signal(&ready);
	pthread_mutex_unlock(&lock);
}

static void finish(const job_t *job, bool ok) {
	if (job->done) {
		job->done(ok, ok ? "" : subsonic_last_error(), job->user);
	}
}

static void run_job(const job_t *job) {
	switch (job->kind) {
	case JOB_FAV_LOAD: {
		char(*got)[SUBSONIC_ID_MAX] = malloc((size_t)FAVORITES_MAX * SUBSONIC_ID_MAX);
		int n = got ? subsonic_starred_ids(got, FAVORITES_MAX) : -1;
		if (n >= 0) {
			pthread_mutex_lock(&lock);
			free(favorites);
			favorites = got;
			favorites_count = n;
			favorites_loaded = true;
			// A star tapped while the list was on its way is still in the
			// queue, not on the server: the list just installed does not
			// have it, and showing that would flip the star back under the
			// user's finger until the write goes out (and for good, since a
			// successful write does not touch the mirror). Their taps win
			// until they are sent -- as in yandexsync.c.
			for (int i = 0; i < queue_len; i++) {
				const job_t *q = &queue[(queue_head + i) % QUEUE_MAX];
				if (q->kind == JOB_STAR) {
					mirror_set_locked(q->id, q->flag);
				}
			}
			pthread_mutex_unlock(&lock);
			got = NULL;
			if (job->changed) {
				job->changed();
			}
		} else {
			fprintf(stderr, "subsonicsync: the starred songs did not load (%s); next try in %d s\n",
					subsonic_last_error(), LOAD_RETRY_SECS);
			pthread_mutex_lock(&lock);
			load_retry_after = monotonic_secs() + LOAD_RETRY_SECS;
			pthread_mutex_unlock(&lock);
		}
		free(got);
		break;
	}
	case JOB_STAR: {
		bool ok = subsonic_star(job->id, false, job->flag);
		if (!ok) {
			pthread_mutex_lock(&lock);
			mirror_set_locked(job->id, !job->flag); // the server said no: put it back
			pthread_mutex_unlock(&lock);
		}
		finish(job, ok);
		break;
	}
	case JOB_STAR_ALBUM:
		finish(job, subsonic_star(job->id, true, job->flag));
		break;
	case JOB_SCROBBLE:
		// No callback and no retry: a missed play count is not worth a popup.
		if (!subsonic_scrobble(job->id, job->flag)) {
			fprintf(stderr, "subsonicsync: scrobble of %s failed: %s\n", job->id, subsonic_last_error());
		}
		break;
	case JOB_PLAYLISTS_LOAD: {
		subsonic_playlist_t *got = malloc(sizeof(*got) * SUBSONICSYNC_PLAYLISTS_MAX);
		int n = got ? subsonic_playlists(0, got, SUBSONICSYNC_PLAYLISTS_MAX) : -1;
		if (n >= 0) {
			pthread_mutex_lock(&lock);
			free(playlists);
			playlists = got;
			playlists_count = n;
			playlists_loaded = true;
			pthread_mutex_unlock(&lock);
			got = NULL;
			if (job->changed) {
				job->changed();
			}
		}
		free(got);
		break;
	}
	case JOB_PLAYLIST_ADD: {
		bool ok = subsonic_playlist_add(job->id2, job->id);
		if (ok) {
			pthread_mutex_lock(&lock);
			playlists_loaded = false; // a count changed: re-read next time
			pthread_mutex_unlock(&lock);
		}
		finish(job, ok);
		break;
	}
	case JOB_PLAYLIST_CREATE: {
		bool ok = subsonic_playlist_create(job->id2, job->id);
		if (ok) {
			pthread_mutex_lock(&lock);
			playlists_loaded = false;
			pthread_mutex_unlock(&lock);
		}
		finish(job, ok);
		break;
	}
	}
}

static void *worker_main(void *arg) {
	(void)arg;
	// Nice ten, not SCHED_IDLE. A star is a tap waiting for its answer, and
	// with Bluetooth encoding and a track decoding the core is 0 to 20 % idle:
	// an idle-class thread then gets well under one percent of it, and the
	// request -- or the starred list a star may queue behind -- took minutes.
	// That is how Yandex's likes failed in 1.2.3 (see yandexsync.c).
	thread_be_low_priority("subsonic sync");
	for (;;) {
		pthread_mutex_lock(&lock);
		while (queue_len == 0) {
			pthread_cond_wait(&ready, &lock);
		}
		job_t job = queue[queue_head];
		queue_head = (queue_head + 1) % QUEUE_MAX;
		queue_len--;
		pthread_mutex_unlock(&lock);
		run_job(&job);
		// Only now: while a load runs, the redraws that ask for the mirror
		// must not queue a second one behind it.
		if (job.kind == JOB_FAV_LOAD) {
			pthread_mutex_lock(&lock);
			load_queued = false;
			pthread_mutex_unlock(&lock);
		}
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// the calls
// ---------------------------------------------------------------------------

void subsonicsync_favorites_refresh(void (*done)(void)) {
	pthread_mutex_lock(&lock);
	bool known = favorites_loaded || load_queued || monotonic_secs() < load_retry_after;
	pthread_mutex_unlock(&lock);
	if (known || !subsonic_logged_in()) {
		return;
	}
	job_t job = {.kind = JOB_FAV_LOAD, .changed = done};
	submit(&job);
}

void subsonicsync_favorite_toggle(const char *song_id, bool want, subsonicsync_done_cb done, void *user) {
	if (!song_id || !song_id[0]) {
		return;
	}
	pthread_mutex_lock(&lock);
	mirror_set_locked(song_id, want);
	pthread_mutex_unlock(&lock);
	job_t job = {.kind = JOB_STAR, .flag = want, .done = done, .user = user};
	snprintf(job.id, sizeof(job.id), "%s", song_id);
	submit(&job);
}

void subsonicsync_album_favorite(const char *album_id, bool want, subsonicsync_done_cb done, void *user) {
	job_t job = {.kind = JOB_STAR_ALBUM, .flag = want, .done = done, .user = user};
	snprintf(job.id, sizeof(job.id), "%s", album_id ? album_id : "");
	submit(&job);
}

void subsonicsync_scrobble(const char *song_id, bool submission) {
	if (!song_id || !song_id[0] || !subsonic_logged_in()) {
		return;
	}
	job_t job = {.kind = JOB_SCROBBLE, .flag = submission};
	snprintf(job.id, sizeof(job.id), "%s", song_id);
	submit(&job);
}

void subsonicsync_playlists_refresh(void (*done)(void)) {
	if (!subsonic_logged_in()) {
		return;
	}
	job_t job = {.kind = JOB_PLAYLISTS_LOAD, .changed = done};
	submit(&job);
}

bool subsonicsync_playlists_known(void) {
	pthread_mutex_lock(&lock);
	bool known = playlists_loaded;
	pthread_mutex_unlock(&lock);
	return known;
}

int subsonicsync_playlists(subsonic_playlist_t *out, int max) {
	if (!out || max <= 0) {
		return 0;
	}
	pthread_mutex_lock(&lock);
	int count = playlists && playlists_count < max ? playlists_count : (playlists ? max : 0);
	if (count > 0) {
		memcpy(out, playlists, sizeof(*out) * (size_t)count);
	}
	pthread_mutex_unlock(&lock);
	return count;
}

void subsonicsync_playlist_add(const char *playlist_id, const char *song_id, subsonicsync_done_cb done, void *user) {
	job_t job = {.kind = JOB_PLAYLIST_ADD, .done = done, .user = user};
	snprintf(job.id, sizeof(job.id), "%s", song_id ? song_id : "");
	snprintf(job.id2, sizeof(job.id2), "%s", playlist_id ? playlist_id : "");
	submit(&job);
}

void subsonicsync_playlist_create_with(const char *name, const char *song_id, subsonicsync_done_cb done, void *user) {
	job_t job = {.kind = JOB_PLAYLIST_CREATE, .done = done, .user = user};
	snprintf(job.id, sizeof(job.id), "%s", song_id ? song_id : "");
	snprintf(job.id2, sizeof(job.id2), "%s", name ? name : "");
	submit(&job);
}

// ---------------------------------------------------------------------------
// the service, as the rest of the program sees it (provider.h)
//
// Paths map to song ids through the `.tags` sidecar (the file name is a hash,
// see subsoniccache.h). That is a small file read, so the last answer is
// remembered: the star asks for the same path on every redraw.
// ---------------------------------------------------------------------------

static pthread_mutex_t memo_lock = PTHREAD_MUTEX_INITIALIZER;
static char memo_path[512];
static char memo_id[SUBSONIC_ID_MAX];

static bool song_id_for(const char *path, char *out, size_t size) {
	pthread_mutex_lock(&memo_lock);
	bool hit = memo_path[0] && strcmp(memo_path, path) == 0;
	if (hit) {
		snprintf(out, size, "%s", memo_id);
	}
	pthread_mutex_unlock(&memo_lock);
	if (hit) {
		return out[0] != '\0';
	}
	if (!subsoniccache_song_id(path, out, size) || !out[0]) {
		return false;
	}
	pthread_mutex_lock(&memo_lock);
	snprintf(memo_path, sizeof(memo_path), "%s", path);
	snprintf(memo_id, sizeof(memo_id), "%s", out);
	pthread_mutex_unlock(&memo_lock);
	return true;
}

static bool provider_is_track(const char *path) {
	char key[SUBSONICCACHE_KEY_LEN];
	char id[SUBSONIC_ID_MAX];
	return subsoniccache_path_key(path, key, sizeof(key)) && song_id_for(path, id, sizeof(id));
}

static bool provider_fav_get(const char *path) {
	char id[SUBSONIC_ID_MAX];
	return song_id_for(path, id, sizeof(id)) && subsonicsync_is_favorite(id);
}

static void provider_fav_toggle(const char *path, bool want, stream_provider_done_cb done, void *user) {
	char id[SUBSONIC_ID_MAX];
	if (song_id_for(path, id, sizeof(id))) {
		subsonicsync_favorite_toggle(id, want, done, user);
	}
}

static int provider_playlists(stream_provider_playlist_t *out, int max) {
	if (!out || max <= 0) {
		return 0;
	}
	if (max > SUBSONICSYNC_PLAYLISTS_MAX) {
		max = SUBSONICSYNC_PLAYLISTS_MAX;
	}
	subsonic_playlist_t *got = malloc(sizeof(*got) * (size_t)max);
	if (!got) {
		return 0;
	}
	int count = subsonicsync_playlists(got, max);
	for (int i = 0; i < count; i++) {
		snprintf(out[i].id, sizeof(out[i].id), "%s", got[i].id);
		snprintf(out[i].name, sizeof(out[i].name), "%s", got[i].name);
		out[i].track_count = got[i].song_count;
	}
	free(got);
	return count;
}

static void provider_playlist_add(const char *playlist_id, const char *path, stream_provider_done_cb done, void *user) {
	char id[SUBSONIC_ID_MAX];
	if (song_id_for(path, id, sizeof(id))) {
		subsonicsync_playlist_add(playlist_id, id, done, user);
	}
}

static void provider_playlist_create_with(const char *name, const char *path, stream_provider_done_cb done,
										  void *user) {
	char id[SUBSONIC_ID_MAX];
	if (song_id_for(path, id, sizeof(id))) {
		subsonicsync_playlist_create_with(name, id, done, user);
	}
}

static stream_provider_t provider = {
	.id = "subsonic",
	.set_root = subsoniccache_set_root,
	.cache_dir = subsoniccache_dir,
	.owns_path = subsoniccache_owns,
	.downloading = subsoniccache_busy,
	.network_wanted = subsoniccache_network_wanted,
	// The cache persists across power cycles (offline listening, see
	// subsoniccache.h); only a factory reset empties it.
	.clear_on_exit = NULL,
	.wipe = subsoniccache_wipe,
	.is_track = provider_is_track,
	.fav_get = provider_fav_get,
	.fav_refresh = subsonicsync_favorites_refresh,
	.fav_toggle = provider_fav_toggle,
	.fav_failed_key = "subsonic_change_refused",
	.album_id = subsoniccache_album_id,
	.album_failed_key = "subsonic_album_failed",
	.playlists_known = subsonicsync_playlists_known,
	.playlists_refresh = subsonicsync_playlists_refresh,
	.playlists = provider_playlists,
	.playlist_add = provider_playlist_add,
	.playlist_create_with = provider_playlist_create_with,
	.playlist_title_key = "playlist_subsonic_playlist",
	.change_refused_key = "subsonic_change_refused",
};

void subsonicsync_register_provider(void) { stream_provider_register(&provider); }

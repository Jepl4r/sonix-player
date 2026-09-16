#include "qobuzsync.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "src/system/core/utils.h"
#include "src/system/core/lang.h"

// How many favourites are cached. Beyond this the oldest show an empty star
// until touched again: the trade-off between always knowing the truth (one
// request per track, on every track change) and never asking at all.
#define FAVORITES_MAX 500

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static long favorites[FAVORITES_MAX];
static int favorites_count;
static bool favorites_loaded;

static qobuz_playlist_t playlists[QOBUZSYNC_PLAYLISTS_MAX];
static int playlists_count;
static bool playlists_loaded;

// ---------------------------------------------------------------------------
// the mirror
// ---------------------------------------------------------------------------

bool qobuzsync_favorites_known(void) {
	pthread_mutex_lock(&lock);
	bool known = favorites_loaded;
	pthread_mutex_unlock(&lock);
	return known;
}

bool qobuzsync_is_favorite(long track_id) {
	if (track_id <= 0) {
		return false;
	}
	pthread_mutex_lock(&lock);
	bool found = false;
	for (int i = 0; i < favorites_count && !found; i++) {
		found = favorites[i] == track_id;
	}
	pthread_mutex_unlock(&lock);
	return found;
}

// Call with the lock held.
static void mirror_set_locked(long track_id, bool on) {
	for (int i = 0; i < favorites_count; i++) {
		if (favorites[i] != track_id) {
			continue;
		}
		if (!on) {
			favorites[i] = favorites[--favorites_count];
		}
		return;
	}
	if (on && favorites_count < FAVORITES_MAX) {
		favorites[favorites_count++] = track_id;
	}
}

static void mirror_set(long track_id, bool on) {
	pthread_mutex_lock(&lock);
	mirror_set_locked(track_id, on);
	pthread_mutex_unlock(&lock);
}

void qobuzsync_forget(void) {
	pthread_mutex_lock(&lock);
	favorites_count = 0;
	favorites_loaded = false;
	playlists_count = 0;
	playlists_loaded = false;
	pthread_mutex_unlock(&lock);
}

// ---------------------------------------------------------------------------
// the worker
//
// One thread, one queue slot. Submitting while a job is already waiting
// replaces it: these are user taps, and the last one wins. A job already
// running is always finished -- aborting a request that writes to the account
// halfway through would leave its state unknown.
// ---------------------------------------------------------------------------

typedef enum {
	JOB_NONE = 0,
	JOB_FAVORITES_LOAD,
	JOB_FAVORITE_TOGGLE,
	JOB_PLAYLISTS_LOAD,
	JOB_PLAYLIST_ADD,
	JOB_PLAYLIST_CREATE,
	JOB_ALBUM_FAVORITE,
	JOB_PLAYLIST_REMOVE,
} job_kind_t;

typedef struct {
	job_kind_t kind;
	long track_id;
	long playlist_id;
	bool want;
	char name[128];
	char album_id[40];
	qobuzsync_done_cb done;
	void *user;
	void (*changed)(void);
} job_t;

static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_ready = PTHREAD_COND_INITIALIZER;
static job_t pending;
static bool have_pending;
static bool worker_started;

static void do_favorites_load(const job_t *job) {
	long ids[FAVORITES_MAX];
	int count = qobuz_favorite_track_ids(ids, FAVORITES_MAX);
	if (count < 0) {
		printf("qobuzsync: favorites not read: %s\n", qobuz_last_error());
		return;
	}

	pthread_mutex_lock(&lock);
	memcpy(favorites, ids, sizeof(long) * (size_t)count);
	favorites_count = count;
	favorites_loaded = true;
	pthread_mutex_unlock(&lock);

	printf("qobuzsync: %d favorite tracks\n", count);
	if (job->changed) {
		job->changed();
	}
}

static void do_favorite_toggle(const job_t *job) {
	bool ok = job->want ? qobuz_favorite_add_track(job->track_id) : qobuz_favorite_remove_track(job->track_id);
	if (!ok) {
		// The mirror was flipped before the request so the star could answer the
		// tap immediately; if the request fails it has to go back, or the player
		// and the account disagree.
		mirror_set(job->track_id, !job->want);
	}
	if (job->done) {
		job->done(ok, ok ? NULL : qobuz_last_error(), job->user);
	}
}

static void do_playlists_load(const job_t *job) {
	qobuz_playlist_t got[QOBUZSYNC_PLAYLISTS_MAX];
	int count = qobuz_user_playlists(0, got, QOBUZSYNC_PLAYLISTS_MAX);
	if (count < 0) {
		printf("qobuzsync: playlists not read: %s\n", qobuz_last_error());
		return;
	}

	pthread_mutex_lock(&lock);
	memcpy(playlists, got, sizeof(qobuz_playlist_t) * (size_t)count);
	playlists_count = count;
	playlists_loaded = true;
	pthread_mutex_unlock(&lock);

	if (job->changed) {
		job->changed();
	}
}

static void do_playlist_add(const job_t *job) {
	bool ok = qobuz_playlist_add_track(job->playlist_id, job->track_id);
	if (job->done) {
		job->done(ok, ok ? NULL : qobuz_last_error(), job->user);
	}
}

static void do_playlist_create(const job_t *job) {
	long id = 0;
	bool ok = qobuz_playlist_create(job->name, &id);
	if (ok && job->track_id > 0) {
		ok = qobuz_playlist_add_track(id, job->track_id);
	}
	if (ok) {
		// The cached list is one playlist out of date; force a reload so the
		// picker shows the new one next time it opens.
		pthread_mutex_lock(&lock);
		playlists_loaded = false;
		pthread_mutex_unlock(&lock);
	}
	if (job->done) {
		job->done(ok, ok ? NULL : qobuz_last_error(), job->user);
	}
}

static void do_album_favorite(const job_t *job) {
	bool ok = job->want ? qobuz_favorite_add_album(job->album_id) : qobuz_favorite_remove_album(job->album_id);
	if (job->done) {
		job->done(ok, ok ? NULL : qobuz_last_error(), job->user);
	}
}

static void do_playlist_remove(const job_t *job) {
	bool ok = qobuz_playlist_delete(job->playlist_id);
	if (ok) {
		pthread_mutex_lock(&lock);
		playlists_loaded = false; // the cached list still holds the deleted one
		pthread_mutex_unlock(&lock);
	}
	if (job->done) {
		job->done(ok, ok ? NULL : qobuz_last_error(), job->user);
	}
}

static void *worker_main(void *arg) {
	(void)arg;
	thread_be_background("qobuz sync");

	for (;;) {
		pthread_mutex_lock(&job_lock);
		while (!have_pending) {
			pthread_cond_wait(&job_ready, &job_lock);
		}
		job_t job = pending;
		have_pending = false;
		pthread_mutex_unlock(&job_lock);

		switch (job.kind) {
		case JOB_FAVORITES_LOAD:
			do_favorites_load(&job);
			break;
		case JOB_FAVORITE_TOGGLE:
			do_favorite_toggle(&job);
			break;
		case JOB_PLAYLISTS_LOAD:
			do_playlists_load(&job);
			break;
		case JOB_PLAYLIST_ADD:
			do_playlist_add(&job);
			break;
		case JOB_PLAYLIST_CREATE:
			do_playlist_create(&job);
			break;
		case JOB_ALBUM_FAVORITE:
			do_album_favorite(&job);
			break;
		case JOB_PLAYLIST_REMOVE:
			do_playlist_remove(&job);
			break;
		default:
			break;
		}
	}
	return NULL;
}

static void submit(const job_t *job) {
	pthread_mutex_lock(&job_lock);
	if (!worker_started) {
		pthread_t thread;
		if (pthread_create(&thread, NULL, worker_main, NULL) == 0) {
			pthread_detach(thread);
			worker_started = true;
		} else {
			pthread_mutex_unlock(&job_lock);
			if (job->done) {
				job->done(false, tr("cannot_start_the_request"), job->user);
			}
			return;
		}
	}
	pending = *job;
	have_pending = true;
	pthread_cond_signal(&job_ready);
	pthread_mutex_unlock(&job_lock);
}

// ---------------------------------------------------------------------------
// public face
// ---------------------------------------------------------------------------

void qobuzsync_favorites_refresh(void (*done)(void)) {
	if (!qobuz_logged_in() || qobuzsync_favorites_known()) {
		return;
	}
	job_t job = {.kind = JOB_FAVORITES_LOAD, .changed = done};
	submit(&job);
}

void qobuzsync_favorite_toggle(long track_id, bool want, qobuzsync_done_cb done, void *user) {
	if (track_id <= 0) {
		if (done) {
			done(false, tr("qobuz_track_without_id"), user);
		}
		return;
	}

	// Mirror first, network after: the star fills under the finger instead of a
	// second and a half later.
	mirror_set(track_id, want);

	job_t job = {.kind = JOB_FAVORITE_TOGGLE, .track_id = track_id, .want = want, .done = done, .user = user};
	submit(&job);
}

void qobuzsync_playlist_add(long playlist_id, long track_id, qobuzsync_done_cb done, void *user) {
	job_t job = {
		.kind = JOB_PLAYLIST_ADD, .playlist_id = playlist_id, .track_id = track_id, .done = done, .user = user};
	submit(&job);
}

void qobuzsync_playlist_create_with(const char *name, long track_id, qobuzsync_done_cb done, void *user) {
	job_t job = {.kind = JOB_PLAYLIST_CREATE, .track_id = track_id, .done = done, .user = user};
	snprintf(job.name, sizeof(job.name), "%s", name ? name : "");
	submit(&job);
}

void qobuzsync_album_favorite(const char *album_id, bool want, qobuzsync_done_cb done, void *user) {
	if (!album_id || !album_id[0]) {
		if (done) {
			done(false, tr("qobuz_album_without_id"), user);
		}
		return;
	}
	job_t job = {.kind = JOB_ALBUM_FAVORITE, .want = want, .done = done, .user = user};
	snprintf(job.album_id, sizeof(job.album_id), "%s", album_id);
	submit(&job);
}

void qobuzsync_playlist_remove(long playlist_id, qobuzsync_done_cb done, void *user) {
	job_t job = {.kind = JOB_PLAYLIST_REMOVE, .playlist_id = playlist_id, .done = done, .user = user};
	submit(&job);
}

void qobuzsync_playlists_refresh(void (*done)(void)) {
	if (!qobuz_logged_in()) {
		return;
	}
	job_t job = {.kind = JOB_PLAYLISTS_LOAD, .changed = done};
	submit(&job);
}

bool qobuzsync_playlists_known(void) {
	pthread_mutex_lock(&lock);
	bool known = playlists_loaded;
	pthread_mutex_unlock(&lock);
	return known;
}

int qobuzsync_playlists(qobuz_playlist_t *out, int max) {
	if (!out || max <= 0) {
		return 0;
	}
	pthread_mutex_lock(&lock);
	int count = playlists_count < max ? playlists_count : max;
	memcpy(out, playlists, sizeof(qobuz_playlist_t) * (size_t)count);
	pthread_mutex_unlock(&lock);
	return count;
}

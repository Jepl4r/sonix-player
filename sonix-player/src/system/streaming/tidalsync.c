#include "tidalsync.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/system/streaming/tidalcache.h"
#include "src/system/streaming/provider.h"

#include "src/system/core/utils.h"
#include "src/system/core/lang.h"

// How many favourites fit in the local mirror. Beyond this the oldest show an
// empty star until touched: the price of not asking Tidal whether each track is
// a favourite on every track change, which would be one extra request per song.
#define FAVORITES_MAX 500

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static long favorites[FAVORITES_MAX];
static int favorites_count;
static bool favorites_loaded;

static tidal_playlist_t playlists[TIDALSYNC_PLAYLISTS_MAX];
static int playlists_count;
static bool playlists_loaded;

// When a failed load of the favourites may be retried (monotonic seconds; see
// the worker). Under `lock`.
static long load_retry_after;
#define LOAD_RETRY_SECS 60

static long monotonic_secs(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long)now.tv_sec;
}

// ---------------------------------------------------------------------------
// the mirror
// ---------------------------------------------------------------------------

bool tidalsync_favorites_known(void) {
	pthread_mutex_lock(&lock);
	bool known = favorites_loaded;
	pthread_mutex_unlock(&lock);
	return known;
}

bool tidalsync_is_favorite(long track_id) {
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

void tidalsync_forget(void) {
	pthread_mutex_lock(&lock);
	favorites_count = 0;
	favorites_loaded = false;
	load_retry_after = 0; // another account: ask at once
	playlists_count = 0;
	playlists_loaded = false;
	pthread_mutex_unlock(&lock);
}

// ---------------------------------------------------------------------------
// the worker
//
// One thread and a short queue, as in subsonicsync.c and yandexsync.c. It used
// to be a single slot where a newer job replaced one still waiting, on the
// idea that these are taps and the last one wins -- but they are not all the
// same tap. A star on one track replaced by a star on the next lost the first
// write while its star stayed filled; a star replaced by the favourites load
// (which the star asks for on every redraw) lost it the same way; and the load
// that did run installed the account's list over the star just tapped, so it
// emptied under the finger. Now jobs run in order, one load at a time is
// queued, and the stars still waiting are put back over a list that has just
// arrived. A job already running is always finished: aborting a request that
// writes to the account halfway through would leave its state unknown.
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
	// Tidal playlists are addressed by UUID, so the job carries its own copy
	// rather than a number: the caller can free its string as soon as submit()
	// returns, without waiting for the worker.
	char playlist_id[TIDAL_ID_MAX];
	bool want;
	char name[128];
	char album_id[TIDAL_ID_MAX];
	tidalsync_done_cb done;
	void *user;
	void (*changed)(void);
} job_t;

#define QUEUE_MAX 8

static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_ready = PTHREAD_COND_INITIALIZER;
static job_t queue[QUEUE_MAX];
static int queue_head;
static int queue_len;
static bool load_queued; // a favourites load is waiting or running: one is enough
static bool worker_started;

static void do_favorites_load(const job_t *job) {
	long ids[FAVORITES_MAX];
	int count = tidal_favorite_track_ids(ids, FAVORITES_MAX);
	if (count < 0) {
		printf("tidalsync: favorites not read: %s; next try in %d s\n", tidal_last_error(), LOAD_RETRY_SECS);
		pthread_mutex_lock(&lock);
		load_retry_after = monotonic_secs() + LOAD_RETRY_SECS;
		pthread_mutex_unlock(&lock);
		return;
	}

	// job_lock before lock, the one order both are ever held in.
	pthread_mutex_lock(&job_lock);
	pthread_mutex_lock(&lock);
	memcpy(favorites, ids, sizeof(long) * (size_t)count);
	favorites_count = count;
	favorites_loaded = true;
	// A star tapped while the list was on its way is still in the queue, not
	// on the account: the list just installed does not have it, and showing
	// that would flip the star back under the user's finger -- for good, since
	// a write that succeeds does not touch the mirror. The taps win until they
	// are sent.
	for (int i = 0; i < queue_len; i++) {
		const job_t *q = &queue[(queue_head + i) % QUEUE_MAX];
		if (q->kind == JOB_FAVORITE_TOGGLE) {
			mirror_set_locked(q->track_id, q->want);
		}
	}
	pthread_mutex_unlock(&lock);
	pthread_mutex_unlock(&job_lock);

	printf("tidalsync: %d favorite tracks\n", count);
	if (job->changed) {
		job->changed();
	}
}

static void do_favorite_toggle(const job_t *job) {
	bool ok = job->want ? tidal_favorite_add_track(job->track_id) : tidal_favorite_remove_track(job->track_id);
	if (!ok) {
		// The mirror was flipped before the request so the star could answer the
		// tap immediately; if the request fails it has to go back, or the player
		// and the account disagree.
		mirror_set(job->track_id, !job->want);
	}
	if (job->done) {
		job->done(ok, ok ? NULL : tidal_last_error(), job->user);
	}
}

static void do_playlists_load(const job_t *job) {
	tidal_playlist_t got[TIDALSYNC_PLAYLISTS_MAX];
	int count = tidal_user_playlists(0, got, TIDALSYNC_PLAYLISTS_MAX);
	if (count < 0) {
		printf("tidalsync: playlists not read: %s\n", tidal_last_error());
		return;
	}

	pthread_mutex_lock(&lock);
	memcpy(playlists, got, sizeof(tidal_playlist_t) * (size_t)count);
	playlists_count = count;
	playlists_loaded = true;
	pthread_mutex_unlock(&lock);

	if (job->changed) {
		job->changed();
	}
}

static void do_playlist_add(const job_t *job) {
	bool ok = tidal_playlist_add_track(job->playlist_id, job->track_id);
	if (job->done) {
		job->done(ok, ok ? NULL : tidal_last_error(), job->user);
	}
}

static void do_playlist_create(const job_t *job) {
	// The new playlist's UUID comes back here: without it there is nowhere to
	// put the track, since the id cannot be guessed.
	char id[TIDAL_ID_MAX] = {0};
	bool ok = tidal_playlist_create(job->name, id, sizeof(id));
	if (ok && job->track_id > 0) {
		ok = tidal_playlist_add_track(id, job->track_id);
	}
	if (ok) {
		// The cached list is one playlist out of date; force a reload so the
		// picker shows the new one next time it opens.
		pthread_mutex_lock(&lock);
		playlists_loaded = false;
		pthread_mutex_unlock(&lock);
	}
	if (job->done) {
		job->done(ok, ok ? NULL : tidal_last_error(), job->user);
	}
}

static void do_album_favorite(const job_t *job) {
	bool ok = job->want ? tidal_favorite_add_album(job->album_id) : tidal_favorite_remove_album(job->album_id);
	if (job->done) {
		job->done(ok, ok ? NULL : tidal_last_error(), job->user);
	}
}

static void do_playlist_remove(const job_t *job) {
	bool ok = tidal_playlist_delete(job->playlist_id);
	if (ok) {
		pthread_mutex_lock(&lock);
		playlists_loaded = false; // the cached list still holds the deleted one
		pthread_mutex_unlock(&lock);
	}
	if (job->done) {
		job->done(ok, ok ? NULL : tidal_last_error(), job->user);
	}
}

static void *worker_main(void *arg) {
	(void)arg;
	// Nice ten, not SCHED_IDLE. A star is a tap waiting for its answer, and
	// with Bluetooth encoding and a track decoding the core is 0 to 20 % idle:
	// an idle-class thread then gets well under one percent of it, the request
	// took minutes, and the star flipped back meanwhile. That is how Yandex's
	// likes failed in 1.2.3 (see yandexsync.c).
	thread_be_low_priority("tidal sync");

	for (;;) {
		pthread_mutex_lock(&job_lock);
		while (queue_len == 0) {
			pthread_cond_wait(&job_ready, &job_lock);
		}
		job_t job = queue[queue_head];
		queue_head = (queue_head + 1) % QUEUE_MAX;
		queue_len--;
		pthread_mutex_unlock(&job_lock);

		switch (job.kind) {
		case JOB_FAVORITES_LOAD:
			do_favorites_load(&job);
			// Only now: while a load runs, the redraws that ask for the
			// mirror must not queue a second one behind it.
			pthread_mutex_lock(&job_lock);
			load_queued = false;
			pthread_mutex_unlock(&job_lock);
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
	if (queue_len == QUEUE_MAX) {
		// Full: the oldest waiting job goes. Eight writes waiting means the
		// service has stopped answering, and the newest tap is the one that
		// matters.
		const job_t *dropped = &queue[queue_head];
		if (dropped->kind == JOB_FAVORITES_LOAD) {
			load_queued = false;
		} else if (dropped->kind == JOB_FAVORITE_TOGGLE) {
			// Never sent: the star goes back to what the account says --
			// unless a later tap on the same track is still waiting, which
			// will say it.
			bool later = false;
			for (int i = 1; i < queue_len && !later; i++) {
				const job_t *q = &queue[(queue_head + i) % QUEUE_MAX];
				later = q->kind == JOB_FAVORITE_TOGGLE && q->track_id == dropped->track_id;
			}
			if (!later) {
				mirror_set(dropped->track_id, !dropped->want);
			}
		}
		queue_head = (queue_head + 1) % QUEUE_MAX;
		queue_len--;
	}
	if (job->kind == JOB_FAVORITES_LOAD) {
		load_queued = true;
	}
	queue[(queue_head + queue_len) % QUEUE_MAX] = *job;
	queue_len++;
	pthread_cond_signal(&job_ready);
	pthread_mutex_unlock(&job_lock);
}

// ---------------------------------------------------------------------------
// public face
// ---------------------------------------------------------------------------

void tidalsync_favorites_refresh(void (*done)(void)) {
	if (!tidal_logged_in() || tidalsync_favorites_known()) {
		return;
	}
	// Asked on every redraw of the star: one load waiting is enough, and one
	// that failed is not asked for again at once.
	pthread_mutex_lock(&job_lock);
	bool waiting = load_queued;
	pthread_mutex_unlock(&job_lock);
	pthread_mutex_lock(&lock);
	bool resting = monotonic_secs() < load_retry_after;
	pthread_mutex_unlock(&lock);
	if (waiting || resting) {
		return;
	}
	job_t job = {.kind = JOB_FAVORITES_LOAD, .changed = done};
	submit(&job);
}

void tidalsync_favorite_toggle(long track_id, bool want, tidalsync_done_cb done, void *user) {
	if (track_id <= 0) {
		if (done) {
			done(false, tr("tidal_track_without_id"), user);
		}
		return;
	}

	// Mirror first, network after: the star fills under the finger instead of a
	// second and a half later.
	mirror_set(track_id, want);

	job_t job = {.kind = JOB_FAVORITE_TOGGLE, .track_id = track_id, .want = want, .done = done, .user = user};
	submit(&job);
}

void tidalsync_playlist_add(const char *playlist_id, long track_id, tidalsync_done_cb done, void *user) {
	if (!playlist_id || !playlist_id[0]) {
		if (done) {
			done(false, tr("tidal_playlist_without_id"), user);
		}
		return;
	}
	job_t job = {.kind = JOB_PLAYLIST_ADD, .track_id = track_id, .done = done, .user = user};
	snprintf(job.playlist_id, sizeof(job.playlist_id), "%s", playlist_id);
	submit(&job);
}

void tidalsync_playlist_create_with(const char *name, long track_id, tidalsync_done_cb done, void *user) {
	job_t job = {.kind = JOB_PLAYLIST_CREATE, .track_id = track_id, .done = done, .user = user};
	snprintf(job.name, sizeof(job.name), "%s", name ? name : "");
	submit(&job);
}

void tidalsync_album_favorite(const char *album_id, bool want, tidalsync_done_cb done, void *user) {
	if (!album_id || !album_id[0]) {
		if (done) {
			done(false, tr("tidal_album_without_id"), user);
		}
		return;
	}
	job_t job = {.kind = JOB_ALBUM_FAVORITE, .want = want, .done = done, .user = user};
	snprintf(job.album_id, sizeof(job.album_id), "%s", album_id);
	submit(&job);
}

void tidalsync_playlist_remove(const char *playlist_id, tidalsync_done_cb done, void *user) {
	if (!playlist_id || !playlist_id[0]) {
		if (done) {
			done(false, tr("tidal_playlist_without_id"), user);
		}
		return;
	}
	job_t job = {.kind = JOB_PLAYLIST_REMOVE, .done = done, .user = user};
	snprintf(job.playlist_id, sizeof(job.playlist_id), "%s", playlist_id);
	submit(&job);
}

void tidalsync_playlists_refresh(void (*done)(void)) {
	if (!tidal_logged_in()) {
		return;
	}
	job_t job = {.kind = JOB_PLAYLISTS_LOAD, .changed = done};
	submit(&job);
}

bool tidalsync_playlists_known(void) {
	pthread_mutex_lock(&lock);
	bool known = playlists_loaded;
	pthread_mutex_unlock(&lock);
	return known;
}

int tidalsync_playlists(tidal_playlist_t *out, int max) {
	if (!out || max <= 0) {
		return 0;
	}
	pthread_mutex_lock(&lock);
	int count = playlists_count < max ? playlists_count : max;
	memcpy(out, playlists, sizeof(tidal_playlist_t) * (size_t)count);
	pthread_mutex_unlock(&lock);
	return count;
}

// ---------------------------------------------------------------------------
// the service, as the rest of the program sees it (provider.h)
//
// Thin adapters: the provider speaks in paths and text ids, Tidal in numeric
// track ids and playlist UUIDs. The path -> id step is tidalcache_track_id(),
// pure string work, so doing it on every call costs nothing.
// ---------------------------------------------------------------------------

static bool provider_downloading(void) { return tidalcache_downloading_id() > 0; }

static bool provider_is_track(const char *path) { return tidalcache_track_id(path) > 0; }

static bool provider_fav_get(const char *path) { return tidalsync_is_favorite(tidalcache_track_id(path)); }

static void provider_fav_toggle(const char *path, bool want, stream_provider_done_cb done, void *user) {
	tidalsync_favorite_toggle(tidalcache_track_id(path), want, done, user);
}

static int provider_playlists(stream_provider_playlist_t *out, int max) {
	if (!out || max <= 0) {
		return 0;
	}
	if (max > TIDALSYNC_PLAYLISTS_MAX) {
		max = TIDALSYNC_PLAYLISTS_MAX;
	}
	// Borrowed for the copy: the service's own records are large (cover URLs)
	// and the picker only needs three fields of each.
	tidal_playlist_t *got = malloc(sizeof(*got) * (size_t)max);
	if (!got) {
		return 0;
	}
	int count = tidalsync_playlists(got, max);
	for (int i = 0; i < count; i++) {
		snprintf(out[i].id, sizeof(out[i].id), "%s", got[i].id);
		snprintf(out[i].name, sizeof(out[i].name), "%s", got[i].name);
		out[i].track_count = got[i].track_count;
	}
	free(got);
	return count;
}

static void provider_playlist_add(const char *playlist_id, const char *path, stream_provider_done_cb done, void *user) {
	tidalsync_playlist_add(playlist_id, tidalcache_track_id(path), done, user);
}

static void provider_playlist_create_with(const char *name, const char *path, stream_provider_done_cb done,
										  void *user) {
	tidalsync_playlist_create_with(name, tidalcache_track_id(path), done, user);
}

static stream_provider_t provider = {
	.id = "tidal",
	.set_root = tidalcache_set_root,
	.cache_dir = tidalcache_dir,
	.owns_path = tidalcache_owns,
	.downloading = provider_downloading,
	.network_wanted = tidalcache_network_wanted,
	.clear_on_exit = tidalcache_clear_on_exit,
	.is_track = provider_is_track,
	.fav_get = provider_fav_get,
	.fav_refresh = tidalsync_favorites_refresh,
	.fav_toggle = provider_fav_toggle,
	.fav_failed_key = "tidal_favourites_failed",
	.album_id = tidalcache_album_id,
	.album_failed_key = "trackmenu_tidal_album_failed",
	.playlists_known = tidalsync_playlists_known,
	.playlists_refresh = tidalsync_playlists_refresh,
	.playlists = provider_playlists,
	.playlist_add = provider_playlist_add,
	.playlist_create_with = provider_playlist_create_with,
	.playlist_title_key = "playlist_tidal_playlist",
	.change_refused_key = "tidal_change_refused",
};

void tidalsync_register_provider(void) { stream_provider_register(&provider); }

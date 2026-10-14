#include "yandexplay.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "src/gui/shell/gui.h"
#include "src/system/core/lang.h"
#include "src/system/decode/growfile.h"
#include "src/system/net/http.h"
#include "src/system/playback/device_state.h"
#include "src/system/streaming/streamerr.h"
#include "src/system/streaming/streamturn.h"
#include "src/system/streaming/yandexcache.h"
#include "src/system/streaming/yandexsaved.h"
#include "src/system/streaming/yandexwave.h"

// See yandexplay.h.

static yandexplay_listener_t listener;

static void tell(yandexplay_event_t event, const char *text) {
	if (listener) {
		listener(event, text ? text : "");
	}
}

// ---------------------------------------------------------------------------
// the queue
//
// `queue` is every Yandex track the player's queue may reach: the list played
// and what menus queued since. Set on the GUI thread; the worker and the
// prepare callback (any thread) take a reference under `queue_lock` and work
// on that, so a new list started meanwhile frees nothing under them.
// ---------------------------------------------------------------------------

static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static yandexlist_t *queue;
static bool queue_is_wave;
// Raised by every list played: a download asked for one queue is not
// started into the next.
static unsigned queue_generation;

static yandexlist_t *queue_get(void) {
	pthread_mutex_lock(&queue_lock);
	yandexlist_t *q = yandexlist_ref(queue);
	pthread_mutex_unlock(&queue_lock);
	return q;
}

// Replaces the queue (GUI thread). Takes over `list`'s reference.
static void queue_set(yandexlist_t *list, bool wave) {
	pthread_mutex_lock(&queue_lock);
	yandexlist_t *old = queue;
	queue = list;
	queue_is_wave = wave;
	queue_generation++;
	pthread_mutex_unlock(&queue_lock);
	yandexlist_free(old);
}

// The track behind a path: a cache file is named after it, a saved file is
// looked up in the index.
static void path_track_id(const char *path, char *id, size_t size) {
	id[0] = '\0';
	if (!path || !path[0]) {
		return;
	}
	yandexcache_track_id(path, id, size);
	if (!id[0]) {
		yandexsaved_track_for_path(path, id, size, NULL, 0);
	}
}

// The file a track plays from: the saved one when the index has it and it can
// be read; else the cache's -- under its real name when it is complete and
// `look` (a stat per extension, so only for the few that matter now), a
// guess at ".flac" otherwise, put right when it is fetched
// (playlist_replace_path). True for a saved file.
static bool track_path_known(const char *id, bool look, bool any_saved, char *out, size_t size) {
	if (any_saved && yandexsaved_has(id) && yandexsaved_local(id, out, size)) {
		return true;
	}
	if (!look || !yandexcache_find(id, out, size)) {
		yandexcache_path(id, "flac", out, size);
	}
	return false;
}

static bool track_path(const char *id, bool look, char *out, size_t size) { return track_path_known(id, look, yandexsaved_count() > 0, out, size); }

static uint32_t id_hash(const char *s) {
	uint32_t h = 2166136261u;
	for (; *s; s++) {
		h = (h ^ (uint8_t)*s) * 16777619u;
	}
	return h;
}

// Whether the player's queue is this one. Something else playing -- the
// Library, another service -- means the saved files go back to being the
// library's. Ours when it holds the very tracks queued here, in any order
// (shuffle deals its own): the sums of the ids' hashes are compared. That is
// a walk over the whole queue, so the verdict is kept until the queue changes
// (playlist_revision).
static bool ours_valid, ours;
static unsigned ours_revision;

static bool queue_is_ours(void) {
	unsigned revision = playlist_revision();
	if (ours_valid && revision == ours_revision) {
		return ours;
	}
	ours_valid = true;
	ours_revision = revision;
	ours = false;
	yandexlist_t *q = queue_get();
	int count = playlist_count();
	if (q && !playlist_is_library_backed() && count == yandexlist_count(q)) {
		uint32_t theirs = 0, mine = 0;
		char path[512], id[YANDEX_ID_MAX];
		bool all = true;
		for (int i = 0; i < count && all; i++) {
			all = playlist_path_at(i, path, sizeof(path));
			path_track_id(path, id, sizeof(id));
			theirs += id_hash(id);
			if (yandexlist_ids(q, i, id, sizeof(id), NULL, 0)) {
				mine += id_hash(id);
			}
		}
		ours = all && theirs == mine;
	}
	yandexlist_free(q);
	return ours;
}

// After a change made here to a queue that was ours: still ours.
static void note_own_change(bool was_ours) {
	ours_valid = was_ours;
	ours = was_ours;
	ours_revision = playlist_revision();
}

// ---------------------------------------------------------------------------
// saved tracks in the queue
//
// The saved files the queue holds are claimed (yandexsaved_claim), so that
// while this queue plays them they are Yandex tracks to the rest of the
// player: the like goes to the account, the badge shows. Only while the queue
// is ours: the watch lets go as soon as something else is playing.
// ---------------------------------------------------------------------------

static bool claims_live;

static void claim_saved(void) {
	if (yandexsaved_count() == 0) {
		// Nothing saved at all: nothing to look up, track by track.
		yandexsaved_claim(NULL, 0);
		claims_live = false;
		return;
	}
	yandexlist_t *q = queue_get();
	int count = yandexlist_count(q);
	char(*paths)[512] = NULL;
	const char **ptrs = NULL;
	int n = 0;
	for (int i = 0; i < count; i++) {
		char id[YANDEX_ID_MAX], path[512];
		if (!yandexlist_ids(q, i, id, sizeof(id), NULL, 0) || !yandexsaved_has(id) || !yandexsaved_local(id, path, sizeof(path))) {
			continue;
		}
		char(*grown)[512] = realloc(paths, (size_t)(n + 1) * sizeof(*paths));
		if (!grown) {
			break;
		}
		paths = grown;
		snprintf(paths[n++], sizeof(paths[0]), "%s", path);
	}
	ptrs = malloc((size_t)(n > 0 ? n : 1) * sizeof(*ptrs));
	if (ptrs) {
		for (int i = 0; i < n; i++) {
			ptrs[i] = paths[i];
		}
		yandexsaved_claim(ptrs, n);
		claims_live = n > 0;
	}
	free(ptrs);
	free(paths);
	yandexlist_free(q);
}

// ---------------------------------------------------------------------------
// the worker: the tapped track, and the track playback needs
//
// Two places, newest wins in each: a tap replaces a tap still waiting (the
// user changed their mind), a fetch replaces a fetch (playback moved on).
// Neither replaces the other.
// ---------------------------------------------------------------------------

typedef enum { JOB_NONE = 0, JOB_PLAY, JOB_FETCH } job_kind_t;

typedef struct {
	job_kind_t kind;
	char id[YANDEX_ID_MAX];
	unsigned generation;
	bool waiting; // playback stopped for this one (prepare), not ahead of it
} job_t;

static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_cond = PTHREAD_COND_INITIALIZER;
static job_t play_pending, fetch_pending;
static job_kind_t running_kind;
static char running_id[YANDEX_ID_MAX];
static bool worker_started;

typedef struct {
	job_kind_t kind;
	unsigned generation;
	bool ok;
	bool waiting;
	char id[YANDEX_ID_MAX];
	char path[512];
	char error[256];
} job_result_t;

static void play_done_async(void *user);
static void fetch_done_async(void *user);

// The details of track `id` for the cache's tags and sidecars: from the
// queue, its page fetched when they are not there; just the id when even
// that fails -- the file still plays.
static void track_details(yandexlist_t *q, const char *id, yandex_track_t *out) {
	int index = yandexlist_find(q, id);
	if (index >= 0 && !yandexlist_track(q, index, out)) {
		yandexlist_fetch_page(q, index / YANDEXLIST_PAGE);
	}
	if (index < 0 || !yandexlist_track(q, index, out)) {
		memset(out, 0, sizeof(*out));
		snprintf(out->id, sizeof(out->id), "%s", id);
		if (index >= 0) {
			yandexlist_ids(q, index, out->id, sizeof(out->id), out->album_id, sizeof(out->album_id));
		}
		out->available = true;
	}
}

// Starts the download of track `t` and returns its real path. The codec, and
// so the extension, is only known now.
static bool fetch_track(const yandex_track_t *t, char *path, size_t size, char *error, size_t error_size) {
	if (yandexcache_find(t->id, path, size)) {
		return true; // prefetched under its real name: no need to ask again
	}
	yandex_file_t file;
	if (!yandex_file_info(t->id, &file)) {
		streamerr_describe("yandex", yandex_last_error(), error, error_size);
		return false;
	}
	if (!yandexcache_start(t, &file, path, size)) {
		streamerr_describe("yandex", yandexcache_last_error(), error, error_size);
		return false;
	}
	yandexcache_write_sidecars(t, path);
	// Counts towards the next sweep and is spared by it until the queue knows.
	yandexcache_note_played(path);
	return true;
}

// Whatever is coming down gives way to what is wanted now -- unless it is
// that very track (the prefetch got there first): abandoning it would only
// restart it from zero.
static void abandon_unless(const char *id) {
	char busy[YANDEX_ID_MAX];
	yandexcache_downloading_id(busy, sizeof(busy));
	if (strcmp(busy, id) != 0) {
		streamturn_abandon_all();
	}
}

static void *worker_main(void *arg) {
	(void)arg;
	// A tapped track is waited on: a dead network is said in seconds.
	http_set_thread_timeout_cap(STREAM_UI_TIMEOUT_SECS);
	for (;;) {
		pthread_mutex_lock(&job_lock);
		running_kind = JOB_NONE;
		running_id[0] = '\0';
		while (play_pending.kind == JOB_NONE && fetch_pending.kind == JOB_NONE) {
			pthread_cond_wait(&job_cond, &job_lock);
		}
		job_t job;
		if (play_pending.kind != JOB_NONE) {
			job = play_pending;
			play_pending.kind = JOB_NONE;
		} else {
			job = fetch_pending;
			fetch_pending.kind = JOB_NONE;
		}
		running_kind = job.kind;
		snprintf(running_id, sizeof(running_id), "%s", job.id);
		pthread_mutex_unlock(&job_lock);

		streamerr_begin();
		job_result_t *r = calloc(1, sizeof(*r));
		if (!r) {
			continue;
		}
		r->kind = job.kind;
		r->generation = job.generation;
		r->waiting = job.waiting;
		snprintf(r->id, sizeof(r->id), "%s", job.id);
		yandexlist_t *q = queue_get();
		yandex_track_t t;
		track_details(q, job.id, &t);
		yandexlist_free(q);
		if (job.kind == JOB_PLAY || job.waiting) {
			abandon_unless(job.id);
		}
		r->ok = fetch_track(&t, r->path, sizeof(r->path), r->error, sizeof(r->error));
		if (!r->ok) {
			fprintf(stderr, "yandex: fetching %s failed: %s\n", job.id, r->error);
		}
		if (!gui_post(job.kind == JOB_PLAY ? play_done_async : fetch_done_async, r)) {
			free(r);
		}
	}
	return NULL;
}

static void submit(job_kind_t kind, const char *id, bool waiting) {
	pthread_mutex_lock(&job_lock);
	if (!worker_started) {
		pthread_t th;
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		pthread_attr_setstacksize(&attr, 256 * 1024);
		worker_started = pthread_create(&th, &attr, worker_main, NULL) == 0;
		pthread_attr_destroy(&attr);
	}
	job_t job = {.kind = kind, .waiting = waiting};
	snprintf(job.id, sizeof(job.id), "%s", id);
	pthread_mutex_lock(&queue_lock);
	job.generation = queue_generation;
	pthread_mutex_unlock(&queue_lock);
	if (kind == JOB_PLAY) {
		play_pending = job;
	} else {
		fetch_pending = job;
	}
	pthread_cond_signal(&job_cond);
	pthread_mutex_unlock(&job_lock);
}

// True while the worker has a track to ask for, or is asking.
static bool worker_fetching(void) {
	pthread_mutex_lock(&job_lock);
	bool busy = play_pending.kind != JOB_NONE || fetch_pending.kind != JOB_NONE || running_kind != JOB_NONE;
	pthread_mutex_unlock(&job_lock);
	return busy;
}

// ---------------------------------------------------------------------------
// starting a list
// ---------------------------------------------------------------------------

// The list waiting for its tapped track (GUI thread): it becomes the queue
// when the track is here.
static int start_index;
static yandexwave_batch_cb wave_cb;

// Hands the whole list to the player, entry `start` playing from `start_path`.
static void play_queue_from(int start, const char *start_path) {
	yandexlist_t *q = queue_get();
	int count = yandexlist_count(q);
	// The names go into one block, each as long as it is (some fifty bytes
	// a track): five thousand of them at 512 bytes apiece would be 2.5 MB at
	// once, on a device with ten free.
	size_t used = 0, room = (size_t)(count > 0 ? count : 1) * 56;
	char *names = malloc(room);
	size_t *offsets = malloc((size_t)(count > 0 ? count : 1) * sizeof(*offsets));
	const char **ptrs = malloc((size_t)(count > 0 ? count : 1) * sizeof(*ptrs));
	bool ok = names && offsets && ptrs && count > 0;
	bool any_saved = yandexsaved_count() > 0;
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	// Exact names for the first few from the start (they will be wanted in
	// minutes), guesses for the rest: thousands of stats on the card would be
	// a pause before the first note.
	for (int i = 0; ok && i < count; i++) {
		char id[YANDEX_ID_MAX], path[512];
		yandexlist_ids(q, i, id, sizeof(id), NULL, 0);
		if (i == start && start_path && start_path[0]) {
			snprintf(path, sizeof(path), "%s", start_path);
		} else {
			track_path_known(id, i >= start && i <= start + 2, any_saved, path, sizeof(path));
		}
		size_t len = strlen(path) + 1;
		if (used + len > room) {
			char *grown = realloc(names, room * 2 + len);
			ok = grown != NULL;
			names = ok ? grown : names;
			room = room * 2 + len;
		}
		if (ok) {
			memcpy(names + used, path, len);
			offsets[i] = used;
			used += len;
		}
	}
	if (!ok) {
		free(names);
		free(offsets);
		free(ptrs);
		yandexlist_free(q);
		return;
	}
	for (int i = 0; i < count; i++) {
		ptrs[i] = names + offsets[i];
	}
	device_state_play_list(ptrs, count, start);
	free(ptrs);
	free(offsets);
	free(names);
	note_own_change(true);
	claim_saved();
	if (queue_is_wave) {
		yandex_track_t *first = calloc((size_t)(count > 0 ? count : 1), sizeof(*first));
		int n = 0;
		for (int i = 0; first && i < count; i++) {
			if (yandexlist_track(q, i, &first[n])) {
				n++;
			}
		}
		yandexwave_begin(first, n, wave_cb, NULL);
		free(first);
	} else {
		yandexwave_end();
	}
	yandexlist_free(q);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
	printf("yandex: queue of %d tracks from %d (%s)%s, built in %ld ms\n", count, start, start_path ? start_path : "", queue_is_wave ? ", My Wave" : "", ms);
}

static void fetch_ahead(void);

static void play_done_async(void *user) {
	job_result_t *r = user;
	pthread_mutex_lock(&queue_lock);
	bool current = r->generation == queue_generation;
	pthread_mutex_unlock(&queue_lock);
	if (current) {
		if (r->ok) {
			play_queue_from(start_index, r->path);
			tell(YANDEXPLAY_STARTED, NULL);
			fetch_ahead();
		} else {
			tell(YANDEXPLAY_FAILED, r->error[0] ? r->error : tr("download_failed"));
		}
	}
	free(r);
}

bool yandexplay_list(yandexlist_t *list, int start, bool wave) {
	int count = yandexlist_count(list);
	if (count == 0 || start < 0 || start >= count) {
		return false;
	}
	yandexlist_t *copy = yandexlist_clone(list);
	if (!copy) {
		return false;
	}
	queue_set(copy, wave);
	start_index = start;
	char id[YANDEX_ID_MAX], path[512];
	yandexlist_ids(copy, start, id, sizeof(id), NULL, 0);
	if (track_path(id, true, path, sizeof(path)) || access(path, F_OK) == 0) {
		// On the card (saved, or still in the cache): it plays now, with or
		// without a network.
		play_queue_from(start, path);
		tell(YANDEXPLAY_STARTED, NULL);
		fetch_ahead();
		return true;
	}
	yandexcache_set_network_wanted(true);
	submit(JOB_PLAY, id, false);
	return true;
}

// ---------------------------------------------------------------------------
// fetched tracks
// ---------------------------------------------------------------------------

// How many tracks in a row playback had to skip, because they could not be
// fetched. Past a few the network is down, not the tracks: playback stops
// instead of running through the whole queue in seconds.
#define SKIPS_MAX 3
static int skips_in_a_row;

// The entry playback is on, as a track id.
static void current_id(char *id, size_t size) {
	char path[512];
	id[0] = '\0';
	if (playlist_current_path(path, sizeof(path))) {
		path_track_id(path, id, size);
	}
}

// A track has arrived: its guessed name is put right in the queue, in place
// (the order, shuffle and what was queued by hand stay), and if playback was
// stopped waiting for it, it starts.
static void fetch_done_async(void *user) {
	job_result_t *r = user;
	char guess[512], now[YANDEX_ID_MAX];
	current_id(now, sizeof(now));
	bool waited_for = strcmp(now, r->id) == 0;
	device_state_t state;
	device_state_get(&state);
	if (r->ok) {
		bool was_ours = queue_is_ours();
		yandexcache_path(r->id, "flac", guess, sizeof(guess));
		if (playlist_replace_path(guess, r->path) > 0) {
			note_own_change(was_ours);
		}
		if (waited_for && state.status != AUDIO_STATUS_PLAYING) {
			skips_in_a_row = 0;
			device_state_play_queue_index(playlist_current_index());
		}
	} else if (waited_for && state.status != AUDIO_STATUS_PLAYING && r->waiting) {
		// The track playback stopped for cannot be had (gone from the
		// catalogue, refused): on to the next, as a player does -- 1.2.5 sat
		// stopped on it. A few in a row and it stops, saying why.
		if (++skips_in_a_row <= SKIPS_MAX && !playlist_at_end()) {
			tell(YANDEXPLAY_SKIPPED, r->error);
			char next[512];
			device_state_next(next, sizeof(next));
		} else {
			skips_in_a_row = 0;
			tell(YANDEXPLAY_FAILED, r->error);
		}
	}
	free(r);
}

// Asks for the track after the playing one (in playback order) ahead of
// time, unless it is on the card, or anything at all is coming down: the
// cache downloads one track at a time and starting another abandons the one
// in flight -- which, right after a track starts, is the very file being
// played. The watch asks again every second, so the next track follows as
// soon as this one is complete. A track already in the cache under another
// name than the queue's guess gets its name at once.
static void fetch_ahead(void) {
	int current = playlist_current_index();
	char path[512], id[YANDEX_ID_MAX], real[512];
	if (current < 0 || !playlist_path_at(current + 1, path, sizeof(path)) || !yandexcache_owns(path)) {
		return; // nothing after it, or saved on the card, or not ours
	}
	if (access(path, F_OK) == 0 || growfile_is_growing(path)) {
		return;
	}
	path_track_id(path, id, sizeof(id));
	if (!id[0]) {
		return;
	}
	if (yandexcache_find(id, real, sizeof(real))) {
		bool was_ours = queue_is_ours();
		if (playlist_replace_path(path, real) > 0) {
			note_own_change(was_ours);
		}
		return;
	}
	if (yandexcache_busy() || worker_fetching()) {
		return;
	}
	yandexlist_t *q = queue_get();
	bool known = yandexlist_find(q, id) >= 0;
	yandexlist_free(q);
	if (!known) {
		return; // another queue's entry, or one the player kept over a restart: fetched when reached
	}
	yandexcache_set_network_wanted(true);
	submit(JOB_FETCH, id, false);
}

// ---------------------------------------------------------------------------
// the prepare callback
// ---------------------------------------------------------------------------

// The "downloading" notice goes through gui_post: device_state calls the
// prepare callback from whatever thread starts the track.
static void downloading_note_async(void *user) {
	(void)user;
	gui_notify_popup("downloading");
}

// A queue entry whose file is not what the queue thought: a saved file that
// has gone (the track streams again), or a track saved since the queue was
// made (it plays from the card instead of being fetched). The entry gets its
// name anew and playback restarts there; for a track that must be fetched,
// starting it lands in the prepare callback again, which fetches it.
static void requeue_async(void *user) {
	char *path = user;
	char id[YANDEX_ID_MAX], fresh[512];
	path_track_id(path, id, sizeof(id));
	if (!id[0]) {
		// A saved file whose entry has just been dropped: the id is only in
		// the queue now.
		yandexsaved_track_for_path(path, id, sizeof(id), NULL, 0);
	}
	if (id[0]) {
		bool was_ours = queue_is_ours();
		track_path(id, true, fresh, sizeof(fresh));
		if (playlist_replace_path(path, fresh) > 0) {
			note_own_change(was_ours);
		}
		claim_saved();
		device_state_play_queue_index(playlist_current_index());
	}
	free(path);
}

// Makes a track known to the queue by id, for one that playback reached
// without the queue having it -- a queue kept over a restart, whose cache was
// emptied at shutdown. Any thread.
static void adopt_id(const char *id) {
	pthread_mutex_lock(&queue_lock);
	if (!queue) {
		queue = yandexlist_new();
	}
	yandexlist_t *q = yandexlist_ref(queue);
	pthread_mutex_unlock(&queue_lock);
	if (q && yandexlist_find(q, id) < 0) {
		yandexlist_add_id(q, id, NULL);
	}
	yandexlist_free(q);
}

bool yandexplay_prepare_track(const char *path) {
	if (!yandexcache_owns(path)) {
		// A saved file starting from another queue (the Library's album) is
		// the library's from its first frame: the claim goes before the
		// player draws the badge and the star. The watch tidies up after.
		if (yandexsaved_claimed(path) && !queue_is_ours()) {
			yandexsaved_claim(NULL, 0);
			return true;
		}
		// One of the queue's saved files, gone since: the track streams.
		if (yandexsaved_claimed(path) && access(path, R_OK) != 0) {
			char *copy = strdup(path);
			if (copy) {
				yandexcache_set_network_wanted(true);
				device_state_stop();
				gui_post(requeue_async, copy);
				return false;
			}
		}
		return true;
	}
	// Downloaded (an MP4 only appears under its name once complete), or
	// arriving right now -- the decoder reads a growing file.
	if (growfile_is_growing(path) || access(path, F_OK) == 0) {
		return true;
	}
	char id[YANDEX_ID_MAX];
	yandexcache_track_id(path, id, sizeof(id));
	if (!id[0]) {
		return true; // not a track: let the decoder say what it thinks
	}
	// Saved on the card since the queue was made: played from there.
	if (yandexsaved_has(id) && yandexsaved_local(id, NULL, 0)) {
		char *copy = strdup(path);
		if (copy) {
			device_state_stop();
			gui_post(requeue_async, copy);
			return false;
		}
	}
	// Any Yandex track reached is fetched, the queue's or not: a queue the
	// player kept over a restart names its files in a cache that was emptied
	// at shutdown, and 1.2.5 stopped on every one of them.
	adopt_id(id);
	// The network is needed now, before the stop: see
	// yandexcache_network_wanted().
	yandexcache_set_network_wanted(true);
	device_state_stop();
	submit(JOB_FETCH, id, true);
	gui_post(downloading_note_async, NULL);
	return false;
}

// ---------------------------------------------------------------------------
// My Wave: the station's next batch, appended
//
// yandexwave.c asks for it -- when two tracks or fewer are left, whatever
// page is up -- and hands it over on its worker; it lands here on the GUI
// thread and goes to the end of the queue, guessed names first, fetched as
// playback nears them. A queue that had already run out by then (the station
// could not be reached for a while) carries on with the first new track.
// ---------------------------------------------------------------------------

typedef struct {
	unsigned generation;
	int count;
	yandex_track_t tracks[];
} wave_batch_t;

static void wave_batch_async(void *user) {
	wave_batch_t *b = user;
	int added = 0;
	pthread_mutex_lock(&queue_lock);
	bool current = b->generation == queue_generation && queue_is_wave;
	pthread_mutex_unlock(&queue_lock);
	bool was_ours = current && queue_is_ours();
	if (was_ours) {
		device_state_t state;
		device_state_get(&state);
		int count_before = playlist_count();
		// Run out: stopped on the last entry, which is a file that played --
		// not one waiting for its download.
		bool ran_out = state.status == AUDIO_STATUS_STOPPED && count_before > 0 && playlist_current_index() == count_before - 1;
		ran_out = ran_out && access(state.current_file, F_OK) == 0 && !worker_fetching();
		yandexlist_t *q = queue_get();
		for (int i = 0; i < b->count; i++) {
			char path[512];
			track_path(b->tracks[i].id, false, path, sizeof(path));
			if (!yandexlist_add_track(q, &b->tracks[i]) || !playlist_insert_at(path, playlist_count())) {
				break;
			}
			added++;
		}
		yandexlist_free(q);
		note_own_change(true);
		if (added > 0) {
			claim_saved();
			device_state_queue_changed();
			if (ran_out) {
				printf("yandex: wave: the queue had run out, going on with the new batch\n");
				device_state_play_queue_index(count_before);
			}
		}
	}
	yandexwave_batch_done(added);
	free(b);
}

// On the wave's worker: copied and bounced to the GUI thread.
static void wave_batch_cb(const yandex_track_t *tracks, int count, bool restarted, void *user) {
	(void)user;
	(void)restarted;
	wave_batch_t *b = malloc(sizeof(*b) + (size_t)count * sizeof(b->tracks[0]));
	if (b) {
		pthread_mutex_lock(&queue_lock);
		b->generation = queue_generation;
		pthread_mutex_unlock(&queue_lock);
		b->count = count;
		memcpy(b->tracks, tracks, (size_t)count * sizeof(b->tracks[0]));
		if (gui_post(wave_batch_async, b)) {
			return;
		}
		free(b);
	}
	yandexwave_batch_done(0); // lost: asked for again
}

// ---------------------------------------------------------------------------
// the rest of the calls
// ---------------------------------------------------------------------------

void yandexplay_init(yandexplay_listener_t l) {
	listener = l;
	wave_cb = wave_batch_cb;
	device_state_add_prepare_cb(yandexplay_prepare_track);
}

int yandexplay_paths_for(const yandex_track_t *tracks, int count, char ***paths_out) {
	*paths_out = NULL;
	if (count <= 0) {
		return 0;
	}
	char **paths = malloc((size_t)count * (sizeof(char *) + 512));
	if (!paths) {
		return 0;
	}
	char *names = (char *)(paths + count);
	bool was_ours = queue_is_ours();
	pthread_mutex_lock(&queue_lock);
	if (!queue) {
		queue = yandexlist_new();
	}
	yandexlist_t *q = yandexlist_ref(queue);
	pthread_mutex_unlock(&queue_lock);
	int n = 0;
	for (int i = 0; q && i < count; i++) {
		if (!tracks[i].id[0] || !yandexlist_add_track(q, &tracks[i])) {
			continue;
		}
		paths[n] = names + (size_t)n * 512;
		track_path(tracks[i].id, true, paths[n], 512);
		n++;
	}
	yandexlist_free(q);
	// The queue grows by these as the player's does: still ours if it was.
	// (Checked again on the next change of the player's queue.)
	ours_valid = false;
	(void)was_ours;
	*paths_out = paths;
	return n;
}

bool yandexplay_current(yandex_track_t *out) {
	char id[YANDEX_ID_MAX];
	current_id(id, sizeof(id));
	yandexlist_t *q = queue_get();
	int index = id[0] ? yandexlist_find(q, id) : -1;
	bool ok = index >= 0;
	if (ok && !yandexlist_track(q, index, out)) {
		memset(out, 0, sizeof(*out));
		yandexlist_ids(q, index, out->id, sizeof(out->id), out->album_id, sizeof(out->album_id));
		out->available = true;
	}
	yandexlist_free(q);
	return ok;
}

bool yandexplay_is_wave(void) {
	pthread_mutex_lock(&queue_lock);
	bool wave = queue_is_wave;
	pthread_mutex_unlock(&queue_lock);
	return wave;
}

void yandexplay_signed_out(void) {
	yandexwave_end();
	pthread_mutex_lock(&queue_lock);
	queue_is_wave = false;
	pthread_mutex_unlock(&queue_lock);
	yandexsaved_claim(NULL, 0);
	claims_live = false;
}

// What the wave watch is told every second: the wave track loaded now, and
// how many are queued after it.
//
// "Ours" is too strict a test here: it holds while the player's queue is
// exactly the wave's list, and a track sent next or to the end from the wave's
// own rows, or an Undo, changes the queue without changing the list. The wave
// then took the playing track as left ("skip" to Yandex) and stopped asking for
// more, so My Wave ran dry after a single "Play next". What matters to the wave
// is narrower: the queue is not a library one, and what plays is one of its
// tracks. A library album started over it fails the first, anything else the
// second, and the wave hears that the track was left, as before.
static void wave_watch(bool ours_now) {
	if (!yandexplay_is_wave()) {
		return;
	}
	yandex_track_t t;
	if ((!ours_now && playlist_is_library_backed()) || !yandexplay_current(&t)) {
		yandexwave_watch(NULL, false, 0);
		return;
	}
	device_state_t state;
	device_state_get(&state);
	int count = playlist_count();
	int current = playlist_current_index();
	yandexwave_watch(&t, state.status == AUDIO_STATUS_PLAYING, current >= 0 ? count - current - 1 : 0);
}

void yandexplay_watch(void) {
	pthread_mutex_lock(&queue_lock);
	bool any = queue != NULL;
	pthread_mutex_unlock(&queue_lock);
	if (!any) {
		return; // nothing Yandex ever played: one comparison and out
	}
	bool library_queue = playlist_is_library_backed();
	bool ours_now = queue_is_ours();

	// Something else plays now: the saved files go back to the library.
	if (claims_live && !ours_now) {
		yandexsaved_claim(NULL, 0);
		claims_live = false;
	}

	// The queued files -- the one playing and those after it -- are kept
	// from the cache's sweep and ceiling.
	static char keep[YANDEXCACHE_PROTECTED_MAX][512];
	static const char *keep_ptr[YANDEXCACHE_PROTECTED_MAX];
	int protect = 0;
	int count = playlist_count();
	for (int i = playlist_current_index(); !library_queue && i >= 0 && i < count && protect < YANDEXCACHE_PROTECTED_MAX; i++) {
		if (playlist_path_at(i, keep[protect], sizeof(keep[0])) && yandexcache_owns(keep[protect])) {
			keep_ptr[protect] = keep[protect];
			protect++;
		}
	}
	yandexcache_set_protected(keep_ptr, protect);

	fetch_ahead();
	// The only place that lowers the flag (those that raise it do so at once).
	yandexcache_set_network_wanted(yandexcache_busy() || worker_fetching() || yandexwave_requesting());
	wave_watch(ours_now);
}

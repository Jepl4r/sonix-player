#ifdef SONIX_ZVUK

#include "zvukplay.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lvgl/lvgl.h"

#include "src/gui/shell/gui.h"
#include "src/system/core/lang.h"
#include "src/system/decode/growfile.h"
#include "src/system/net/http.h"
#include "src/system/playback/device_state.h"
#include "src/system/streaming/streamerr.h"
#include "src/system/streaming/streamturn.h"
#include "src/system/streaming/zvukcache.h"
#include "src/system/streaming/zvukdl.h"

// See zvukplay.h. The shape of yandexplay.c, and its comments hold here; what
// differs is said where it differs.

static zvukplay_listener_t listener;

static void tell(zvukplay_event_t event, const char *text) {
	if (listener) {
		listener(event, text ? text : "");
	}
}

// ---------------------------------------------------------------------------
// the queue
// ---------------------------------------------------------------------------

static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static zvuklist_t *queue;
static unsigned queue_generation;

static zvuklist_t *queue_get(void) {
	pthread_mutex_lock(&queue_lock);
	zvuklist_t *q = zvuklist_ref(queue);
	pthread_mutex_unlock(&queue_lock);
	return q;
}

static void queue_set(zvuklist_t *list) {
	pthread_mutex_lock(&queue_lock);
	zvuklist_t *old = queue;
	queue = list;
	queue_generation++;
	pthread_mutex_unlock(&queue_lock);
	zvuklist_free(old);
}

static void path_track_id(const char *path, char *id, size_t size) {
	id[0] = '\0';
	if (!path || !path[0]) {
		return;
	}
	zvukcache_track_id(path, id, size);
	if (!id[0]) {
		zvukdl_track_for_path(path, id, size, NULL, 0);
	}
}

void zvukplay_playing_id(char *out, size_t size) {
	char path[512];
	out[0] = '\0';
	if (playlist_current_path(path, sizeof(path)) && (zvukcache_owns(path) || zvukdl_claimed(path))) {
		path_track_id(path, out, size);
	}
}

// The file a track plays from: saved, or the cache's (its real name when
// complete and `look`, the ".flac" guess otherwise). True for a saved file.
static bool track_path_known(const char *id, bool look, bool any_saved, char *out, size_t size) {
	if (any_saved && zvukdl_saved(id) && zvukdl_local(id, out, size)) {
		return true;
	}
	if (!look || !zvukcache_find(id, out, size)) {
		zvukcache_path(id, "flac", out, size);
	}
	return false;
}

static bool track_path(const char *id, bool look, char *out, size_t size) {
	return track_path_known(id, look, zvukdl_saved_count() > 0, out, size);
}

static uint32_t id_hash(const char *s) {
	uint32_t h = 2166136261u;
	for (; *s; s++) {
		h = (h ^ (uint8_t)*s) * 16777619u;
	}
	return h;
}

// Whether the player's queue is this one (see yandexplay.c): kept until the
// queue changes.
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
	zvuklist_t *q = queue_get();
	int count = playlist_count();
	if (q && !playlist_is_library_backed() && count == zvuklist_count(q)) {
		uint32_t theirs = 0, mine = 0;
		char path[512], id[ZVUK_ID_MAX];
		bool all = true;
		for (int i = 0; i < count && all; i++) {
			all = playlist_path_at(i, path, sizeof(path));
			path_track_id(path, id, sizeof(id));
			theirs += id_hash(id);
			if (zvuklist_id(q, i, id, sizeof(id))) {
				mine += id_hash(id);
			}
		}
		ours = all && theirs == mine;
	}
	zvuklist_free(q);
	return ours;
}

static void note_own_change(bool was_ours) {
	ours_valid = was_ours;
	ours = was_ours;
	ours_revision = playlist_revision();
}

static bool claims_live;

static void claim_saved(void) {
	if (zvukdl_saved_count() == 0) {
		zvukdl_claim(NULL, 0);
		claims_live = false;
		return;
	}
	zvuklist_t *q = queue_get();
	int count = zvuklist_count(q);
	char(*paths)[512] = NULL;
	int n = 0;
	for (int i = 0; i < count; i++) {
		char id[ZVUK_ID_MAX], path[512];
		if (!zvuklist_id(q, i, id, sizeof(id)) || !zvukdl_saved(id) || !zvukdl_local(id, path, sizeof(path))) {
			continue;
		}
		char(*grown)[512] = realloc(paths, (size_t)(n + 1) * sizeof(*paths));
		if (!grown) {
			break;
		}
		paths = grown;
		snprintf(paths[n++], sizeof(paths[0]), "%s", path);
	}
	const char **ptrs = malloc((size_t)(n > 0 ? n : 1) * sizeof(*ptrs));
	if (ptrs) {
		for (int i = 0; i < n; i++) {
			ptrs[i] = paths[i];
		}
		zvukdl_claim(ptrs, n);
		claims_live = n > 0;
	}
	free(ptrs);
	free(paths);
	zvuklist_free(q);
}

// ---------------------------------------------------------------------------
// the worker: the tapped track, and the track playback needs
// ---------------------------------------------------------------------------

typedef enum { JOB_NONE = 0, JOB_PLAY, JOB_FETCH } job_kind_t;

typedef struct {
	job_kind_t kind;
	char id[ZVUK_ID_MAX];
	unsigned generation;
	bool waiting;
} job_t;

static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_cond = PTHREAD_COND_INITIALIZER;
static job_t play_pending, fetch_pending;
static job_kind_t running_kind;
static bool worker_started;

typedef struct {
	job_kind_t kind;
	unsigned generation;
	bool ok, waiting;
	char id[ZVUK_ID_MAX];
	char path[512];
	char error[256];
} job_result_t;

static void play_done_async(void *user);
static void fetch_done_async(void *user);

static void track_details(zvuklist_t *q, const char *id, zvuk_track_t *out) {
	int index = zvuklist_find(q, id);
	if (index >= 0 && !zvuklist_track(q, index, out)) {
		zvuklist_fetch_page(q, index / ZVUKLIST_PAGE);
	}
	if (index < 0 || !zvuklist_track(q, index, out)) {
		memset(out, 0, sizeof(*out));
		snprintf(out->id, sizeof(out->id), "%s", id);
		out->available = true;
	}
}

static bool fetch_track(const zvuk_track_t *t, char *path, size_t size, char *error, size_t error_size) {
	if (zvukcache_find(t->id, path, size)) {
		return true;
	}
	zvuk_stream_t stream;
	if (!zvuk_stream(t->id, &stream)) {
		streamerr_describe("zvuk", zvuk_last_error(), error, error_size);
		return false;
	}
	if (!zvukcache_start(t, &stream, path, size)) {
		streamerr_describe("zvuk", zvukcache_last_error(), error, error_size);
		return false;
	}
	zvukcache_write_sidecars(t, path);
	zvukcache_note_played(path);
	return true;
}

static void abandon_unless(const char *id) {
	char busy[ZVUK_ID_MAX];
	zvukcache_downloading_id(busy, sizeof(busy));
	if (strcmp(busy, id) != 0) {
		streamturn_abandon_all();
	}
}

static void *worker_main(void *arg) {
	(void)arg;
	http_set_thread_timeout_cap(STREAM_UI_TIMEOUT_SECS);
	for (;;) {
		pthread_mutex_lock(&job_lock);
		running_kind = JOB_NONE;
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
		zvuklist_t *q = queue_get();
		zvuk_track_t t;
		track_details(q, job.id, &t);
		zvuklist_free(q);
		if (job.kind == JOB_PLAY || job.waiting) {
			abandon_unless(job.id);
		}
		r->ok = fetch_track(&t, r->path, sizeof(r->path), r->error, sizeof(r->error));
		if (!r->ok) {
			fprintf(stderr, "zvuk: fetching %s failed: %s\n", job.id, r->error);
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

static bool worker_fetching(void) {
	pthread_mutex_lock(&job_lock);
	bool busy = play_pending.kind != JOB_NONE || fetch_pending.kind != JOB_NONE || running_kind != JOB_NONE;
	pthread_mutex_unlock(&job_lock);
	return busy;
}

// ---------------------------------------------------------------------------
// the watch: a timer that exists only while something Zvuk is in play
// ---------------------------------------------------------------------------

static lv_timer_t *watch_timer;
static void watch_cb(lv_timer_t *timer);

static void watch_start(void) {
	if (!watch_timer) {
		watch_timer = lv_timer_create(watch_cb, 1000, NULL);
	}
}

static void watch_start_async(void *user) {
	(void)user;
	watch_start();
}

// ---------------------------------------------------------------------------
// starting a list
// ---------------------------------------------------------------------------

static int start_index;

static void fetch_ahead(void);

static void play_queue_from(int start, const char *start_path) {
	zvuklist_t *q = queue_get();
	int count = zvuklist_count(q);
	// One block for the names, as yandexplay.c: thousands of 512-byte
	// buffers would be megabytes at once.
	size_t used = 0, room = (size_t)(count > 0 ? count : 1) * 56;
	char *names = malloc(room);
	size_t *offsets = malloc((size_t)(count > 0 ? count : 1) * sizeof(*offsets));
	const char **ptrs = malloc((size_t)(count > 0 ? count : 1) * sizeof(*ptrs));
	bool ok = names && offsets && ptrs && count > 0;
	bool any_saved = zvukdl_saved_count() > 0;
	for (int i = 0; ok && i < count; i++) {
		char id[ZVUK_ID_MAX], path[512];
		zvuklist_id(q, i, id, sizeof(id));
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
	if (ok) {
		for (int i = 0; i < count; i++) {
			ptrs[i] = names + offsets[i];
		}
		device_state_play_list(ptrs, count, start);
		note_own_change(true);
		claim_saved();
		printf("zvuk: queue of %d tracks from %d\n", count, start);
	}
	free(ptrs);
	free(offsets);
	free(names);
	zvuklist_free(q);
	watch_start();
}

static void play_done_async(void *user) {
	job_result_t *r = user;
	pthread_mutex_lock(&queue_lock);
	bool current = r->generation == queue_generation;
	pthread_mutex_unlock(&queue_lock);
	if (current) {
		if (r->ok) {
			play_queue_from(start_index, r->path);
			tell(ZVUKPLAY_STARTED, NULL);
			fetch_ahead();
		} else {
			tell(ZVUKPLAY_FAILED, r->error[0] ? r->error : tr("download_failed"));
		}
	}
	free(r);
}

bool zvukplay_list(zvuklist_t *list, int start) {
	int count = zvuklist_count(list);
	if (count == 0 || start < 0 || start >= count) {
		return false;
	}
	zvuklist_t *copy = zvuklist_clone(list);
	if (!copy) {
		return false;
	}
	queue_set(copy);
	start_index = start;
	char id[ZVUK_ID_MAX], path[512];
	zvuklist_id(copy, start, id, sizeof(id));
	if (track_path(id, true, path, sizeof(path)) || zvukcache_find(id, NULL, 0)) {
		// On the card: it plays now, network or not, session or not.
		play_queue_from(start, path);
		tell(ZVUKPLAY_STARTED, NULL);
		fetch_ahead();
		return true;
	}
	if (!zvuk_logged_in()) {
		tell(ZVUKPLAY_FAILED, tr("zvuk_not_signed_in"));
		return false;
	}
	zvukcache_set_network_wanted(true);
	submit(JOB_PLAY, id, false);
	watch_start();
	return true;
}

// ---------------------------------------------------------------------------
// fetched tracks
// ---------------------------------------------------------------------------

#define SKIPS_MAX 3
static int skips_in_a_row;

static void current_id(char *id, size_t size) {
	char path[512];
	id[0] = '\0';
	if (playlist_current_path(path, sizeof(path))) {
		path_track_id(path, id, size);
	}
}

static void fetch_done_async(void *user) {
	job_result_t *r = user;
	char guess[512], now[ZVUK_ID_MAX];
	current_id(now, sizeof(now));
	bool waited_for = strcmp(now, r->id) == 0;
	device_state_t state;
	device_state_get(&state);
	if (r->ok) {
		bool was_ours = queue_is_ours();
		zvukcache_path(r->id, "flac", guess, sizeof(guess));
		if (playlist_replace_path(guess, r->path) > 0) {
			note_own_change(was_ours);
		}
		if (waited_for && state.status != AUDIO_STATUS_PLAYING) {
			skips_in_a_row = 0;
			device_state_play_queue_index(playlist_current_index());
		}
	} else if (waited_for && state.status != AUDIO_STATUS_PLAYING && r->waiting) {
		if (++skips_in_a_row <= SKIPS_MAX && !playlist_at_end()) {
			tell(ZVUKPLAY_SKIPPED, r->error);
			char next[512];
			device_state_next(next, sizeof(next));
		} else {
			skips_in_a_row = 0;
			tell(ZVUKPLAY_FAILED, r->error);
		}
	}
	free(r);
}

static void fetch_ahead(void) {
	int current = playlist_current_index();
	char path[512], id[ZVUK_ID_MAX], real[512];
	if (current < 0 || !playlist_path_at(current + 1, path, sizeof(path)) || !zvukcache_owns(path)) {
		return;
	}
	if (access(path, F_OK) == 0 || growfile_is_growing(path)) {
		return;
	}
	path_track_id(path, id, sizeof(id));
	if (!id[0]) {
		return;
	}
	if (zvukcache_find(id, real, sizeof(real))) {
		bool was_ours = queue_is_ours();
		if (playlist_replace_path(path, real) > 0) {
			note_own_change(was_ours);
		}
		return;
	}
	if (zvukcache_busy() || worker_fetching() || !zvuk_logged_in()) {
		return;
	}
	zvuklist_t *q = queue_get();
	bool known = zvuklist_find(q, id) >= 0;
	zvuklist_free(q);
	if (known) {
		zvukcache_set_network_wanted(true);
		submit(JOB_FETCH, id, false);
	}
}

// ---------------------------------------------------------------------------
// the prepare callback
// ---------------------------------------------------------------------------

static void downloading_note_async(void *user) {
	(void)user;
	gui_notify_popup("downloading");
}

static void signed_out_note_async(void *user) {
	(void)user;
	gui_notify_popup(tr(zvuk_session_expired() ? "zvuk_token_expired" : "zvuk_not_signed_in"));
}

static void requeue_async(void *user) {
	char *path = user;
	char id[ZVUK_ID_MAX], fresh[512];
	path_track_id(path, id, sizeof(id));
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

static void adopt_id(const char *id) {
	pthread_mutex_lock(&queue_lock);
	if (!queue) {
		queue = zvuklist_new();
	}
	zvuklist_t *q = zvuklist_ref(queue);
	pthread_mutex_unlock(&queue_lock);
	if (q && zvuklist_find(q, id) < 0) {
		zvuklist_add_id(q, id);
	}
	zvuklist_free(q);
}

bool zvukplay_prepare_track(const char *path) {
	if (!zvukcache_owns(path)) {
		if (zvukdl_claimed(path) && !queue_is_ours()) {
			zvukdl_claim(NULL, 0);
			return true;
		}
		if (zvukdl_claimed(path) && access(path, R_OK) != 0) {
			char *copy = strdup(path);
			if (copy) {
				zvukcache_set_network_wanted(true);
				device_state_stop();
				gui_post(requeue_async, copy);
				return false;
			}
		}
		return true;
	}
	if (growfile_is_growing(path) || access(path, F_OK) == 0) {
		return true;
	}
	char id[ZVUK_ID_MAX];
	zvukcache_track_id(path, id, sizeof(id));
	if (!id[0]) {
		return true;
	}
	if (zvukdl_saved(id) && zvukdl_local(id, NULL, 0)) {
		char *copy = strdup(path);
		if (copy) {
			device_state_stop();
			gui_post(requeue_async, copy);
			return false;
		}
	}
	if (!zvuk_logged_in()) {
		// Signed out (the token expired): what is not on the card cannot
		// come. Said, and playback waits there; saved tracks play on.
		device_state_stop();
		gui_post(signed_out_note_async, NULL);
		return false;
	}
	adopt_id(id);
	zvukcache_set_network_wanted(true);
	device_state_stop();
	submit(JOB_FETCH, id, true);
	gui_post(downloading_note_async, NULL);
	gui_post(watch_start_async, NULL);
	return false;
}

// ---------------------------------------------------------------------------
// the rest
// ---------------------------------------------------------------------------

void zvukplay_init(zvukplay_listener_t l) {
	listener = l;
	device_state_add_prepare_cb(zvukplay_prepare_track);
}

int zvukplay_paths_for(const zvuk_track_t *tracks, int count, char ***paths_out) {
	*paths_out = NULL;
	if (count <= 0) {
		return 0;
	}
	char **paths = malloc((size_t)count * (sizeof(char *) + 512));
	if (!paths) {
		return 0;
	}
	char *names = (char *)(paths + count);
	pthread_mutex_lock(&queue_lock);
	if (!queue) {
		queue = zvuklist_new();
	}
	zvuklist_t *q = zvuklist_ref(queue);
	pthread_mutex_unlock(&queue_lock);
	int n = 0;
	for (int i = 0; q && i < count; i++) {
		if (!tracks[i].id[0] || !zvuklist_add_track(q, &tracks[i])) {
			continue;
		}
		paths[n] = names + (size_t)n * 512;
		track_path(tracks[i].id, true, paths[n], 512);
		n++;
	}
	zvuklist_free(q);
	ours_valid = false;
	*paths_out = paths;
	watch_start();
	return n;
}

bool zvukplay_current(zvuk_track_t *out) {
	char id[ZVUK_ID_MAX];
	current_id(id, sizeof(id));
	zvuklist_t *q = queue_get();
	int index = id[0] ? zvuklist_find(q, id) : -1;
	bool ok = index >= 0;
	if (ok && !zvuklist_track(q, index, out)) {
		memset(out, 0, sizeof(*out));
		snprintf(out->id, sizeof(out->id), "%s", id);
		out->available = true;
	}
	zvuklist_free(q);
	return ok;
}

void zvukplay_signed_out(void) {
	zvukdl_claim(NULL, 0);
	claims_live = false;
}

// Once a second while something Zvuk is in play: the queued files are kept
// from the sweep, the next one is fetched ahead, the network is kept up while
// it is needed. When the queue is no longer ours and nothing is coming down,
// the timer goes; the next Zvuk track brings it back.
static int idle_ticks;

static void watch_cb(lv_timer_t *timer) {
	(void)timer;
	bool library_queue = playlist_is_library_backed();
	bool ours_now = queue_is_ours();
	if (claims_live && !ours_now) {
		zvukdl_claim(NULL, 0);
		claims_live = false;
	}
	static char keep[ZVUKCACHE_PROTECTED_MAX][512];
	static const char *keep_ptr[ZVUKCACHE_PROTECTED_MAX];
	int protect = 0;
	int count = playlist_count();
	for (int i = playlist_current_index(); !library_queue && i >= 0 && i < count && protect < ZVUKCACHE_PROTECTED_MAX;
		 i++) {
		if (playlist_path_at(i, keep[protect], sizeof(keep[0])) && zvukcache_owns(keep[protect])) {
			keep_ptr[protect] = keep[protect];
			protect++;
		}
	}
	zvukcache_set_protected(keep_ptr, protect);
	fetch_ahead();
	bool busy = zvukcache_busy() || worker_fetching();
	zvukcache_set_network_wanted(busy);
	idle_ticks = (!ours_now && protect == 0 && !busy) ? idle_ticks + 1 : 0;
	if (idle_ticks >= 5) {
		lv_timer_delete(watch_timer);
		watch_timer = NULL;
		idle_ticks = 0;
	}
}

#endif /* SONIX_ZVUK */

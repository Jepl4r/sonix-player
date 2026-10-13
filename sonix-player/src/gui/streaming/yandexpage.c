#include "yandexpage.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/spinner.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"
#include "src/system/decode/growfile.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/provider.h"
#include "src/system/streaming/streamturn.h"
#include "src/system/streaming/yandex.h"
#include "src/system/streaming/yandexcache.h"
#include "src/system/streaming/yandexdl.h"
#include "src/system/streaming/yandexsync.h"

// See yandexpage.h. Deliberately smaller than the Tidal page: no artwork in
// the lists (the cover still arrives with each track, beside the file), no
// paging (the API hands whole lists), one list page for every kind of row.

#define MAX_ROWS 150
#define LOGIN_QR_SIZE 220

typedef enum { ROW_TRACK, ROW_ALBUM, ROW_ARTIST, ROW_PLAYLIST } row_kind_t;

typedef struct {
	row_kind_t kind;
	union {
		yandex_track_t track;
		yandex_album_t album;
		yandex_artist_t artist;
		yandex_playlist_t playlist;
	} u;
} row_t;

typedef enum {
	JOB_NONE = 0,
	JOB_LOGIN_BEGIN,
	JOB_LOGIN_POLL,
	JOB_LIKED,
	JOB_PLAYLISTS,
	JOB_PLAYLIST,
	JOB_ALBUM,
	JOB_ARTIST,
	JOB_SEARCH,
	JOB_WAVE,
	JOB_WAVE_MORE,
	JOB_PLAY,
	JOB_FETCH,
	JOB_FEEDBACK,
	JOB_DISLIKE,
} job_kind_t;

typedef struct {
	job_kind_t kind;
	char a[YANDEX_TITLE_MAX]; // id, owner, search text...
	char b[YANDEX_ID_MAX];	  // kind, album id
	int n;					  // index, or the feedback event
	int secs;				  // feedback: seconds played
	unsigned generation;	  // list jobs: list_generation when asked
} job_t;

// One job's answer, handed to the GUI thread whole through gui_post's user
// pointer, which then owns it.
//
// The worker used to fill shared buffers and post a bare "done": having posted,
// it was free to take the next job and start overwriting those buffers while
// the GUI thread was still reading them -- a list half one answer and half the
// next, or a count that does not match the rows. A block per answer cannot be
// overwritten by anyone.
typedef struct {
	job_kind_t kind;
	unsigned generation;
	int index; // JOB_PLAY: the queue entry, -1 when it failed
	char path[512];
	char error[256];
	yandex_login_state_t login_state;
	yandex_login_t login; // a copy: the worker keeps polling with its own
	int count;
	row_t rows[]; // `count` of them
} job_result_t;

lv_obj_t *yandex_screen;
static lv_obj_t *login_screen, *list_screen, *search_screen;

// home
static lv_obj_t *home_signed_in, *home_signed_out, *home_user;
// login
static lv_obj_t *login_qr, *login_code, *login_message, *login_spin;
static lv_timer_t *login_timer;
static yandex_login_t login;
static time_t login_deadline;
// list
static lv_obj_t *list_title, *list_box, *list_spin, *wave_bar, *album_bar;
// The album a JOB_ALBUM list shows, for its "Save album" button.
static char list_album_id[YANDEX_ID_MAX];
static char list_album_title[YANDEX_TITLE_MAX];
static gui_config_t *list_cfg;
// search
static lv_obj_t *search_field;

// The rows on screen (GUI thread): those of the answer shown, which owns
// them. NULL before the first list.
static job_result_t *shown;
static row_t *rows;
static int row_count;
static bool list_is_wave;
// Raised by each list asked for (GUI thread): an answer carrying an older
// number belongs to a list the user has since left, and is dropped.
static unsigned list_generation;
// The answer being built (worker thread only), copied into a job_result_t
// when the job is done.
static row_t *result;
static int result_count;
static char result_error[256];

// The queue: what was played from a list, with the track behind each path.
static yandex_track_t *queued;
static char (*queued_paths)[512];
static int queued_count;
static bool queue_is_wave;

// Worker: one job pending at a time, a newer request replaces an older one
// still waiting (the user changed their mind), except feedback which is
// fire-and-forget and must not be lost to a list request.
static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_cond = PTHREAD_COND_INITIALIZER;
static job_t pending;
// Feedback and dislikes queue up instead: each one is an event the service
// should hear about, and a skip lost behind the next trackStarted would teach
// My Wave the wrong thing.
#define FEEDBACK_MAX 8
static job_t feedback[FEEDBACK_MAX];
static int feedback_count;
static bool worker_started;
// What the worker is doing right now (JOB_NONE when waiting), for the watch:
// a track being asked for is a reason to keep Wi-Fi up, and a second fetch is
// not queued over one already running. Under job_lock.
static job_kind_t running_kind;

static void job_done_async(void *user);
static void fetch_done_async(void *user);
static void play_done_async(void *user);

static void submit(const job_t *job) {
	pthread_mutex_lock(&job_lock);
	if (job->kind == JOB_FEEDBACK || job->kind == JOB_DISLIKE) {
		if (feedback_count < FEEDBACK_MAX) {
			feedback[feedback_count++] = *job;
		}
	} else {
		pending = *job;
	}
	pthread_cond_signal(&job_cond);
	pthread_mutex_unlock(&job_lock);
}

static void ensure_buffers(void) {
	if (!result) {
		result = calloc(MAX_ROWS, sizeof(*result));
		queued = calloc(MAX_ROWS, sizeof(*queued));
		queued_paths = calloc(MAX_ROWS, sizeof(*queued_paths));
	}
}

// ---------------------------------------------------------------------------
// worker
// ---------------------------------------------------------------------------

static int fill_tracks(int got, yandex_track_t *tmp) {
	result_count = 0;
	for (int i = 0; i < got && i < MAX_ROWS; i++) {
		result[result_count].kind = ROW_TRACK;
		result[result_count].u.track = tmp[i];
		result_count++;
	}
	return got;
}

// Starts the download of queue entry `index` and returns its real path. The
// codec, and so the extension, is only known now.
static bool fetch_queued(int index, char *path, size_t size) {
	yandex_track_t *t = &queued[index];
	if (yandexcache_find(t->id, path, size)) {
		return true; // prefetched under its real name: no need to ask again
	}
	yandex_file_t file;
	if (!yandex_file_info(t->id, &file)) {
		snprintf(result_error, sizeof(result_error), "%s", yandex_last_error());
		return false;
	}
	if (!yandexcache_start(t, &file, path, size)) {
		snprintf(result_error, sizeof(result_error), "%s", yandexcache_last_error());
		return false;
	}
	yandexcache_write_sidecars(t, path);
	// Counts towards the next sweep and is spared by it until the queue knows.
	yandexcache_note_played(path);
	return true;
}

static void run_list_job(const job_t *job) {
	yandex_track_t *tmp = calloc(MAX_ROWS, sizeof(*tmp));
	if (!tmp) {
		return;
	}
	int got = -1;
	result_count = 0;
	switch (job->kind) {
	case JOB_LIKED:
		got = fill_tracks(yandex_liked_tracks(tmp, MAX_ROWS), tmp);
		break;
	case JOB_PLAYLIST:
		got = fill_tracks(yandex_playlist_tracks(job->a, job->b, tmp, MAX_ROWS), tmp);
		break;
	case JOB_ALBUM:
		got = fill_tracks(yandex_album_tracks(job->a, tmp, MAX_ROWS), tmp);
		break;
	case JOB_WAVE:
		got = fill_tracks(yandex_wave_start(tmp, MAX_ROWS), tmp);
		break;
	case JOB_PLAYLISTS: {
		yandex_playlist_t *p = (yandex_playlist_t *)tmp; // smaller than a track
		got = yandex_playlists(p, MAX_ROWS);
		for (int i = 0; i < got; i++) {
			result[i].kind = ROW_PLAYLIST;
			result[i].u.playlist = p[i];
		}
		result_count = got > 0 ? got : 0;
		break;
	}
	case JOB_ARTIST: {
		yandex_album_t *a = (yandex_album_t *)tmp;
		got = yandex_artist_albums(job->a, a, MAX_ROWS);
		for (int i = 0; i < got; i++) {
			result[i].kind = ROW_ALBUM;
			result[i].u.album = a[i];
		}
		result_count = got > 0 ? got : 0;
		break;
	}
	case JOB_SEARCH: {
		// Artists first, then albums, then tracks: one list, short of each.
		yandex_artist_t ar[5];
		yandex_album_t al[10];
		int nr = 5, na = 10, nt = 40;
		int n = yandex_search(job->a, ar, &nr, al, &na, tmp, &nt) ? 0 : -1;
		for (int i = 0; n == 0 && i < nr; i++) {
			result[result_count].kind = ROW_ARTIST;
			result[result_count++].u.artist = ar[i];
		}
		for (int i = 0; n == 0 && i < na; i++) {
			result[result_count].kind = ROW_ALBUM;
			result[result_count++].u.album = al[i];
		}
		for (int i = 0; n == 0 && i < nt && result_count < MAX_ROWS; i++) {
			result[result_count].kind = ROW_TRACK;
			result[result_count++].u.track = tmp[i];
		}
		got = n < 0 ? -1 : result_count;
		break;
	}
	default:
		break;
	}
	if (got < 0) {
		snprintf(result_error, sizeof(result_error), "%s", yandex_last_error());
	}
	free(tmp);
}

// Whatever is coming down gives way to what the user just asked for -- unless
// it is that very track (prefetch got there first): abandoning it would only
// restart it from zero. The cache itself never abandons (yandexcache_start),
// this is the page's call, as on the Tidal page.
static void abandon_unless(const char *id) {
	char busy[YANDEX_ID_MAX];
	yandexcache_downloading_id(busy, sizeof(busy));
	if (strcmp(busy, id) != 0) {
		streamturn_abandon_all();
	}
}

// Packs the job's answer -- the rows built in `result`, the error, whatever
// else it carries -- into a block of its own, for the GUI thread to take over
// and free. NULL when there is no memory for even the bare block: nothing is
// posted then, and the spinner stays up, which is the truth.
static job_result_t *job_result_new(const job_t *job, int count) {
	if (count < 0) {
		count = 0;
	}
	job_result_t *r = malloc(sizeof(*r) + (size_t)count * sizeof(row_t));
	if (!r && count > 0) {
		count = 0; // the rows do not fit: say what went wrong instead
		r = malloc(sizeof(*r));
		if (r) {
			snprintf(result_error, sizeof(result_error), "%s", tr("out_of_memory"));
		}
	}
	if (!r) {
		return NULL;
	}
	memset(r, 0, sizeof(*r));
	r->kind = job->kind;
	r->generation = job->generation;
	r->index = -1;
	snprintf(r->error, sizeof(r->error), "%s", result_error);
	r->count = count;
	if (count > 0) {
		memcpy(r->rows, result, (size_t)count * sizeof(row_t));
	}
	return r;
}

static void *worker_main(void *arg) {
	(void)arg;
	yandex_login_state_t login_state = YANDEX_LOGIN_PENDING;
	for (;;) {
		pthread_mutex_lock(&job_lock);
		running_kind = JOB_NONE;
		while (pending.kind == JOB_NONE && feedback_count == 0) {
			pthread_cond_wait(&job_cond, &job_lock);
		}
		job_t job;
		if (feedback_count > 0) {
			job = feedback[0];
			memmove(feedback, feedback + 1, (size_t)(--feedback_count) * sizeof(feedback[0]));
		} else {
			job = pending;
			pending.kind = JOB_NONE;
		}
		running_kind = job.kind;
		pthread_mutex_unlock(&job_lock);

		result_error[0] = '\0';
		result_count = 0;
		switch (job.kind) {
		case JOB_LOGIN_BEGIN:
			login_state = yandex_login_begin(&login) ? YANDEX_LOGIN_PENDING : YANDEX_LOGIN_ERROR;
			if (login_state == YANDEX_LOGIN_ERROR) {
				snprintf(result_error, sizeof(result_error), "%s", yandex_last_error());
			}
			break;
		case JOB_LOGIN_POLL:
			login_state = yandex_login_poll(&login);
			if (login_state == YANDEX_LOGIN_ERROR) {
				snprintf(result_error, sizeof(result_error), "%s", yandex_last_error());
			}
			break;
		case JOB_FEEDBACK:
			yandex_wave_feedback((yandex_wave_event_t)job.n, job.a, job.b, job.secs);
			continue;
		case JOB_DISLIKE:
			if (!yandex_dislike_track(job.a, job.b)) {
				snprintf(result_error, sizeof(result_error), "%s", yandex_last_error());
			}
			break;
		case JOB_PLAY: {
			// The tapped track first; the rest download as playback reaches
			// them (yandexpage_prepare_track) or ahead of it (JOB_FETCH).
			abandon_unless(queued[job.n].id);
			char path[512];
			bool ok = fetch_queued(job.n, path, sizeof(path));
			job_result_t *r = job_result_new(&job, 0);
			if (r) {
				// The queue's path is set by the GUI thread, which owns the
				// queue (play_done_async).
				r->index = ok ? job.n : -1;
				snprintf(r->path, sizeof(r->path), "%s", ok ? path : "");
				gui_post(play_done_async, r);
			}
			continue;
		}
		case JOB_FETCH: {
			char path[512];
			if (job.n >= 0 && job.n < queued_count && fetch_queued(job.n, path, sizeof(path))) {
				char *owned = strdup(path);
				if (owned) {
					gui_post(fetch_done_async, owned);
				}
			} else {
				fprintf(stderr, "yandex: fetching %d failed: %s\n", job.n, result_error);
			}
			continue;
		}
		case JOB_WAVE_MORE: {
			yandex_track_t more[5];
			const char *played[1] = {job.a};
			int got = yandex_wave_more(played, job.a[0] ? 1 : 0, more, 5);
			result_count = 0;
			for (int i = 0; i < got; i++) {
				result[result_count].kind = ROW_TRACK;
				result[result_count++].u.track = more[i];
			}
			break;
		}
		default:
			run_list_job(&job);
			break;
		}
		job_result_t *r = job_result_new(&job, result_count);
		if (r) {
			r->login_state = login_state;
			if (job.kind == JOB_LOGIN_BEGIN || job.kind == JOB_LOGIN_POLL) {
				r->login = login;
			}
			gui_post(job_done_async, r);
		}
	}
	return NULL;
}

static void start_worker(void) {
	if (worker_started) {
		return;
	}
	ensure_buffers();
	pthread_t th;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, 256 * 1024);
	worker_started = pthread_create(&th, &attr, worker_main, NULL) == 0;
	pthread_attr_destroy(&attr);
}

static void run(job_kind_t kind, const char *a, const char *b, int n) {
	start_worker();
	job_t job = {.kind = kind, .n = n, .generation = list_generation};
	snprintf(job.a, sizeof(job.a), "%s", a ? a : "");
	snprintf(job.b, sizeof(job.b), "%s", b ? b : "");
	submit(&job);
}

// ---------------------------------------------------------------------------
// playback
// ---------------------------------------------------------------------------

static int queue_index_of(const char *path) {
	char id[YANDEX_ID_MAX];
	yandexcache_track_id(path, id, sizeof(id));
	for (int i = 0; id[0] && i < queued_count; i++) {
		if (strcmp(queued[i].id, id) == 0) {
			return i;
		}
	}
	return -1;
}

// True while the worker has a track to ask for, or is asking: pending, or
// running. Under job_lock, which is held only for the read.
static bool worker_fetching(void) {
	pthread_mutex_lock(&job_lock);
	job_kind_t kinds[2] = {pending.kind, running_kind};
	pthread_mutex_unlock(&job_lock);
	bool fetching = false;
	for (int i = 0; i < 2; i++) {
		fetching |= kinds[i] == JOB_PLAY || kinds[i] == JOB_FETCH || kinds[i] == JOB_WAVE_MORE;
	}
	return fetching;
}

// Asks for queue entry `index` ahead of playback, unless it is already on the
// card, or anything at all is coming down: the cache downloads one track at a
// time and starting another abandons the one in flight -- which, right after a
// track starts, is the very file being played. The watch below asks again
// every second, so the next track follows as soon as this one is complete.
static void fetch_ahead(int index) {
	if (index < 0 || index >= queued_count || yandexcache_busy() || worker_fetching()) {
		return;
	}
	if (yandexcache_find(queued[index].id, NULL, 0)) {
		return;
	}
	yandexcache_set_network_wanted(true);
	run(JOB_FETCH, NULL, NULL, index);
}

static void play_from(int start) {
	static const char *ptrs[MAX_ROWS];
	for (int i = 0; i < queued_count; i++) {
		ptrs[i] = queued_paths[i];
	}
	device_state_play_list(ptrs, queued_count, start);
}

static void play_done_async(void *user) {
	job_result_t *r = user;
	lv_obj_set_hidden(list_spin, true);
	int index = r->index;
	if (index < 0 || index >= queued_count) {
		toast_error(r->error[0] ? r->error : tr("download_failed"));
		free(r);
		return;
	}
	snprintf(queued_paths[index], sizeof(queued_paths[0]), "%s", r->path);
	free(r);
	play_from(index);
	player_refresh_now_playing();
	switch_screen(player_screen);
	fetch_ahead(index + 1);
}

// A queued track has arrived after playback asked for it: the queue gets its
// real name (the extension was a guess) and playback starts there.
static void fetch_done_async(void *user) {
	char *path = user;
	int index = queue_index_of(path);
	if (index >= 0) {
		bool was_guess = strcmp(queued_paths[index], path) != 0;
		snprintf(queued_paths[index], sizeof(queued_paths[0]), "%s", path);
		device_state_t state;
		device_state_get(&state);
		if (state.status != AUDIO_STATUS_PLAYING || was_guess) {
			// Stopped by prepare_track, waiting for this one.
			if (state.status != AUDIO_STATUS_PLAYING) {
				play_from(index);
				player_refresh_now_playing();
			}
		}
	}
	free(path);
}

// The "downloading" notice goes through gui_post: device_state calls
// yandexpage_prepare_track from whatever thread starts the track -- the UI
// thread for a tap, but also another page's worker starting its queue -- and
// LVGL may only be touched from the UI thread (see tidalpage.c).
static void downloading_note_async(void *user) {
	(void)user;
	gui_notify_popup("downloading");
}

bool yandexpage_prepare_track(const char *path) {
	if (!yandexcache_owns(path)) {
		return true;
	}
	// Downloaded (an MP4 only appears under its name once complete), or
	// arriving right now -- the decoder reads a growing file.
	if (growfile_is_growing(path) || access(path, F_OK) == 0) {
		return true;
	}
	int index = queue_index_of(path);
	if (index < 0) {
		return true; // not ours to fetch: let the decoder say what it thinks
	}
	// The network is needed now, before the stop: see
	// yandexcache_network_wanted().
	yandexcache_set_network_wanted(true);
	device_state_stop();
	// Playback moved on to a track that is not here yet: the one still coming
	// down, if any, is no longer what is wanted.
	abandon_unless(queued[index].id);
	run(JOB_FETCH, NULL, NULL, index);
	gui_post(downloading_note_async, NULL);
	return false;
}

// My Wave bookkeeping: what started, what was skipped, and a new batch when
// the queue runs low. Called by the watch when the playing track changes.
static char wave_current[YANDEX_ID_MAX], wave_current_album[YANDEX_ID_MAX];
static time_t wave_started_at;
static int wave_duration;
static bool wave_asked_more;

static void wave_track_changed(const char *id, int index) {
	if (wave_current[0]) {
		int played = (int)(time(NULL) - wave_started_at);
		bool finished = wave_duration > 0 && played >= wave_duration - 5;
		job_t job = {.kind = JOB_FEEDBACK, .n = finished ? YANDEX_WAVE_TRACK_FINISHED : YANDEX_WAVE_SKIP, .secs = played};
		snprintf(job.a, sizeof(job.a), "%s", wave_current);
		snprintf(job.b, sizeof(job.b), "%s", wave_current_album);
		start_worker();
		submit(&job);
	}
	snprintf(wave_current, sizeof(wave_current), "%s", id);
	snprintf(wave_current_album, sizeof(wave_current_album), "%s", index >= 0 ? queued[index].album_id : "");
	wave_duration = index >= 0 ? queued[index].duration : 0;
	wave_started_at = time(NULL);
	run(JOB_FEEDBACK, id, wave_current_album, YANDEX_WAVE_TRACK_STARTED);
	if (index >= 0 && index + 2 >= queued_count && !wave_asked_more) {
		wave_asked_more = true;
		yandexcache_set_network_wanted(true);
		run(JOB_WAVE_MORE, id, NULL, 0);
	}
}

// The queue watch, once a second on the GUI thread, the Qobuz page's in small:
//   * protects the queued files -- the one playing and those after it -- from
//     the cache's sweep and ceiling;
//   * asks for the next track once nothing else is coming down;
//   * works out whether Yandex still needs the network, which is the only
//     place that lowers the flag (those that raise it do so at once);
//   * and tells My Wave what happened.
// Nothing of ours ever queued: one comparison and out.
static void queue_watch_cb(lv_timer_t *timer) {
	(void)timer;
	if (queued_count == 0) {
		return;
	}
	bool library_queue = playlist_is_library_backed();
	int count = playlist_count();
	int current = playlist_current_index();

	static char keep[YANDEXCACHE_PROTECTED_MAX][512];
	static const char *keep_ptr[YANDEXCACHE_PROTECTED_MAX];
	int protect = 0;
	for (int i = current; !library_queue && i >= 0 && i < count && protect < YANDEXCACHE_PROTECTED_MAX; i++) {
		if (playlist_path_at(i, keep[protect], sizeof(keep[0])) && yandexcache_owns(keep[protect])) {
			keep_ptr[protect] = keep[protect];
			protect++;
		}
	}
	yandexcache_set_protected(keep_ptr, protect);

	device_state_t state;
	device_state_get(&state);
	int index = queue_index_of(state.current_file);
	if (index >= 0 && !library_queue) {
		fetch_ahead(index + 1);
	}
	yandexcache_set_network_wanted(yandexcache_busy() || worker_fetching());

	if (!queue_is_wave) {
		return;
	}
	char id[YANDEX_ID_MAX];
	yandexcache_track_id(state.current_file, id, sizeof(id));
	if (id[0] && strcmp(id, wave_current) != 0) {
		wave_track_changed(id, index);
	}
}

// ---------------------------------------------------------------------------
// list page
// ---------------------------------------------------------------------------

static void row_clicked_cb(lv_event_t *e);

static void row_texts(const row_t *r, char *name, size_t ns, char *detail, size_t ds) {
	switch (r->kind) {
	case ROW_TRACK:
		snprintf(name, ns, "%s", r->u.track.title);
		snprintf(detail, ds, "%s%s", r->u.track.artist, r->u.track.available ? "" : " \xC2\xB7 —");
		break;
	case ROW_ALBUM:
		snprintf(name, ns, "%s", r->u.album.title);
		if (r->u.album.year) {
			snprintf(detail, ds, "%s \xC2\xB7 %d", r->u.album.artist, r->u.album.year);
		} else {
			snprintf(detail, ds, "%s", r->u.album.artist);
		}
		break;
	case ROW_ARTIST:
		snprintf(name, ns, "%s", r->u.artist.name);
		snprintf(detail, ds, "%s", tr("yandex_artist"));
		break;
	case ROW_PLAYLIST:
		snprintf(name, ns, "%s", r->u.playlist.title);
		snprintf(detail, ds, tr("yandex_n_tracks"), r->u.playlist.track_count);
		break;
	}
}

static void row_menu_cb(lv_event_t *e);

// Rows with something to offer on their own get the three dots: a track (its
// like, saving it) and an album (saving it whole).
static bool row_has_menu(const row_t *r) { return (r->kind == ROW_TRACK && r->u.track.id[0]) || (r->kind == ROW_ALBUM && r->u.album.id[0]); }

static void make_row(int index) {
	char name[YANDEX_TITLE_MAX], detail[YANDEX_TITLE_MAX + 32];
	row_texts(&rows[index], name, sizeof(name), detail, sizeof(detail));

	lv_obj_t *row = lv_btn_create(list_box);
	lv_obj_set_size(row, lv_pct(100), 80);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, 12, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_left(row, 16, 0);
	lv_obj_set_style_pad_right(row, 6, 0);
	lv_obj_set_style_pad_column(row, 4, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_event_bubble(row, true);
	lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

	lv_obj_t *texts = lv_obj_create(row);
	lv_obj_set_flex_grow(texts, 1);
	lv_obj_set_height(texts, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(texts, 0, 0);
	lv_obj_set_style_border_width(texts, 0, 0);
	lv_obj_set_style_pad_all(texts, 0, 0);
	lv_obj_set_style_pad_row(texts, 4, 0);
	lv_obj_set_scrollable(texts, false);
	lv_obj_set_event_bubble(texts, true);
	lv_obj_set_clickable(texts, false);
	lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);

	lv_obj_t *label = lv_label_create(texts);
	lv_label_set_text(label, name);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);

	lv_obj_t *sub = lv_label_create(texts);
	lv_label_set_text(sub, detail);
	lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
	lv_obj_set_width(sub, lv_pct(100));
	lv_obj_add_style(sub, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(sub, &font_ui_18, 0);

	if (row_has_menu(&rows[index])) {
		lv_obj_t *menu_btn = lv_btn_create(row);
		lv_obj_set_size(menu_btn, 48, 48);
		lv_obj_set_style_bg_opa(menu_btn, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(menu_btn, 0, 0);
		lv_obj_set_style_shadow_width(menu_btn, 0, 0);
		lv_obj_set_style_pad_all(menu_btn, 0, 0);
		lv_obj_set_ext_click_area(menu_btn, 12);
		lv_obj_add_event_cb(menu_btn, row_menu_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);
		lv_obj_t *dots = lv_image_create(menu_btn);
		lv_image_set_src(dots, &icon_ellipsis_vertical);
		lv_obj_add_style(dots, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(dots, LV_OPA_COVER, 0);
		lv_obj_center(dots);
	}
}

static void fill_list(void) {
	lv_obj_clean(list_box);
	for (int i = 0; i < row_count; i++) {
		make_row(i);
	}
	if (row_count == 0) {
		lv_obj_t *empty = lv_label_create(list_box);
		lv_label_set_text(empty, shown && shown->error[0] ? shown->error : tr("yandex_empty"));
		lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(empty, lv_pct(100));
		lv_obj_add_style(empty, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(empty, &font_ui_20, 0);
	}
	lv_obj_scroll_to_y(list_box, 0, LV_ANIM_OFF);
}

static void open_list(const char *title, job_kind_t kind, const char *a, const char *b) {
	lv_label_set_text(list_title, title);
	list_is_wave = kind == JOB_WAVE;
	bool is_album = kind == JOB_ALBUM && a && a[0];
	if (is_album) {
		snprintf(list_album_id, sizeof(list_album_id), "%s", a);
		snprintf(list_album_title, sizeof(list_album_title), "%s", title);
	}
	lv_obj_set_hidden(wave_bar, !list_is_wave);
	lv_obj_set_hidden(album_bar, !is_album);
	// The list starts under the like/dislike bar (or the album's) only when
	// there is one.
	int top = settingsrow_content_top(list_cfg);
	int y = top + (list_is_wave || is_album ? 66 : 0);
	lv_obj_set_pos(list_box, list_cfg->padding, y);
	lv_obj_set_height(list_box, list_cfg->screen_height - y - 4);
	lv_obj_clean(list_box);
	row_count = 0;
	lv_obj_set_hidden(list_spin, false);
	switch_screen(list_screen);
	list_generation++;
	run(kind, a, b, 0);
}

// Queues every playable track of the list and plays the tapped one. Paths
// for the rest are guesses (".flac") until they are fetched; the real name
// replaces the guess when it arrives.
static void play_rows(int tapped) {
	queued_count = 0;
	int start = 0;
	for (int i = 0; i < row_count && queued_count < MAX_ROWS; i++) {
		if (rows[i].kind != ROW_TRACK || !rows[i].u.track.available) {
			continue;
		}
		if (i == tapped) {
			start = queued_count;
		}
		queued[queued_count] = rows[i].u.track;
		if (!yandexcache_find(queued[queued_count].id, queued_paths[queued_count], sizeof(queued_paths[0]))) {
			yandexcache_path(queued[queued_count].id, "flac", queued_paths[queued_count], sizeof(queued_paths[0]));
		}
		queued_count++;
	}
	if (queued_count == 0) {
		return;
	}
	queue_is_wave = list_is_wave;
	wave_current[0] = '\0';
	wave_asked_more = false;
	lv_obj_set_hidden(list_spin, false);
	yandexcache_set_network_wanted(true);
	run(JOB_PLAY, NULL, NULL, start);
}

static void row_clicked_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= row_count) {
		return;
	}
	const row_t *r = &rows[index];
	switch (r->kind) {
	case ROW_TRACK:
		play_rows(index);
		break;
	case ROW_ALBUM:
		open_list(r->u.album.title, JOB_ALBUM, r->u.album.id, NULL);
		break;
	case ROW_ARTIST:
		open_list(r->u.artist.name, JOB_ARTIST, r->u.artist.id, NULL);
		break;
	case ROW_PLAYLIST:
		open_list(r->u.playlist.title, JOB_PLAYLIST, r->u.playlist.owner, r->u.playlist.kind);
		break;
	}
}

static void like_failed_async(void *user) {
	char *why = user;
	toast_error(why && why[0] ? why : tr("yandex_like_failed"));
	free(why);
}

// The worker's answer to a like from the bar below; a refusal is said out
// loud (the star on Now Playing has already been put back by yandexsync).
static void like_done(bool ok, const char *error, void *user) {
	(void)user;
	if (!ok) {
		gui_post(like_failed_async, strdup(error ? error : ""));
	}
}

// ---------------------------------------------------------------------------
// the per-row menu
//
// A track's like from any list, not only from what is playing: liking what a
// search or a playlist shows used to mean playing it first. The menu is
// modal, so one remembered row is enough; the row itself is copied, because
// the list may be replaced by an answer arriving while the menu is open.
// ---------------------------------------------------------------------------

static row_t menu_row;

static void row_like_done_async(void *user) {
	char *why = user; // NULL on success
	if (why) {
		toast_error(why[0] ? why : tr("yandex_like_failed"));
		free(why);
	}
}

static void row_like_done(bool ok, const char *error, void *user) {
	(void)user;
	gui_post(row_like_done_async, ok ? NULL : strdup(error ? error : ""));
}

static void do_row_like(void *user) {
	bool like = user != NULL;
	const yandex_track_t *t = &menu_row.u.track;
	yandexsync_like(t->id, t->album_id, like, row_like_done, like ? (void *)1 : NULL);
	toast_success(tr(like ? "yandex_liked_it" : "yandex_unliked_it"));
}

static void save_refused(void) { toast_error(tr("yandex_save_refused")); }

static void do_row_save(void *user) {
	(void)user;
	if (menu_row.kind == ROW_ALBUM) {
		if (!yandexdl_album(menu_row.u.album.id, menu_row.u.album.title)) {
			save_refused();
		}
	} else if (!yandexdl_track(&menu_row.u.track)) {
		save_refused();
	}
}

static void row_menu_cb(lv_event_t *e) {
	lv_event_stop_bubbling(e); // the tap belongs to the menu, not the row
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= row_count) {
		return;
	}
	menu_row = rows[index];
	popover_item_t items[2];
	memset(items, 0, sizeof(items));
	int n = 0;
	if (menu_row.kind == ROW_TRACK) {
		// Until the account's list has arrived nothing is known to be
		// liked, and "Like" on a liked track is harmless: adding twice
		// changes nothing on the account.
		bool liked = yandex_is_liked(menu_row.u.track.id);
		items[n].label = liked ? "yandex_unlike" : "yandex_like";
		items[n].action = do_row_like;
		items[n].user = liked ? NULL : (void *)1;
		n++;
		if (menu_row.u.track.available) {
			items[n].label = "yandex_save";
			items[n].action = do_row_save;
			n++;
		}
	} else if (menu_row.kind == ROW_ALBUM) {
		items[n].label = "yandex_save_album";
		items[n].action = do_row_save;
		n++;
	}
	if (n > 0) {
		popover_show(lv_event_get_current_target(e), items, n);
	}
}

// Like and dislike act on what is playing now, from My Wave. The like goes
// the way the player's star goes (yandexsync.c), so the two always agree.
static void wave_rate_cb(lv_event_t *e) {
	bool like = lv_event_get_user_data(e) != NULL;
	device_state_t state;
	device_state_get(&state);
	int index = queue_index_of(state.current_file);
	if (index < 0) {
		return;
	}
	if (like) {
		yandexsync_like(queued[index].id, queued[index].album_id, true, like_done, NULL);
	} else {
		run(JOB_DISLIKE, queued[index].id, queued[index].album_id, 1);
	}
	toast_success(tr(like ? "yandex_liked_it" : "yandex_disliked_it"));
	if (!like) {
		char next[512];
		device_state_next(next, sizeof(next));
	}
}

static lv_obj_t *wave_button(lv_obj_t *parent, const char *key, bool like) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, LV_SIZE_CONTENT, 52);
	lv_obj_set_style_pad_hor(btn, 22, 0);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_add_event_cb(btn, wave_rate_cb, LV_EVENT_CLICKED, like ? (void *)1 : NULL);
	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, tr(key));
	lv_obj_set_style_text_font(label, &font_ui_20, 0);
	lv_obj_center(label);
	return btn;
}

static void album_save_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	if (!list_album_id[0] || !yandexdl_album(list_album_id, list_album_title)) {
		save_refused();
	}
}

static void build_list_page(gui_config_t *cfg) {
	list_cfg = cfg;
	list_screen = lv_obj_create(NULL);
	lv_obj_add_style(list_screen, &theme_style_screen, 0);
	list_title = settingsrow_title(list_screen, cfg, "");
	int top = settingsrow_content_top(cfg);

	wave_bar = lv_obj_create(list_screen);
	lv_obj_set_size(wave_bar, cfg->screen_width - 2 * cfg->padding, 60);
	lv_obj_align(wave_bar, LV_ALIGN_TOP_MID, 0, top);
	lv_obj_set_style_bg_opa(wave_bar, 0, 0);
	lv_obj_set_style_border_width(wave_bar, 0, 0);
	lv_obj_set_style_pad_all(wave_bar, 0, 0);
	lv_obj_set_flex_flow(wave_bar, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(wave_bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	wave_button(wave_bar, "yandex_like", true);
	wave_button(wave_bar, "yandex_dislike", false);
	lv_obj_set_hidden(wave_bar, true);

	// An album's own bar: the whole record into the library.
	album_bar = lv_obj_create(list_screen);
	lv_obj_set_size(album_bar, cfg->screen_width - 2 * cfg->padding, 60);
	lv_obj_align(album_bar, LV_ALIGN_TOP_MID, 0, top);
	lv_obj_set_style_bg_opa(album_bar, 0, 0);
	lv_obj_set_style_border_width(album_bar, 0, 0);
	lv_obj_set_style_pad_all(album_bar, 0, 0);
	lv_obj_set_flex_flow(album_bar, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(album_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_t *save = lv_btn_create(album_bar);
	lv_obj_set_size(save, LV_SIZE_CONTENT, 52);
	lv_obj_set_style_pad_hor(save, 22, 0);
	lv_obj_set_style_radius(save, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(save, 0, 0);
	lv_obj_add_event_cb(save, album_save_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *save_label = lv_label_create(save);
	lv_label_set_text(save_label, tr("yandex_save_album"));
	lv_obj_set_style_text_font(save_label, &font_ui_20, 0);
	lv_obj_center(save_label);
	lv_obj_set_hidden(album_bar, true);

	list_box = lv_obj_create(list_screen);
	lv_obj_set_size(list_box, cfg->screen_width - 2 * cfg->padding, cfg->screen_height - top - 70);
	lv_obj_set_pos(list_box, cfg->padding, top + 66);
	lv_obj_set_style_bg_opa(list_box, 0, 0);
	lv_obj_set_style_border_width(list_box, 0, 0);
	lv_obj_set_style_pad_all(list_box, 0, 0);
	lv_obj_set_style_pad_row(list_box, 10, 0);
	lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_COLUMN);

	list_spin = spinner_create(list_screen, &icon_loader_big);
	lv_obj_center(list_spin);
	lv_obj_set_hidden(list_spin, true);
	switcher_attach_back_gesture(list_screen);
}

// ---------------------------------------------------------------------------
// login
// ---------------------------------------------------------------------------

static void login_tick_cb(lv_timer_t *timer) {
	(void)timer;
	if (time(NULL) > login_deadline) {
		lv_timer_pause(login_timer);
		lv_label_set_text(login_message, tr("yandex_code_expired"));
		return;
	}
	run(JOB_LOGIN_POLL, NULL, NULL, 0);
}

static void paint_home(void);

static void login_result(const job_result_t *r) {
	const yandex_login_t *issued = &r->login;
	if (r->kind == JOB_LOGIN_BEGIN) {
		lv_obj_set_hidden(login_spin, true);
		if (r->login_state == YANDEX_LOGIN_ERROR) {
			lv_label_set_text(login_message, r->error);
			return;
		}
		lv_label_set_text(login_code, issued->user_code);
		lv_qrcode_update(login_qr, issued->verification_url, (uint32_t)strlen(issued->verification_url));
		lv_obj_set_hidden(login_qr, false);
		lv_label_set_text_fmt(login_message, "%s\n%s", tr("yandex_login_hint"), issued->verification_url);
		login_deadline = time(NULL) + issued->expires_secs;
		lv_timer_set_period(login_timer, (uint32_t)issued->interval_secs * 1000);
		lv_timer_resume(login_timer);
		return;
	}
	switch (r->login_state) {
	case YANDEX_LOGIN_OK:
		lv_timer_pause(login_timer);
		toast_success(tr("yandex_signed_in"));
		paint_home();
		switch_screen_return_to(yandex_screen);
		break;
	case YANDEX_LOGIN_EXPIRED:
		lv_timer_pause(login_timer);
		lv_label_set_text(login_message, tr("yandex_code_expired"));
		break;
	case YANDEX_LOGIN_ERROR:
		lv_timer_pause(login_timer);
		lv_label_set_text(login_message, r->error);
		break;
	default:
		break;
	}
}

static void open_login_cb(lv_event_t *e) {
	(void)e;
	lv_label_set_text(login_code, "");
	lv_label_set_text(login_message, "");
	lv_obj_set_hidden(login_qr, true);
	lv_obj_set_hidden(login_spin, false);
	switch_screen(login_screen);
	run(JOB_LOGIN_BEGIN, NULL, NULL, 0);
}

static void login_unloaded_cb(lv_event_t *e) {
	(void)e;
	lv_timer_pause(login_timer);
}

static void build_login_page(gui_config_t *cfg) {
	login_screen = lv_obj_create(NULL);
	lv_obj_add_style(login_screen, &theme_style_screen, 0);
	settingsrow_title(login_screen, cfg, "yandex_sign_in");
	int top = settingsrow_content_top(cfg);

	lv_obj_t *box = lv_obj_create(login_screen);
	lv_obj_set_size(box, cfg->screen_width - 2 * cfg->padding, LV_SIZE_CONTENT);
	lv_obj_align(box, LV_ALIGN_TOP_MID, 0, top + 10);
	lv_obj_set_style_bg_opa(box, 0, 0);
	lv_obj_set_style_border_width(box, 0, 0);
	lv_obj_set_style_pad_all(box, 0, 0);
	lv_obj_set_style_pad_row(box, 20, 0);
	lv_obj_set_scrollable(box, false);
	lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	// Black on white with a white quiet zone, for the same reasons as on the
	// Tidal page: a QR is an optical target, not decoration.
	login_qr = lv_qrcode_create(box);
	lv_qrcode_set_size(login_qr, LOGIN_QR_SIZE);
	lv_qrcode_set_dark_color(login_qr, lv_color_black());
	lv_qrcode_set_light_color(login_qr, lv_color_white());
	lv_obj_set_style_border_color(login_qr, lv_color_white(), 0);
	lv_obj_set_style_border_width(login_qr, 10, 0);

	login_code = lv_label_create(box);
	lv_obj_set_style_text_font(login_code, &font_ui_36_bold, 0);
	lv_obj_set_style_text_letter_space(login_code, 5, 0);
	lv_obj_add_style(login_code, &theme_style_text, 0);
	lv_label_set_text(login_code, "");

	login_spin = spinner_create(box, &icon_loader_big);

	login_message = lv_label_create(box);
	lv_label_set_long_mode(login_message, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(login_message, lv_pct(100));
	lv_obj_set_style_text_align(login_message, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(login_message, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(login_message, &font_ui_20, 0);
	lv_label_set_text(login_message, "");

	login_timer = lv_timer_create(login_tick_cb, 5000, NULL);
	lv_timer_pause(login_timer);
	lv_obj_add_event_cb(login_screen, login_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(login_screen);
}

// ---------------------------------------------------------------------------
// search
// ---------------------------------------------------------------------------

static void search_accept_cb(lv_event_t *e) {
	(void)e;
	const char *text = lv_textarea_get_text(search_field);
	if (text && text[0]) {
		open_list(text, JOB_SEARCH, text, NULL);
	}
}

static void build_search_page(gui_config_t *cfg) {
	search_screen = lv_obj_create(NULL);
	lv_obj_add_style(search_screen, &theme_style_screen, 0);
	settingsrow_title(search_screen, cfg, "yandex_search");
	int top = settingsrow_content_top(cfg);
	search_field = lv_textarea_create(search_screen);
	lv_textarea_set_one_line(search_field, true);
	lv_textarea_set_placeholder_text(search_field, tr("artist_album_or_track"));
	lv_obj_set_size(search_field, cfg->screen_width - 2 * cfg->padding, 62);
	lv_obj_align(search_field, LV_ALIGN_TOP_LEFT, cfg->padding, top);
	lv_obj_add_style(search_field, &theme_style_card, 0);
	lv_obj_set_style_radius(search_field, 12, 0);
	lv_obj_set_style_border_width(search_field, 0, 0);
	lv_obj_set_style_pad_all(search_field, 14, 0);
	lv_obj_set_style_text_font(search_field, &font_ui_24, 0);
	keyboard_style_caret(search_field);
	keyboard_create(search_screen, cfg->screen_width, 316, search_field, NULL, "ok", search_accept_cb, NULL);
	switcher_attach_back_gesture(search_screen);
}

// ---------------------------------------------------------------------------
// home
// ---------------------------------------------------------------------------

static void job_done_async(void *user) {
	job_result_t *r = user;
	if (r->kind == JOB_LOGIN_BEGIN || r->kind == JOB_LOGIN_POLL) {
		login_result(r);
		free(r);
		return;
	}
	if (r->kind == JOB_DISLIKE) {
		if (r->error[0]) {
			toast_error(r->error);
		}
		free(r);
		return;
	}
	if (r->kind == JOB_WAVE_MORE) {
		// New tracks at the end of the queue: guessed names, fetched as
		// playback nears them.
		for (int i = 0; i < r->count && queued_count < MAX_ROWS; i++) {
			queued[queued_count] = r->rows[i].u.track;
			yandexcache_path(queued[queued_count].id, "flac", queued_paths[queued_count], sizeof(queued_paths[0]));
			playlist_insert_at(queued_paths[queued_count], playlist_count());
			queued_count++;
		}
		device_state_queue_changed();
		wave_asked_more = false;
		free(r);
		return;
	}
	// A list the user has since left (or asked for again): another answer is
	// on its way, and this one would only flash past -- or, arriving last,
	// stay up under the wrong title.
	if (r->generation != list_generation) {
		free(r);
		return;
	}
	lv_obj_set_hidden(list_spin, true);
	free(shown);
	shown = r;
	rows = r->rows;
	row_count = r->count;
	fill_list();
	// The rows' menus say "Like" or "Remove like" from the account's list:
	// asked for now if it is not here yet (once a session, see yandexsync).
	yandexsync_likes_refresh(NULL);
	// My Wave plays as soon as it has something: that is what the button means.
	if (r->kind == JOB_WAVE && row_count > 0) {
		play_rows(0);
	}
}

static void open_wave_cb(lv_event_t *e) {
	(void)e;
	open_list(tr("yandex_my_wave"), JOB_WAVE, NULL, NULL);
}
static void open_liked_cb(lv_event_t *e) {
	(void)e;
	open_list(tr("yandex_liked"), JOB_LIKED, NULL, NULL);
}
static void open_playlists_cb(lv_event_t *e) {
	(void)e;
	open_list(tr("yandex_playlists"), JOB_PLAYLISTS, NULL, NULL);
}
static void open_search_cb(lv_event_t *e) {
	(void)e;
	switch_screen(search_screen);
}
static void logout_cb(lv_event_t *e) {
	(void)e;
	yandexdl_cancel_all(); // the account's files are not to be fetched any more
	yandex_logout();
	queue_is_wave = false;
	paint_home();
}

// Streaming quality, as pills on the home page (the Qobuz page's choice, made
// once and left alone). Below them, what the last track really came as: the
// service picks the file from what is offered, and "Lossless" on a track that
// only exists as AAC is otherwise a silent surprise -- as is an account
// without Plus, which gets no lossless at all.
static lv_obj_t *quality_pills[3];
static lv_obj_t *quality_format;

static void quality_refresh(void) {
	yandex_quality_t current = yandex_get_quality();
	for (int i = 0; i < 3; i++) {
		if (quality_pills[i]) {
			settingsrow_pill_active(quality_pills[i], (int)current == i);
		}
	}
	char format[48];
	yandex_last_format(format, sizeof(format));
	if (current == YANDEX_QUALITY_LOSSLESS && yandex_needs_plus()) {
		lv_label_set_text(quality_format, tr("yandex_needs_plus"));
	} else if (format[0]) {
		lv_label_set_text_fmt(quality_format, tr("yandex_last_format"), format);
	} else {
		lv_label_set_text(quality_format, tr("yandex_quality_note"));
	}
}

static void quality_pick_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	yandex_set_quality((yandex_quality_t)(intptr_t)lv_event_get_user_data(e));
	quality_refresh();
}

// ---------------------------------------------------------------------------
// saving to the library: what the worker says, and a line on the home page
//
// The line and its once-a-second timer exist only while something is being
// saved: the timer deletes itself when the queue is done.
// ---------------------------------------------------------------------------

static lv_obj_t *save_status;
static lv_timer_t *save_timer;

static void save_status_refresh(void) {
	yandexdl_status_t st;
	yandexdl_status(&st);
	if (!st.active || !st.title[0]) {
		lv_obj_set_hidden(save_status, true);
		return;
	}
	char line[200];
	int current = st.done + st.failed + 1;
	snprintf(line, sizeof(line), tr("yandex_saving"), current, current + st.queued, st.title);
	if (st.percent >= 0) {
		size_t used = strlen(line);
		snprintf(line + used, sizeof(line) - used, " \xC2\xB7 %d%%", st.percent);
	}
	lv_label_set_text(save_status, line);
	lv_obj_set_hidden(save_status, false);
}

static void save_timer_cb(lv_timer_t *timer) {
	(void)timer;
	save_status_refresh();
	if (!yandexdl_busy()) {
		lv_timer_delete(save_timer);
		save_timer = NULL;
	}
}

typedef struct {
	yandexdl_event_t event;
	int count;
	char text[400];
} save_note_t;

static void save_note_async(void *user) {
	save_note_t *note = user;
	char line[480];
	switch (note->event) {
	case YANDEXDL_QUEUED:
		snprintf(line, sizeof(line), tr("yandex_save_queued"), note->text);
		toast_success(line);
		if (!save_timer) {
			save_timer = lv_timer_create(save_timer_cb, 1000, NULL);
		}
		break;
	case YANDEXDL_FAILED:
		toast_error(note->text);
		break;
	case YANDEXDL_FINISHED:
		if (note->count > 0) {
			snprintf(line, sizeof(line), tr("yandex_saved_to"), note->text, note->count);
			toast_success(line);
		}
		break;
	case YANDEXDL_STOPPED:
		toast_plain(note->text);
		break;
	}
	save_status_refresh();
	free(note);
}

// From the save worker (or whoever cancelled): bounced to the interface.
static void save_listener(yandexdl_event_t event, int count, const char *text) {
	save_note_t *note = malloc(sizeof(*note));
	if (!note) {
		return;
	}
	note->event = event;
	note->count = count;
	snprintf(note->text, sizeof(note->text), "%s", text ? text : "");
	gui_post(save_note_async, note);
}

static void paint_home(void) {
	save_status_refresh();
	bool in = yandex_logged_in();
	lv_obj_set_hidden(home_signed_in, !in);
	lv_obj_set_hidden(home_signed_out, in);
	lv_label_set_text(home_user, yandex_display_name());
	quality_refresh();
}

static void home_loaded_cb(lv_event_t *e) {
	(void)e;
	paint_home();
}

static lv_obj_t *column(lv_obj_t *parent, gui_config_t *cfg, int top) {
	lv_obj_t *c = lv_obj_create(parent);
	lv_obj_set_size(c, cfg->screen_width - 2 * cfg->padding, cfg->screen_height - top - 10);
	lv_obj_align(c, LV_ALIGN_TOP_MID, 0, top);
	lv_obj_set_style_bg_opa(c, 0, 0);
	lv_obj_set_style_border_width(c, 0, 0);
	lv_obj_set_style_pad_all(c, 0, 0);
	lv_obj_set_style_pad_row(c, 12, 0);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	return c;
}

bool yandexpage_available(void) { return yandex_configured(); }

bool yandexpage_open_album(const char *album_id, const char *title) {
	if (!album_id || !album_id[0] || !yandex_logged_in()) {
		return false;
	}
	open_list(title && title[0] ? title : tr("yandex"), JOB_ALBUM, album_id, NULL);
	return true;
}

static void *provider_open_album(const char *album_id, const char *title) { return yandexpage_open_album(album_id, title) ? list_screen : NULL; }

void yandexpage_init(gui_config_t *cfg) {
	// yandex.c is started from main() with the other services, and the cache
	// follows the card through the registry (stream_provider_set_root_all):
	// here only the parts that need the GUI -- the mark over the artwork and
	// "Show album" (provider.h).
	const stream_provider_gui_t provider_gui = {.badge = &icon_yandex_badge, .open_album = provider_open_album};
	stream_provider_attach_gui("yandex", &provider_gui);

	yandex_screen = lv_obj_create(NULL);
	lv_obj_add_style(yandex_screen, &theme_style_screen, 0);
	settingsrow_title(yandex_screen, cfg, "yandex");
	int top = settingsrow_content_top(cfg);

	home_signed_in = column(yandex_screen, cfg, top);
	settingsrow_action(home_signed_in, "yandex_my_wave", open_wave_cb, NULL);
	settingsrow_action(home_signed_in, "yandex_liked", open_liked_cb, NULL);
	settingsrow_action(home_signed_in, "yandex_playlists", open_playlists_cb, NULL);
	settingsrow_action(home_signed_in, "yandex_search", open_search_cb, NULL);
	// What is being saved to the library, while something is: under the
	// rows, where it is seen without scrolling.
	save_status = lv_label_create(home_signed_in);
	lv_label_set_long_mode(save_status, LV_LABEL_LONG_DOT);
	lv_obj_set_width(save_status, lv_pct(100));
	lv_obj_add_style(save_status, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(save_status, &font_ui_18, 0);
	lv_obj_set_hidden(save_status, true);
	yandexdl_set_listener(save_listener);
	lv_obj_t *pills;
	lv_obj_t *quality_card = settingsrow_pills(home_signed_in, "audio_quality", &pills);
	quality_pills[YANDEX_QUALITY_LOSSLESS] = settingsrow_pill(pills, "yandex_quality_lossless", YANDEX_QUALITY_LOSSLESS, quality_pick_cb);
	quality_pills[YANDEX_QUALITY_HIGH] = settingsrow_pill(pills, "yandex_quality_high", YANDEX_QUALITY_HIGH, quality_pick_cb);
	quality_pills[YANDEX_QUALITY_NORMAL] = settingsrow_pill(pills, "yandex_quality_normal", YANDEX_QUALITY_NORMAL, quality_pick_cb);
	quality_format = lv_label_create(quality_card);
	lv_label_set_long_mode(quality_format, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(quality_format, lv_pct(100));
	lv_obj_add_style(quality_format, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(quality_format, &font_ui_18, 0);
	theme_register_refresh(quality_refresh);
	settingsrow_action(home_signed_in, "yandex_sign_out", logout_cb, NULL);
	home_user = lv_label_create(home_signed_in);
	lv_obj_add_style(home_user, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(home_user, &font_ui_18, 0);

	home_signed_out = column(yandex_screen, cfg, top);
	settingsrow_action(home_signed_out, "yandex_sign_in", open_login_cb, NULL);

	lv_obj_add_event_cb(yandex_screen, home_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(yandex_screen);
	paint_home();

	build_login_page(cfg);
	build_list_page(cfg);
	build_search_page(cfg);

	device_state_add_prepare_cb(yandexpage_prepare_track);
	lv_timer_create(queue_watch_cb, 1000, NULL);
}

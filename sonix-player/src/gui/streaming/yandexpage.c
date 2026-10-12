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
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/spinner.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"
#include "src/system/decode/growfile.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/streamturn.h"
#include "src/system/streaming/yandex.h"
#include "src/system/streaming/yandexcache.h"

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
	JOB_LIKE,
	JOB_DISLIKE,
} job_kind_t;

typedef struct {
	job_kind_t kind;
	char a[YANDEX_TITLE_MAX]; // id, owner, search text...
	char b[YANDEX_ID_MAX];	  // kind, album id
	int n;					  // index, or the feedback event
	int secs;				  // feedback: seconds played
} job_t;

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
static lv_obj_t *list_title, *list_box, *list_spin, *wave_bar;
static gui_config_t *list_cfg;
// search
static lv_obj_t *search_field;

// The rows on screen (GUI thread) and the worker's result being built
// (worker thread); swapped in job_done.
static row_t *rows, *result;
static int row_count, result_count;
static bool list_is_wave;
static char result_error[256];
static job_kind_t result_kind;
static yandex_login_state_t login_state;

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
// Feedback and likes queue up instead: each one is an event the service
// should hear about, and a skip lost behind the next trackStarted would teach
// My Wave the wrong thing.
#define FEEDBACK_MAX 8
static job_t feedback[FEEDBACK_MAX];
static int feedback_count;
static bool worker_started;

static void job_done_async(void *user);
static void fetch_done_async(void *user);
static void play_done_async(void *user);

static void submit(const job_t *job) {
	pthread_mutex_lock(&job_lock);
	if (job->kind == JOB_FEEDBACK || job->kind == JOB_LIKE || job->kind == JOB_DISLIKE) {
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
	if (!rows) {
		rows = calloc(MAX_ROWS, sizeof(*rows));
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

static void *worker_main(void *arg) {
	(void)arg;
	for (;;) {
		pthread_mutex_lock(&job_lock);
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
		pthread_mutex_unlock(&job_lock);

		result_error[0] = '\0';
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
		case JOB_LIKE:
			if (!yandex_like_track(job.a, job.b, job.n != 0)) {
				snprintf(result_error, sizeof(result_error), "%s", yandex_last_error());
			}
			break;
		case JOB_DISLIKE:
			if (!yandex_dislike_track(job.a, job.b)) {
				snprintf(result_error, sizeof(result_error), "%s", yandex_last_error());
			}
			break;
		case JOB_PLAY: {
			// The tapped track first; the rest download as playback reaches
			// them (yandexpage_prepare_track) or ahead of it (JOB_FETCH).
			streamturn_abandon_all();
			char path[512];
			bool ok = fetch_queued(job.n, path, sizeof(path));
			if (ok) {
				snprintf(queued_paths[job.n], sizeof(queued_paths[0]), "%s", path);
			}
			gui_post(play_done_async, ok ? (void *)(intptr_t)(job.n + 1) : NULL);
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
		result_kind = job.kind;
		gui_post(job_done_async, NULL);
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
	job_t job = {.kind = kind, .n = n};
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

// Asks for queue entry `index` ahead of playback, unless it is already on the
// card or on its way: starting it again would abandon the download in flight.
static void fetch_ahead(int index) {
	if (index < 0 || index >= queued_count) {
		return;
	}
	char busy[YANDEX_ID_MAX];
	yandexcache_downloading_id(busy, sizeof(busy));
	if (strcmp(busy, queued[index].id) == 0 || yandexcache_find(queued[index].id, NULL, 0)) {
		return;
	}
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
	lv_obj_set_hidden(list_spin, true);
	int index = (int)(intptr_t)user - 1;
	if (index < 0) {
		toast_error(result_error[0] ? result_error : tr("download_failed"));
		return;
	}
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
	device_state_stop();
	run(JOB_FETCH, NULL, NULL, index);
	gui_notify_popup("downloading");
	return false;
}

// My Wave bookkeeping, once a second: what started, what was skipped, and a
// new batch when the queue runs low.
static char wave_current[YANDEX_ID_MAX], wave_current_album[YANDEX_ID_MAX];
static time_t wave_started_at;
static int wave_duration;
static bool wave_asked_more;

static void wave_watch_cb(lv_timer_t *timer) {
	(void)timer;
	if (!queue_is_wave) {
		return;
	}
	device_state_t state;
	device_state_get(&state);
	char id[YANDEX_ID_MAX];
	yandexcache_track_id(state.current_file, id, sizeof(id));
	if (!id[0] || strcmp(id, wave_current) == 0) {
		return;
	}
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
	int index = queue_index_of(state.current_file);
	snprintf(wave_current_album, sizeof(wave_current_album), "%s", index >= 0 ? queued[index].album_id : "");
	wave_duration = index >= 0 ? queued[index].duration : 0;
	wave_started_at = time(NULL);
	run(JOB_FEEDBACK, id, wave_current_album, YANDEX_WAVE_TRACK_STARTED);
	if (index >= 0 && index + 2 >= queued_count && !wave_asked_more) {
		wave_asked_more = true;
		run(JOB_WAVE_MORE, id, NULL, 0);
	} else if (index >= 0) {
		fetch_ahead(index + 1);
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
	lv_obj_set_style_pad_hor(row, 16, 0);
	lv_obj_set_style_pad_row(row, 4, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_event_bubble(row, true);
	lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text(label, name);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);

	lv_obj_t *sub = lv_label_create(row);
	lv_label_set_text(sub, detail);
	lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
	lv_obj_set_width(sub, lv_pct(100));
	lv_obj_add_style(sub, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(sub, &font_ui_18, 0);
}

static void fill_list(void) {
	lv_obj_clean(list_box);
	for (int i = 0; i < row_count; i++) {
		make_row(i);
	}
	if (row_count == 0) {
		lv_obj_t *empty = lv_label_create(list_box);
		lv_label_set_text(empty, result_error[0] ? result_error : tr("yandex_empty"));
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
	lv_obj_set_hidden(wave_bar, !list_is_wave);
	// The list starts under the like/dislike bar only when there is one.
	int top = settingsrow_content_top(list_cfg);
	int y = top + (list_is_wave ? 66 : 0);
	lv_obj_set_pos(list_box, list_cfg->padding, y);
	lv_obj_set_height(list_box, list_cfg->screen_height - y - 4);
	lv_obj_clean(list_box);
	row_count = 0;
	lv_obj_set_hidden(list_spin, false);
	switch_screen(list_screen);
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

// Like and dislike act on what is playing now, from My Wave.
static void wave_rate_cb(lv_event_t *e) {
	bool like = lv_event_get_user_data(e) != NULL;
	device_state_t state;
	device_state_get(&state);
	int index = queue_index_of(state.current_file);
	if (index < 0) {
		return;
	}
	run(like ? JOB_LIKE : JOB_DISLIKE, queued[index].id, queued[index].album_id, 1);
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

static void login_result(void) {
	if (result_kind == JOB_LOGIN_BEGIN) {
		lv_obj_set_hidden(login_spin, true);
		if (login_state == YANDEX_LOGIN_ERROR) {
			lv_label_set_text(login_message, result_error);
			return;
		}
		lv_label_set_text(login_code, login.user_code);
		lv_qrcode_update(login_qr, login.verification_url, (uint32_t)strlen(login.verification_url));
		lv_obj_set_hidden(login_qr, false);
		lv_label_set_text_fmt(login_message, "%s\n%s", tr("yandex_login_hint"), login.verification_url);
		login_deadline = time(NULL) + login.expires_secs;
		lv_timer_set_period(login_timer, (uint32_t)login.interval_secs * 1000);
		lv_timer_resume(login_timer);
		return;
	}
	switch (login_state) {
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
		lv_label_set_text(login_message, result_error);
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
	(void)user;
	if (result_kind == JOB_LOGIN_BEGIN || result_kind == JOB_LOGIN_POLL) {
		login_result();
		return;
	}
	if (result_kind == JOB_LIKE || result_kind == JOB_DISLIKE) {
		if (result_error[0]) {
			toast_error(result_error);
		}
		return;
	}
	if (result_kind == JOB_WAVE_MORE) {
		// New tracks at the end of the queue: guessed names, fetched as
		// playback nears them.
		for (int i = 0; i < result_count && queued_count < MAX_ROWS; i++) {
			queued[queued_count] = result[i].u.track;
			yandexcache_path(queued[queued_count].id, "flac", queued_paths[queued_count], sizeof(queued_paths[0]));
			playlist_insert_at(queued_paths[queued_count], playlist_count());
			queued_count++;
		}
		device_state_queue_changed();
		wave_asked_more = false;
		return;
	}
	lv_obj_set_hidden(list_spin, true);
	row_t *swap = rows;
	rows = result;
	result = swap;
	row_count = result_count;
	fill_list();
	// My Wave plays as soon as it has something: that is what the button means.
	if (result_kind == JOB_WAVE && row_count > 0) {
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
	yandex_logout();
	queue_is_wave = false;
	paint_home();
}

static void paint_home(void) {
	bool in = yandex_logged_in();
	lv_obj_set_hidden(home_signed_in, !in);
	lv_obj_set_hidden(home_signed_out, in);
	lv_label_set_text(home_user, yandex_display_name());
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

void yandexpage_init(gui_config_t *cfg) {
	yandex_init();
	yandexcache_set_root(cfg->sd_root_path);

	yandex_screen = lv_obj_create(NULL);
	lv_obj_add_style(yandex_screen, &theme_style_screen, 0);
	settingsrow_title(yandex_screen, cfg, "yandex");
	int top = settingsrow_content_top(cfg);

	home_signed_in = column(yandex_screen, cfg, top);
	settingsrow_action(home_signed_in, "yandex_my_wave", open_wave_cb, NULL);
	settingsrow_action(home_signed_in, "yandex_liked", open_liked_cb, NULL);
	settingsrow_action(home_signed_in, "yandex_playlists", open_playlists_cb, NULL);
	settingsrow_action(home_signed_in, "yandex_search", open_search_cb, NULL);
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
	lv_timer_create(wave_watch_cb, 1000, NULL);
}

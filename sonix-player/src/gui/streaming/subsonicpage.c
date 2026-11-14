#include "subsonicpage.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/spinner.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/system/core/lang.h"
#include "src/system/device/system.h"
#include "src/system/net/wifi.h"
#include "src/system/net/wifitransfer.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/subsonic.h"
#include "src/system/streaming/subsonicaccount.h"
#include "src/system/streaming/subsoniccache.h"

// ---------------------------------------------------------------------------
// Subsonic front end: Navidrome, Airsonic, Gonic, Supysonic.
//
// Three screens and one worker:
//
//   subsonic_screen  the account, and the way into the library
//   account_screen   the server, the user and the password
//   list_screen      artists, their albums, an album's songs
//
// Everything that talks to the network blocks (see subsonic.h), so none of it
// runs on the GUI thread: there is a single worker with a single job slot, and
// results come back through gui_post(). Playback is the exception the other
// services already make -- the fetch for a queued song runs on a thread of its
// own, because it lasts as long as the file.
//
// What this page deliberately does not do, in this first version: no covers, no
// search, no playlists, no favourites. It connects, it browses, it plays.
// ---------------------------------------------------------------------------

// How many entries one list holds. Every entry is static memory on a device
// with 64 MB in total, so the lists stop here rather than at the first hundred
// thousand songs. The server returns them alphabetically, which is what makes a
// cut-off list a short one and not a random one.
#define SUBSONIC_LIST_MAX 200

#define LIST_ROW_H 88

// Global rather than hidden behind a function: the Streaming grid wants the
// address of the variable (grid_entry_t.target), as for every other page.
lv_obj_t *subsonic_screen;

// ---------------------------------------------------------------------------
// the worker
// ---------------------------------------------------------------------------

typedef enum {
	JOB_NONE = 0,
	JOB_CONNECT,
	JOB_ARTISTS,
	JOB_ALBUMS,
	JOB_SONGS,
	JOB_PLAY,
} job_kind_t;

typedef struct {
	job_kind_t kind;
	char id[SUBSONIC_ID_MAX];	  // artist or album identifier
	char text[SUBSONIC_NAME_MAX]; // what the list is called
	char server[SUBSONIC_URL_MAX];
	char user[SUBSONIC_USER_MAX];
	char password[SUBSONIC_PASSWORD_MAX];
	int index; // which song playback starts from
} job_t;

static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_ready = PTHREAD_COND_INITIALIZER;
static job_t pending;
static bool have_pending;
static bool worker_started;

// What came back. Only the worker writes here, and only while the GUI is not
// reading (it reads inside the async, which runs after the worker is done).
//
// One union rather than three arrays: only one of the three lists is on screen
// at any moment, and three arrays would be three times the memory for nothing.
static union {
	subsonic_artist_t artists[SUBSONIC_LIST_MAX];
	subsonic_album_t albums[SUBSONIC_LIST_MAX];
	subsonic_song_t songs[SUBSONIC_LIST_MAX];
} results;
static int result_count;
static job_kind_t result_kind;
static char result_error[256];
static char result_title[SUBSONIC_NAME_MAX];

// What the list is showing right now, so a row tap knows what to do.
static job_kind_t list_kind;

static void submit(const job_t *job) {
	pthread_mutex_lock(&job_lock);
	pending = *job;
	have_pending = true;
	pthread_cond_signal(&job_ready);
	pthread_mutex_unlock(&job_lock);
}

// ---------------------------------------------------------------------------
// the wait indicator
// ---------------------------------------------------------------------------

static lv_obj_t *busy_layer;

static void busy_hide(void) {
	if (busy_layer) {
		lv_obj_set_hidden(busy_layer, true);
	}
}

static void busy_show(void) {
	if (busy_layer) {
		lv_obj_set_hidden(busy_layer, false);
		lv_obj_move_foreground(busy_layer);
	}
}

static void build_busy_layer(void) {
	busy_layer = lv_obj_create(lv_layer_sys());
	lv_obj_set_size(busy_layer, lv_pct(100), lv_pct(100));
	lv_obj_set_pos(busy_layer, 0, 0);
	lv_obj_set_style_bg_color(busy_layer, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(busy_layer, LV_OPA_50, 0);
	lv_obj_set_style_border_width(busy_layer, 0, 0);
	lv_obj_set_style_radius(busy_layer, 0, 0);
	lv_obj_set_scrollable(busy_layer, false);
	lv_obj_set_hidden(busy_layer, true);
	lv_obj_set_flex_flow(busy_layer, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(busy_layer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *spin = spinner_create(busy_layer, &icon_loader_big);
	lv_obj_set_style_image_recolor(spin, lv_color_white(), 0);
	lv_obj_set_style_image_recolor_opa(spin, LV_OPA_COVER, 0);
}

// ---------------------------------------------------------------------------
// the result list
// ---------------------------------------------------------------------------

static lv_obj_t *list_screen;
static lv_obj_t *list_container;
static lv_obj_t *list_title;
static lv_obj_t *list_empty;

static void rebuild_home(void);
static void row_clicked_cb(lv_event_t *e);

// "3:42"
static void duration_text(int seconds, char *out, size_t size) {
	if (seconds <= 0) {
		out[0] = '\0';
		return;
	}
	snprintf(out, size, "%d:%02d", seconds / 60, seconds % 60);
}

// One list row: name on top, detail below. No cover in this version: fetching
// one per row is a request per row, and the first version is about connecting
// and playing.
static lv_obj_t *make_row(const char *name, const char *detail, int index) {
	lv_obj_t *row = lv_btn_create(list_container);
	lv_obj_set_size(row, lv_pct(100), LIST_ROW_H);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, 12, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, 16, 0);
	lv_obj_set_style_pad_ver(row, 0, 0);
	lv_obj_set_style_pad_column(row, 8, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	// Gestures (back, and pulling in the player sheet) often start on a row,
	// and a button keeps the press to itself: without this neither drag works
	// on these lists.
	lv_obj_set_event_bubble(row, true);
	lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

	lv_obj_t *texts = lv_obj_create(row);
	lv_obj_set_flex_grow(texts, 1);
	lv_obj_set_height(texts, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(texts, 0, 0);
	lv_obj_set_style_border_width(texts, 0, 0);
	lv_obj_set_style_pad_all(texts, 0, 0);
	lv_obj_set_style_pad_row(texts, 6, 0);
	lv_obj_set_scrollable(texts, false);
	lv_obj_set_event_bubble(texts, true);
	lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(texts, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_obj_t *label = lv_label_create(texts);
	lv_label_set_text(label, name);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_set_height(label, lv_font_get_line_height(&font_ui_22));
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);

	if (detail && detail[0]) {
		lv_obj_t *sub = lv_label_create(texts);
		lv_label_set_text(sub, detail);
		lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
		lv_obj_set_width(sub, lv_pct(100));
		lv_obj_set_height(sub, lv_font_get_line_height(&font_ui_18));
		lv_obj_add_style(sub, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(sub, &font_ui_18, 0);
	}

	return row;
}

static void fill_list(void) {
	lv_obj_clean(list_container);
	list_kind = result_kind;

	for (int i = 0; i < result_count; i++) {
		char detail[SUBSONIC_NAME_MAX + 32];

		switch (result_kind) {
		case JOB_ARTISTS: {
			const subsonic_artist_t *a = &results.artists[i];
			// "12 albums" and not "1 albums": the count only when it is not one
			// reads as a fact about the artist rather than a label to translate
			// with a plural rule this interface has no place for.
			if (a->album_count > 1) {
				snprintf(detail, sizeof(detail), "%d %s", a->album_count, tr("albums"));
			} else {
				detail[0] = '\0';
			}
			make_row(a->name, detail, i);
			break;
		}
		case JOB_ALBUMS: {
			const subsonic_album_t *a = &results.albums[i];
			if (a->year > 0) {
				snprintf(detail, sizeof(detail), "%s \xC2\xB7 %d", a->artist, a->year);
			} else {
				snprintf(detail, sizeof(detail), "%s", a->artist);
			}
			make_row(a->name, detail, i);
			break;
		}
		case JOB_SONGS: {
			const subsonic_song_t *s = &results.songs[i];
			char length[16];
			duration_text(s->duration, length, sizeof(length));
			if (s->track > 0 && length[0]) {
				snprintf(detail, sizeof(detail), "%d \xC2\xB7 %s", s->track, length);
			} else {
				snprintf(detail, sizeof(detail), "%s", length);
			}
			make_row(s->title, detail, i);
			break;
		}
		default:
			break;
		}
	}

	lv_obj_set_hidden(list_empty, result_count != 0);
	lv_label_set_text(list_title, result_title);
}

// ---------------------------------------------------------------------------
// playback
//
// The tapped song is fetched before it plays: the cache publishes a file only
// when the whole of it has arrived, so unlike Qobuz and Tidal there is nothing
// to start halfway through. The rest of the album is queued by the path each
// song WILL have, and the prepare callback fetches them one at a time as the
// queue reaches them.
// ---------------------------------------------------------------------------

static subsonic_song_t queued_songs[SUBSONIC_LIST_MAX];
static char queued_paths[SUBSONIC_LIST_MAX][512];
static const char *queued_ptr[SUBSONIC_LIST_MAX];
static int queued_count;

// The path playback is stopped on, waiting for its file.
static char awaiting_path[512];
static bool waiting_for_track;

static pthread_mutex_t fetch_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fetch_ready = PTHREAD_COND_INITIALIZER;
static int fetch_want = -1;
static bool fetch_started;

static void resume_if_waiting_async(void *user) {
	(void)user;
	if (!waiting_for_track) {
		return;
	}
	if (wifitransfer_get_enabled()) {
		return; // a file server is running on the card this track is written to
	}
	int index = playlist_current_index();
	char path[512];
	if (index < 0 || !playlist_path_at(index, path, sizeof(path))) {
		return;
	}
	if (strcmp(path, awaiting_path) != 0) {
		return; // something else arrived, not what the queue is waiting for
	}
	waiting_for_track = false;
	fprintf(stderr, "subsonic: restarting the queue from slot %d ('%s')\n", index, path);
	device_state_play_queue_index(index);
	player_refresh_now_playing();
}

static bool song_is_cached(const char *song_id) { return subsoniccache_has(song_id); }

// Called by device_state an instant before a file is started. Same contract as
// the Qobuz and Tidal pages: false stops the load, and the queue is restarted
// by resume_if_waiting_async() once the file is here.
static bool subsonic_prepare_track(const char *path) {
	if (!subsoniccache_owns(path)) {
		waiting_for_track = false;
		return true; // not one of ours
	}

	int index = -1;
	for (int i = 0; i < queued_count; i++) {
		if (strcmp(queued_ptr[i], path) == 0) {
			index = i;
			break;
		}
	}
	if (index < 0) {
		// A cache file the queue does not know about: nothing to fetch, since
		// fetching needs the song's identifier. Let the decoder try it.
		return true;
	}
	if (song_is_cached(queued_songs[index].id)) {
		waiting_for_track = false;
		return true;
	}

	fprintf(stderr, "subsonic: '%s' not ready, stopping and fetching it\n", path);
	subsoniccache_set_network_wanted(true);
	snprintf(awaiting_path, sizeof(awaiting_path), "%s", path);
	waiting_for_track = true;
	device_state_stop();

	pthread_mutex_lock(&fetch_lock);
	fetch_want = index;
	pthread_cond_signal(&fetch_ready);
	pthread_mutex_unlock(&fetch_lock);

	gui_notify_popup("loading");
	return false;
}

// The fetch thread: one song at a time, the last request wins. A new request
// arriving mid-download is served as soon as the running one is done -- the
// fetch itself cannot be cancelled without leaving a partial file behind.
static void *fetch_main(void *arg) {
	(void)arg;

	for (;;) {
		pthread_mutex_lock(&fetch_lock);
		while (fetch_want < 0) {
			pthread_cond_wait(&fetch_ready, &fetch_lock);
		}
		int index = fetch_want;
		fetch_want = -1;
		pthread_mutex_unlock(&fetch_lock);

		if (index < 0 || index >= queued_count) {
			continue;
		}

		const subsonic_song_t *song = &queued_songs[index];
		bool ok = subsoniccache_fetch(song->id, song->suffix);
		if (!ok) {
			const char *why = subsonic_last_error();
			fprintf(stderr, "subsonic: '%s' did not download: %s\n", song->title, why && why[0] ? why : "?");
		}

		// Every queued song is asked for as playback reaches it, so there is
		// nothing left to fetch when this returns: the radio may park again.
		bool more;
		pthread_mutex_lock(&fetch_lock);
		more = fetch_want >= 0;
		pthread_mutex_unlock(&fetch_lock);
		if (!more) {
			subsoniccache_set_network_wanted(false);
		}

		if (ok) {
			gui_post(resume_if_waiting_async, NULL);
		}
	}
	return NULL;
}

static void start_fetch_thread(void) {
	if (fetch_started) {
		return;
	}
	pthread_t thread;
	if (pthread_create(&thread, NULL, fetch_main, NULL) == 0) {
		pthread_detach(thread);
		fetch_started = true;
	}
}

static void play_started_async(void *user) {
	(void)user;
	player_refresh_now_playing();
	switch_screen(player_screen);
}

static void do_play(int index) {
	if (index < 0 || index >= result_count) {
		return;
	}

	subsoniccache_set_root(storage_sd_root());
	if (!subsoniccache_ready()) {
		snprintf(result_error, sizeof(result_error), "%s", tr("download_failed"));
		return;
	}

	// The whole list is queued now, each song under the name its file will
	// have. Only the tapped one has to be here before the first note.
	queued_count = 0;
	int start = 0;
	for (int i = 0; i < result_count && queued_count < SUBSONIC_LIST_MAX; i++) {
		const subsonic_song_t *s = &results.songs[i];
		char path[512];
		if (!subsoniccache_path(s->id, s->suffix, path, sizeof(path))) {
			continue;
		}
		if (i == index) {
			start = queued_count;
		}
		queued_songs[queued_count] = *s;
		snprintf(queued_paths[queued_count], sizeof(queued_paths[0]), "%s", path);
		queued_ptr[queued_count] = queued_paths[queued_count];
		queued_count++;
	}
	if (queued_count == 0) {
		snprintf(result_error, sizeof(result_error), "%s", tr("download_failed"));
		return;
	}

	start_fetch_thread();
	subsoniccache_set_network_wanted(true);

	const subsonic_song_t *wanted = &results.songs[index];
	if (!song_is_cached(wanted->id) && !subsoniccache_fetch(wanted->id, wanted->suffix)) {
		const char *why = subsonic_last_error();
		snprintf(result_error, sizeof(result_error), "%s", why && why[0] ? why : tr("download_failed"));
		subsoniccache_set_network_wanted(false);
		return;
	}

	device_state_play_list(queued_ptr, queued_count, start);
	gui_post(play_started_async, NULL);

	// Once the first song is here only the rest of the queue may still need the
	// network, and the prepare callback asks for it when it does.
	bool missing = false;
	for (int i = 0; i < queued_count && !missing; i++) {
		if (!song_is_cached(queued_songs[i].id)) {
			missing = true;
		}
	}
	if (!missing) {
		subsoniccache_set_network_wanted(false);
	}
}

// ---------------------------------------------------------------------------
// what comes back from the worker, on the GUI thread
// ---------------------------------------------------------------------------

static void job_done_cb(void *user) {
	(void)user;
	busy_hide();

	if (result_error[0]) {
		toast_busy_end();
		gui_notify_popup(result_error);
		return;
	}

	switch (result_kind) {
	case JOB_CONNECT:
		rebuild_home();
		switch_screen(subsonic_screen);
		toast_success("signed_in");
		break;

	case JOB_PLAY:
		toast_busy_end();
		break; // the worker already opened the player

	default:
		fill_list();
		if (lv_screen_active() != list_screen) {
			switch_screen(list_screen);
		}
		break;
	}
}

// ---------------------------------------------------------------------------
// the worker thread
// ---------------------------------------------------------------------------

static void *worker_main(void *arg) {
	(void)arg;

	for (;;) {
		pthread_mutex_lock(&job_lock);
		while (!have_pending) {
			pthread_cond_wait(&job_ready, &job_lock);
		}
		job_t job = pending;
		have_pending = false;
		pthread_mutex_unlock(&job_lock);

		result_error[0] = '\0';
		result_kind = job.kind;

		if (job.kind != JOB_PLAY && job.kind != JOB_CONNECT) {
			result_count = 0;
		}

		int n = 0;
		switch (job.kind) {
		case JOB_CONNECT:
			if (!subsonic_set_account(job.server, job.user, job.password)) {
				const char *why = subsonic_last_error();
				snprintf(result_error, sizeof(result_error), "%s", why && why[0] ? why : tr("signed_out"));
			} else if (!subsonic_ping()) {
				// The account stays as typed: a wrong password is corrected
				// on this page, and clearing it would ask for the address
				// again for no reason.
				const char *why = subsonic_last_error();
				snprintf(result_error, sizeof(result_error), "%s", why && why[0] ? why : tr("connect"));
			}
			break;

		case JOB_ARTISTS:
			n = subsonic_get_artists(results.artists, SUBSONIC_LIST_MAX);
			snprintf(result_title, sizeof(result_title), "%s", tr("artists"));
			break;

		case JOB_ALBUMS:
			n = subsonic_get_albums(job.id, results.albums, SUBSONIC_LIST_MAX);
			snprintf(result_title, sizeof(result_title), "%.150s", job.text);
			break;

		case JOB_SONGS:
			n = subsonic_get_songs(job.id, results.songs, SUBSONIC_LIST_MAX);
			snprintf(result_title, sizeof(result_title), "%.150s", job.text);
			break;

		case JOB_PLAY:
			do_play(job.index);
			break;

		default:
			break;
		}

		if (n < 0) {
			const char *why = subsonic_last_error();
			snprintf(result_error, sizeof(result_error), "%s", why && why[0] ? why : tr("no_results"));
			n = 0;
		}
		if (job.kind != JOB_PLAY && job.kind != JOB_CONNECT) {
			result_count = n;
		}

		gui_post(job_done_cb, NULL);
	}
	return NULL;
}

static void start_worker(void) {
	if (worker_started) {
		return;
	}
	pthread_t thread;
	if (pthread_create(&thread, NULL, worker_main, NULL) == 0) {
		pthread_detach(thread);
		worker_started = true;
	}
}

// ---------------------------------------------------------------------------
// going back inside the list page
//
// The three levels live on one screen, so the screen manager has nothing to
// stack: without this, backing out of an album's songs would leave the page
// entirely instead of returning to the artist's albums.
// ---------------------------------------------------------------------------

#define LIST_HISTORY_DEPTH 4
static job_t list_history[LIST_HISTORY_DEPTH];
static int list_history_len;
static job_t current_list_job;
static bool have_current_list;

static bool list_back_has_step(void) { return list_history_len > 0; }

static bool list_back_guard(void) {
	if (list_history_len == 0) {
		have_current_list = false;
		return false;
	}
	job_t previous = list_history[--list_history_len];
	have_current_list = true;
	current_list_job = previous;
	start_worker();
	busy_show();
	submit(&previous);
	return true;
}

static void run(const job_t *job) {
	if (job->kind != JOB_PLAY && job->kind != JOB_CONNECT) {
		bool same = have_current_list && current_list_job.kind == job->kind && strcmp(current_list_job.id, job->id) == 0 && strcmp(current_list_job.text, job->text) == 0;
		if (have_current_list && !same && lv_screen_active() == list_screen && list_history_len < LIST_HISTORY_DEPTH) {
			list_history[list_history_len++] = current_list_job;
		} else if (lv_screen_active() != list_screen) {
			list_history_len = 0;
		}
		current_list_job = *job;
		have_current_list = true;
	}

	start_worker();
	if (job->kind == JOB_PLAY) {
		// The tapped song has to arrive whole before it can play, unlike a
		// Qobuz track: the toast is the only sign anything is happening.
		toast_busy("loading");
	} else {
		busy_show();
	}
	submit(job);
}

static void row_clicked_cb(lv_event_t *e) {
	// A gesture that ends over a row is not a tap on the row: events bubble, so
	// the click that closes a drag arrives here too.
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= result_count) {
		return;
	}

	job_t job = {0};

	switch (list_kind) {
	case JOB_ARTISTS:
		job.kind = JOB_ALBUMS;
		snprintf(job.id, sizeof(job.id), "%s", results.artists[index].id);
		snprintf(job.text, sizeof(job.text), "%s", results.artists[index].name);
		run(&job);
		return;

	case JOB_ALBUMS:
		job.kind = JOB_SONGS;
		snprintf(job.id, sizeof(job.id), "%s", results.albums[index].id);
		snprintf(job.text, sizeof(job.text), "%s", results.albums[index].name);
		run(&job);
		return;

	case JOB_SONGS:
		job.kind = JOB_PLAY;
		job.index = index;
		run(&job);
		return;

	default:
		return;
	}
}

// ---------------------------------------------------------------------------
// the list page
// ---------------------------------------------------------------------------

static void build_list_page(gui_config_t *cfg) {
	list_screen = lv_obj_create(NULL);
	list_container = settingsrow_page(list_screen, cfg, "subsonic");
	list_title = settingsrow_page_title(list_screen);

	list_empty = lv_label_create(list_screen);
	lv_label_set_text(list_empty, tr("no_results"));
	lv_obj_set_style_text_align(list_empty, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(list_empty, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(list_empty, &font_ui_24, 0);
	lv_obj_align(list_empty, LV_ALIGN_TOP_MID, 0, settingsrow_content_top(cfg) + 100);
	lv_obj_set_hidden(list_empty, true);

	switcher_attach_back_gesture(list_screen);
	switcher_set_back_guard(list_screen, list_back_guard);
	switcher_set_back_guard_peek(list_screen, list_back_has_step);
	player_sheet_attach_drag(list_screen, true);
	player_sheet_attach_drag(list_container, true);
}

// ---------------------------------------------------------------------------
// the account page
// ---------------------------------------------------------------------------

static lv_obj_t *account_screen;
static lv_obj_t *server_field;
static lv_obj_t *user_field;
static lv_obj_t *password_field;
static keyboard_t *account_keyboard;

static void focus_field(lv_obj_t *field) {
	lv_obj_t *const others[] = {server_field, user_field, password_field};
	keyboard_set_field(account_keyboard, field);
	for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
		if (others[i] == field) {
			lv_obj_add_state(others[i], LV_STATE_FOCUSED);
			keyboard_show_caret(others[i], true);
		} else {
			lv_obj_remove_state(others[i], LV_STATE_FOCUSED);
			keyboard_show_caret(others[i], false);
		}
	}
}

static void field_focus_cb(lv_event_t *e) { focus_field(lv_event_get_target(e)); }

static lv_obj_t *make_field(lv_obj_t *parent, const char *placeholder) {
	lv_obj_t *field = lv_textarea_create(parent);
	lv_textarea_set_one_line(field, true);
	lv_textarea_set_placeholder_text(field, tr(placeholder));
	lv_obj_set_size(field, lv_pct(100), 62);
	lv_obj_set_scrollbar_mode(field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_style(field, &theme_style_card, 0);
	lv_obj_set_style_radius(field, 12, 0);
	lv_obj_set_style_border_width(field, 0, 0);
	lv_obj_set_style_shadow_width(field, 0, 0);
	lv_obj_set_style_pad_all(field, 14, 0);
	lv_obj_set_style_text_font(field, &font_ui_24, 0);
	keyboard_style_caret(field);
	lv_obj_add_event_cb(field, field_focus_cb, LV_EVENT_CLICKED, NULL);
	return field;
}

static void connect_accept_cb(lv_event_t *e) {
	(void)e;
	const char *server = lv_textarea_get_text(server_field);
	const char *user = lv_textarea_get_text(user_field);
	const char *password = lv_textarea_get_text(password_field);
	if (!server || !*server || !user || !*user || !password || !*password) {
		gui_notify_popup("credentials_required");
		return;
	}

	job_t job = {0};
	job.kind = JOB_CONNECT;
	snprintf(job.server, sizeof(job.server), "%s", server);
	snprintf(job.user, sizeof(job.user), "%s", user);
	snprintf(job.password, sizeof(job.password), "%s", password);
	run(&job);

	// The password does not stay in the field a second longer than needed.
	lv_textarea_set_text(password_field, "");
}

static void account_loaded_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(server_field, subsonic_server());
	lv_textarea_set_text(user_field, subsonic_user());
	lv_textarea_set_text(password_field, "");
	keyboard_reset(account_keyboard);
	focus_field(server_field);
}

static void build_account_page(gui_config_t *cfg) {
	account_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(account_screen, cfg, "subsonic");

	server_field = make_field(container, "server");
	user_field = make_field(container, "subsonic_user");
	password_field = make_field(container, "password");
	keyboard_style_password(password_field, 0);

	lv_obj_t *connect_row = settingsrow_action(container, "connect", connect_accept_cb, NULL);
	lv_obj_set_style_margin_top(connect_row, 8, 0);

	lv_obj_t *note = lv_label_create(container);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_18, 0);
	lv_label_set_text(note, tr("subsonic_note"));
	lv_obj_set_style_margin_top(note, 8, 0);

	account_keyboard = keyboard_create(account_screen, cfg->screen_width, 316, server_field, NULL, "ok", connect_accept_cb, NULL);
	lv_obj_add_event_cb(account_screen, account_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(account_screen);
}

static void open_account_cb(lv_event_t *e) {
	(void)e;
	switch_screen(account_screen);
}

// ---------------------------------------------------------------------------
// the home page
// ---------------------------------------------------------------------------

static lv_obj_t *home_container;

static bool wifi_is_connected(void) {
#ifdef HOST_BUILD
	return true;
#else
	wifi_status_t status;
	wifi_get_status(&status);
	return status.state == WIFI_STATE_CONNECTED && status.ip[0] != '\0';
#endif
}

static void open_wifi_cb(lv_event_t *e) {
	(void)e;
	switch_screen(wifisettings_screen);
}

static void open_artists_cb(lv_event_t *e) {
	(void)e;
	if (!subsonic_configured()) {
		gui_notify_popup("subsonic_note");
		open_account_cb(NULL);
		return;
	}
	job_t job = {0};
	job.kind = JOB_ARTISTS;
	run(&job);
}

static void sign_out_cb(lv_event_t *e);

static void do_sign_out(void *user) {
	(void)user;
	subsonic_clear_account();
	rebuild_home();
	toast_success("signed_out");
}

static void sign_out_cb(lv_event_t *e) {
	(void)e;
	confirm_show("subsonic", "subsonic_sign_out_confirm", "sign_out", do_sign_out, NULL);
}

static void rebuild_home(void) {
	lv_obj_clean(home_container);

	if (!wifi_is_connected()) {
		lv_obj_t *note = lv_label_create(home_container);
		lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(note, lv_pct(100));
		lv_obj_add_style(note, &theme_style_text, 0);
		lv_obj_set_style_text_font(note, &font_ui_22, 0);
		lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_pad_top(note, 14, 0);
		lv_label_set_text(note, tr("enable_wi_fi_first"));
		lv_obj_t *row = settingsrow_add(home_container, "wi_fi_settings", NULL, open_wifi_cb, NULL);
		lv_obj_set_style_margin_top(row, 16, 0);
		return;
	}

	if (!subsonic_configured()) {
		lv_obj_t *note = lv_label_create(home_container);
		lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(note, lv_pct(100));
		lv_obj_add_style(note, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(note, &font_ui_20, 0);
		lv_label_set_text(note, tr("subsonic_note"));
		settingsrow_add(home_container, "server", NULL, open_account_cb, NULL);
		return;
	}

	lv_obj_t *address = NULL;
	settingsrow_add(home_container, "server", &address, open_account_cb, NULL);
	lv_label_set_text(address, subsonic_server());
	settingsrow_add(home_container, "artists", NULL, open_artists_cb, NULL);
	settingsrow_action(home_container, "sign_out", sign_out_cb, NULL);
}

static void home_loaded_cb(lv_event_t *e) {
	(void)e;
	// The card may have been inserted or removed since the page was built.
	subsoniccache_set_root(storage_sd_root());
	rebuild_home();
}

void subsonicpage_init(gui_config_t *cfg) {

	subsonic_screen = lv_obj_create(NULL);
	home_container = settingsrow_page(subsonic_screen, cfg, "subsonic");

	build_account_page(cfg);
	build_list_page(cfg);
	build_busy_layer();

	device_state_add_prepare_cb(subsonic_prepare_track);
	start_fetch_thread();

	subsonic_init();
	subsoniccache_set_root(storage_sd_root());
	rebuild_home();

	lv_obj_add_event_cb(subsonic_screen, home_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(subsonic_screen);
	player_sheet_attach_drag(subsonic_screen, true);
	player_sheet_attach_drag(home_container, true);
}

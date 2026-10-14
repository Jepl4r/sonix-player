#ifdef SONIX_ZVUK

#include "zvukpage.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/sx_card.h"
#include "src/gui/shell/sx_row.h"
#include "src/gui/shell/sx_sheet.h"
#include "src/gui/shell/sx_styles.h"
#include "src/gui/shell/sx_toast.h"
#include "src/gui/shell/sx_tokens.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/streaming/streamerror.h"
#include "src/gui/streaming/svclist.h"
#include "src/gui/streaming/svcui.h"
#include "src/system/core/lang.h"
#include "src/system/device/system.h"
#include "src/system/library/library.h"
#include "src/system/net/http.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/cardimport.h"
#include "src/system/streaming/provider.h"
#include "src/system/streaming/zvuk.h"
#include "src/system/streaming/zvukdl.h"
#include "src/system/streaming/zvuklist.h"
#include "src/system/streaming/zvukplay.h"
#include "src/system/streaming/zvuksync.h"

// See zvukpage.h. The Yandex page's structure (yandexpage.c), smaller: no
// sign-in page (the file is the sign-in), no My Wave, and search with pills.

#define MAX_ROWS 150
#define SLOT_COUNT 24
#define ARTIST_TRACKS 20

typedef enum { ROW_TRACK, ROW_ALBUM, ROW_ARTIST, ROW_PLAYLIST } row_kind_t;

typedef struct {
	row_kind_t kind;
	union {
		zvuk_track_t track;
		zvuk_album_t album;
		zvuk_artist_t artist;
		zvuk_playlist_t playlist;
	} u;
} row_t;

typedef enum {
	JOB_NONE = 0,
	// lists
	JOB_LIKED,
	JOB_ALBUMS,
	JOB_ARTISTS,
	JOB_PLAYLISTS,
	JOB_PLAYLIST,
	JOB_ALBUM,
	JOB_ARTIST,
	JOB_SEARCH, // `n`: the kind (zvuk_search_kind_t)
	// one page of the list on screen
	JOB_PAGE,
	// in order, none dropped: a like (`n` 1/0, `b` the kind), an album queued
	// (`n` where), the account's profile, the connection check
	JOB_LIKE,
	JOB_QUEUE_ALBUM,
	JOB_PROFILE,
	JOB_CHECK,
	// built on the GUI thread from the index of saved tracks
	JOB_SAVED,
	JOB_SAVED_ALBUM,
} job_kind_t;

typedef struct {
	job_kind_t kind;
	char a[ZVUK_TITLE_MAX];
	char b[ZVUK_ID_MAX];
	int n;
	unsigned generation;
	zvuklist_t *list;
} job_t;

typedef struct {
	job_kind_t kind;
	unsigned generation;
	int page;
	int n;
	int item;				// JOB_LIKE: the kind liked
	char id[ZVUK_ID_MAX];	// and which
	char error[512];
	stream_err_t error_class;
	zvuklist_t *list;
	int count;
	row_t rows[];
} job_result_t;

lv_obj_t *zvuk_screen;
static lv_obj_t *list_screen, *search_screen;
static gui_config_t *page_cfg;

// ---------------------------------------------------------------------------
// the worker
// ---------------------------------------------------------------------------

static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_cond = PTHREAD_COND_INITIALIZER;
static job_t pending, page_pending;
#define EVENTS_MAX 8
static job_t events[EVENTS_MAX];
static int events_count;
static bool worker_started;
static unsigned list_generation;

// Worker side: the answer being built.
static row_t *result;
static int result_count;
static zvuklist_t *result_list;
static char result_error[512];
static stream_err_t result_class;

static void job_done_async(void *user);

static bool is_event(job_kind_t kind) {
	return kind == JOB_LIKE || kind == JOB_QUEUE_ALBUM || kind == JOB_PROFILE || kind == JOB_CHECK;
}

static void submit(const job_t *job) {
	pthread_mutex_lock(&job_lock);
	if (is_event(job->kind)) {
		if (events_count < EVENTS_MAX) {
			events[events_count++] = *job;
		}
	} else if (job->kind == JOB_PAGE) {
		zvuklist_free(page_pending.list);
		page_pending = *job;
	} else {
		pending = *job;
	}
	pthread_cond_signal(&job_cond);
	pthread_mutex_unlock(&job_lock);
}

static void add_id_cb(const char *id, void *user) { zvuklist_add_id(user, id); }

static int list_from_tracks(const zvuk_track_t *tracks, int got) {
	if (got < 0) {
		return got;
	}
	result_list = zvuklist_new();
	for (int i = 0; result_list && i < got; i++) {
		zvuklist_add_track(result_list, &tracks[i]);
	}
	return result_list ? got : -1;
}

static int list_from_ids(int got, const char *what) {
	if (got < 0 || !result_list) {
		zvuklist_free(result_list);
		result_list = NULL;
		return -1;
	}
	if (got > 0 && zvuklist_fetch_page(result_list, 0) < 0) {
		fprintf(stderr, "zvuk: %s: the first page did not come (%s)\n", what, zvuk_last_error());
	}
	printf("zvuk: %s: %d tracks, the list holds %zu bytes\n", what, got, zvuklist_memory(result_list));
	return got;
}

static void add_row(row_kind_t kind, const void *what) {
	if (result_count >= MAX_ROWS) {
		return;
	}
	row_t *r = &result[result_count++];
	r->kind = kind;
	switch (kind) {
	case ROW_TRACK:
		r->u.track = *(const zvuk_track_t *)what;
		break;
	case ROW_ALBUM:
		r->u.album = *(const zvuk_album_t *)what;
		break;
	case ROW_ARTIST:
		r->u.artist = *(const zvuk_artist_t *)what;
		break;
	case ROW_PLAYLIST:
		r->u.playlist = *(const zvuk_playlist_t *)what;
		break;
	}
}

static void run_list_job(const job_t *job) {
	void *tmp = calloc(MAX_ROWS, sizeof(zvuk_track_t));
	if (!tmp) {
		snprintf(result_error, sizeof(result_error), "%s", tr("out_of_memory"));
		return;
	}
	zvuk_track_t *tracks = tmp;
	int got = -1;
	switch (job->kind) {
	case JOB_LIKED:
		result_list = zvuklist_new();
		got = list_from_ids(result_list ? zvuk_liked_ids(add_id_cb, result_list) : -1, "liked tracks");
		break;
	case JOB_PLAYLIST:
		result_list = zvuklist_new();
		got = list_from_ids(result_list ? zvuk_playlist_ids(job->a, add_id_cb, result_list) : -1, "playlist");
		break;
	case JOB_ALBUM:
		if (!zvuk_liked_known(ZVUK_ITEM_RELEASE)) {
			zvuk_collection_refresh(); // the album's Like in its sheet
		}
		got = list_from_tracks(tracks, zvuk_album_tracks(job->a, tracks, MAX_ROWS, NULL));
		break;
	case JOB_ALBUMS: {
		zvuk_album_t *a = tmp;
		got = zvuk_liked_albums(a, MAX_ROWS);
		for (int i = 0; i < got; i++) {
			add_row(ROW_ALBUM, &a[i]);
		}
		break;
	}
	case JOB_ARTISTS: {
		zvuk_artist_t *a = tmp;
		got = zvuk_liked_artists(a, MAX_ROWS);
		for (int i = 0; i < got; i++) {
			add_row(ROW_ARTIST, &a[i]);
		}
		break;
	}
	case JOB_PLAYLISTS: {
		zvuk_playlist_t *p = tmp;
		got = zvuk_playlists(p, MAX_ROWS);
		for (int i = 0; i < got; i++) {
			add_row(ROW_PLAYLIST, &p[i]);
		}
		break;
	}
	case JOB_ARTIST: {
		if (!zvuk_liked_known(ZVUK_ITEM_ARTIST)) {
			zvuk_collection_refresh();
		}
		int nt = 0, na = 0;
		zvuk_album_t *albums = calloc(MAX_ROWS, sizeof(*albums));
		got = albums ? zvuk_artist_page(job->a, tracks, ARTIST_TRACKS, &nt, albums, MAX_ROWS - ARTIST_TRACKS, &na) : -1;
		for (int i = 0; i < nt; i++) {
			add_row(ROW_TRACK, &tracks[i]);
		}
		for (int i = 0; i < na; i++) {
			add_row(ROW_ALBUM, &albums[i]);
		}
		free(albums);
		break;
	}
	case JOB_SEARCH: {
		zvuk_search_kind_t kind = (zvuk_search_kind_t)job->n;
		got = zvuk_search(job->a, kind, tmp, kind == ZVUK_SEARCH_TRACKS ? 60 : 40);
		static const row_kind_t ROWS[ZVUK_SEARCH_KINDS] = {ROW_TRACK, ROW_ALBUM, ROW_ARTIST, ROW_PLAYLIST};
		static const size_t SIZES[ZVUK_SEARCH_KINDS] = {sizeof(zvuk_track_t), sizeof(zvuk_album_t),
														sizeof(zvuk_artist_t), sizeof(zvuk_playlist_t)};
		for (int i = 0; i < got && (unsigned)kind < ZVUK_SEARCH_KINDS; i++) {
			add_row(ROWS[kind], (const char *)tmp + (size_t)i * SIZES[kind]);
		}
		break;
	}
	default:
		break;
	}
	if (got < 0) {
		zvuklist_free(result_list);
		result_list = NULL;
		result_count = 0;
		result_class = streamerr_describe("zvuk", zvuk_last_error(), result_error, sizeof(result_error));
	}
	free(tmp);
}

static job_result_t *job_result_new(const job_t *job) {
	int count = result_count;
	job_result_t *r = malloc(sizeof(*r) + (size_t)count * sizeof(row_t));
	if (!r) {
		zvuklist_free(result_list);
		result_list = NULL;
		return NULL;
	}
	memset(r, 0, sizeof(*r));
	r->kind = job->kind;
	r->generation = job->generation;
	r->page = job->n;
	r->n = job->n;
	r->item = atoi(job->b);
	snprintf(r->id, sizeof(r->id), "%.*s", ZVUK_ID_MAX - 1, job->a);
	snprintf(r->error, sizeof(r->error), "%s", result_error);
	r->error_class = result_class;
	r->list = result_list;
	result_list = NULL;
	r->count = count;
	if (count > 0) {
		memcpy(r->rows, result, (size_t)count * sizeof(row_t));
	}
	return r;
}

static void free_result(job_result_t *r) {
	if (r) {
		zvuklist_free(r->list);
		free(r);
	}
}

static void *worker_main(void *arg) {
	(void)arg;
	http_set_thread_timeout_cap(STREAM_UI_TIMEOUT_SECS);
	for (;;) {
		pthread_mutex_lock(&job_lock);
		while (pending.kind == JOB_NONE && page_pending.kind == JOB_NONE && events_count == 0) {
			pthread_cond_wait(&job_cond, &job_lock);
		}
		job_t job;
		if (pending.kind != JOB_NONE) {
			job = pending;
			pending.kind = JOB_NONE;
		} else if (page_pending.kind != JOB_NONE) {
			job = page_pending;
			page_pending.kind = JOB_NONE;
			page_pending.list = NULL;
		} else {
			job = events[0];
			memmove(events, events + 1, (size_t)(--events_count) * sizeof(events[0]));
		}
		pthread_mutex_unlock(&job_lock);

		result_error[0] = '\0';
		result_class = STREAM_ERR_NONE;
		result_count = 0;
		result_list = NULL;
		streamerr_begin();
		switch (job.kind) {
		case JOB_PAGE:
			if (zvuklist_fetch_page(job.list, job.n) < 0) {
				result_class = streamerr_describe("zvuk", zvuk_last_error(), result_error, sizeof(result_error));
			}
			result_list = job.list;
			job.list = NULL;
			break;
		case JOB_LIKE:
			if (!zvuk_like((zvuk_item_t)atoi(job.b), job.a, job.n != 0)) {
				snprintf(result_error, sizeof(result_error), "%s", zvuk_last_error());
			}
			break;
		case JOB_QUEUE_ALBUM: {
			zvuk_track_t *tmp = calloc(MAX_ROWS, sizeof(*tmp));
			int got = tmp ? zvuk_album_tracks(job.a, tmp, MAX_ROWS, NULL) : -1;
			if (got < 0) {
				result_class = streamerr_describe("zvuk", tmp ? zvuk_last_error() : tr("out_of_memory"),
												  result_error, sizeof(result_error));
			}
			for (int i = 0; i < got; i++) {
				add_row(ROW_TRACK, &tmp[i]);
			}
			free(tmp);
			break;
		}
		case JOB_PROFILE:
			zvuk_refresh_profile();
			break;
		case JOB_CHECK:
			zvuk_check_connection(result_error, sizeof(result_error));
			break;
		default:
			run_list_job(&job);
			break;
		}
		job_result_t *r = job_result_new(&job);
		if (r && !gui_post(job_done_async, r)) {
			free_result(r);
		}
	}
	return NULL;
}

static void start_worker(void) {
	if (worker_started) {
		return;
	}
	if (!result) {
		result = calloc(MAX_ROWS, sizeof(*result));
	}
	pthread_t th;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, 256 * 1024);
	worker_started = result && pthread_create(&th, &attr, worker_main, NULL) == 0;
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
// the list page
// ---------------------------------------------------------------------------

static lv_obj_t *list_title, *list_box, *list_spin, *list_message, *list_error;
static lv_obj_t *album_bar, *album_state_label, *album_btn_label;
static lv_obj_t *artist_bar, *artist_like_label;
static lv_obj_t *search_bar, *search_pills[ZVUK_SEARCH_KINDS];
static svclist_t *list;
static job_kind_t list_kind;
static job_result_t *shown;
static char list_album_id[ZVUK_ID_MAX], list_album_title[ZVUK_TITLE_MAX];
static char list_artist_id[ZVUK_ID_MAX];
static int page_asked = -1, page_failed = -1;
static time_t page_failed_at;
static char playing_id[ZVUK_ID_MAX];

static int view_count(void) {
	if (!shown) {
		return 0;
	}
	return shown->list ? zvuklist_count(shown->list) : shown->count;
}

static bool view_row(int index, row_t *out) {
	memset(out, 0, sizeof(*out));
	out->kind = ROW_TRACK;
	if (!shown || index < 0 || index >= view_count()) {
		return false;
	}
	if (!shown->list) {
		*out = shown->rows[index];
		return true;
	}
	if (zvuklist_track(shown->list, index, &out->u.track)) {
		return true;
	}
	zvuklist_id(shown->list, index, out->u.track.id, sizeof(out->u.track.id));
	return false;
}

static bool row_saved(const row_t *r) {
	if (r->kind == ROW_TRACK) {
		return r->u.track.id[0] && zvukdl_saved(r->u.track.id);
	}
	if (r->kind == ROW_ALBUM && r->u.album.id[0] && r->u.album.track_count > 0) {
		return zvukdl_album_saved_count(r->u.album.id) >= r->u.album.track_count;
	}
	return false;
}

static void row_texts(const row_t *r, char *name, size_t ns, char *detail, size_t ds) {
	switch (r->kind) {
	case ROW_TRACK:
		snprintf(name, ns, "%s", r->u.track.title);
		snprintf(detail, ds, "%s", r->u.track.artist);
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
		snprintf(detail, ds, "%s", tr("zvuk_artist"));
		break;
	case ROW_PLAYLIST:
		snprintf(name, ns, "%s", r->u.playlist.title);
		if (r->u.playlist.track_count > 0) {
			lv_snprintf(detail, ds, tr("zvuk_n_tracks"), r->u.playlist.track_count);
		} else {
			snprintf(detail, ds, "%s", tr("zvuk_playlist"));
		}
		break;
	}
}

static bool playing_update(void) {
	char id[ZVUK_ID_MAX];
	zvukplay_playing_id(id, sizeof(id));
	if (strcmp(id, playing_id) == 0) {
		return false;
	}
	snprintf(playing_id, sizeof(playing_id), "%s", id);
	return true;
}

static void length_text(int seconds, char *out, size_t size) {
	if (seconds <= 0) {
		out[0] = '\0';
		return;
	}
	snprintf(out, size, "%d:%02d", seconds / 60, seconds % 60);
}

static void first_char(const char *text, char *out, size_t size) {
	unsigned char c = (unsigned char)(text ? text[0] : 0);
	size_t n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
	if (!c || n >= size || strlen(text) < n) {
		snprintf(out, size, "#");
		return;
	}
	memcpy(out, text, n);
	out[n] = '\0';
	if (n == 1 && out[0] >= 'a' && out[0] <= 'z') {
		out[0] = (char)(out[0] - 'a' + 'A');
	}
}

static void list_bind(lv_obj_t *row, int index) {
	row_t r;
	bool have = view_row(index, &r);
	lv_obj_set_style_opa(row, LV_OPA_COVER, 0);
	sx_row_set_playing(row, false);
	if (!have) {
		sx_row_set_title(row, tr("loading"));
		sx_row_set_subtitle(row, "", NULL, NULL);
		sx_row_set_thumb(row, NULL, svcui_tone(r.u.track.id));
		sx_row_set_trail(row, SX_ROW_TRAIL_NONE);
		sx_row_set_swipes(row, true, true);
		return;
	}
	char name[ZVUK_TITLE_MAX], detail[ZVUK_TITLE_MAX + 32], letter[8];
	row_texts(&r, name, sizeof(name), detail, sizeof(detail));
	sx_row_set_title(row, name);
	switch (r.kind) {
	case ROW_TRACK: {
		char length[16];
		length_text(r.u.track.duration, length, sizeof(length));
		sx_row_set_subtitle(row, r.u.track.artist, row_saved(&r) ? tr("svc_badge_saved") : NULL, length);
		if ((list_kind == JOB_ALBUM || list_kind == JOB_SAVED_ALBUM) && r.u.track.track_number > 0) {
			sx_row_set_number(row, r.u.track.track_number);
		} else {
			sx_row_set_thumb(row, NULL, svcui_tone(r.u.track.album_id[0] ? r.u.track.album_id : r.u.track.title));
		}
		// Signed out (the token expired), the saved tracks still play but a
		// star could only fail: none is offered.
		sx_row_set_trail(row, zvuk_logged_in() ? SX_ROW_TRAIL_STAR : SX_ROW_TRAIL_NONE);
		sx_row_set_starred(row, zvuk_is_liked(ZVUK_ITEM_TRACK, r.u.track.id));
		sx_row_set_playing(row, playing_id[0] && strcmp(playing_id, r.u.track.id) == 0);
		sx_row_set_swipes(row, r.u.track.available, r.u.track.available);
		if (!r.u.track.available) {
			lv_obj_set_style_opa(row, LV_OPA_50, 0);
		}
		break;
	}
	case ROW_ALBUM:
		sx_row_set_subtitle(row, detail, row_saved(&r) ? tr("svc_badge_saved") : NULL, NULL);
		sx_row_set_thumb(row, NULL, svcui_tone(r.u.album.id[0] ? r.u.album.id : r.u.album.title));
		sx_row_set_trail(row, SX_ROW_TRAIL_CHEVRON);
		sx_row_set_swipes(row, true, true);
		break;
	case ROW_ARTIST:
		first_char(r.u.artist.name, letter, sizeof(letter));
		sx_row_set_subtitle(row, detail, NULL, NULL);
		sx_row_set_letter(row, letter, svcui_tone(r.u.artist.name), true);
		sx_row_set_trail(row, SX_ROW_TRAIL_CHEVRON);
		sx_row_set_swipes(row, false, false);
		break;
	case ROW_PLAYLIST:
		first_char(r.u.playlist.title, letter, sizeof(letter));
		sx_row_set_subtitle(row, detail, NULL, NULL);
		sx_row_set_letter(row, letter, svcui_tone(r.u.playlist.title), false);
		sx_row_set_trail(row, SX_ROW_TRAIL_CHEVRON);
		sx_row_set_swipes(row, false, false);
		break;
	}
}

static void request_page(int page) {
	if (page == page_asked || (page == page_failed && time(NULL) - page_failed_at < 5)) {
		return;
	}
	page_asked = page;
	start_worker();
	job_t job = {.kind = JOB_PAGE, .n = page, .generation = list_generation, .list = zvuklist_ref(shown->list)};
	submit(&job);
}

static void list_visible(int first, int last) {
	int count = view_count();
	if (!shown || !shown->list || count <= 0) {
		return;
	}
	int pages[3] = {first / ZVUKLIST_PAGE, last / ZVUKLIST_PAGE, (last + ZVUKLIST_PAGE / 2) / ZVUKLIST_PAGE};
	for (int i = 0; i < 3; i++) {
		if (pages[i] * ZVUKLIST_PAGE < count && !zvuklist_page_ready(shown->list, pages[i])) {
			request_page(pages[i]);
			break;
		}
	}
}

static void refresh_marks(void) {
	if (list) {
		svclist_rebind(list);
	}
}

static void album_saved_counts(int *saved, int *playable) {
	*saved = *playable = 0;
	int count = view_count();
	for (int i = 0; i < count; i++) {
		row_t r;
		if (view_row(i, &r) && r.kind == ROW_TRACK && r.u.track.available) {
			(*playable)++;
			*saved += zvukdl_saved(r.u.track.id);
		}
	}
}

static void update_album_bar(void) {
	if (!album_bar || lv_obj_is_hidden(album_bar)) {
		return;
	}
	int saved, playable;
	album_saved_counts(&saved, &playable);
	bool complete = playable > 0 && saved == playable;
	if (complete) {
		lv_label_set_text(album_state_label, tr("zvuk_saved_on_card"));
	} else if (saved > 0) {
		lv_label_set_text_fmt(album_state_label, tr("zvuk_saved_some"), saved, playable);
	} else {
		lv_label_set_text(album_state_label, "");
	}
	lv_label_set_text(album_btn_label, tr(complete ? "delete" : "zvuk_save_album"));
}

static void artist_bar_refresh(void) {
	if (artist_like_label) {
		lv_label_set_text(artist_like_label,
						  tr(zvuk_is_liked(ZVUK_ITEM_ARTIST, list_artist_id) ? "zvuk_unlike" : "zvuk_like"));
	}
}

// --- history inside the one list page (see yandexpage.c) -------------------

typedef struct {
	job_kind_t kind;
	char title[ZVUK_TITLE_MAX];
	char a[ZVUK_TITLE_MAX];
	char b[ZVUK_ID_MAX];
	int n;
} list_step_t;

#define LIST_HISTORY_DEPTH 8
static list_step_t list_history[LIST_HISTORY_DEPTH];
static int list_history_len;
static list_step_t list_current;
static bool list_has_current;

static void show_list(const list_step_t *step);
static void ensure_list(void);

static bool list_back_has_step(void) { return list_history_len > 0; }

static bool list_back_guard(void) {
	if (list_history_len == 0) {
		list_has_current = false;
		return false;
	}
	list_current = list_history[--list_history_len];
	show_list(&list_current);
	return true;
}

static void open_list(const char *title, job_kind_t kind, const char *a, const char *b, int n) {
	list_step_t step;
	memset(&step, 0, sizeof(step));
	step.kind = kind;
	step.n = n;
	snprintf(step.title, sizeof(step.title), "%s", title ? title : "");
	snprintf(step.a, sizeof(step.a), "%s", a ? a : "");
	snprintf(step.b, sizeof(step.b), "%s", b ? b : "");
	if (lv_screen_active() != list_screen) {
		list_history_len = 0;
	} else if (list_has_current && memcmp(&list_current, &step, sizeof(step)) != 0) {
		if (list_history_len == LIST_HISTORY_DEPTH) {
			memmove(list_history, list_history + 1, (LIST_HISTORY_DEPTH - 1) * sizeof(list_history[0]));
			list_history_len--;
		}
		list_history[list_history_len++] = list_current;
	}
	list_current = step;
	list_has_current = true;
	show_list(&step);
}

static job_result_t *saved_result(job_kind_t kind, const char *album_id) {
	job_result_t *r = calloc(1, sizeof(*r) + MAX_ROWS * sizeof(row_t));
	zvuk_track_t *tracks = calloc(MAX_ROWS, sizeof(*tracks));
	if (!r || !tracks) {
		free(r);
		free(tracks);
		return NULL;
	}
	r->kind = kind;
	r->generation = list_generation;
	int count = 0;
	if (kind == JOB_SAVED) {
		zvuk_album_t *albums = (zvuk_album_t *)tracks;
		int n = zvukdl_albums(albums, MAX_ROWS);
		for (int i = 0; i < n; i++) {
			r->rows[count].kind = ROW_ALBUM;
			r->rows[count++].u.album = albums[i];
		}
		n = zvukdl_tracks(NULL, tracks, MAX_ROWS - count);
		for (int i = 0; i < n; i++) {
			r->rows[count].kind = ROW_TRACK;
			r->rows[count++].u.track = tracks[i];
		}
	} else {
		int n = zvukdl_tracks(album_id, tracks, MAX_ROWS);
		for (int i = 0; i < n; i++) {
			r->rows[count].kind = ROW_TRACK;
			r->rows[count++].u.track = tracks[i];
		}
	}
	free(tracks);
	r->count = count;
	if (count == 0) {
		snprintf(r->error, sizeof(r->error), "%s", tr("zvuk_saved_none"));
	}
	return r;
}

static void place_list(int bar_height) {
	int top = settingsrow_content_top(page_cfg);
	int y = top + bar_height;
	lv_obj_set_pos(list_box, SX_BODY_PAD_L, y);
	lv_obj_set_height(list_box, page_cfg->screen_height - y);
}

static void paint_search_pills(int kind) {
	for (int i = 0; i < ZVUK_SEARCH_KINDS; i++) {
		sx_pill_set_active(search_pills[i], i == kind);
	}
}

static void show_list(const list_step_t *step) {
	ensure_list();
	list_kind = step->kind;
	lv_label_set_text(list_title, step->title);
	bool is_album = (step->kind == JOB_ALBUM || step->kind == JOB_SAVED_ALBUM) && step->a[0];
	if (is_album) {
		snprintf(list_album_id, sizeof(list_album_id), "%.*s", (int)sizeof(list_album_id) - 1, step->a);
		snprintf(list_album_title, sizeof(list_album_title), "%s", step->title);
	}
	bool is_artist = step->kind == JOB_ARTIST && step->a[0];
	if (is_artist) {
		snprintf(list_artist_id, sizeof(list_artist_id), "%.*s", (int)sizeof(list_artist_id) - 1, step->a);
		artist_bar_refresh();
	}
	bool is_search = step->kind == JOB_SEARCH;
	if (is_search) {
		paint_search_pills(step->n);
	}
	lv_obj_set_hidden(album_bar, !is_album);
	lv_obj_set_hidden(artist_bar, !is_artist);
	lv_obj_set_hidden(search_bar, !is_search);
	place_list(is_album || is_artist || is_search ? 72 : 0);
	svclist_set_count(list, 0, false);
	lv_obj_set_hidden(list_message, true);
	streamerror_hide(list_error);
	page_asked = -1;
	if (lv_screen_active() != list_screen) {
		switch_screen(list_screen);
	}
	list_generation++;
	if (step->kind == JOB_SAVED || step->kind == JOB_SAVED_ALBUM) {
		job_result_t *r = saved_result(step->kind, step->a);
		if (r) {
			job_done_async(r);
		}
		return;
	}
	lv_obj_set_hidden(list_spin, false);
	run(step->kind, step->a, step->b, step->n);
}

static void retry_list(void) {
	if (list_has_current) {
		show_list(&list_current);
	}
}

static void fill_list(void) {
	int count = view_count();
	playing_update();
	svclist_set_count(list, count, false);
	lv_obj_set_hidden(list_message, true);
	update_album_bar();
	if (count == 0 && shown && shown->error[0] && shown->error_class != STREAM_ERR_NONE) {
		// The token's end is not a "sign in again" button here: the way in
		// is the card, and the panel says so in its sentence.
		const char *text = shown->error_class == STREAM_ERR_SIGN_IN && zvuk_session_expired()
							   ? tr("zvuk_token_expired")
							   : shown->error;
		streamerror_show(list_error, shown->error_class, text, retry_list, NULL);
		return;
	}
	streamerror_hide(list_error);
	if (count == 0) {
		lv_label_set_text(list_message, shown && shown->error[0] ? shown->error : tr("zvuk_empty"));
		lv_obj_set_hidden(list_message, false);
	}
}

// --- playing -----------------------------------------------------------------

static void play_rows(int tapped) {
	row_t r;
	bool have = view_row(tapped, &r);
	if (have && r.kind == ROW_TRACK && !r.u.track.available) {
		toast_error(tr("zvuk_no_file"));
		return;
	}
	lv_obj_set_hidden(list_spin, false);
	if (shown && shown->list) {
		if (!zvukplay_list(shown->list, tapped)) {
			lv_obj_set_hidden(list_spin, true);
		}
		return;
	}
	zvuklist_t *tracks = zvuklist_new();
	int start = -1;
	int count = view_count();
	for (int i = 0; tracks && i < count; i++) {
		if (view_row(i, &r) && r.kind == ROW_TRACK && r.u.track.available && zvuklist_add_track(tracks, &r.u.track) &&
			i == tapped) {
			start = zvuklist_count(tracks) - 1;
		}
	}
	if (start < 0 || !zvukplay_list(tracks, start)) {
		lv_obj_set_hidden(list_spin, true);
	}
	zvuklist_free(tracks);
}

static void playback_event(zvukplay_event_t event, const char *text) {
	if (list_spin) {
		lv_obj_set_hidden(list_spin, true);
	}
	switch (event) {
	case ZVUKPLAY_STARTED:
		player_refresh_now_playing();
		if (list && playing_update()) {
			svclist_rebind(list);
		}
		break;
	case ZVUKPLAY_FAILED:
		toast_error(text[0] ? text : tr("download_failed"));
		break;
	case ZVUKPLAY_SKIPPED: {
		char line[320];
		snprintf(line, sizeof(line), tr("zvuk_skipped"), text);
		toast_error(line);
		break;
	}
	}
}

static void list_tap(lv_obj_t *row, void *user) {
	(void)row;
	int index = (int)(intptr_t)user;
	row_t r;
	if (index < 0 || index >= view_count()) {
		return;
	}
	if (!view_row(index, &r)) {
		if (shown && shown->list) {
			play_rows(index);
		}
		return;
	}
	switch (r.kind) {
	case ROW_TRACK:
		play_rows(index);
		break;
	case ROW_ALBUM:
		open_list(r.u.album.title, list_kind == JOB_SAVED ? JOB_SAVED_ALBUM : JOB_ALBUM, r.u.album.id, NULL, 0);
		break;
	case ROW_ARTIST:
		open_list(r.u.artist.name, JOB_ARTIST, r.u.artist.id, NULL, 0);
		break;
	case ROW_PLAYLIST:
		open_list(r.u.playlist.title, JOB_PLAYLIST, r.u.playlist.id, NULL, 0);
		break;
	}
}

// --- the star, likes --------------------------------------------------------

static row_t menu_row;

static void row_like_done_async(void *user) {
	char *why = user;
	if (why) {
		toast_error(why[0] ? why : tr("zvuk_like_failed"));
		free(why);
	}
	refresh_marks();
}

static void row_like_done(bool ok, const char *error, void *user) {
	(void)user;
	gui_post(row_like_done_async, ok ? NULL : strdup(error ? error : ""));
}

static zvuk_track_t star_track;
static bool star_on;

static void star_apply(bool like) {
	star_on = like;
	zvuksync_like(ZVUK_ITEM_TRACK, star_track.id, like, row_like_done, NULL);
	refresh_marks();
}

static void star_undo(void *user) {
	(void)user;
	star_apply(!star_on);
}

static void track_like(const zvuk_track_t *t, bool like) {
	star_track = *t;
	star_apply(like);
	svcui_star_toast(like, "svc_liked", "svc_unliked", t->title, star_undo, NULL);
}

static void do_row_like(void *user) { track_like(&menu_row.u.track, user != NULL); }

static void list_star(lv_obj_t *row, void *user) {
	(void)row;
	row_t r;
	if (view_row((int)(intptr_t)user, &r) && r.kind == ROW_TRACK && r.u.track.id[0]) {
		track_like(&r.u.track, !zvuk_is_liked(ZVUK_ITEM_TRACK, r.u.track.id));
	}
}

// A release, an artist, a playlist: the mirror flips at once, the toast says
// what the service answered.
static void like_object(zvuk_item_t kind, const char *id, bool like) {
	char k[8];
	snprintf(k, sizeof(k), "%d", (int)kind);
	zvuk_liked_mark(kind, id, like);
	run(JOB_LIKE, id, k, like ? 1 : 0);
	if (kind == ZVUK_ITEM_ARTIST && strcmp(id, list_artist_id) == 0) {
		artist_bar_refresh();
	}
}

static void do_object_like(void *user) {
	bool like = user != NULL;
	switch (menu_row.kind) {
	case ROW_ALBUM:
		like_object(ZVUK_ITEM_RELEASE, menu_row.u.album.id, like);
		break;
	case ROW_ARTIST:
		like_object(ZVUK_ITEM_ARTIST, menu_row.u.artist.id, like);
		break;
	case ROW_PLAYLIST:
		like_object(ZVUK_ITEM_PLAYLIST, menu_row.u.playlist.id, like);
		break;
	default:
		break;
	}
}

static void artist_like_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active() || !list_artist_id[0]) {
		return;
	}
	like_object(ZVUK_ITEM_ARTIST, list_artist_id, !zvuk_is_liked(ZVUK_ITEM_ARTIST, list_artist_id));
}

static void like_object_done(const job_result_t *r, zvuk_item_t kind, const char *id) {
	static const char *const ON[ZVUK_ITEM_KINDS] = {"svc_liked", "zvuk_album_liked", "zvuk_artist_liked",
													"zvuk_playlist_liked"};
	static const char *const OFF[ZVUK_ITEM_KINDS] = {"svc_unliked", "zvuk_album_unliked", "zvuk_artist_unliked",
													 "zvuk_playlist_unliked"};
	if (r->error[0]) {
		zvuk_liked_mark(kind, id, r->n == 0); // put back
		toast_error(r->error);
	} else if ((unsigned)kind < ZVUK_ITEM_KINDS && kind != ZVUK_ITEM_TRACK) {
		toast_success(tr(r->n ? ON[kind] : OFF[kind]));
	}
	artist_bar_refresh();
}

// --- queueing ----------------------------------------------------------------

static void queue_tracks(const zvuk_track_t *tracks, int count, playlist_queue_where_t where, const char *what) {
	char **paths = NULL;
	int n = zvukplay_paths_for(tracks, count, &paths);
	if (n > 0) {
		svcui_queue((const char *const *)paths, n, where, what);
	} else {
		gui_notify_popup("playlist_cannot_add_the_tracks");
	}
	free(paths);
}

static char queue_album_title[ZVUK_TITLE_MAX];

static void queue_row(const row_t *r, playlist_queue_where_t where) {
	if (r->kind == ROW_TRACK) {
		if (!r->u.track.available) {
			toast_error(tr("zvuk_no_file"));
			return;
		}
		queue_tracks(&r->u.track, 1, where, r->u.track.title);
		return;
	}
	if (r->kind != ROW_ALBUM) {
		return;
	}
	int saved = zvukdl_album_saved_count(r->u.album.id);
	if (saved > 0 && (list_kind == JOB_SAVED || row_saved(r))) {
		zvuk_track_t *tracks = calloc((size_t)saved, sizeof(*tracks));
		int n = tracks ? zvukdl_tracks(r->u.album.id, tracks, saved) : 0;
		queue_tracks(tracks, n, where, r->u.album.title);
		free(tracks);
		return;
	}
	snprintf(queue_album_title, sizeof(queue_album_title), "%s", r->u.album.title);
	run(JOB_QUEUE_ALBUM, r->u.album.id, NULL, (int)where);
}

static void do_row_queue(void *user) { queue_row(&menu_row, (playlist_queue_where_t)(intptr_t)user); }

static void list_swipe(int index, playlist_queue_where_t where) {
	row_t r;
	if (view_row(index, &r)) {
		queue_row(&r, where);
	} else if (shown && shown->list && r.u.track.id[0]) {
		r.u.track.available = true;
		queue_tracks(&r.u.track, 1, where, tr("loading"));
	}
}

static void list_swipe_next(lv_obj_t *row, void *user) {
	(void)row;
	list_swipe((int)(intptr_t)user, PLAYLIST_QUEUE_NEXT);
}

static void list_swipe_end(lv_obj_t *row, void *user) {
	(void)row;
	list_swipe((int)(intptr_t)user, PLAYLIST_QUEUE_END);
}

static void queue_album_done(job_result_t *r) {
	if (r->count == 0) {
		toast_error(r->error[0] ? r->error : tr("playlist_cannot_add_the_tracks"));
		return;
	}
	zvuk_track_t *tracks = malloc((size_t)r->count * sizeof(*tracks));
	if (!tracks) {
		toast_error(tr("out_of_memory"));
		return;
	}
	for (int i = 0; i < r->count; i++) {
		tracks[i] = r->rows[i].u.track;
	}
	queue_tracks(tracks, r->count, (playlist_queue_where_t)r->n, queue_album_title);
	free(tracks);
}

// --- saving and deleting -----------------------------------------------------

static void save_refused(void) { toast_error(tr("zvuk_save_refused")); }

static void do_row_save(void *user) {
	(void)user;
	if (menu_row.kind == ROW_ALBUM) {
		if (!zvukdl_album(menu_row.u.album.id, menu_row.u.album.title)) {
			save_refused();
		}
	} else if (!zvukdl_track(&menu_row.u.track)) {
		save_refused();
	}
}

static void after_delete(void) {
	if (storage_sd_root() && storage_sd_root()[0]) {
		library_card_returned(storage_sd_root());
	}
	if (lv_screen_active() == list_screen && (list_kind == JOB_SAVED || list_kind == JOB_SAVED_ALBUM)) {
		show_list(&list_current);
	} else {
		refresh_marks();
		update_album_bar();
	}
}

static void delete_confirmed(void *user) {
	(void)user;
	bool ok;
	if (menu_row.kind == ROW_ALBUM) {
		ok = zvukdl_delete_album(menu_row.u.album.id) > 0 || zvukdl_album_saved_count(menu_row.u.album.id) == 0;
	} else {
		ok = zvukdl_delete(menu_row.u.track.id);
	}
	if (ok) {
		toast_success(tr("zvuk_deleted_from_card"));
	} else {
		toast_error(tr("zvuk_delete_failed"));
	}
	after_delete();
}

static void do_row_delete(void *user) {
	(void)user;
	const char *name = menu_row.kind == ROW_ALBUM ? menu_row.u.album.title : menu_row.u.track.title;
	confirm_show(menu_row.kind == ROW_ALBUM ? "zvuk_delete_album_question" : "zvuk_delete_track_question", name,
				 "delete", delete_confirmed, NULL);
}

static void list_hold(lv_obj_t *row, void *user) {
	(void)row;
	int index = (int)(intptr_t)user;
	if (index < 0 || !view_row(index, &menu_row)) {
		return;
	}
	sx_sheet_item_t items[7];
	memset(items, 0, sizeof(items));
	int n = 0;
	bool saved = row_saved(&menu_row);
	bool some = saved || (menu_row.kind == ROW_ALBUM && zvukdl_album_saved_count(menu_row.u.album.id) > 0);
	bool queueable = (menu_row.kind == ROW_TRACK && menu_row.u.track.available) || menu_row.kind == ROW_ALBUM;
	if (queueable) {
		items[n++] = (sx_sheet_item_t){.label = tr("queue_play_next"),
									   .icon = &icon_list_start,
									   .action = do_row_queue,
									   .user = (void *)(intptr_t)PLAYLIST_QUEUE_NEXT};
		items[n++] = (sx_sheet_item_t){.label = tr("queue_add_to_end"),
									   .icon = &icon_list_plus_small,
									   .action = do_row_queue,
									   .user = (void *)(intptr_t)PLAYLIST_QUEUE_END};
	}
	bool liked = false;
	switch (menu_row.kind) {
	case ROW_TRACK:
		liked = zvuk_is_liked(ZVUK_ITEM_TRACK, menu_row.u.track.id);
		items[n++] = (sx_sheet_item_t){.label = tr(liked ? "zvuk_unlike" : "zvuk_like"),
									   .icon = liked ? &icon_star_filled : &icon_star,
									   .action = do_row_like,
									   .user = liked ? NULL : (void *)1};
		break;
	case ROW_ALBUM:
		liked = zvuk_is_liked(ZVUK_ITEM_RELEASE, menu_row.u.album.id);
		break;
	case ROW_ARTIST:
		liked = zvuk_is_liked(ZVUK_ITEM_ARTIST, menu_row.u.artist.id);
		break;
	case ROW_PLAYLIST:
		liked = zvuk_is_liked(ZVUK_ITEM_PLAYLIST, menu_row.u.playlist.id);
		break;
	}
	if (menu_row.kind != ROW_TRACK) {
		items[n++] = (sx_sheet_item_t){.label = tr(liked ? "zvuk_unlike" : "zvuk_like"),
									   .icon = liked ? &icon_star_filled : &icon_star,
									   .action = do_object_like,
									   .user = liked ? NULL : (void *)1};
	}
	if (menu_row.kind == ROW_TRACK || menu_row.kind == ROW_ALBUM) {
		if (saved) {
			items[n++] = (sx_sheet_item_t){.label = tr("zvuk_saved_on_card"), .checked = true};
		} else if (menu_row.kind == ROW_ALBUM || menu_row.u.track.available) {
			items[n++] = (sx_sheet_item_t){.label = tr(menu_row.kind == ROW_ALBUM ? "zvuk_save_album" : "zvuk_save"),
										   .icon = &icon_download,
										   .action = do_row_save};
		}
	}
	if (some) {
		items[n++] = (sx_sheet_item_t){
			.label = tr("zvuk_delete_from_card"), .icon = &icon_trash, .destructive = true, .action = do_row_delete};
	}
	static char title[ZVUK_TITLE_MAX], subtitle[ZVUK_TITLE_MAX + 32];
	row_texts(&menu_row, title, sizeof(title), subtitle, sizeof(subtitle));
	sx_sheet_menu(title, subtitle, items, n);
}

// --- the bars ----------------------------------------------------------------

static lv_obj_t *bar_create(lv_obj_t *parent, lv_flex_align_t align) {
	int top = settingsrow_content_top(page_cfg);
	lv_obj_t *bar = lv_obj_create(parent);
	lv_obj_set_size(bar, page_cfg->screen_width - 2 * SX_BODY_PAD_L, 60);
	lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, top);
	lv_obj_set_style_bg_opa(bar, 0, 0);
	lv_obj_set_style_border_width(bar, 0, 0);
	lv_obj_set_style_pad_all(bar, 0, 0);
	lv_obj_set_style_pad_column(bar, 10, 0);
	lv_obj_set_scrollable(bar, false);
	lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(bar, align, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_hidden(bar, true);
	switcher_attach_back_gesture(bar);
	return bar;
}

static lv_obj_t *bar_button(lv_obj_t *parent, const char *key, lv_event_cb_t cb, lv_obj_t **label_out) {
	lv_obj_t *btn = lv_btn_create(parent);
	sx_pill_style(btn);
	lv_obj_set_style_pad_hor(btn, 24, 0);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, tr(key));
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_obj_center(label);
	if (label_out) {
		*label_out = label;
	}
	return btn;
}

static void album_save_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	int saved, playable;
	album_saved_counts(&saved, &playable);
	if (playable > 0 && saved == playable) {
		memset(&menu_row, 0, sizeof(menu_row));
		menu_row.kind = ROW_ALBUM;
		snprintf(menu_row.u.album.id, sizeof(menu_row.u.album.id), "%s", list_album_id);
		snprintf(menu_row.u.album.title, sizeof(menu_row.u.album.title), "%s", list_album_title);
		do_row_delete(NULL);
		return;
	}
	if (!list_album_id[0] || !zvukdl_album(list_album_id, list_album_title)) {
		save_refused();
	}
}

// A filter pill: the same words, another kind of result, in place of this
// list (not a step to go back to).
static void search_pill_cb(lv_event_t *e) {
	if (switcher_back_drag_active() || list_current.kind != JOB_SEARCH) {
		return;
	}
	int kind = (int)(intptr_t)lv_event_get_user_data(e);
	if (kind == list_current.n) {
		return;
	}
	list_current.n = kind;
	show_list(&list_current);
}

static void open_saved(void) { open_list(tr("zvuk_saved_on_card"), JOB_SAVED, NULL, NULL, 0); }

static void list_loaded_cb(lv_event_t *e) {
	(void)e;
	if (list && playing_update()) {
		svclist_rebind(list);
	}
}

// The playing row follows the track: a timer only while the list is up.
static lv_timer_t *list_timer;

static void list_timer_cb(lv_timer_t *t) {
	(void)t;
	if (list && playing_update()) {
		svclist_rebind(list);
	}
}

static void list_shown_cb(lv_event_t *e) {
	(void)e;
	if (!list_timer) {
		list_timer = lv_timer_create(list_timer_cb, 1000, NULL);
	}
}

static void list_hidden_cb(lv_event_t *e) {
	(void)e;
	if (list_timer) {
		lv_timer_delete(list_timer);
		list_timer = NULL;
	}
}

static void ensure_list(void) {
	if (list) {
		return;
	}
	static const sx_row_handlers_t handlers = {
		.on_tap = list_tap,
		.on_long_press = list_hold,
		.on_swipe_next = list_swipe_next,
		.on_swipe_end = list_swipe_end,
		.on_star = list_star,
	};
	static const svclist_cbs_t cbs = {.bind = list_bind, .visible = list_visible};
	list = svclist_create(list_screen, &handlers, &cbs, SLOT_COUNT, false);
	list_box = svclist_obj(list);
	int top = settingsrow_content_top(page_cfg);
	lv_obj_set_size(list_box, page_cfg->screen_width - 2 * SX_BODY_PAD_L, page_cfg->screen_height - top);
	lv_obj_set_pos(list_box, SX_BODY_PAD_L, top);
	lv_obj_move_to_index(list_box, 1);
}

static void build_list_page(gui_config_t *cfg) {
	list_screen = lv_obj_create(NULL);
	lv_obj_add_style(list_screen, &theme_style_screen, 0);
	list_title = settingsrow_title(list_screen, cfg, "");
	int top = settingsrow_content_top(cfg);

	album_bar = bar_create(list_screen, LV_FLEX_ALIGN_CENTER);
	album_state_label = lv_label_create(album_bar);
	lv_obj_add_style(album_state_label, &sx_style_text2, 0);
	lv_obj_set_style_text_font(album_state_label, &font_ui_20, 0);
	lv_label_set_text(album_state_label, "");
	bar_button(album_bar, "zvuk_save_album", album_save_cb, &album_btn_label);

	artist_bar = bar_create(list_screen, LV_FLEX_ALIGN_CENTER);
	bar_button(artist_bar, "zvuk_like", artist_like_cb, &artist_like_label);

	// The kinds of result: pills under the title, as on the Subsonic page.
	search_bar = bar_create(list_screen, LV_FLEX_ALIGN_START);
	// Four kinds do not fit the width in every language: the row scrolls
	// sideways, as chips do.
	lv_obj_set_scrollable(search_bar, true);
	lv_obj_set_scroll_dir(search_bar, LV_DIR_HOR);
	lv_obj_set_scrollbar_mode(search_bar, LV_SCROLLBAR_MODE_OFF);
	static const char *const KINDS[ZVUK_SEARCH_KINDS] = {"zvuk_tracks", "zvuk_albums", "zvuk_artists",
														 "zvuk_playlists"};
	for (int i = 0; i < ZVUK_SEARCH_KINDS; i++) {
		search_pills[i] = sx_pill(search_bar, tr(KINDS[i]), i, search_pill_cb);
	}

	list_message = lv_label_create(list_screen);
	lv_label_set_long_mode(list_message, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(list_message, cfg->screen_width - 2 * SX_BODY_PAD_L - 12);
	lv_obj_align(list_message, LV_ALIGN_TOP_MID, 0, top + 92);
	lv_obj_add_style(list_message, &sx_style_text2, 0);
	lv_obj_set_style_text_font(list_message, &font_ui_22, 0);
	lv_obj_set_hidden(list_message, true);
	list_error = streamerror_create(list_screen);
	lv_obj_set_width(list_error, cfg->screen_width - 2 * SX_BODY_PAD_L);
	lv_obj_align(list_error, LV_ALIGN_TOP_MID, 0, top);
	streamerror_set_service(list_error, tr("zvuk"));
	streamerror_set_offline(list_error, open_saved);

	list_spin = svcui_spinner(list_screen);
	switcher_attach_back_gesture(list_screen);
	lv_obj_set_press_lock(list_screen, true);
	switcher_set_back_guard(list_screen, list_back_guard);
	switcher_set_back_guard_peek(list_screen, list_back_has_step);
	lv_obj_add_event_cb(list_screen, list_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(list_screen, list_shown_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(list_screen, list_hidden_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
}

// ---------------------------------------------------------------------------
// search
// ---------------------------------------------------------------------------

static lv_obj_t *search_field;

static void search_accept_cb(lv_event_t *e) {
	(void)e;
	const char *text = lv_textarea_get_text(search_field);
	if (text && text[0]) {
		open_list(text, JOB_SEARCH, text, NULL, ZVUK_SEARCH_TRACKS);
	}
}

static void build_search_page(gui_config_t *cfg) {
	search_screen = lv_obj_create(NULL);
	lv_obj_add_style(search_screen, &theme_style_screen, 0);
	settingsrow_title(search_screen, cfg, "zvuk_search");
	int top = settingsrow_content_top(cfg);
	search_field = lv_textarea_create(search_screen);
	lv_textarea_set_one_line(search_field, true);
	lv_textarea_set_placeholder_text(search_field, tr("artist_album_or_track"));
	lv_obj_set_size(search_field, cfg->screen_width - 2 * SX_BODY_PAD_L, 60);
	lv_obj_set_scrollbar_mode(search_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(search_field, LV_ALIGN_TOP_LEFT, SX_BODY_PAD_L, top);
	lv_obj_add_style(search_field, &sx_style_surface, 0);
	lv_obj_add_style(search_field, &sx_style_text, 0);
	lv_obj_set_style_radius(search_field, SX_CARD_R, 0);
	lv_obj_set_style_border_width(search_field, 0, 0);
	lv_obj_set_style_shadow_width(search_field, 0, 0);
	lv_obj_set_style_pad_ver(search_field, 14, 0);
	lv_obj_set_style_pad_hor(search_field, SX_CARD_PAD_H, 0);
	lv_obj_set_style_text_font(search_field, &font_ui_24, 0);
	keyboard_style_caret(search_field);
	keyboard_create(search_screen, cfg->screen_width, 316, search_field, NULL, "ok", search_accept_cb, NULL);
	switcher_attach_back_gesture(search_screen);
}

// ---------------------------------------------------------------------------
// home
// ---------------------------------------------------------------------------

static lv_obj_t *home_in, *home_out;
static lv_obj_t *account_card, *saved_card, *saved_out_card, *save_status, *import_note;
static lv_obj_t *quality_pills[ZVUK_QUALITY_COUNT], *quality_format;
static lv_timer_t *save_timer;
static void paint_home(void);

static void quality_refresh(void) {
	zvuk_quality_t current = zvuk_get_quality();
	for (int i = 0; i < ZVUK_QUALITY_COUNT; i++) {
		if (quality_pills[i]) {
			sx_pill_set_active(quality_pills[i], (int)current == i);
		}
	}
	char format[48];
	zvuk_last_format(format, sizeof(format));
	if (current != ZVUK_QUALITY_MID && zvuk_needs_subscription()) {
		lv_label_set_text(quality_format, tr("zvuk_needs_subscription"));
	} else if (format[0]) {
		lv_label_set_text_fmt(quality_format, tr("zvuk_last_format"), format);
	} else {
		lv_label_set_text(quality_format, tr("zvuk_quality_note"));
	}
}

static void quality_pick_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	zvuk_set_quality((zvuk_quality_t)(intptr_t)lv_event_get_user_data(e));
	quality_refresh();
}

static void save_status_refresh(void) {
	zvukdl_status_t st;
	zvukdl_status(&st);
	if (!save_status) {
		return;
	}
	if (!st.active || !st.title[0]) {
		lv_obj_set_hidden(save_status, true);
		return;
	}
	char line[200];
	int current = st.done + st.failed + 1;
	lv_snprintf(line, sizeof(line), tr("zvuk_saving"), current, current + st.queued, st.title);
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
	if (!zvukdl_busy()) {
		lv_timer_delete(save_timer);
		save_timer = NULL;
	}
}

typedef struct {
	zvukdl_event_t event;
	int count;
	char text[400];
} save_note_t;

static void save_note_async(void *user) {
	save_note_t *note = user;
	char line[480];
	switch (note->event) {
	case ZVUKDL_QUEUED:
		lv_snprintf(line, sizeof(line), tr("zvuk_save_queued"), note->text);
		toast_success(line);
		if (!save_timer) {
			save_timer = lv_timer_create(save_timer_cb, 1000, NULL);
		}
		break;
	case ZVUKDL_SAVED:
		refresh_marks();
		update_album_bar();
		break;
	case ZVUKDL_FAILED:
		toast_error(note->text);
		break;
	case ZVUKDL_FINISHED:
		if (note->count > 0) {
			lv_snprintf(line, sizeof(line), tr("zvuk_saved_to"), note->text, note->count);
			toast_success(line);
		}
		break;
	case ZVUKDL_STOPPED:
		toast_plain(note->text);
		break;
	}
	save_status_refresh();
	free(note);
}

static void save_listener(zvukdl_event_t event, int count, const char *text) {
	save_note_t *note = malloc(sizeof(*note));
	if (!note) {
		return;
	}
	note->event = event;
	note->count = count;
	snprintf(note->text, sizeof(note->text), "%s", text ? text : "");
	gui_post(save_note_async, note);
}

// What became of the token file, under the instructions: the expired
// session's sentence, the last attempt's outcome, or "reading it now".
static void import_note_refresh(void) {
	if (!import_note) {
		return;
	}
	char message[256] = "";
	cardimport_result_t result;
	if (cardimport_busy("zvuk")) {
		snprintf(message, sizeof(message), "%s", tr("zvuk_import_busy"));
	} else if (cardimport_last("zvuk", &result, message, sizeof(message)) && result != CARDIMPORT_OK) {
		// the sentence the attempt left
	} else if (zvuk_session_expired()) {
		snprintf(message, sizeof(message), "%s", tr("zvuk_token_expired"));
	} else {
		message[0] = '\0';
	}
	lv_label_set_text(import_note, message);
	lv_obj_set_hidden(import_note, !message[0]);
}

static void paint_home(void) {
	bool in = zvuk_logged_in();
	lv_obj_set_hidden(home_in, !in);
	lv_obj_set_hidden(home_out, in);
	int saved = zvukdl_saved_count();
	char line[64] = "";
	if (saved > 0) {
		lv_snprintf(line, sizeof(line), tr("zvuk_n_tracks"), saved);
	}
	sx_card_subtitle(saved_card, line, false);
	sx_card_subtitle(saved_out_card, line, false);
	lv_obj_set_hidden(saved_out_card, saved == 0);
	char name[ZVUK_NAME_MAX], plan[64];
	zvuk_account(name, sizeof(name), plan, sizeof(plan), NULL);
	sx_card_title(account_card, name[0] ? name : tr("zvuk"));
	sx_card_subtitle(account_card, plan[0] ? plan : tr("zvuk_no_plan"), false);
	quality_refresh();
	save_status_refresh();
	import_note_refresh();
}

// The import thread's word, on the GUI thread: the home repaints (the
// toast is streaming.c's, for every service).
static void import_async(void *user) {
	(void)user;
	paint_home();
	if (zvuk_logged_in()) {
		zvuksync_likes_refresh(NULL);
	}
}

static void import_listener(const char *service, cardimport_result_t result, const char *message) {
	(void)result;
	(void)message;
	if (strcmp(service, "zvuk") == 0) {
		gui_post(import_async, NULL);
	}
}

static bool profile_asked;

static void home_loaded_cb(lv_event_t *e) {
	(void)e;
	// The file on the card, every time the page opens: a new token is picked
	// up here without a restart.
	cardimport_check("zvuk");
	paint_home();
	if (zvuk_logged_in() && !profile_asked) {
		profile_asked = true; // the name and the plan, once a session
		run(JOB_PROFILE, NULL, NULL, 0);
	}
}

static void check_again_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	cardimport_check("zvuk");
	import_note_refresh();
}

static void logout_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	zvukdl_cancel_all();
	zvuk_logout();
	zvukplay_signed_out();
	profile_asked = false;
	paint_home();
}

static void open_cb(lv_event_t *e) {
	job_kind_t kind = (job_kind_t)(intptr_t)lv_event_get_user_data(e);
	static const char *const TITLES[] = {[JOB_LIKED] = "zvuk_liked",
										 [JOB_ALBUMS] = "zvuk_albums",
										 [JOB_ARTISTS] = "zvuk_artists",
										 [JOB_PLAYLISTS] = "zvuk_playlists",
										 [JOB_SAVED] = "zvuk_saved_on_card"};
	open_list(tr(TITLES[kind]), kind, NULL, NULL, 0);
}

static void open_search_cb(lv_event_t *e) {
	(void)e;
	switch_screen(search_screen);
}

static void job_done_async(void *user) {
	job_result_t *r = user;
	switch (r->kind) {
	case JOB_LIKE:
		like_object_done(r, (zvuk_item_t)r->item, r->id);
		free_result(r);
		return;
	case JOB_QUEUE_ALBUM:
		queue_album_done(r);
		free_result(r);
		return;
	case JOB_PROFILE:
		paint_home();
		free_result(r);
		return;
	case JOB_CHECK: {
		sx_toast_busy_end();
		static char report[512];
		snprintf(report, sizeof(report), "%s", r->error);
		sx_sheet_t sheet = {.title = tr("zvuk_check_title"), .subtitle = report, .action_label = tr("ok")};
		sx_sheet_open(&sheet);
		paint_home();
		free_result(r);
		return;
	}
	case JOB_PAGE: {
		bool here = shown && r->list == shown->list;
		page_asked = -1;
		if (r->error[0]) {
			page_failed = r->page;
			page_failed_at = time(NULL);
		}
		if (here && !r->error[0] && list) {
			int first, last;
			svclist_visible(list, &first, &last);
			svclist_rebind(list);
			list_visible(first, last);
			update_album_bar();
		}
		free_result(r);
		return;
	}
	default:
		break;
	}
	if (r->generation != list_generation) {
		free_result(r);
		return;
	}
	lv_obj_set_hidden(list_spin, true);
	free_result(shown);
	shown = r;
	fill_list();
	if (r->kind == JOB_ARTIST) {
		artist_bar_refresh();
	}
	// A session that just ended (the token expired) shows on the home too.
	if (!zvuk_logged_in()) {
		paint_home();
	}
	zvuksync_likes_refresh(NULL);
}

bool zvukpage_open_album(const char *album_id, const char *title) {
	if (!album_id || !album_id[0] || !zvuk_logged_in()) {
		return false;
	}
	open_list(title && title[0] ? title : tr("zvuk"), JOB_ALBUM, album_id, NULL, 0);
	return true;
}

static void *provider_open_album(const char *album_id, const char *title) {
	return zvukpage_open_album(album_id, title) ? list_screen : NULL;
}

void zvukpage_check_connection_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	if (!zvuk_logged_in()) {
		sx_sheet_t sheet = {.title = tr("zvuk_check_title"), .subtitle = tr("zvuk_check_no_token"),
							.action_label = tr("ok")};
		sx_sheet_open(&sheet);
		return;
	}
	sx_toast_busy(tr("zvuk_check_running"), false);
	run(JOB_CHECK, NULL, NULL, 0);
}

// The steps of the way in, numbered, in the card's subtitle.
static void build_signed_out(lv_obj_t *column) {
	lv_obj_t *card = sx_card_create(column);
	sx_card_title(card, tr("zvuk_sign_in_title"));
	lv_obj_t *steps = sx_card_subtitle(card, tr("zvuk_sign_in_steps"), false);
	lv_label_set_long_mode(steps, LV_LABEL_LONG_WRAP);
	sx_card_action(card, tr("zvuk_check_card"), check_again_cb, NULL);
	import_note = svcui_note(column, "");
	lv_obj_set_hidden(import_note, true);
	saved_out_card = svcui_nav_card(column, tr("zvuk_saved_on_card"), NULL, open_cb, (void *)(intptr_t)JOB_SAVED);
}

void zvukpage_init(gui_config_t *cfg) {
	page_cfg = cfg;
	const stream_provider_gui_t provider_gui = {.badge = &icon_zvuk_badge, .open_album = provider_open_album};
	stream_provider_attach_gui("zvuk", &provider_gui);

	zvuk_screen = lv_obj_create(NULL);
	lv_obj_add_style(zvuk_screen, &theme_style_screen, 0);
	settingsrow_title(zvuk_screen, cfg, "zvuk");

	home_in = svcui_column(zvuk_screen, cfg);
	svcui_nav_card(home_in, tr("zvuk_liked"), NULL, open_cb, (void *)(intptr_t)JOB_LIKED);
	svcui_nav_card(home_in, tr("zvuk_albums"), NULL, open_cb, (void *)(intptr_t)JOB_ALBUMS);
	svcui_nav_card(home_in, tr("zvuk_artists"), NULL, open_cb, (void *)(intptr_t)JOB_ARTISTS);
	svcui_nav_card(home_in, tr("zvuk_playlists"), NULL, open_cb, (void *)(intptr_t)JOB_PLAYLISTS);
	svcui_nav_card(home_in, tr("zvuk_search"), NULL, open_search_cb, NULL);
	saved_card = svcui_nav_card(home_in, tr("zvuk_saved_on_card"), NULL, open_cb, (void *)(intptr_t)JOB_SAVED);
	save_status = svcui_note(home_in, "");
	lv_label_set_long_mode(save_status, LV_LABEL_LONG_DOT);
	lv_obj_set_hidden(save_status, true);
	zvukdl_set_listener(save_listener);

	lv_obj_t *quality_card = sx_card_create(home_in);
	sx_card_title(quality_card, tr("audio_quality"));
	lv_obj_t *pills = sx_card_pills(quality_card);
	static const char *const QUALITY_KEYS[ZVUK_QUALITY_COUNT] = {"zvuk_quality_flac", "zvuk_quality_high",
																 "zvuk_quality_mid"};
	for (int i = 0; i < ZVUK_QUALITY_COUNT; i++) {
		quality_pills[i] = sx_pill(pills, tr(QUALITY_KEYS[i]), i, quality_pick_cb);
	}
	quality_format = lv_label_create(quality_card);
	lv_label_set_long_mode(quality_format, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(quality_format, lv_pct(100));
	lv_obj_add_style(quality_format, &sx_style_text2, 0);
	lv_obj_set_style_text_font(quality_format, &font_ui_20, 0);
	lv_obj_set_style_pad_bottom(quality_format, 8, 0);

	account_card = sx_card_create(home_in);
	sx_card_title(account_card, "");
	sx_card_subtitle(account_card, "", false);
	sx_card_action(account_card, tr("zvuk_sign_out"), logout_cb, NULL);

	home_out = svcui_column(zvuk_screen, cfg);
	build_signed_out(home_out);
	switcher_attach_back_gesture(home_in);
	switcher_attach_back_gesture(home_out);

	lv_obj_add_event_cb(zvuk_screen, home_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(zvuk_screen);
	lv_obj_set_press_lock(zvuk_screen, true);
	theme_register_refresh(quality_refresh);
	cardimport_add_listener(import_listener);
	paint_home();

	build_list_page(cfg);
	build_search_page(cfg);
	zvukplay_init(playback_event);
}

#endif /* SONIX_ZVUK */

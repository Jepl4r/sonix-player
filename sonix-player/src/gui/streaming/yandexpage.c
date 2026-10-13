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
#include "src/gui/nowplaying/trackmenu.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/spinner.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/streaming/streamerror.h"
#include "src/system/core/lang.h"
#include "src/system/decode/growfile.h"
#include "src/system/device/system.h"
#include "src/system/library/library.h"
#include "src/system/playback/device_state.h"
#include "src/system/net/http.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/provider.h"
#include "src/system/streaming/streamturn.h"
#include "src/system/streaming/yandex.h"
#include "src/system/streaming/yandexcache.h"
#include "src/system/streaming/yandexdl.h"
#include "src/system/streaming/yandexlist.h"
#include "src/system/streaming/yandexplay.h"
#include "src/system/streaming/yandexsaved.h"
#include "src/system/streaming/yandexsync.h"
#include "src/system/streaming/yandexwave.h"

// See yandexpage.h. Deliberately smaller than the Tidal page: no artwork in
// the lists (the cover still arrives with each track, beside the file), one
// list page for every kind of row.
//
// What is not the page lives beside it, for any page to reuse: the service
// (yandex.c), long lists by the page (yandexlist.c), playing them
// (yandexplay.c), My Wave (yandexwave.c), likes (yandexsync.c).

#define MAX_ROWS 150
#define LOGIN_QR_SIZE 220

// The list is drawn as a window over it: SLOT_COUNT rows exist and are given
// whichever entries are on screen as it scrolls, so a liked list of five
// thousand costs the same two dozen rows as a list of ten (1.2.5 built every
// row, seven objects each). Rows are ROW_H tall, ROW_PITCH apart.
#define SLOT_COUNT 24
#define ROW_H 80
#define ROW_PITCH (ROW_H + 10)

// ROW_MORE: "All tracks" on an artist's page, the artist in `u.artist`, how
// many in `count`.
typedef enum { ROW_TRACK, ROW_ALBUM, ROW_ARTIST, ROW_PLAYLIST, ROW_MORE } row_kind_t;

typedef struct {
	row_kind_t kind;
	int count;
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
	// The rest of the collection, and what the service puts forward.
	JOB_LIKED_ARTISTS,
	JOB_LIKED_ALBUMS,
	JOB_FEED,
	JOB_CHART,
	JOB_NEW_RELEASES,
	JOB_ARTIST_TRACKS,
	JOB_DISLIKE,
	// Liking an artist or an album (`n`: 1 like, 0 remove), sent in order
	// with the dislikes.
	JOB_LIKE_ARTIST,
	JOB_LIKE_ALBUM,
	// An album's tracks for "Play next" / "Add to end of queue" (`n`: where),
	// in order with the above.
	JOB_QUEUE_ALBUM,
	// The details of one page of the list on screen (yandexlist.h).
	JOB_PAGE,
	// Built on the GUI thread from the index of saved tracks, never sent to
	// the worker: they must open with the network down, and the worker may be
	// waiting out a request that will not be answered.
	JOB_SAVED,
	JOB_SAVED_ALBUM,
} job_kind_t;

typedef struct {
	job_kind_t kind;
	char a[YANDEX_TITLE_MAX]; // id, owner, search text...
	char b[YANDEX_ID_MAX];	  // kind, album id
	int n;					  // the page; for a dislike the seconds heard
	unsigned generation;	  // list jobs: list_generation when asked
	yandexlist_t *list;		  // JOB_PAGE: a reference to the list, the job's
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
	int page; // JOB_PAGE: which
	char error[256];
	stream_err_t error_class; // what kind of failure `error` is (streamerr.h)
	yandex_login_state_t login_state;
	yandex_login_t login; // a copy: the worker keeps polling with its own
	// A list of tracks (liked, a playlist, an album, My Wave): the ids, with
	// details by the page. NULL for the other lists, whose rows follow.
	yandexlist_t *list;
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
// The album bar: what of the record is on the card, and Save or Delete.
static lv_obj_t *album_state, *album_state_label, *album_btn_label, *album_tick;
// The rows drawn (see SLOT_COUNT): each shows entry `index` of the list, -1
// none. A slot is the entry's index modulo SLOT_COUNT, so a scroll rebinds
// only the rows that came into view.
typedef struct {
	lv_obj_t *row, *title, *sub, *mark, *menu;
	int index;
} slot_t;
static slot_t slots[SLOT_COUNT];
// What gives the list its height (an invisible point at the bottom), and an
// empty or error message in place of rows.
static lv_obj_t *list_end, *list_message;
// What the list on screen is (JOB_LIKED, JOB_ALBUM, JOB_SAVED...).
static job_kind_t list_kind;
// The album a JOB_ALBUM list shows, for its "Save album" button.
static char list_album_id[YANDEX_ID_MAX];
static char list_album_title[YANDEX_TITLE_MAX];
// An artist's page: its Like button, and which artist.
static lv_obj_t *artist_bar, *artist_like_label;
static char list_artist_id[YANDEX_ID_MAX];
static gui_config_t *list_cfg;
// search
static lv_obj_t *search_field;

// The answer on screen (GUI thread), which owns its rows or its list. NULL
// before the first list.
static job_result_t *shown;
static bool list_is_wave;
// Raised by each list asked for (GUI thread): an answer carrying an older
// number belongs to a list the user has since left, and is dropped.
static unsigned list_generation;
// The answer being built (worker thread only), copied into a job_result_t
// when the job is done.
static row_t *result;
static int result_count;
static yandexlist_t *result_list;
static char result_error[256];
static stream_err_t result_class;

// Worker. Three kinds of work, each with its own place, so that none can
// push another out:
//   * `pending`: what the user just asked for -- a list, a sign-in step. One
//     at a time: a newer request replaces an older one still waiting (the
//     user changed their mind).
//   * `page_pending`: the details of the rows coming into view. A newer page
//     replaces an older one: the list has scrolled on.
//   * `events`: dislikes, each one sent.
// Playing (yandexplay.c) and My Wave (yandexwave.c) have workers of their
// own: in 1.2.5 the fetch of the next track and the wave's "more" shared the
// one `pending` slot with the lists, and whichever came second wiped out the
// other without a word.
static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_cond = PTHREAD_COND_INITIALIZER;
static job_t pending;
static job_t page_pending;
#define EVENTS_MAX 8
static job_t events[EVENTS_MAX];
static int events_count;
static bool worker_started;
// The page asked for and not answered yet, so a scroll does not ask for it
// again every frame; and the last one that failed, not asked again for a few
// seconds -- with the network down every frame of a scroll would (GUI thread).
static int page_asked = -1;
static int page_failed = -1;
static time_t page_failed_at;

static void job_done_async(void *user);

static void submit(const job_t *job) {
	pthread_mutex_lock(&job_lock);
	if (job->kind == JOB_DISLIKE || job->kind == JOB_LIKE_ARTIST || job->kind == JOB_LIKE_ALBUM || job->kind == JOB_QUEUE_ALBUM) {
		if (events_count < EVENTS_MAX) {
			events[events_count++] = *job;
		}
	} else if (job->kind == JOB_PAGE) {
		yandexlist_free(page_pending.list); // replaced: its reference goes
		page_pending = *job;
	} else {
		pending = *job;
	}
	pthread_cond_signal(&job_cond);
	pthread_mutex_unlock(&job_lock);
}

static void ensure_buffers(void) {
	if (!result) {
		result = calloc(MAX_ROWS, sizeof(*result));
	}
}

// ---------------------------------------------------------------------------
// worker
// ---------------------------------------------------------------------------

// yandex_ref_cb: each entry of a long list, into the list being built.
static void add_ref_cb(const char *track_id, const char *album_id, const yandex_track_t *track, void *user) {
	yandexlist_t *list = user;
	if (track) {
		yandexlist_add_track(list, track);
	} else {
		yandexlist_add_id(list, track_id, album_id);
	}
}

// A list of tracks whose details came whole (an album, My Wave's first
// batch), as a yandexlist like the long ones. -1 when out of memory.
static int list_from_tracks(const yandex_track_t *tracks, int got) {
	if (got < 0) {
		return got;
	}
	result_list = yandexlist_new();
	for (int i = 0; result_list && i < got; i++) {
		yandexlist_add_track(result_list, &tracks[i]);
	}
	if (!result_list) {
		snprintf(result_error, sizeof(result_error), "%s", tr("out_of_memory"));
		return -1;
	}
	return got;
}

// A long list: the ids (one request), and the details of its first page (a
// second), so the first rows show at once. What it costs is logged.
static int list_from_refs(int got, const char *what) {
	if (got < 0 || !result_list) {
		yandexlist_free(result_list);
		result_list = NULL;
		return -1;
	}
	if (got > 0 && !yandexlist_page_ready(result_list, 0) && yandexlist_fetch_page(result_list, 0) < 0) {
		// The ids are here: the rows show "loading" and ask again as they
		// come into view.
		fprintf(stderr, "yandex: %s: the first page did not come (%s)\n", what, yandex_last_error());
	}
	printf("yandex: %s: %d tracks, the list holds %zu bytes\n", what, got, yandexlist_memory(result_list));
	return got;
}

// How many of an artist's tracks its page lists before "All tracks".
#define ARTIST_POPULAR 10

static void add_playlist_rows(const yandex_playlist_t *p, int got) {
	for (int i = 0; i < got && result_count < MAX_ROWS; i++) {
		result[result_count].kind = ROW_PLAYLIST;
		result[result_count++].u.playlist = p[i];
	}
}

static void add_album_rows(const yandex_album_t *a, int got) {
	for (int i = 0; i < got && result_count < MAX_ROWS; i++) {
		result[result_count].kind = ROW_ALBUM;
		result[result_count++].u.album = a[i];
	}
}

// The liked artists and albums, read once a session: the artist page's
// Like button and the album menus say "Remove like" from them.
static void ensure_collection(void) {
	if (!yandex_liked_artists_known()) {
		yandex_liked_artists(NULL, 0);
	}
	if (!yandex_liked_albums_known()) {
		yandex_liked_albums(NULL, 0);
	}
}

static void run_list_job(const job_t *job) {
	// Room for the answers that come whole (an album, a search...); the long
	// lists come as ids and need none.
	yandex_track_t *tmp = NULL;
	if (job->kind != JOB_LIKED && job->kind != JOB_PLAYLIST && job->kind != JOB_CHART && job->kind != JOB_ARTIST_TRACKS) {
		tmp = calloc(MAX_ROWS * 2, sizeof(*tmp));
		if (!tmp) {
			snprintf(result_error, sizeof(result_error), "%s", tr("out_of_memory"));
			return;
		}
	}
	int got = -1;
	result_count = 0;
	result_list = NULL;
	switch (job->kind) {
	case JOB_LIKED:
		result_list = yandexlist_new();
		got = list_from_refs(result_list ? yandex_liked_ids(add_ref_cb, result_list) : -1, "liked list");
		break;
	case JOB_PLAYLIST:
		result_list = yandexlist_new();
		got = list_from_refs(result_list ? yandex_playlist_ids(job->a, job->b, add_ref_cb, result_list) : -1, "playlist");
		break;
	case JOB_ALBUM:
		ensure_collection(); // the album's Like in its menu
		got = list_from_tracks(tmp, yandex_album_tracks(job->a, tmp, MAX_ROWS * 2));
		break;
	case JOB_WAVE:
		got = list_from_tracks(tmp, yandex_wave_start(NULL, 0, tmp, MAX_ROWS));
		break;
	case JOB_CHART:
		result_list = yandexlist_new();
		got = list_from_refs(result_list ? yandex_chart(add_ref_cb, result_list) : -1, "chart");
		break;
	case JOB_ARTIST_TRACKS:
		result_list = yandexlist_new();
		got = list_from_refs(result_list ? yandex_artist_track_ids(job->a, add_ref_cb, result_list) : -1, "artist's tracks");
		break;
	case JOB_PLAYLISTS: {
		// The account's own, then those it liked.
		yandex_playlist_t *p = (yandex_playlist_t *)tmp; // smaller than a track
		got = yandex_playlists(p, MAX_ROWS);
		int liked = got >= 0 ? yandex_liked_playlists(p + got, MAX_ROWS - got) : -1;
		got += liked > 0 ? liked : 0;
		add_playlist_rows(p, got);
		break;
	}
	case JOB_FEED: {
		yandex_playlist_t *p = (yandex_playlist_t *)tmp;
		got = yandex_feed_playlists(p, MAX_ROWS);
		add_playlist_rows(p, got);
		break;
	}
	case JOB_LIKED_ARTISTS: {
		yandex_artist_t *a = (yandex_artist_t *)tmp;
		got = yandex_liked_artists(a, MAX_ROWS);
		for (int i = 0; i < got; i++) {
			result[result_count].kind = ROW_ARTIST;
			result[result_count++].u.artist = a[i];
		}
		break;
	}
	case JOB_LIKED_ALBUMS:
	case JOB_NEW_RELEASES: {
		if (job->kind == JOB_NEW_RELEASES) {
			ensure_collection(); // the rows' menus say "Remove like" for a liked album
		}
		yandex_album_t *a = (yandex_album_t *)tmp;
		got = job->kind == JOB_LIKED_ALBUMS ? yandex_liked_albums(a, MAX_ROWS) : yandex_new_releases(a, MAX_ROWS);
		add_album_rows(a, got);
		break;
	}
	case JOB_ARTIST: {
		// The artist's page: the most listened-to tracks, "All tracks" when
		// there are more, then the albums. The liked artists are read the
		// first time, for the page's Like button.
		ensure_collection();
		int total = 0;
		int nt = yandex_artist_tracks(job->a, tmp, ARTIST_POPULAR, &total);
		for (int i = 0; i < nt; i++) {
			result[result_count].kind = ROW_TRACK;
			result[result_count++].u.track = tmp[i];
		}
		if (total > nt && nt > 0) {
			row_t *more = &result[result_count++];
			memset(more, 0, sizeof(*more));
			more->kind = ROW_MORE;
			more->count = total;
			snprintf(more->u.artist.id, sizeof(more->u.artist.id), "%.*s", (int)sizeof(more->u.artist.id) - 1, job->a); // an id: digits
		}
		yandex_album_t *a = (yandex_album_t *)(tmp + ARTIST_POPULAR + 1); // past the tracks
		int na = yandex_artist_albums(job->a, a, MAX_ROWS - result_count);
		add_album_rows(a, na);
		got = nt < 0 && na < 0 ? -1 : result_count;
		break;
	}
	case JOB_SEARCH: {
		// Artists first, then albums, then tracks: one list, short of each.
		ensure_collection();
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
		yandexlist_free(result_list);
		result_list = NULL;
		result_class = streamerr_describe("yandex", yandex_last_error(), result_error, sizeof(result_error));
	}
	free(tmp);
}

// Packs the job's answer -- the rows built in `result` or the list in
// `result_list`, the error, whatever else it carries -- into a block of its
// own, for the GUI thread to take over and free. NULL when there is no memory
// for even the bare block: nothing is posted then, and the spinner stays up,
// which is the truth.
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
		yandexlist_free(result_list);
		result_list = NULL;
		return NULL;
	}
	memset(r, 0, sizeof(*r));
	r->kind = job->kind;
	r->generation = job->generation;
	snprintf(r->error, sizeof(r->error), "%s", result_error);
	r->error_class = result_class;
	r->count = count;
	r->list = result_list;
	result_list = NULL;
	if (count > 0) {
		memcpy(r->rows, result, (size_t)count * sizeof(row_t));
	}
	return r;
}

static void free_result(job_result_t *r) {
	if (r) {
		yandexlist_free(r->list);
		free(r);
	}
}

static void *worker_main(void *arg) {
	(void)arg;
	yandex_login_state_t login_state = YANDEX_LOGIN_PENDING;
	// The page waits on these requests: a dead network is said in seconds.
	http_set_thread_timeout_cap(STREAM_UI_TIMEOUT_SECS);
	for (;;) {
		pthread_mutex_lock(&job_lock);
		while (pending.kind == JOB_NONE && page_pending.kind == JOB_NONE && events_count == 0) {
			pthread_cond_wait(&job_cond, &job_lock);
		}
		// What the user asked for first, then the rows coming into view, then
		// the dislikes (sent in order, none dropped).
		job_t job;
		if (pending.kind != JOB_NONE) {
			job = pending;
			pending.kind = JOB_NONE;
		} else if (page_pending.kind != JOB_NONE) {
			job = page_pending;
			page_pending.kind = JOB_NONE;
			page_pending.list = NULL; // the job's now
		} else {
			job = events[0];
			memmove(events, events + 1, (size_t)(--events_count) * sizeof(events[0]));
		}
		pthread_mutex_unlock(&job_lock);

		result_error[0] = '\0';
		result_class = STREAM_ERR_NONE;
		streamerr_begin();
		result_count = 0;
		result_list = NULL;
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
		case JOB_DISLIKE:
			if (!yandex_dislike_track(job.a, job.b, job.n)) {
				snprintf(result_error, sizeof(result_error), "%s", yandex_last_error());
			}
			break;
		case JOB_QUEUE_ALBUM: {
			yandex_track_t *tmp = calloc(MAX_ROWS * 2, sizeof(*tmp));
			int got = tmp ? yandex_album_tracks(job.a, tmp, MAX_ROWS * 2) : -1;
			if (got < 0) {
				result_class = streamerr_describe("yandex", tmp ? yandex_last_error() : tr("out_of_memory"), result_error, sizeof(result_error));
			}
			for (int i = 0; i < got && result_count < MAX_ROWS; i++) {
				if (tmp[i].available) {
					result[result_count].kind = ROW_TRACK;
					result[result_count++].u.track = tmp[i];
				}
			}
			free(tmp);
			break;
		}
		case JOB_LIKE_ARTIST:
		case JOB_LIKE_ALBUM: {
			bool ok = job.kind == JOB_LIKE_ARTIST ? yandex_like_artist(job.a, job.n != 0) : yandex_like_album(job.a, job.n != 0);
			if (!ok) {
				snprintf(result_error, sizeof(result_error), "%s", yandex_last_error());
			}
			break;
		}
		case JOB_PAGE:
			if (yandexlist_fetch_page(job.list, job.n) < 0) {
				result_class = streamerr_describe("yandex", yandex_last_error(), result_error, sizeof(result_error));
			}
			result_list = job.list; // the reference travels back with the answer
			job.list = NULL;
			break;
		default:
			run_list_job(&job);
			break;
		}
		job_result_t *r = job_result_new(&job, result_count);
		if (r) {
			r->login_state = login_state;
			r->page = job.n;
			if (job.kind == JOB_LOGIN_BEGIN || job.kind == JOB_LOGIN_POLL) {
				r->login = login;
			}
			if (!gui_post(job_done_async, r)) {
				free_result(r);
			}
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
// playback (yandexplay.c does it; the page hears how it went)
// ---------------------------------------------------------------------------

static void playback_event(yandexplay_event_t event, const char *text) {
	lv_obj_set_hidden(list_spin, true);
	switch (event) {
	case YANDEXPLAY_STARTED:
		player_refresh_now_playing();
		switch_screen(player_screen);
		break;
	case YANDEXPLAY_FAILED:
		toast_error(text[0] ? text : tr("download_failed"));
		break;
	case YANDEXPLAY_SKIPPED: {
		char line[320];
		snprintf(line, sizeof(line), tr("yandex_skipped"), text);
		toast_error(line);
		break;
	}
	}
}

// Once a second on the GUI thread, whatever page is up.
static void queue_watch_cb(lv_timer_t *timer) {
	(void)timer;
	yandexplay_watch();
}

// ---------------------------------------------------------------------------
// list page
// ---------------------------------------------------------------------------

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
	case ROW_MORE:
		snprintf(name, ns, tr("yandex_all_tracks"), r->count);
		detail[0] = '\0';
		break;
	}
}

static void row_menu_cb(lv_event_t *e);

// Rows with something to offer on their own get the three dots: a track (its
// like, saving it, queueing it), an album (the same for the whole record), an
// artist (its like).
static bool row_has_menu(const row_t *r) { return (r->kind == ROW_TRACK && r->u.track.id[0]) || (r->kind == ROW_ALBUM && r->u.album.id[0]) || (r->kind == ROW_ARTIST && r->u.artist.id[0]); }

// How many entries the list on screen has, and entry `index` as a row: from
// the answer's rows, or from its list of tracks. False for a track whose
// details have not come yet: `out` then carries its ids only, and the row
// says "Loading...".
static int view_count(void) {
	if (!shown) {
		return 0;
	}
	return shown->list ? yandexlist_count(shown->list) : shown->count;
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
	if (yandexlist_track(shown->list, index, &out->u.track)) {
		return true;
	}
	yandexlist_ids(shown->list, index, out->u.track.id, sizeof(out->u.track.id), out->u.track.album_id, sizeof(out->u.track.album_id));
	return false;
}

// ---------------------------------------------------------------------------
// what is on the card: the marks and the album bar
// ---------------------------------------------------------------------------

// A track is saved when the index has it; an album when the index holds as
// many of its tracks as the service says it has (an album row that does not
// say is not marked: a part saved must not look like the whole). In the Saved
// list the album rows count only what is saved, so they are always marked
// there -- they are on the card.
static bool row_saved(const row_t *r) {
	if (r->kind == ROW_TRACK) {
		return r->u.track.id[0] && yandexsaved_has(r->u.track.id);
	}
	if (r->kind == ROW_ALBUM && r->u.album.id[0] && r->u.album.track_count > 0) {
		return yandexsaved_album_count(r->u.album.id) >= r->u.album.track_count;
	}
	return false;
}

// The ticks are the accent colour, set by hand on an image: a new accent
// reaches them through the theme's refresh.
static void marks_recolor(void) {
	lv_color_t accent = theme()->accent;
	if (album_tick) {
		lv_obj_set_style_image_recolor(album_tick, accent, 0);
	}
	for (int i = 0; i < SLOT_COUNT; i++) {
		if (slots[i].mark) {
			lv_obj_set_style_image_recolor(slots[i].mark, accent, 0);
		}
	}
}

// ---------------------------------------------------------------------------
// the rows: a window over the list
// ---------------------------------------------------------------------------

static void slot_clicked_cb(lv_event_t *e);

static void make_slot(slot_t *slot) {
	lv_obj_t *row = lv_btn_create(list_box);
	lv_obj_set_size(row, lv_pct(100), ROW_H);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, 12, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_event_bubble(row, true);
	lv_obj_add_event_cb(row, slot_clicked_cb, LV_EVENT_CLICKED, slot);

	// The two lines sit on the row itself, left of the mark and the dots: no
	// flex container between, two objects fewer a row.
	slot->title = lv_label_create(row);
	lv_label_set_long_mode(slot->title, LV_LABEL_LONG_DOT);
	lv_obj_add_style(slot->title, &theme_style_text, 0);
	lv_obj_set_style_text_font(slot->title, &font_ui_22, 0);
	lv_obj_align(slot->title, LV_ALIGN_TOP_LEFT, 16, 12);

	slot->sub = lv_label_create(row);
	lv_label_set_long_mode(slot->sub, LV_LABEL_LONG_DOT);
	lv_obj_add_style(slot->sub, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(slot->sub, &font_ui_18, 0);
	lv_obj_align(slot->sub, LV_ALIGN_TOP_LEFT, 16, 44);

	// The mark: a small tick for a track on the card, or an album all of
	// whose tracks are.
	slot->mark = lv_image_create(row);
	lv_image_set_src(slot->mark, &icon_check_small);
	lv_obj_add_style(slot->mark, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor(slot->mark, theme()->accent, 0);
	lv_obj_set_style_image_recolor_opa(slot->mark, LV_OPA_COVER, 0);
	lv_obj_align(slot->mark, LV_ALIGN_RIGHT_MID, -58, 0);
	lv_obj_set_hidden(slot->mark, true);

	lv_obj_t *menu_btn = lv_btn_create(row);
	lv_obj_set_size(menu_btn, 48, 48);
	lv_obj_set_style_bg_opa(menu_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(menu_btn, 0, 0);
	lv_obj_set_style_shadow_width(menu_btn, 0, 0);
	lv_obj_set_style_pad_all(menu_btn, 0, 0);
	lv_obj_set_ext_click_area(menu_btn, 12);
	lv_obj_align(menu_btn, LV_ALIGN_RIGHT_MID, -6, 0);
	lv_obj_add_event_cb(menu_btn, row_menu_cb, LV_EVENT_CLICKED, slot);
	lv_obj_t *dots = lv_image_create(menu_btn);
	lv_image_set_src(dots, &icon_ellipsis_vertical);
	lv_obj_add_style(dots, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(dots, LV_OPA_COVER, 0);
	lv_obj_center(dots);
	slot->menu = menu_btn;

	slot->row = row;
	slot->index = -1;
	lv_obj_set_hidden(row, true);
}

// Puts entry `index` on the slot: its texts, its mark, its dots.
static void bind_slot(slot_t *slot, int index) {
	slot->index = index;
	row_t r;
	bool have = view_row(index, &r);
	char name[YANDEX_TITLE_MAX], detail[YANDEX_TITLE_MAX + 32];
	if (have) {
		row_texts(&r, name, sizeof(name), detail, sizeof(detail));
	} else {
		snprintf(name, sizeof(name), "%s", tr("loading"));
		detail[0] = '\0';
	}
	lv_label_set_text(slot->title, name);
	lv_label_set_text(slot->sub, detail);
	// A row of one line ("All tracks") has it in the middle.
	lv_obj_set_hidden(slot->sub, !detail[0]);
	lv_obj_align(slot->title, detail[0] ? LV_ALIGN_TOP_LEFT : LV_ALIGN_LEFT_MID, 16, detail[0] ? 12 : 0);
	bool menu = have && row_has_menu(&r);
	bool mark = have && (r.kind == ROW_TRACK || r.kind == ROW_ALBUM) && row_saved(&r);
	lv_obj_set_hidden(slot->menu, !menu);
	lv_obj_set_hidden(slot->mark, !mark);
	// The texts stop short of whatever is on their right.
	int right = menu ? 66 : 16;
	right += mark ? 30 : 0;
	int width = lv_obj_get_content_width(list_box) - 16 - right;
	lv_obj_set_width(slot->title, width);
	lv_obj_set_width(slot->sub, width);
	lv_obj_set_pos(slot->row, 0, index * ROW_PITCH);
	lv_obj_set_hidden(slot->row, false);
}

static void request_page(int page);

// Gives the rows in view (and a few either side) their entries, and asks for
// the details of the pages they show -- the next page too once the view nears
// its end, so scrolling on finds it there. Cheap when nothing moved: a slot
// already showing its entry is left alone.
static void bind_view(bool force) {
	if (!slots[0].row) {
		return; // no list shown yet
	}
	int count = view_count();
	int first = (int)(lv_obj_get_scroll_y(list_box) / ROW_PITCH) - 4;
	if (first > count - SLOT_COUNT) {
		first = count - SLOT_COUNT;
	}
	if (first < 0) {
		first = 0;
	}
	for (int k = 0; k < SLOT_COUNT; k++) {
		int index = first + k;
		slot_t *slot = &slots[index % SLOT_COUNT];
		if (index >= count) {
			slot->index = -1;
			lv_obj_set_hidden(slot->row, true);
		} else if (force || slot->index != index) {
			bind_slot(slot, index);
		}
	}
	if (shown && shown->list && count > 0) {
		int last = first + SLOT_COUNT - 1 < count - 1 ? first + SLOT_COUNT - 1 : count - 1;
		int pages[3] = {first / YANDEXLIST_PAGE, last / YANDEXLIST_PAGE, (last + YANDEXLIST_PAGE / 2) / YANDEXLIST_PAGE};
		for (int i = 0; i < 3; i++) {
			if (pages[i] * YANDEXLIST_PAGE < count && !yandexlist_page_ready(shown->list, pages[i])) {
				request_page(pages[i]);
				break;
			}
		}
	}
}

static void request_page(int page) {
	if (page == page_asked || (page == page_failed && time(NULL) - page_failed_at < 5)) {
		return;
	}
	page_asked = page;
	start_worker();
	job_t job = {.kind = JOB_PAGE, .n = page, .generation = list_generation, .list = yandexlist_ref(shown->list)};
	submit(&job);
}

static void list_scrolled_cb(lv_event_t *e) {
	(void)e;
	bind_view(false);
}

static void refresh_marks(void) {
	for (int i = 0; i < SLOT_COUNT; i++) {
		if (slots[i].row && slots[i].index >= 0) {
			row_t r;
			bool have = view_row(slots[i].index, &r);
			lv_obj_set_hidden(slots[i].mark, !(have && (r.kind == ROW_TRACK || r.kind == ROW_ALBUM) && row_saved(&r)));
		}
	}
}

// The album page's own tracks are the measure: how many of the playable ones
// are on the card.
static void album_saved_counts(int *saved, int *playable) {
	*saved = *playable = 0;
	int count = view_count();
	for (int i = 0; i < count; i++) {
		row_t r;
		if (view_row(i, &r) && r.kind == ROW_TRACK && r.u.track.available) {
			(*playable)++;
			*saved += yandexsaved_has(r.u.track.id);
		}
	}
}

static void update_album_bar(void) {
	if (lv_obj_is_hidden(album_bar)) {
		return;
	}
	int saved, playable;
	album_saved_counts(&saved, &playable);
	bool complete = playable > 0 && saved == playable;
	lv_obj_set_hidden(album_state, saved == 0);
	if (complete) {
		lv_label_set_text(album_state_label, tr("yandex_saved_on_card"));
	} else if (saved > 0) {
		lv_label_set_text_fmt(album_state_label, tr("yandex_saved_some"), saved, playable);
	}
	lv_label_set_text(album_btn_label, tr(complete ? "delete" : "yandex_save_album"));
}

static void retry_list(void);

// Empties the page: no rows bound, no message. The rows themselves are made
// here the first time a list is shown: a player that never opens a Yandex
// list never has them.
static void clear_list(void) {
	if (!slots[0].row) {
		for (int i = 0; i < SLOT_COUNT; i++) {
			make_slot(&slots[i]);
		}
	}
	for (int i = 0; i < SLOT_COUNT; i++) {
		slots[i].index = -1;
		lv_obj_set_hidden(slots[i].row, true);
	}
	if (list_message) {
		lv_obj_delete(list_message);
		list_message = NULL;
	}
	lv_obj_set_pos(list_end, 0, 0);
	page_asked = -1;
}

static void fill_list(void) {
	clear_list();
	int count = view_count();
	lv_obj_scroll_to_y(list_box, 0, LV_ANIM_OFF);
	// The bottom of the list, for the scroll to know its height.
	lv_obj_set_pos(list_end, 0, count > 0 ? count * ROW_PITCH - 10 : 0);
	bind_view(true);
	update_album_bar();
	if (count == 0 && shown && shown->error[0] && shown->error_class != STREAM_ERR_NONE) {
		// What went wrong and "Retry", as on the other services' pages.
		list_message = streamerror_create(list_box);
		lv_obj_set_width(list_message, lv_pct(100));
		streamerror_show(list_message, shown->error_class, shown->error, retry_list, NULL);
	} else if (count == 0) {
		list_message = lv_label_create(list_box);
		// An empty list with a word of its own (nothing saved on the card yet)
		// says it; any other empty list the usual line.
		lv_label_set_text(list_message, shown && shown->error[0] ? shown->error : tr("yandex_empty"));
		lv_label_set_long_mode(list_message, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(list_message, lv_pct(100));
		lv_obj_add_style(list_message, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(list_message, &font_ui_20, 0);
	}
}

// ---------------------------------------------------------------------------
// going back inside the list page
//
// Every Yandex list -- search results, an artist's albums, an album, a
// playlist, the saved ones -- lives on ONE reused page, as on the Qobuz page.
// The screen manager sees the same screen whichever list is up, so it has
// nothing to stack: from an album opened off the search results, back used to
// land on the search page, skipping the results (and, opened off an artist off
// the results, skipping both). The page keeps its own history of the lists it
// showed; the chevron and the swipe go through list_back_guard() first and
// only leave the page once it is empty.
//
// My Wave is never stacked: going back to it would start a new session and
// throw away what is playing. A list opened from outside the page (the home
// rows, "Show album" from the player over another page) starts the history
// afresh.
// ---------------------------------------------------------------------------

typedef struct {
	job_kind_t kind;
	char title[YANDEX_TITLE_MAX];
	char a[YANDEX_TITLE_MAX];
	char b[YANDEX_ID_MAX];
} list_step_t;

#define LIST_HISTORY_DEPTH 8
static list_step_t list_history[LIST_HISTORY_DEPTH];
static int list_history_len;
static list_step_t list_current;
static bool list_has_current;

static void show_list(const list_step_t *step);
static void artist_bar_refresh(void);

static bool list_back_has_step(void) { return list_history_len > 0; }

static bool list_back_guard(void) {
	if (list_history_len == 0) {
		list_has_current = false;
		return false; // nothing underneath: leave the page
	}
	list_current = list_history[--list_history_len];
	show_list(&list_current);
	return true;
}

static void open_list(const char *title, job_kind_t kind, const char *a, const char *b) {
	list_step_t step;
	memset(&step, 0, sizeof(step));
	step.kind = kind;
	snprintf(step.title, sizeof(step.title), "%s", title ? title : "");
	snprintf(step.a, sizeof(step.a), "%s", a ? a : "");
	snprintf(step.b, sizeof(step.b), "%s", b ? b : "");
	if (lv_screen_active() != list_screen) {
		list_history_len = 0; // entering from outside: the history restarts
	} else if (list_has_current && list_current.kind != JOB_WAVE && memcmp(&list_current, &step, sizeof(step)) != 0) {
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

// The rows of a list built from the index of saved tracks: the albums, then
// tracks saved without one; or an album's tracks. A block like a worker's
// answer, so job_done_async shows it the same way -- built here, on the GUI
// thread, never in the worker's buffers.
static job_result_t *saved_result(job_kind_t kind, const char *album_id) {
	job_result_t *r = calloc(1, sizeof(*r) + MAX_ROWS * sizeof(row_t));
	yandex_track_t *tracks = calloc(MAX_ROWS, sizeof(*tracks));
	if (!r || !tracks) {
		free(r);
		free(tracks);
		return NULL;
	}
	r->kind = kind;
	r->generation = list_generation;
	int count = 0;
	if (kind == JOB_SAVED) {
		yandex_album_t *albums = (yandex_album_t *)tracks; // smaller than a track
		int n = yandexsaved_albums(albums, MAX_ROWS);
		for (int i = 0; i < n; i++) {
			r->rows[count].kind = ROW_ALBUM;
			r->rows[count++].u.album = albums[i];
		}
		n = yandexsaved_tracks(NULL, tracks, MAX_ROWS - count);
		for (int i = 0; i < n; i++) {
			r->rows[count].kind = ROW_TRACK;
			r->rows[count++].u.track = tracks[i];
		}
	} else {
		int n = yandexsaved_tracks(album_id, tracks, MAX_ROWS);
		for (int i = 0; i < n; i++) {
			r->rows[count].kind = ROW_TRACK;
			r->rows[count++].u.track = tracks[i];
		}
	}
	free(tracks);
	r->count = count;
	if (count == 0) {
		snprintf(r->error, sizeof(r->error), "%s", tr("yandex_saved_none"));
	}
	return r;
}

static void show_list(const list_step_t *step) {
	job_kind_t kind = step->kind;
	const char *title = step->title, *a = step->a, *b = step->b;
	list_kind = kind;
	lv_label_set_text(list_title, title);
	list_is_wave = kind == JOB_WAVE;
	bool is_album = (kind == JOB_ALBUM || kind == JOB_SAVED_ALBUM) && a[0];
	if (is_album) {
		snprintf(list_album_id, sizeof(list_album_id), "%.*s", (int)sizeof(list_album_id) - 1, a); // an id: digits
		snprintf(list_album_title, sizeof(list_album_title), "%s", title);
	}
	bool is_artist = kind == JOB_ARTIST && a[0];
	if (is_artist) {
		snprintf(list_artist_id, sizeof(list_artist_id), "%.*s", (int)sizeof(list_artist_id) - 1, a);
		artist_bar_refresh();
	}
	lv_obj_set_hidden(wave_bar, !list_is_wave);
	lv_obj_set_hidden(album_bar, !is_album);
	lv_obj_set_hidden(artist_bar, !is_artist);
	// The list starts under the like/dislike bar (or the album's, or the
	// artist's) only when there is one.
	int top = settingsrow_content_top(list_cfg);
	int y = top + (list_is_wave || is_album || is_artist ? 66 : 0);
	lv_obj_set_pos(list_box, list_cfg->padding, y);
	lv_obj_set_height(list_box, list_cfg->screen_height - y - 4);
	clear_list();
	switch_screen(list_screen);
	list_generation++;
	if (kind == JOB_SAVED || kind == JOB_SAVED_ALBUM) {
		job_result_t *r = saved_result(kind, a);
		if (r) {
			job_done_async(r);
		}
		return;
	}
	lv_obj_set_hidden(list_spin, false);
	run(kind, a, b, 0);
}

// The error panel's "Retry": the list that failed, asked for again, without
// a new step in the page's history.
static void retry_list(void) {
	if (list_has_current) {
		show_list(&list_current);
	}
}

// Plays the list from the tapped track: the whole of it becomes the queue --
// a liked list of thousands as ids, each track's details and file fetched
// when playback nears it (yandexplay.c). A list of mixed rows (search, an
// artist, what is saved) queues its tracks.
static void play_rows(int tapped) {
	row_t r;
	bool have = view_row(tapped, &r);
	if (have && r.kind == ROW_TRACK && !r.u.track.available) {
		toast_error(tr("yandex_no_file"));
		return;
	}
	lv_obj_set_hidden(list_spin, false);
	if (shown && shown->list) {
		if (!yandexplay_list(shown->list, tapped, list_is_wave)) {
			lv_obj_set_hidden(list_spin, true);
		}
		return;
	}
	yandexlist_t *tracks = yandexlist_new();
	int start = -1;
	int count = view_count();
	for (int i = 0; tracks && i < count; i++) {
		if (view_row(i, &r) && r.kind == ROW_TRACK && r.u.track.available && yandexlist_add_track(tracks, &r.u.track) && i == tapped) {
			start = yandexlist_count(tracks) - 1;
		}
	}
	if (start < 0 || !yandexplay_list(tracks, start, false)) {
		lv_obj_set_hidden(list_spin, true);
	}
	yandexlist_free(tracks);
}

static void slot_clicked_cb(lv_event_t *e) {
	const slot_t *slot = lv_event_get_user_data(e);
	row_t r;
	if (!slot || slot->index < 0) {
		return;
	}
	if (!view_row(slot->index, &r)) {
		// A track still loading: it plays all the same, its details come
		// with its file.
		if (shown && shown->list) {
			play_rows(slot->index);
		}
		return;
	}
	switch (r.kind) {
	case ROW_TRACK:
		play_rows(slot->index);
		break;
	case ROW_ALBUM:
		// In the Saved list an album opens from the card, network or not.
		open_list(r.u.album.title, list_kind == JOB_SAVED ? JOB_SAVED_ALBUM : JOB_ALBUM, r.u.album.id, NULL);
		break;
	case ROW_ARTIST:
		open_list(r.u.artist.name, JOB_ARTIST, r.u.artist.id, NULL);
		break;
	case ROW_PLAYLIST:
		open_list(r.u.playlist.title, JOB_PLAYLIST, r.u.playlist.owner, r.u.playlist.kind);
		break;
	case ROW_MORE:
		// Every track of the artist, by the page; titled as the artist.
		open_list(list_current.title, JOB_ARTIST_TRACKS, r.u.artist.id, NULL);
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

// ---------------------------------------------------------------------------
// liking an artist or an album
//
// Sent on the worker with the dislikes; the toast says what happened once
// the service has answered. The artist page's button shows the new state at
// once and goes back if the service refuses.
// ---------------------------------------------------------------------------

static void artist_bar_refresh(void) {
	if (artist_like_label) {
		lv_label_set_text(artist_like_label, tr(yandex_artist_liked(list_artist_id) ? "yandex_unlike" : "yandex_like"));
	}
}

static void like_object(job_kind_t kind, const char *id, bool like) {
	run(kind, id, NULL, like ? 1 : 0);
	if (kind == JOB_LIKE_ARTIST && strcmp(id, list_artist_id) == 0 && artist_like_label) {
		lv_label_set_text(artist_like_label, tr(like ? "yandex_unlike" : "yandex_like"));
	}
}

static void do_album_like(void *user) { like_object(JOB_LIKE_ALBUM, menu_row.u.album.id, user != NULL); }
static void do_artist_like(void *user) { like_object(JOB_LIKE_ARTIST, menu_row.u.artist.id, user != NULL); }

static void artist_like_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active() || !list_artist_id[0]) {
		return;
	}
	like_object(JOB_LIKE_ARTIST, list_artist_id, !yandex_artist_liked(list_artist_id));
}

// The answer: said in a toast; the button follows what the account now says.
static void like_object_done(const job_result_t *r) {
	bool artist = r->kind == JOB_LIKE_ARTIST;
	if (r->error[0]) {
		toast_error(tr("yandex_like_failed"));
	} else if (artist) {
		toast_success(tr(r->page ? "yandex_artist_liked" : "yandex_artist_unliked"));
	} else {
		toast_success(tr(r->page ? "yandex_album_liked" : "yandex_album_unliked"));
	}
	artist_bar_refresh();
}

// ---------------------------------------------------------------------------
// "Play next" and "Add to end of queue"
//
// The same two lines as on the Library's rows, through the same door
// (trackmenu_queue_paths: the queue written down once, the toast). A Yandex
// track goes in by its file -- the saved one, or the cache's, fetched when
// playback reaches it (yandexplay.c knows the track from here on, whatever
// queue it lands in). An album's tracks are asked for first, unless the
// album is on the card.
// ---------------------------------------------------------------------------

static void queue_tracks(const yandex_track_t *tracks, int count, playlist_queue_where_t where) {
	char **paths = NULL;
	int n = yandexplay_paths_for(tracks, count, &paths);
	if (n > 0) {
		trackmenu_queue_paths((const char *const *)paths, n, where);
	} else {
		gui_notify_popup("playlist_cannot_add_the_tracks");
	}
	free(paths);
}

static void do_row_queue(void *user) {
	playlist_queue_where_t where = (playlist_queue_where_t)(intptr_t)user;
	if (menu_row.kind == ROW_TRACK) {
		queue_tracks(&menu_row.u.track, 1, where);
		return;
	}
	if (menu_row.kind != ROW_ALBUM) {
		return;
	}
	// In the Saved list, or saved whole: from the card, network or not.
	int saved = yandexsaved_album_count(menu_row.u.album.id);
	if (saved > 0 && (list_kind == JOB_SAVED || row_saved(&menu_row))) {
		yandex_track_t *tracks = calloc((size_t)saved, sizeof(*tracks));
		int n = tracks ? yandexsaved_tracks(menu_row.u.album.id, tracks, saved) : 0;
		queue_tracks(tracks, n, where);
		free(tracks);
		return;
	}
	run(JOB_QUEUE_ALBUM, menu_row.u.album.id, NULL, (int)where);
}

// The album's tracks have come: queued now.
static void queue_album_done(job_result_t *r) {
	if (r->count == 0) {
		toast_error(r->error[0] ? r->error : tr("playlist_cannot_add_the_tracks"));
		return;
	}
	yandex_track_t *tracks = malloc((size_t)r->count * sizeof(*tracks));
	if (!tracks) {
		toast_error(tr("out_of_memory"));
		return;
	}
	for (int i = 0; i < r->count; i++) {
		tracks[i] = r->rows[i].u.track;
	}
	queue_tracks(tracks, r->count, (playlist_queue_where_t)r->page);
	free(tracks);
}

// The two lines, into `items` (room for two). Labels are the Library's keys.
static int queue_items(popover_item_t *items) {
	items[0] = (popover_item_t){"queue_play_next", do_row_queue, (void *)(intptr_t)PLAYLIST_QUEUE_NEXT, false};
	items[1] = (popover_item_t){"queue_add_to_end", do_row_queue, (void *)(intptr_t)PLAYLIST_QUEUE_END, false};
	return 2;
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

// ---------------------------------------------------------------------------
// deleting from the card
//
// Asked first: it removes files. The files, any 1.2.4 sidecar beside them, the
// index entries and the folders left empty go (yandexsaved_delete); the
// library then looks for what changed, as after a save.
// ---------------------------------------------------------------------------

static void library_look_after_delete(void) {
	if (storage_sd_root() && storage_sd_root()[0]) {
		library_card_returned(storage_sd_root());
	}
}

// The lists that come from the index show what is left.
static void after_delete(void) {
	library_look_after_delete();
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
		ok = yandexsaved_delete_album(menu_row.u.album.id) > 0 || yandexsaved_album_count(menu_row.u.album.id) == 0;
	} else {
		ok = yandexsaved_delete(menu_row.u.track.id);
	}
	if (ok) {
		toast_success(tr("yandex_deleted_from_card"));
	} else {
		toast_error(tr("yandex_delete_failed"));
	}
	after_delete();
}

static void do_row_delete(void *user) {
	(void)user;
	const char *name = menu_row.kind == ROW_ALBUM ? menu_row.u.album.title : menu_row.u.track.title;
	confirm_show(menu_row.kind == ROW_ALBUM ? "yandex_delete_album_question" : "yandex_delete_track_question", name, "delete", delete_confirmed, NULL);
}

static void row_menu_cb(lv_event_t *e) {
	lv_event_stop_bubbling(e); // the tap belongs to the menu, not the row
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	const slot_t *slot = lv_event_get_user_data(e);
	if (!slot || slot->index < 0 || !view_row(slot->index, &menu_row)) {
		return;
	}
	popover_item_t items[6];
	memset(items, 0, sizeof(items));
	int n = 0;
	// On the card: the save is done, said with a tick, and the way to undo it
	// is offered instead. Part of an album on the card: both.
	bool saved = row_saved(&menu_row);
	bool some = saved || (menu_row.kind == ROW_ALBUM && yandexsaved_album_count(menu_row.u.album.id) > 0);
	if (menu_row.kind == ROW_TRACK) {
		// Until the account's list has arrived nothing is known to be
		// liked, and "Like" on a liked track is harmless: adding twice
		// changes nothing on the account.
		bool liked = yandex_is_liked(menu_row.u.track.id);
		items[n].label = liked ? "yandex_unlike" : "yandex_like";
		items[n].action = do_row_like;
		items[n].user = liked ? NULL : (void *)1;
		n++;
		// The two "add to" lines next to each other, as on the Library's
		// rows. Not for a track the service cannot give.
		if (menu_row.u.track.available) {
			n += queue_items(&items[n]);
		}
		if (saved) {
			items[n].label = "yandex_saved_on_card";
			items[n].checked = true;
			n++;
		} else if (menu_row.u.track.available) {
			items[n].label = "yandex_save";
			items[n].action = do_row_save;
			n++;
		}
	} else if (menu_row.kind == ROW_ALBUM) {
		bool liked = yandex_album_liked(menu_row.u.album.id);
		items[n].label = liked ? "yandex_unlike" : "yandex_like";
		items[n].action = do_album_like;
		items[n].user = liked ? NULL : (void *)1;
		n++;
		n += queue_items(&items[n]);
		if (saved) {
			items[n].label = "yandex_saved_on_card";
			items[n].checked = true;
		} else {
			items[n].label = "yandex_save_album";
			items[n].action = do_row_save;
		}
		n++;
	} else if (menu_row.kind == ROW_ARTIST) {
		bool liked = yandex_artist_liked(menu_row.u.artist.id);
		items[n].label = liked ? "yandex_unlike" : "yandex_like";
		items[n].action = do_artist_like;
		items[n].user = liked ? NULL : (void *)1;
		n++;
	}
	if (some) {
		items[n].label = "yandex_delete_from_card";
		items[n].action = do_row_delete;
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
	yandex_track_t t;
	if (!yandexplay_current(&t)) {
		return;
	}
	if (like) {
		yandexsync_like(t.id, t.album_id, true, like_done, NULL);
	} else {
		// The dislike carries how long the track was heard; the skip that
		// follows is then not reported on top of it.
		run(JOB_DISLIKE, t.id, t.album_id, yandexwave_disliked(t.id));
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
	int saved, playable;
	album_saved_counts(&saved, &playable);
	if (playable > 0 && saved == playable) {
		// All of it on the card already: the button is Delete.
		memset(&menu_row, 0, sizeof(menu_row));
		menu_row.kind = ROW_ALBUM;
		snprintf(menu_row.u.album.id, sizeof(menu_row.u.album.id), "%s", list_album_id);
		snprintf(menu_row.u.album.title, sizeof(menu_row.u.album.title), "%s", list_album_title);
		do_row_delete(NULL);
		return;
	}
	if (!list_album_id[0] || !yandexdl_album(list_album_id, list_album_title)) {
		save_refused();
	}
}

// A swipe back that starts on the page's bare margin and crosses into the
// list was lost: a screen, having no parent, is the one object LVGL creates
// without PRESS_LOCK, so the press passed to whatever the finger moved onto
// and the screen heard "press lost". Locked, the margin keeps the press.
static void hold_press(lv_obj_t *screen) { lv_obj_set_press_lock(screen, true); }

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
	album_btn_label = lv_label_create(save);
	lv_label_set_text(album_btn_label, tr("yandex_save_album"));
	lv_obj_set_style_text_font(album_btn_label, &font_ui_20, 0);
	lv_obj_center(album_btn_label);
	// Left of the button: a tick and how much of the record is on the card.
	album_state = lv_obj_create(album_bar);
	lv_obj_move_to_index(album_state, 0);
	lv_obj_set_size(album_state, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(album_state, 0, 0);
	lv_obj_set_style_border_width(album_state, 0, 0);
	lv_obj_set_style_pad_all(album_state, 0, 0);
	lv_obj_set_style_pad_column(album_state, 6, 0);
	lv_obj_set_style_margin_right(album_state, 14, 0);
	lv_obj_set_scrollable(album_state, false);
	lv_obj_set_clickable(album_state, false);
	lv_obj_set_flex_flow(album_state, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(album_state, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	album_tick = lv_image_create(album_state);
	lv_image_set_src(album_tick, &icon_check_small);
	lv_obj_add_style(album_tick, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor(album_tick, theme()->accent, 0);
	lv_obj_set_style_image_recolor_opa(album_tick, LV_OPA_COVER, 0);
	theme_register_refresh(marks_recolor);
	album_state_label = lv_label_create(album_state);
	lv_obj_add_style(album_state_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(album_state_label, &font_ui_20, 0);
	lv_obj_set_hidden(album_state, true);
	lv_obj_set_hidden(album_bar, true);

	// An artist's own bar: its Like.
	artist_bar = lv_obj_create(list_screen);
	lv_obj_set_size(artist_bar, cfg->screen_width - 2 * cfg->padding, 60);
	lv_obj_align(artist_bar, LV_ALIGN_TOP_MID, 0, top);
	lv_obj_set_style_bg_opa(artist_bar, 0, 0);
	lv_obj_set_style_border_width(artist_bar, 0, 0);
	lv_obj_set_style_pad_all(artist_bar, 0, 0);
	lv_obj_set_flex_flow(artist_bar, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(artist_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_t *like = lv_btn_create(artist_bar);
	lv_obj_set_size(like, LV_SIZE_CONTENT, 52);
	lv_obj_set_style_pad_hor(like, 22, 0);
	lv_obj_set_style_radius(like, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(like, 0, 0);
	lv_obj_add_event_cb(like, artist_like_cb, LV_EVENT_CLICKED, NULL);
	artist_like_label = lv_label_create(like);
	lv_label_set_text(artist_like_label, tr("yandex_like"));
	lv_obj_set_style_text_font(artist_like_label, &font_ui_20, 0);
	lv_obj_center(artist_like_label);
	lv_obj_set_hidden(artist_bar, true);

	list_box = lv_obj_create(list_screen);
	lv_obj_set_size(list_box, cfg->screen_width - 2 * cfg->padding, cfg->screen_height - top - 70);
	lv_obj_set_pos(list_box, cfg->padding, top + 66);
	lv_obj_set_style_bg_opa(list_box, 0, 0);
	lv_obj_set_style_border_width(list_box, 0, 0);
	lv_obj_set_style_pad_all(list_box, 0, 0);
	lv_obj_set_scroll_dir(list_box, LV_DIR_VER);
	// No layout: the rows are placed by hand, a window over the list (see
	// SLOT_COUNT), and this point at the bottom gives the scroll its height.
	list_end = lv_obj_create(list_box);
	lv_obj_set_size(list_end, 1, 1);
	lv_obj_set_style_bg_opa(list_end, 0, 0);
	lv_obj_set_style_border_width(list_end, 0, 0);
	lv_obj_set_clickable(list_end, false);
	lv_obj_add_event_cb(list_box, list_scrolled_cb, LV_EVENT_SCROLL, NULL);

	list_spin = spinner_create(list_screen, &icon_loader_big);
	lv_obj_center(list_spin);
	lv_obj_set_hidden(list_spin, true);
	switcher_attach_back_gesture(list_screen);
	hold_press(list_screen);
	// The swipe back has to start somewhere: the list and the bars cover
	// most of the page, and a press on them never reached the screen (the
	// rows pass theirs up to the list).
	switcher_attach_back_gesture(list_box);
	switcher_attach_back_gesture(wave_bar);
	switcher_attach_back_gesture(album_bar);
	switcher_attach_back_gesture(artist_bar);
	switcher_set_back_guard(list_screen, list_back_guard);
	switcher_set_back_guard_peek(list_screen, list_back_has_step);
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
		free_result(r);
		return;
	}
	if (r->kind == JOB_DISLIKE) {
		if (r->error[0]) {
			toast_error(r->error);
		}
		free_result(r);
		return;
	}
	if (r->kind == JOB_LIKE_ARTIST || r->kind == JOB_LIKE_ALBUM) {
		like_object_done(r);
		free_result(r);
		return;
	}
	if (r->kind == JOB_QUEUE_ALBUM) {
		queue_album_done(r);
		free_result(r);
		return;
	}
	if (r->kind == JOB_PAGE) {
		// A page of the list on screen came (or failed: its rows keep saying
		// "Loading..." and ask again when scrolled to). The rows showing it
		// get their texts; the next page in view, if any, is asked for.
		bool here = shown && r->list == shown->list;
		page_asked = -1;
		if (r->error[0]) {
			page_failed = r->page;
			page_failed_at = time(NULL);
		}
		if (here && !r->error[0]) {
			for (int i = 0; i < SLOT_COUNT; i++) {
				if (slots[i].row && slots[i].index >= 0 && slots[i].index / YANDEXLIST_PAGE == r->page) {
					bind_slot(&slots[i], slots[i].index);
				}
			}
			bind_view(false);
			update_album_bar();
		}
		free_result(r);
		return;
	}
	// A list the user has since left (or asked for again): another answer is
	// on its way, and this one would only flash past -- or, arriving last,
	// stay up under the wrong title.
	if (r->generation != list_generation) {
		free_result(r);
		return;
	}
	lv_obj_set_hidden(list_spin, true);
	free_result(shown);
	shown = r;
	fill_list();
	if (r->kind == JOB_ARTIST) {
		artist_bar_refresh(); // the liked artists may have been read just now
	}
	// The rows' menus say "Like" or "Remove like" from the account's list:
	// asked for now if it is not here yet (once a session, see yandexsync).
	yandexsync_likes_refresh(NULL);
	// My Wave plays as soon as it has something: that is what the button means.
	if (r->kind == JOB_WAVE && view_count() > 0) {
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
static void open_artists_cb(lv_event_t *e) {
	(void)e;
	open_list(tr("yandex_artists"), JOB_LIKED_ARTISTS, NULL, NULL);
}
static void open_albums_cb(lv_event_t *e) {
	(void)e;
	open_list(tr("yandex_albums"), JOB_LIKED_ALBUMS, NULL, NULL);
}
static void open_feed_cb(lv_event_t *e) {
	(void)e;
	open_list(tr("yandex_made_for_you"), JOB_FEED, NULL, NULL);
}
static void open_chart_cb(lv_event_t *e) {
	(void)e;
	open_list(tr("yandex_chart"), JOB_CHART, NULL, NULL);
}
static void open_new_releases_cb(lv_event_t *e) {
	(void)e;
	open_list(tr("yandex_new_releases"), JOB_NEW_RELEASES, NULL, NULL);
}
static void open_search_cb(lv_event_t *e) {
	(void)e;
	switch_screen(search_screen);
}
// What is saved on the card, from the index: opens with Wi-Fi off, and even
// signed out -- the files are the user's.
static void open_saved_cb(lv_event_t *e) {
	(void)e;
	open_list(tr("yandex_saved_on_card"), JOB_SAVED, NULL, NULL);
}
static lv_obj_t *saved_signed_out;
static void logout_cb(lv_event_t *e) {
	(void)e;
	yandexdl_cancel_all(); // the account's files are not to be fetched any more
	yandex_logout();
	yandexplay_signed_out();
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
	case YANDEXDL_SAVED:
		// The list on screen may show it: its tick, the album's bar.
		refresh_marks();
		update_album_bar();
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
	if (!in) {
		lv_obj_set_hidden(saved_signed_out, yandexsaved_count() == 0);
	}
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
	settingsrow_action(home_signed_in, "yandex_artists", open_artists_cb, NULL);
	settingsrow_action(home_signed_in, "yandex_albums", open_albums_cb, NULL);
	settingsrow_action(home_signed_in, "yandex_playlists", open_playlists_cb, NULL);
	settingsrow_action(home_signed_in, "yandex_made_for_you", open_feed_cb, NULL);
	settingsrow_action(home_signed_in, "yandex_chart", open_chart_cb, NULL);
	settingsrow_action(home_signed_in, "yandex_new_releases", open_new_releases_cb, NULL);
	settingsrow_action(home_signed_in, "yandex_search", open_search_cb, NULL);
	settingsrow_action(home_signed_in, "yandex_saved_on_card", open_saved_cb, NULL);
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
	saved_signed_out = settingsrow_action(home_signed_out, "yandex_saved_on_card", open_saved_cb, NULL);
	// As on the list page: the columns cover the page, the swipe starts there.
	switcher_attach_back_gesture(home_signed_in);
	switcher_attach_back_gesture(home_signed_out);

	lv_obj_add_event_cb(yandex_screen, home_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(yandex_screen);
	hold_press(yandex_screen);
	paint_home();

	build_login_page(cfg);
	build_list_page(cfg);
	build_search_page(cfg);

	yandexplay_init(playback_event);
	lv_timer_create(queue_watch_cb, 1000, NULL);
}

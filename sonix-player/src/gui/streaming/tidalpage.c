#include "tidalpage.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <malloc.h>

#include "src/gui/shell/confirm.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/popover.h"
#include "src/gui/streaming/qobuzart.h"
#include "src/gui/shell/sx_card.h"
#include "src/gui/shell/sx_row.h"
#include "src/gui/shell/sx_sheet.h"
#include "src/gui/shell/sx_styles.h"
#include "src/gui/shell/sx_toast.h"
#include "src/gui/shell/sx_tokens.h"
#include "src/gui/streaming/streamerror.h"
#include "src/gui/streaming/svclist.h"
#include "src/gui/streaming/svcui.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/spinner.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/touch.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/system/decode/growfile.h"
#include "src/system/playback/device_state.h"
#include "src/system/core/lang.h"
#include "src/system/playback/playlist.h"
#include "src/system/device/system.h"
#include "src/system/streaming/tidal.h"
#include "src/system/streaming/tidalcache.h"
#include "src/system/streaming/provider.h"
#include "src/system/streaming/streamerr.h"
#include "src/system/net/http.h"
#include "src/system/streaming/streamturn.h"
#include "src/system/streaming/tidalsync.h"
#include "src/system/core/utils.h"
#include "src/system/net/wifi.h"
#include "src/system/net/wifitransfer.h"

// ---------------------------------------------------------------------------
// Tidal, front end.
//
// Shaped like the Qobuz page. Everything that talks to the network blocks (see
// tidal.h) and cannot run on the GUI thread: one worker, one slot for the job
// to do, and results come back via gui_post() (lv_async_call is not
// thread-safe, see gui.h). One slot rather than a queue -- if a second entry is
// tapped while the first is still loading, the second is the one that matters.
//
// Three things differ from Qobuz: login is a code typed elsewhere rather than a
// password; playlists are identified by UUID rather than by number; and a
// track's address is a manifest that can be a file or a list of segments, so it
// travels in a large struct instead of a string.
// ---------------------------------------------------------------------------

#define TIDAL_PAGE_LIMIT 50
// Cap on held entries: four pages, like the radio stations. Reaching the end
// asks for fifty more; past this cap the list really does stop, because every
// entry is static memory on a device with 64 MB in total.
#define TIDAL_MAX_HELD (4 * TIDAL_PAGE_LIMIT)


static lv_obj_t *login_screen;
lv_obj_t *tidal_list_screen;
#define list_screen tidal_list_screen
static lv_obj_t *search_screen;

// Global rather than behind a function: the Streaming grid wants the address
// of the variable (grid_entry_t.target), as for every other page.
lv_obj_t *tidal_screen;

// ---------------------------------------------------------------------------
// the worker
// ---------------------------------------------------------------------------

typedef enum {
	JOB_NONE = 0,
	JOB_LOGIN_BEGIN,
	JOB_LOGIN_POLL,
	JOB_SEARCH_TRACKS,
	JOB_SEARCH_ALBUMS,
	JOB_SEARCH_ARTISTS,
	JOB_ALBUM_TRACKS,
	JOB_ARTIST_ALBUMS,
	JOB_PLAYLIST_TRACKS,
	JOB_FAV_TRACKS,
	JOB_FAV_ALBUMS,
	JOB_USER_PLAYLISTS,
	JOB_FEATURED,
	JOB_PLAY,
	// Not a list: it leaves the list on screen alone.
	JOB_QUEUE_ALBUM, // an album's tracks for the queue (`index`: where)
} job_kind_t;

typedef struct {
	job_kind_t kind;
	char text[256];	  // search terms, or the title the list will carry
	char text2[64];	  // which showcase, for the featured lists
	// Album and playlist share one field: both identifiers are text (Tidal
	// playlists are named by UUID), and a job can never concern an album and a
	// playlist at the same time.
	char id_text[TIDAL_ID_MAX];
	long id;   // numeric identifier: artists only
	int index; // which track playback starts from; which showcase for the featured lists
	int offset; // which entry this page starts at: 0 = new list
	unsigned list_serial; // list jobs: list_serial when asked
} job_t;

static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_ready = PTHREAD_COND_INITIALIZER;
static job_t pending;
static bool have_pending;
static bool worker_started;

// Job generation: when it changes, a late result belongs to a request nobody
// cares about any more and is discarded.
static unsigned job_serial;

// What came back. Only the worker writes here, and only while the GUI is not
// reading (it reads inside the async, which runs after the worker is done).
static tidal_track_t result_tracks[TIDAL_MAX_HELD];
static tidal_album_t result_albums[TIDAL_MAX_HELD];
static tidal_artist_t result_artists[TIDAL_MAX_HELD];
static tidal_playlist_t result_playlists[TIDAL_MAX_HELD];
static int result_count;
static int result_from; // where the last page received starts (0 = new list)
// The result just delivered has not been read yet.
//
// The result_* buffers are a single slot, written by the worker and read by the
// GUI inside the async, so the worker waits for the result to be taken before
// picking up the next job. Code-based login polls every two seconds, fast
// enough for the next job to overwrite a result the GUI has not read yet.
//
// The wait is bounded: if the gui_post queue is full the message is dropped and
// nothing will ever take the result, and a permanently stuck page would be
// worse than one lost result.
static bool result_pending;
static pthread_cond_t result_taken = PTHREAD_COND_INITIALIZER;

static job_kind_t result_kind;
static char result_error[256];
static char result_title[160];
static stream_err_t result_class; // what kind of failure result_error is

// The showcase that came back empty, as a slot among the three featured
// entries; -1 when it worked.
static int result_featured_gone = -1;

// What the list currently shows, so a tap on a row knows what to do.
static job_kind_t list_kind;

// Raised by each new list asked for (GUI thread): an answer carrying an older
// number belongs to a list the user has since left, and is dropped. The
// worker hands the number of the answer it posts in result_list_serial.
static unsigned list_serial;
static unsigned result_list_serial;
// JOB_QUEUE_ALBUM's answer: the album's tracks (owned by whoever takes them),
// where they go and the album's title.
static tidal_track_t *result_queue_tracks;
static int result_queue_count;
static int result_queue_where;
static char result_queue_what[256];

// Pagination, like the radio: the last page came back full, so another one may
// exist -- and only one request at a time.
static bool page_more;
static bool page_loading;

static void submit(const job_t *job) {
	pthread_mutex_lock(&job_lock);
	pending = *job;
	have_pending = true;
	job_serial++;
	pthread_cond_signal(&job_ready);
	pthread_mutex_unlock(&job_lock);
}

// ---------------------------------------------------------------------------
// waiting
//
// 2.0 has no veils (the handoff: no full-screen dimming). While a list is on
// its way the rows of the one before come off -- they would read the arrays
// being refilled -- and the wait shows: on the list page as the spinner where
// the rows will be, from the main or the search page as the busy toast, which
// a pull puts away (the page stays usable). The list page opens when the list
// has come, as before: that is when the history moves. Neither playback (the
// track starts while it downloads, see tidalcache_start) nor login waits
// here; login has its own spinner on the code page.
// ---------------------------------------------------------------------------

static void wait_begin(void);
static void wait_end(void);

// ---------------------------------------------------------------------------
// the result list
//
// 2.0 rows over the list (svclist.h): a pool of rows rides the scroll, so two
// hundred entries cost the rows of a screen and a half, not two hundred
// widgets of seven objects. A track: its cover (a plaque in the album's tone
// until it comes), the artist, "· 24/96" in the accent for a hi-res one, its
// length, the star; one the subscription does not cover stays in its place,
// faint, saying so. An album: the cover, "artist · year", the chevron. An
// artist or a playlist: its picture, the chevron.
//
// The gestures are the handoff's matrix: a tap plays a track (the page stays,
// the mini player comes up) or opens the rest; a swipe right puts a track or a
// whole album next, left at the end of the queue; the star adds a track to
// the favourites or takes it out at once, with Undo; a hold brings up the
// sheet of a track, an album or a playlist.
// ---------------------------------------------------------------------------

#define ROW_POOL 14

static svclist_t *list;
static lv_obj_t *list_title;
static lv_obj_t *list_empty;
// A list that could not be loaded: what went wrong, and "Retry" (streamerror.h).
static lv_obj_t *list_error;
// The spinner while a list is on its way: where the rows will be.
static lv_obj_t *list_spin;

// The entries the rows show: what the worker last put in the result arrays
// for the list on screen. Its own count, not result_count -- the worker
// writes the next page's tail while the rows are read.
static int shown_count;

static void rebuild_home(void);
static void login_code_arrived(void);
static void login_poll_result(void);
static void login_failed(const char *why);
static void featured_gone(int slot);

// The list history lives at the end of the file, but job_done_cb() below
// decides whether to move it: the arrival of the result is what says whether
// the page actually opened.
static bool job_is_list(job_kind_t kind);
static void list_nav_commit(void);
static void list_nav_drop(void);

// "3:42"
static void duration_text(int seconds, char *out, size_t size) {
	if (seconds <= 0) {
		out[0] = '\0';
		return;
	}
	snprintf(out, size, "%d:%02d", seconds / 60, seconds % 60);
}

static bool kind_is_tracks(job_kind_t kind) {
	return kind == JOB_SEARCH_TRACKS || kind == JOB_ALBUM_TRACKS || kind == JOB_PLAYLIST_TRACKS ||
		   kind == JOB_FAV_TRACKS;
}

static bool kind_is_albums(job_kind_t kind) {
	return kind == JOB_SEARCH_ALBUMS || kind == JOB_FAV_ALBUMS || kind == JOB_ARTIST_ALBUMS || kind == JOB_FEATURED;
}

static void run(const job_t *job, const char *waiting_text);

// ---------------------------------------------------------------------------
// favourites: the star, and the sheet's line
//
// Changed at once and offered back in the toast (the handoff's principle 6):
// nothing asks first any more -- a star taken off by mistake is one tap on
// Undo away. The mirror of favourite tracks flips before Tidal answers
// (tidalsync.h); a refusal is said, and the rows follow the mirror again.
// ---------------------------------------------------------------------------

static void list_refresh_async(void *user) {
	(void)user;
	svclist_rebind(list);
}

static void refused_async(void *user) {
	char *why = user;
	toast_error(why && why[0] ? why : tr("tidal_change_refused"));
	free(why);
	svclist_rebind(list);
}

static void sync_done(bool ok, const char *error, void *user) {
	(void)user;
	if (!ok) {
		gui_post(refused_async, strdup(error ? error : ""));
	}
}

// The star of a track, or of an album from its sheet. Copied: the toast
// outlives the rows it was raised on.
static long star_track_id;
static char star_album_id[40];
static bool star_on;

static void star_apply(bool on) {
	star_on = on;
	if (star_album_id[0]) {
		tidalsync_album_favorite(star_album_id, on, sync_done, NULL);
	} else {
		tidalsync_favorite_toggle(star_track_id, on, sync_done, NULL);
	}
	svclist_rebind(list);
}

static void star_undo(void *user) {
	(void)user;
	star_apply(!star_on);
}

static void star_set(long track_id, const char *album_id, bool on, const char *what) {
	star_track_id = track_id;
	snprintf(star_album_id, sizeof(star_album_id), "%s", album_id ? album_id : "");
	star_apply(on);
	svcui_star_toast(on, "svc_fav_added", "svc_fav_removed", what, star_undo, NULL);
}

// ---------------------------------------------------------------------------
// "Play next" and "Add to end of queue"
//
// A Tidal track goes into the queue by its cache path -- the file when it is
// on the card, the name it will have otherwise -- and becomes known to the
// prefetch, which fetches it when playback nears it as it does for a list
// played from here (tidalpage_prepare_track()). An album's tracks are asked for
// first.
// ---------------------------------------------------------------------------

// The whole queued list: needed by the prefetch thread, and needed by
// on-demand requests -- when playback reaches a track that has not downloaded
// yet, this is where what to ask Tidal for is found. The list played, and
// what was queued from the lists since. Under its lock: the prefetch thread
// reads it.
static tidal_track_t queued_tracks[TIDAL_MAX_HELD];
static int queued_count;
static pthread_mutex_t queued_lock = PTHREAD_MUTEX_INITIALIZER;

// Makes `t` known to the prefetch (the oldest known track gives way when the
// list is full), and writes its tags beside the cache name, so the queue
// shows its title. Its path into `path`.
static void queued_add(const tidal_track_t *t, char *path, size_t size) {
	const char *expected = tidal_expected_mime();
	if (!tidalcache_find(t->id, path, size)) {
		tidalcache_path(t->id, expected, path, size);
	}
	tidalcache_write_sidecars(t->id, expected, t->title, t->artist, t->album, t->album_id, t->track_number, NULL);
	pthread_mutex_lock(&queued_lock);
	bool known = false;
	for (int i = 0; i < queued_count && !known; i++) {
		known = queued_tracks[i].id == t->id;
	}
	if (!known) {
		if (queued_count == TIDAL_MAX_HELD) {
			memmove(queued_tracks, queued_tracks + 1, (TIDAL_MAX_HELD - 1) * sizeof(*queued_tracks));
			queued_count--;
		}
		queued_tracks[queued_count++] = *t;
	}
	pthread_mutex_unlock(&queued_lock);
}

static void queue_tracks(const tidal_track_t *tracks, int count, playlist_queue_where_t where, const char *what) {
	char *block = count > 0 ? malloc((size_t)count * 512) : NULL;
	const char **paths = count > 0 ? malloc((size_t)count * sizeof(*paths)) : NULL;
	if (!block || !paths) {
		free(block);
		free(paths);
		gui_notify_popup("playlist_cannot_add_the_tracks");
		return;
	}
	int n = 0;
	for (int i = 0; i < count; i++) {
		if (!tracks[i].streamable) {
			continue; // not on this subscription: it does not go in the queue
		}
		queued_add(&tracks[i], block + (size_t)n * 512, 512);
		paths[n] = block + (size_t)n * 512;
		n++;
	}
	if (n > 0) {
		svcui_queue(paths, n, where, what);
	} else {
		toast_error(tr("stream_not_in_subscription"));
	}
	free(paths);
	free(block);
}

static void queue_album(const tidal_album_t *a, playlist_queue_where_t where) {
	job_t job = {0};
	job.kind = JOB_QUEUE_ALBUM;
	job.index = (int)where;
	snprintf(job.id_text, sizeof(job.id_text), "%s", a->id_text);
	snprintf(job.text, sizeof(job.text), "%s", a->title);
	run(&job, NULL);
}

static void queue_entry(int index, playlist_queue_where_t where) {
	if (index < 0 || index >= shown_count) {
		return;
	}
	if (kind_is_tracks(list_kind)) {
		queue_tracks(&result_tracks[index], 1, where, result_tracks[index].title);
	} else if (kind_is_albums(list_kind)) {
		queue_album(&result_albums[index], where);
	}
}

// ---------------------------------------------------------------------------
// the sheet
// ---------------------------------------------------------------------------

static int menu_index = -1;
static job_kind_t menu_kind;

static bool menu_valid(void) { return menu_index >= 0 && menu_index < shown_count && menu_kind == list_kind; }

static void sheet_queue(void *user) {
	if (menu_valid()) {
		queue_entry(menu_index, (playlist_queue_where_t)(intptr_t)user);
	}
}

static void sheet_star(void *user) {
	if (!menu_valid()) {
		return;
	}
	bool on = user != NULL;
	if (kind_is_tracks(list_kind)) {
		star_set(result_tracks[menu_index].id, NULL, on, result_tracks[menu_index].title);
	} else {
		star_set(0, result_albums[menu_index].id_text, on, result_albums[menu_index].title);
	}
}

static void sheet_album(void *user) {
	(void)user;
	if (!menu_valid() || !kind_is_tracks(list_kind)) {
		return;
	}
	const tidal_track_t *t = &result_tracks[menu_index];
	job_t job = {0};
	job.kind = JOB_ALBUM_TRACKS;
	snprintf(job.id_text, sizeof(job.id_text), "%s", t->album_id);
	snprintf(job.text, sizeof(job.text), "%s", t->album[0] ? t->album : tr("tidal"));
	run(&job, "loading");
}

// Deleting a playlist on Tidal cannot be undone: asked first, in red. Tidal
// names its playlists by UUID.
static char delete_playlist_id[TIDAL_ID_MAX];

static void reload_list_async(void *user) {
	(void)user;
	if (list_kind == JOB_USER_PLAYLISTS) {
		job_t job = {0};
		job.kind = JOB_USER_PLAYLISTS;
		run(&job, "loading");
	}
}

static void playlist_deleted(bool ok, const char *error, void *user) {
	(void)user;
	if (!ok) {
		gui_post(refused_async, strdup(error ? error : ""));
	} else {
		gui_post(reload_list_async, NULL);
	}
}

static void do_playlist_delete(void *user) {
	(void)user;
	tidalsync_playlist_remove(delete_playlist_id, playlist_deleted, NULL);
}

static void sheet_playlist_delete(void *user) {
	(void)user;
	if (!menu_valid() || list_kind != JOB_USER_PLAYLISTS) {
		return;
	}
	snprintf(delete_playlist_id, sizeof(delete_playlist_id), "%s", result_playlists[menu_index].id);
	char message[TIDAL_TITLE_MAX + 64];
	snprintf(message, sizeof(message), tr("tidal_delete_playlist_confirm_note"), result_playlists[menu_index].name);
	confirm_show("delete_the_playlist", message, "delete", do_playlist_delete, NULL);
}

static void row_detail(int index, char *out, size_t size);

static void list_hold(lv_obj_t *row, void *user) {
	(void)row;
	int index = (int)(intptr_t)user;
	if (index < 0 || index >= shown_count) {
		return;
	}
	menu_index = index;
	menu_kind = list_kind;
	sx_sheet_item_t items[5];
	memset(items, 0, sizeof(items));
	int n = 0;
	static char title[TIDAL_TITLE_MAX];
	if (kind_is_tracks(list_kind) || kind_is_albums(list_kind)) {
		items[n++] = (sx_sheet_item_t){.label = tr("queue_play_next"), .icon = &icon_list_start, .action = sheet_queue,
									   .user = (void *)(intptr_t)PLAYLIST_QUEUE_NEXT};
		items[n++] = (sx_sheet_item_t){.label = tr("queue_add_to_end"), .icon = &icon_list_plus_small,
									   .action = sheet_queue, .user = (void *)(intptr_t)PLAYLIST_QUEUE_END};
	}
	if (kind_is_tracks(list_kind)) {
		const tidal_track_t *t = &result_tracks[index];
		if (!t->streamable) {
			n = 0; // nothing of the queue for a track that will not play
		}
		bool fav = tidalsync_is_favorite(t->id);
		items[n++] = (sx_sheet_item_t){.label = tr(fav ? "remove_from_favourites" : "medialist_add_to_favourites"),
									   .icon = fav ? &icon_star_filled : &icon_star,
									   .action = sheet_star,
									   .user = fav ? NULL : (void *)1};
		if (t->album_id[0] && list_kind != JOB_ALBUM_TRACKS) {
			items[n++] = (sx_sheet_item_t){.label = tr("show_album"), .icon = &icon_chevron_right, .action = sheet_album};
		}
		snprintf(title, sizeof(title), "%s", t->title);
	} else if (kind_is_albums(list_kind)) {
		// Tidal says whether an album is a favourite only in the list of
		// favourites: there it is offered for removal, elsewhere for adding.
		bool fav = list_kind == JOB_FAV_ALBUMS;
		items[n++] = (sx_sheet_item_t){.label = tr(fav ? "remove_from_favourites" : "medialist_add_to_favourites"),
									   .icon = fav ? &icon_star_filled : &icon_star,
									   .action = sheet_star,
									   .user = fav ? NULL : (void *)1};
		snprintf(title, sizeof(title), "%s", result_albums[index].title);
	} else if (list_kind == JOB_USER_PLAYLISTS) {
		items[n++] = (sx_sheet_item_t){
			.label = tr("delete_playlist"), .icon = &icon_trash, .destructive = true, .action = sheet_playlist_delete};
		snprintf(title, sizeof(title), "%s", result_playlists[index].name);
	} else {
		return; // an artist: nothing but opening it
	}
	static char subtitle[400];
	row_detail(index, subtitle, sizeof(subtitle));
	sx_sheet_menu(title, subtitle, items, n);
}

// Side of a row's cover as fetched: the lead's own, so nothing is scaled.
static int thumb_px(void) { return sx_row_height(false) > SX_ROW_H ? SX_ROW_THUMB_BIG : SX_ROW_THUMB; }

// Row cover art.
//
// The loader has twelve slots and a list can hold far more entries than that,
// so slots are a shared pool and not assigned seats: they go to the entries in
// view and move to new ones as the list scrolls. The pictures are kept by
// entry, so a row scrolled away and back finds its cover still there.
static cover_image_t row_images[TIDAL_MAX_HELD];
static bool row_has_image[TIDAL_MAX_HELD];

// This entry already asked for its cover and did not get one: dead URL, image
// that fails to decode, host that does not answer. Without this one broken
// entry would be asked for forever and keep a slot from those that can be
// served.
static bool row_art_failed[TIDAL_MAX_HELD];

// The token that claims the cover slots: only its address matters, never its
// value. See qobuzart.h.
static const char tidal_art_owner;

// Which entry occupies each slot right now, -1 = free.
static int slot_owner[QOBUZART_SLOTS];

// The timer that collects covers as they arrive. It lives exactly as long as
// the requests do: started when a slot is taken (art_wake()) and pausing itself
// when the last one frees up, so it costs nothing when no art is in flight.
static lv_timer_t *art_timer;

static bool art_any_pending(void) {
	for (int i = 0; i < QOBUZART_SLOTS; i++) {
		if (slot_owner[i] >= 0) {
			return true;
		}
	}
	return false;
}

static void art_wake(void) {
	if (art_timer) {
		lv_timer_resume(art_timer);
	}
}

static void art_release_slot(int slot) {
	if (slot < 0 || slot >= QOBUZART_SLOTS) {
		return;
	}
	qobuzart_release(&tidal_art_owner, slot);
	slot_owner[slot] = -1;
}

// The cover URL of entry `index` of the list on screen, "" for none.
static const char *entry_cover(int index) {
	if (index < 0 || index >= shown_count) {
		return "";
	}
	if (kind_is_tracks(list_kind)) {
		return result_tracks[index].cover;
	}
	if (kind_is_albums(list_kind)) {
		return result_albums[index].cover;
	}
	if (list_kind == JOB_SEARCH_ARTISTS) {
		return result_artists[index].image;
	}
	if (list_kind == JOB_USER_PLAYLISTS) {
		return result_playlists[index].image;
	}
	return "";
}

static void art_request_range(int first, int last);
static int art_first = 0, art_last = -1; // the entries in view, last told

// Finished thumbnails are collected here periodically: same shape as the local
// cover loader -- the thread works, the list draws, and whatever is ready shows
// up on the next pass.
static void art_poll_cb(lv_timer_t *timer) {
	bool freed = false;
	if (!art_any_pending()) {
		lv_timer_pause(timer); // nothing in flight: art_wake() restarts it
		return;
	}
	for (int slot = 0; slot < QOBUZART_SLOTS; slot++) {
		int entry = slot_owner[slot];
		if (entry < 0 || entry >= TIDAL_MAX_HELD) {
			continue;
		}
		cover_image_t image;
		bool finished = false;
		if (!qobuzart_take(&tidal_art_owner, slot, &image, &finished)) {
			if (finished) {
				row_art_failed[entry] = true; // failed: never requested again
				freed = true;
				slot_owner[slot] = -1;
			}
			continue;
		}
		cover_free(&row_images[entry]);
		row_images[entry] = image;
		row_has_image[entry] = true;
		svclist_rebind_index(list, entry);
		// Served: the slot goes back to the pool for the next entry.
		slot_owner[slot] = -1;
		freed = true;
	}
	// A slot that just freed up is handed out at once: there are more entries
	// than slots, and without this pass the ones beyond the twelfth would wait
	// for the next scroll.
	if (freed) {
		art_request_range(art_first, art_last);
	}
}

// Requests covers for the entries in view, plus half a screen either side so
// slow scrolling finds them already there.
static void art_request_range(int first, int last) {
	art_first = first;
	art_last = last;
	int margin = (last - first) / 2 + 1;
	int from = first - margin < 0 ? 0 : first - margin;
	int to = last + margin >= shown_count ? shown_count - 1 : last + margin;
	for (int entry = from; entry <= to && entry < TIDAL_MAX_HELD; entry++) {
		const char *url = entry_cover(entry);
		if (!url[0] || row_has_image[entry] || row_art_failed[entry]) {
			continue;
		}
		bool queued = false;
		int free_slot = -1;
		for (int slot = 0; slot < QOBUZART_SLOTS; slot++) {
			queued |= slot_owner[slot] == entry;
			if (slot_owner[slot] < 0 && free_slot < 0) {
				free_slot = slot;
			}
		}
		if (queued) {
			continue;
		}
		if (free_slot < 0) {
			return; // all busy: the rest on the next pass
		}
		slot_owner[free_slot] = entry;
		art_wake();
		qobuzart_request(&tidal_art_owner, free_slot, url, thumb_px());
	}
}

static void maybe_load_more(void);

// A new list is coming: the rows show nothing (they would read the arrays the
// worker is about to fill), and the pictures go.
static void art_forget_rows(void) {
	svclist_set_count(list, 0, false);
	shown_count = 0;
	for (int slot = 0; slot < QOBUZART_SLOTS; slot++) {
		art_release_slot(slot);
	}
	for (int entry = 0; entry < TIDAL_MAX_HELD; entry++) {
		row_has_image[entry] = false;
		row_art_failed[entry] = false;
		cover_free(&row_images[entry]);
	}
	art_first = 0;
	art_last = -1;
}

// Gives the decoded thumbnails back without taking the list apart: the rows
// go back to their plaques. row_art_failed is deliberately NOT cleared: a
// cover that would not download still will not.
static void art_drop_pictures(void) {
	for (int slot = 0; slot < QOBUZART_SLOTS; slot++) {
		art_release_slot(slot);
	}
	for (int entry = 0; entry < TIDAL_MAX_HELD; entry++) {
		row_has_image[entry] = false;
	}
	// The rows let go of the pictures before they are freed.
	svclist_rebind(list);
	for (int entry = 0; entry < TIDAL_MAX_HELD; entry++) {
		cover_free(&row_images[entry]);
	}
}

// Two hundred thumbnails of 56 px are 1.2 MB, held until the list is rebuilt,
// so the pictures are given back a while after the page is left. Not the
// moment it is left: coming straight back is the ordinary way to use this
// page.
#define ART_DROP_DELAY_MS 20000

static lv_timer_t *art_drop_timer;

static void art_drop_cb(lv_timer_t *timer) {
	lv_timer_pause(timer);
	art_drop_pictures();
	// And back to the kernel, not merely back to the arena: a thumbnail is
	// under the mmap threshold main.c sets, so freeing them alone leaves the
	// arena holding the space.
	malloc_trim(0);
}

static void list_loaded_cb(lv_event_t *e) {
	(void)e;
	if (art_drop_timer) {
		lv_timer_pause(art_drop_timer); // back before the wait ran out
	}
	if (list) {
		// Whatever was given back while the page was away is asked for
		// again; free when nothing was dropped.
		int first, last;
		svclist_visible(list, &first, &last);
		art_request_range(first, last);
	}
}

static void list_unloaded_cb(lv_event_t *e) {
	(void)e;
	if (art_drop_timer) {
		lv_timer_reset(art_drop_timer);
		lv_timer_resume(art_drop_timer);
	}
}

// The line under an entry's title.
static void row_detail(int index, char *out, size_t size) {
	out[0] = '\0';
	if (kind_is_tracks(list_kind)) {
		const tidal_track_t *t = &result_tracks[index];
		if (!t->streamable) {
			snprintf(out, size, "%s \xC2\xB7 %s", t->artist, tr("not_included"));
		} else {
			snprintf(out, size, "%s", t->artist);
		}
	} else if (kind_is_albums(list_kind)) {
		const tidal_album_t *a = &result_albums[index];
		snprintf(out, size, "%s%s%s", a->artist, a->released[0] ? " \xC2\xB7 " : "", a->released);
	} else if (list_kind == JOB_SEARCH_ARTISTS) {
		const tidal_artist_t *a = &result_artists[index];
		if (a->album_count > 0) {
			snprintf(out, size, tr(a->album_count == 1 ? "subsonic_one_album" : "subsonic_albums_n"), a->album_count);
		}
	} else if (list_kind == JOB_USER_PLAYLISTS) {
		const tidal_playlist_t *p = &result_playlists[index];
		snprintf(out, size, "%s \xC2\xB7 %d", p->owner, p->track_count);
	}
}

// The track playing now, as a Tidal id, for the playing row.
static long playing_id;

static bool playing_update(void) {
	char path[512];
	long id = 0;
	if (playlist_current_path(path, sizeof(path)) && tidalcache_owns(path)) {
		id = tidalcache_track_id(path);
	}
	if (id == playing_id) {
		return false;
	}
	playing_id = id;
	return true;
}

// "· 24/96", "· 24/44.1": a hi-res track's resolution, the handoff's accent
// badge.
static void hires_badge(const tidal_track_t *t, char *out, size_t size) {
	out[0] = '\0';
	if (!t->hires || t->sample_rate <= 0 || t->bit_depth <= 0) {
		return;
	}
	int khz = t->sample_rate / 1000, tenth = (t->sample_rate % 1000) / 100;
	if (tenth) {
		snprintf(out, size, "\xC2\xB7 %d/%d.%d", t->bit_depth, khz, tenth);
	} else {
		snprintf(out, size, "\xC2\xB7 %d/%d", t->bit_depth, khz);
	}
}

static void list_bind(lv_obj_t *row, int index) {
	if (index < 0 || index >= shown_count) {
		return;
	}
	char detail[400];
	row_detail(index, detail, sizeof(detail));
	const lv_image_dsc_t *image = index < TIDAL_MAX_HELD && row_has_image[index] ? &row_images[index].dsc : NULL;
	sx_row_set_playing(row, false);
	lv_obj_set_style_opa(row, LV_OPA_COVER, 0);
	if (kind_is_tracks(list_kind)) {
		const tidal_track_t *t = &result_tracks[index];
		char length[16], badge[24];
		duration_text(t->duration, length, sizeof(length));
		hires_badge(t, badge, sizeof(badge));
		sx_row_set_title(row, t->title);
		sx_row_set_subtitle(row, detail, badge[0] ? badge : NULL, length);
		if (list_kind == JOB_ALBUM_TRACKS && t->track_number > 0) {
			sx_row_set_number(row, t->track_number);
		} else {
			sx_row_set_thumb(row, image, svcui_tone(t->album_id[0] ? t->album_id : t->title));
		}
		sx_row_set_trail(row, SX_ROW_TRAIL_STAR);
		sx_row_set_starred(row, tidalsync_is_favorite(t->id));
		sx_row_set_swipes(row, t->streamable, t->streamable);
		sx_row_set_playing(row, playing_id != 0 && playing_id == t->id);
		// A track the subscription does not cover stays in the list, faint:
		// hiding it would leave a gap in the album with no explanation.
		if (!t->streamable) {
			lv_obj_set_style_opa(row, LV_OPA_50, 0);
		}
	} else if (kind_is_albums(list_kind)) {
		const tidal_album_t *a = &result_albums[index];
		sx_row_set_title(row, a->title);
		sx_row_set_subtitle(row, detail, NULL, NULL);
		sx_row_set_thumb(row, image, svcui_tone(a->id_text));
		sx_row_set_trail(row, SX_ROW_TRAIL_CHEVRON);
		sx_row_set_swipes(row, true, true);
	} else if (list_kind == JOB_SEARCH_ARTISTS) {
		const tidal_artist_t *a = &result_artists[index];
		sx_row_set_title(row, a->name);
		sx_row_set_subtitle(row, detail, NULL, NULL);
		sx_row_set_thumb(row, image, svcui_tone(a->name));
		sx_row_set_trail(row, SX_ROW_TRAIL_CHEVRON);
		sx_row_set_swipes(row, false, false);
	} else if (list_kind == JOB_USER_PLAYLISTS) {
		const tidal_playlist_t *p = &result_playlists[index];
		sx_row_set_title(row, p->name);
		sx_row_set_subtitle(row, detail, NULL, NULL);
		sx_row_set_thumb(row, image, svcui_tone(p->id));
		sx_row_set_trail(row, SX_ROW_TRAIL_CHEVRON);
		sx_row_set_swipes(row, false, false);
	}
}

static void list_visible_cb(int first, int last) { art_request_range(first, last); }

static void favourites_known(void) { gui_post(list_refresh_async, NULL); }

// `from` is the first entry new: 0 a new list, a higher value a page added at
// the bottom -- the rows already there and the scroll stay.
static void fill_list(job_kind_t kind, int from, int count, const char *title) {
	if (from == 0) {
		art_forget_rows();
	}
	list_kind = kind;
	shown_count = count;
	lv_obj_set_hidden(list_spin, true);
	playing_update();
	svclist_set_count(list, shown_count, from > 0);
	lv_obj_set_hidden(list_empty, shown_count != 0);
	lv_label_set_text(list_title, title);
	// The stars read the mirror of favourite tracks: asked for when not known
	// yet, the rows told when it comes.
	if (kind_is_tracks(list_kind)) {
		tidalsync_favorites_refresh(favourites_known);
	}
}

// The toast is ours to end only if we raised it.
static bool wait_toast;

static void wait_begin(void) {
	art_forget_rows();
	lv_obj_set_hidden(list_empty, true);
	if (lv_screen_active() == list_screen) {
		lv_obj_set_hidden(list_spin, false);
	} else {
		sx_toast_busy(tr("loading"), true);
		wait_toast = true;
	}
}

static void wait_end(void) {
	lv_obj_set_hidden(list_spin, true);
	if (wait_toast) {
		wait_toast = false;
		sx_toast_busy_end();
	}
}

// ---------------------------------------------------------------------------
// what comes back from the worker, on the GUI thread
// ---------------------------------------------------------------------------

static void job_result_taken(void) {
	pthread_mutex_lock(&job_lock);
	result_pending = false;
	pthread_cond_broadcast(&result_taken);
	pthread_mutex_unlock(&job_lock);
}

static void retry_list(void);
static void error_sign_in(void);

static void job_done_cb(void *user) {
	(void)user;
	// Before reading any result_*: from here on the worker is held and nothing
	// writes underneath. It is released on the way out, in job_result_taken().
	if (result_kind == JOB_QUEUE_ALBUM) {
		if (result_error[0] || result_queue_count <= 0) {
			toast_error(result_error[0] ? result_error : tr("playlist_cannot_add_the_tracks"));
		} else {
			queue_tracks(result_queue_tracks, result_queue_count, (playlist_queue_where_t)result_queue_where,
						 result_queue_what);
		}
		free(result_queue_tracks);
		result_queue_tracks = NULL;
		job_result_taken();
		return;
	}
	// A list the user has since left (or asked for again): another answer is
	// on its way, and this one would only flash past. Its navigation is the
	// newer job's, and stays.
	if (job_is_list(result_kind) && result_list_serial != list_serial) {
		job_result_taken();
		return;
	}
	if (job_is_list(result_kind)) {
		wait_end();
	}

	if (result_error[0]) {
		// Login has its own page to report on. A transient popup would leave
		// the spinner turning on a code that will never arrive, with no way to
		// retry.
		if (result_kind == JOB_LOGIN_BEGIN || result_kind == JOB_LOGIN_POLL) {
			login_failed(result_error);
			job_result_taken();
			return;
		}
		page_loading = false;
		if (job_is_list(result_kind) && result_from == 0) {
			// Said where the list would have been, with a way to ask again,
			// rather than in a popup over the previous list: the failed list
			// is a page now (its title, no rows, the error), so it is a step
			// in the history like any other and back leads where it would
			// have. The worker left no rows for it (result_count 0).
			list_nav_commit();
			fill_list(result_kind, 0, 0, result_title);
			lv_obj_set_hidden(list_empty, true);
			page_more = false;
			streamerror_show(list_error, result_class, result_error, retry_list, error_sign_in);
			if (lv_screen_active() != list_screen) {
				switch_screen(list_screen);
			}
			job_result_taken();
			return;
		}
		// A further page: the rows already there stay (the worker put their
		// count back), the history does not move, and scrolling does not ask
		// again by itself.
		if (job_is_list(result_kind)) {
			list_nav_drop();
			page_more = false;
			toast_error(result_error);
			job_result_taken();
			return;
		}
		gui_notify_popup(result_error);
		job_result_taken();
		return;
	}
	if (job_is_list(result_kind) && result_from == 0 && result_featured_gone < 0) {
		streamerror_hide(list_error);
	}

	switch (result_kind) {
	case JOB_LOGIN_BEGIN:
		login_code_arrived();
		break;

	case JOB_LOGIN_POLL:
		login_poll_result();
		break;

	case JOB_PLAY:
		// Nothing: the music started long ago (play_started_async). This runs
		// much later -- after the next track has also been prefetched, which is
		// minutes.
		break;

	case JOB_FEATURED:
		// A showcase that does not answer disappears from the main page rather
		// than opening an empty list: see the comment on
		// tidal_featured_albums.
		if (result_featured_gone >= 0) {
			list_nav_drop(); // no list opens: the history does not move
			featured_gone(result_featured_gone);
			job_result_taken();
			return;
		}
		fill_list(result_kind, result_from, result_count, result_title);
		page_loading = false;
		page_more = (result_count - result_from == TIDAL_PAGE_LIMIT) && result_count < TIDAL_MAX_HELD;
		// Before switch_screen(): to tell whether the page is being entered
		// from outside, the history looks at which screen is active, and must
		// see it as it was before the change.
		list_nav_commit();
		if (lv_screen_active() != list_screen && result_from == 0) {
			switch_screen(list_screen);
		}
		break;

	default:
		fill_list(result_kind, result_from, result_count, result_title);
		// A full last page probably means another one follows; a short one
		// means the list really ended and nothing more is requested.
		page_loading = false;
		page_more = (result_count - result_from == TIDAL_PAGE_LIMIT) && result_count < TIDAL_MAX_HELD;
		list_nav_commit();
		if (lv_screen_active() != list_screen && result_from == 0) {
			switch_screen(list_screen);
		}
		break;
	}
	job_result_taken();
}

// ---------------------------------------------------------------------------
// playback
//
// The tapped track downloads and starts on its own. The ones after it in the
// same list download while it plays and queue up as they arrive, so an album
// plays straight through without waiting for the whole thing before the first
// note.
// ---------------------------------------------------------------------------

// How many tracks to prefetch. Two and not four: a hi-res track runs to about
// a hundred and ninety megabytes, and four ahead fills the cache (and the
// card) for a queue nobody may listen to.
#define PREFETCH_AHEAD 2

// The network is slower than the track and a chunk has to arrive before
// playback starts; say so instead of leaving the screen frozen. A transient
// popup, not a blocking veil.
static void slow_start_async(void *user) {
	int seconds = (int)(intptr_t)user;
	char message[128];
	snprintf(message, sizeof(message), tr("stream_slow_connection"), seconds);
	gui_notify_popup(message);
}

static void on_slow_start(int seconds) { gui_post(slow_start_async, (void *)(intptr_t)seconds); }

// Playback stopped on a track that was not there yet: when the file arrives,
// it resumes from where it stopped.
static bool waiting_for_track;

static bool track_is_playable(const char *path);
static void prefetch_urgent(const char *wanted_path);

static void resume_if_waiting_async(void *user) {
	(void)user;
	if (!waiting_for_track) {
		return;
	}
	// The Wi-Fi transfer is a file server running as root on the card this
	// track was just written to. Nothing starts playing under it.
	if (wifitransfer_get_enabled()) {
		return;
	}
	int index = playlist_current_index();
	char path[512];
	if (index < 0 || !playlist_path_at(index, path, sizeof(path))) {
		return;
	}
	if (tidalcache_owns(path) && !track_is_playable(path)) {
		// A different track arrived, not this one. Logged: when the resume
		// does not fire, this is the place to look.
		fprintf(stderr, "tidal: something arrived but the queue is waiting for '%s', which is not ready\n", path);
		return;
	}
	waiting_for_track = false;
	fprintf(stderr, "tidal: restarting the queue from slot %d ('%s')\n", index, path);
	device_state_play_queue_index(index);
	player_refresh_now_playing();
}

static bool track_is_playable(const char *path) {
	long id = tidalcache_track_id(path);
	// Fully downloaded, under this name or under another container -- which
	// happens more often than on Qobuz: which container arrives is decided by
	// the manifest, only seen at play time.
	if (tidalcache_find(id, NULL, 0)) {
		return true;
	}
	// Or it is downloading right now: the decoder can read a growing file, and
	// that is how the first note plays without waiting for the end.
	//
	// Two questions, not one: growfile compares paths, the cache compares the
	// track id. Both are asked, and a disagreement is logged.
	bool growing = growfile_is_growing(path);
	bool downloading = id != 0 && tidalcache_downloading_id() == id;
	if (growing != downloading) {
		fprintf(stderr, "tidal: '%s' -- growfile says %d, the cache says %d: THEY DISAGREE\n", path, growing,
				downloading);
	}
	return growing || downloading;
}

// The "downloading" notice must go through gui_post.
//
// device_state calls tidalpage_prepare_track when starting a file, and it
// calls it from whatever thread it happens to be on: the UI thread when a
// finger picked the track, but also this page's worker when do_play() started
// the queue. LVGL is not reentrant and has no locks (LV_USE_OS is off), so
// drawing a popup from there is the class of fault that shows up as an
// occasional crash with no pattern.
static void downloading_note_async(void *user) {
	(void)user;
	gui_notify_popup("downloading");
}

bool tidalpage_prepare_track(const char *path) {
	if (!tidalcache_owns(path)) {
		waiting_for_track = false;
		return true; // not a Tidal file
	}
	// If another page (Qobuz) says "not yet" first, this function is not called
	// at all -- device_state stops at the first refusal. So the flag is also
	// cleared in queue_watch_cb, which runs every second regardless; a flag
	// raised here and never lowered keeps the Wi-Fi awake forever.
	if (track_is_playable(path)) {
		waiting_for_track = false;
		return true;
	}
	// Not there. Log the reason in full: when this decision is wrong the
	// player loops, and a silent log gives no way to see it.
	fprintf(stderr, "tidal: '%s' not ready (id=%ld, downloading=%ld): stopping and requesting it\n", path,
			tidalcache_track_id(path), tidalcache_downloading_id());

	// The network is needed right now: tell the Wi-Fi idle logic before even
	// stopping playback.
	tidalcache_set_network_wanted(true);
	waiting_for_track = true;
	// Stop what was playing first, and not out of politeness: the previous
	// track is reading a file that is still downloading, and it is one of the
	// downloads prefetch_urgent() is about to abandon. Pulling the file out
	// from under the audio thread makes it report "track finished", the queue
	// advances by itself onto another missing track, and the loop starts over.
	// Stopped, there is nothing to pull out from under anyone.
	device_state_stop();
	prefetch_urgent(path);
	gui_post(downloading_note_async, NULL);
	return false;
}

// A track's cover arrives with the track, so it can land an instant after the
// player has already checked for one. If it belongs to the playing track the
// player has to be told, or the grey note stays until the next track.
static void cover_ready_async(void *user) {
	char *path = user;
	device_state_t state;
	device_state_get(&state);
	if (path && strcmp(path, state.current_file) == 0) {
		player_refresh_now_playing();
	}
	free(path);
}

// 2.0: the page stays where it is, the mini player comes up with the track
// (the player is a tap on it away), and the row playing is marked.
static void play_started_async(void *user) {
	(void)user;
	player_refresh_now_playing();
	if (playing_update()) {
		svclist_rebind(list);
	}
}


// ---------------------------------------------------------------------------
// prefetch, on its own thread
//
// Not on the worker that serves taps: prefetching waits its turn among the
// downloads (tidalcache_wait_idle), which on a real network is minutes, and
// for all that time tapping another entry would do nothing. The worker always
// responds, and this thread checks before every step whether the track it is
// fetching is still wanted.
// ---------------------------------------------------------------------------

static pthread_mutex_t prefetch_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t prefetch_ready = PTHREAD_COND_INITIALIZER;

// Which queued track to download now, as a Tidal track id; 0 = nothing to do.
// Written by the queue watchdog on the GUI thread, the only one that knows
// where playback is and which files already exist.
//
// An id, not a position: with shuffle on the play queue has its own order, and
// position N of the queue is not position N of the list it came from. An id
// always names the same track in any order.
static long prefetch_want_id;
static bool prefetch_started;

// The last track that failed to download and when it was tried: wait a moment
// before going at the same id again.
static long prefetch_failed_id;
static uint32_t prefetch_failed_ms;
#define PREFETCH_RETRY_MS 4000

static void *prefetch_main(void *arg);

// Whether a download is still worth doing is decided by looking the track up in
// queued_tracks, not by comparing job serials: the serial is bumped by every job
// on this page, browsing included, so it says nothing about what is playing.

static void prefetch_submit(int index) {
	(void)index;
	pthread_mutex_lock(&prefetch_lock);

	if (!prefetch_started) {
		pthread_t thread;
		if (pthread_create(&thread, NULL, prefetch_main, NULL) == 0) {
			pthread_detach(thread);
			prefetch_started = true;
		}
	}
	pthread_cond_broadcast(&prefetch_ready);
	pthread_mutex_unlock(&prefetch_lock);
}

// The tracks the prefetch knows: queued_tracks, with the lists' code above.

// The track needed right now -- prev was pressed, or the queue reached one
// that was never prefetched.
//
// Two things, both of which remove seconds of waiting after a key press: the
// thread is asked immediately instead of waiting for the watchdog tick, and
// the running download is abandoned -- only one downloads at a time, and
// letting it finish means waiting a whole track before this one starts.
//
// Two things not done: never abandon while music is still playing (the caller
// stops first, see tidalpage_prepare_track), and never abandon when the
// download in progress is already the wanted track.
static void prefetch_urgent(const char *wanted_path) {
	long wanted = tidalcache_track_id(wanted_path ? wanted_path : "");
	if (wanted == 0) {
		return; // not a cache track: nothing to download
	}

	// The name stem, without extension: the real container is decided by the
	// manifest, so the file in progress for this track may be named otherwise.
	char stem[512];
	snprintf(stem, sizeof(stem), "%s", wanted_path ? wanted_path : "");
	char *dot = strrchr(stem, '.');
	if (dot) {
		*(dot + 1) = '\0'; // "<dir>/12345678."
	}
	// "Is this exact track already downloading?" asked both ways: by path to
	// growfile and by id to the cache. Abandoning the download of the very
	// track being waited for is the most expensive mistake here -- it restarts
	// from zero every time and never finishes.
	bool already_ours = (stem[0] && growfile_prefix_is_growing(stem)) || tidalcache_downloading_id() == wanted;
	if (!already_ours) {
		streamturn_abandon_all();
	} else {
		fprintf(stderr, "tidal: %ld is already downloading, not dropping it\n", wanted);
	}

	tidalcache_set_network_wanted(true);

	pthread_mutex_lock(&prefetch_lock);
	prefetch_want_id = wanted;
	pthread_cond_broadcast(&prefetch_ready);
	pthread_mutex_unlock(&prefetch_lock);
}

// Prefetch of one list track while another plays: manifest, download, and the
// sidecar files (tags and cover), so everything is ready by the time its turn
// comes.
static bool prepare_track(const tidal_track_t *track, char *path, size_t size) {
	// On the heap, not the stack: the struct carries the signed segment URLs
	// and runs to just under three kilobytes, which is a lot on a service
	// thread's stack (tidal.h says so, and it is that function's only usage
	// rule).
	tidal_stream_t *stream = calloc(1, sizeof(*stream));
	if (!stream) {
		return false;
	}
	if (!tidal_track_stream(track->id, stream)) {
		free(stream);
		return false;
	}

	// Between manifest and download the request may already have changed:
	// fetching the manifest costs hundreds of milliseconds of real network,
	// and inside that window a next abandons everything and asks for another
	// track. A download that starts after that abandon can no longer be
	// stopped: it holds the single slot for a whole file, and the track the
	// user is looking at waits its turn.
	pthread_mutex_lock(&prefetch_lock);
	bool superseded = prefetch_want_id != 0 && prefetch_want_id != track->id;
	pthread_mutex_unlock(&prefetch_lock);
	if (superseded) {
		fprintf(stderr, "tidal: %ld no longer needed, not even starting it\n", track->id);
		free(stream);
		return false;
	}

	if (!tidalcache_start(track->id, stream, track->duration, path, size)) {
		free(stream);
		return false;
	}

	// The real name against the one the queue guessed. If they differ, the
	// whole queue points at a file that does not exist -- exactly the kind of
	// thing only this line catches.
	char guessed[512];
	tidalcache_path(track->id, tidal_expected_mime(), guessed, sizeof(guessed));
	if (strcmp(guessed, path) != 0) {
		fprintf(stderr, "tidal: WARNING real name '%s' but the queue expects '%s'\n", path, guessed);
	}

	// The real MIME type from the manifest, not the guessed one: a FLAC
	// stitched back together from segments carries no tags of its own, and
	// these sidecars are all the player will have to read.
	tidalcache_write_sidecars(track->id, stream->mime, track->title, track->artist, track->album, track->album_id,
							  track->track_number, track->cover);
	free(stream);

	// Prefetched tracks count too: they occupy the card, which is what the
	// periodic cleanup has to keep in check.
	tidalcache_note_played(path);

	char *owned = strdup(path);
	if (owned) {
		gui_post(cover_ready_async, owned);
	}
	return true;
}

static void do_play(int index) {
	if (index < 0 || index >= result_count) {
		return;
	}

	const tidal_track_t *track = &result_tracks[index];
	if (!track->streamable) {
		snprintf(result_error, sizeof(result_error), "%s", tr("stream_not_in_subscription"));
		return;
	}

	// Whatever was downloading for another track is no longer needed: the
	// bandwidth belongs to this one.
	streamturn_abandon_all();

	tidal_stream_t *stream = calloc(1, sizeof(*stream));
	if (!stream) {
		snprintf(result_error, sizeof(result_error), "%s", tr("download_failed"));
		return;
	}

	if (!tidal_track_stream(track->id, stream)) {
		const char *why = tidal_last_error();
		result_class = streamerr_describe("tidal", why && why[0] ? why : tr("download_failed"), result_error,
										  sizeof(result_error));
		free(stream);
		return;
	}

	char path[512];
	if (!tidalcache_start(track->id, stream, track->duration, path, sizeof(path))) {
		// The cache says what went wrong: no card, no network, card full. The
		// generic "download failed" is only for when even it does not know.
		const char *why = tidalcache_last_error();
		result_class = streamerr_describe("tidal", why && why[0] ? why : tr("download_failed"), result_error,
										  sizeof(result_error));
		free(stream);
		return;
	}

	// Only the MIME type is still needed from the manifest, and everything
	// after this takes a while: holding three kilobytes through the queue
	// build would be three kilobytes wasted.
	char mime[sizeof(stream->mime)];
	snprintf(mime, sizeof(mime), "%s", stream->mime);
	free(stream);

	// One more track in the cache; it prunes itself from time to time.
	tidalcache_note_played(path);

	// Tags before starting playback: that is when device_state reads the
	// metadata, and a moment later would be too late.
	tidalcache_write_sidecars(track->id, mime, track->title, track->artist, track->album, track->album_id,
							  track->track_number, NULL);

	// --- the queue: the whole list, not only what has downloaded ------------
	//
	// Every track's path is known before asking for the manifest: the file
	// name is the id plus the extension, and the extension is guessed from the
	// chosen quality (tidal_expected_mime). So the queue is built in full up
	// front and the files land underneath -- the thread prefetches the ones
	// ahead, and if playback reaches a missing one first, device_state asks for
	// it and it downloads then.
	//
	// The guess is less safe than on Qobuz, because the manifest decides the
	// container: if the track already exists under another name, the real one
	// is used, which tidalcache_find locates by trying the extensions.
	static char paths[TIDAL_MAX_HELD][512];
	static const char *path_ptr[TIDAL_MAX_HELD];
	const char *expected = tidal_expected_mime();

	pthread_mutex_lock(&queued_lock);
	queued_count = 0;
	int start = 0;
	for (int i = 0; i < result_count && queued_count < TIDAL_MAX_HELD; i++) {
		if (!result_tracks[i].streamable) {
			continue; // unplayable: keep it out of the queue
		}
		if (i == index) {
			start = queued_count;
			// The tapped one has already downloaded: queue its real name.
			snprintf(paths[queued_count], sizeof(paths[0]), "%s", path);
		} else if (!tidalcache_find(result_tracks[i].id, paths[queued_count], sizeof(paths[0]))) {
			tidalcache_path(result_tracks[i].id, expected, paths[queued_count], sizeof(paths[0]));
		}
		path_ptr[queued_count] = paths[queued_count];
		queued_tracks[queued_count] = result_tracks[i];
		queued_count++;
	}
	pthread_mutex_unlock(&queued_lock);

	if (queued_count == 0) {
		return;
	}

	// Tags for the whole queue, right away. They are text files of a few dozen
	// bytes and cost nothing (no covers here: those are downloads and arrive
	// with the track). Without them the Queue page would show the track id
	// instead of the title for everything not yet downloaded.
	for (int i = 0; i < queued_count; i++) {
		if (i == start) {
			continue; // already written, and with the real MIME type
		}
		const tidal_track_t *t = &queued_tracks[i];
		tidalcache_write_sidecars(t->id, expected, t->title, t->artist, t->album, t->album_id, t->track_number, NULL);
	}

	device_state_play_list(path_ptr, queued_count, start);

	// The cover now, with the music already playing: it is a download and not
	// worth delaying the first note for. It goes before the player opens on
	// purpose, so the image is already there when the page appears.
	tidalcache_write_sidecars(track->id, mime, track->title, track->artist, track->album, track->album_id,
							  track->track_number, track->cover);

	gui_post(play_started_async, NULL);

	// The following tracks are prefetched on their own thread.
	prefetch_submit(start);
}

// ---------------------------------------------------------------------------

// The queue watchdog, once a second on the GUI thread.
//
// It does two things, both of which need to know where playback is -- which
// cannot be asked from another thread, because the queue belongs to the GUI.
//
//   1. tells the cache which files it must not discard (the ones still to
//      play): a fully downloaded track is no longer protected by growfile, and
//      deleting it while queued stops playback halfway through an album;
//   2. tells the prefetch thread which track to download now: the first
//      missing one between the playing track and the two after it.
static void queue_watch_cb(lv_timer_t *timer) {
	(void)timer;
	// The playing row follows the track, which moves on by itself; only
	// while the list is on screen.
	if (list && shown_count > 0 && lv_screen_active() == list_screen && playing_update()) {
		svclist_rebind(list);
	}
	// Never signed in and nothing of ours ever queued: nothing to watch, and
	// this tick costs one comparison (as on the Subsonic page).
	if (queued_count == 0 && !waiting_for_track && !tidal_logged_in()) {
		return;
	}
	// A queue that is a library list holds no cache files, and walking it here
	// would be a database read per entry, once a second, for nothing. The
	// bookkeeping below still has to run: something was very likely being
	// waited for when the user started that list, and the flag keeping Wi-Fi
	// awake is only ever lowered here.
	bool library_queue = playlist_is_library_backed();
	if (library_queue) {
		tidalcache_set_protected(NULL, 0);
		waiting_for_track = false;
	}

	// Nothing further is asked of the network while the Wi-Fi transfer is on:
	// the track would land on a card a server outside this process is writing,
	// and the wait it feeds is a wait to start playing, which is what
	// switching the transfer on put a stop to. The queued files stay protected
	// below either way -- they are still the queue.
	bool transfer_on = wifitransfer_get_enabled();
	if (transfer_on) {
		waiting_for_track = false;
	}

	int count = playlist_count();
	int current = playlist_current_index();

	static char queued[TIDALCACHE_PROTECTED_MAX][512];
	static const char *queued_ptr[TIDALCACHE_PROTECTED_MAX];
	int protect = 0;
	for (int i = current; !library_queue && i >= 0 && i < count && protect < TIDALCACHE_PROTECTED_MAX; i++) {
		if (playlist_path_at(i, queued[protect], sizeof(queued[0]))) {
			queued_ptr[protect] = queued[protect];
			protect++;
		}
	}
	if (!library_queue) {
		tidalcache_set_protected(queued_ptr, protect);
	}

	// The first missing one in the window, `current` included: if that is the
	// missing one, it is what playback is waiting for.
	//
	// The id is read from the path the queue gives for that position -- the
	// queue is what knows its own order, shuffle included. `queued_tracks[i]`
	// for the same `i` can be an entirely different track.
	long want_id = 0;
	if (!library_queue && !transfer_on && current >= 0 && queued_count > 0) {
		for (int i = current; i <= current + PREFETCH_AHEAD && i < count && want_id == 0; i++) {
			char path[512];
			if (!playlist_path_at(i, path, sizeof(path)) || !tidalcache_owns(path)) {
				continue;
			}
			long id = tidalcache_track_id(path);
			if (id != 0 && !tidalcache_find(id, NULL, 0)) {
				want_id = id;
			}
		}
	}

	pthread_mutex_lock(&prefetch_lock);
	if (want_id != prefetch_want_id) {
		prefetch_want_id = want_id;
		pthread_cond_broadcast(&prefetch_ready);
	}
	pthread_mutex_unlock(&prefetch_lock);

	// The flag that keeps the Wi-Fi awake is recomputed here, the only place
	// that reconsiders it in full. Whoever raises it (prefetch_urgent,
	// tidalpage_prepare_track) does so immediately to avoid a gap; lowering it
	// is this pass's job.
	//
	// The waiting flag only holds while the track the queue waits for is a
	// Tidal one. Once the queue moves to another file -- a local track, or a
	// Qobuz one -- "waiting" describes nothing: left raised it keeps the radio
	// awake forever and, when a Tidal download finishes, restarts a track
	// nobody is listening to any more.
	//
	// The queue is what is asked, not device_state_get().current_file:
	// audio_stop() leaves the previous track's path in current_file, so right
	// after device_state_stop() -- exactly when the wait begins -- current_file
	// still names the earlier file. The queue already points at the new track.
	if (waiting_for_track) {
		int index = playlist_current_index();
		char wanted[512];
		if (index >= 0 && playlist_path_at(index, wanted, sizeof(wanted)) && !tidalcache_owns(wanted)) {
			waiting_for_track = false;
		}
	}

	tidalcache_set_network_wanted(want_id != 0 || waiting_for_track);
}

static void *prefetch_main(void *arg) {
	(void)arg;
	thread_be_background("tidal prefetch");

	for (;;) {
		pthread_mutex_lock(&prefetch_lock);
		while (prefetch_want_id == 0) {
			pthread_cond_wait(&prefetch_ready, &prefetch_lock);
		}
		long id = prefetch_want_id;
		pthread_mutex_unlock(&prefetch_lock);

		// A track that just failed is not retried at once: the watchdog would
		// requeue it every second, hammering Tidal sixty times a minute for the
		// same answer.
		if (id == prefetch_failed_id && lv_tick_get() - prefetch_failed_ms < PREFETCH_RETRY_MS) {
			usleep(300 * 1000);
			continue;
		}

		// From id to track: the data (title, duration, cover) is looked up by
		// searching the queued list, never by trusting a position.
		int index = -1;
		tidal_track_t track = {0};
		pthread_mutex_lock(&queued_lock);
		for (int i = 0; i < queued_count && index < 0; i++) {
			if (queued_tracks[i].id == id) {
				index = i;
				track = queued_tracks[i];
			}
		}
		pthread_mutex_unlock(&queued_lock);

		if (index < 0) {
			// Not in the queue being played: wait for the watchdog to
			// recompute rather than download for the previous queue.
			fprintf(stderr, "tidal: %ld is not in the list we know, not downloading it\n", id);
			prefetch_failed_id = id;
			prefetch_failed_ms = lv_tick_get();
			pthread_mutex_lock(&prefetch_lock);
			if (prefetch_want_id == id) {
				prefetch_want_id = 0;
			}
			pthread_mutex_unlock(&prefetch_lock);
			continue;
		}

		tidalcache_wait_idle();

		// The request may have changed while waiting for a turn: prev is
		// pressed and another track is needed. Without this check the one
		// chosen before the wait would download, and whoever pressed would wait
		// a whole extra track.
		pthread_mutex_lock(&prefetch_lock);
		bool moved_on = prefetch_want_id != id;
		pthread_mutex_unlock(&prefetch_lock);
		if (moved_on) {
			continue;
		}

		char next_path[512];
		bool ok = prepare_track(&track, next_path, sizeof(next_path));
		if (!ok) {
			fprintf(stderr, "tidal: %ld does not download: %s\n", track.id, tidalcache_last_error());
			prefetch_failed_id = id;
			prefetch_failed_ms = lv_tick_get();
		} else if (prefetch_failed_id == id) {
			prefetch_failed_id = 0;
		}

		// Either way this request is done: the watchdog recomputes on its next
		// pass. Without this, a track that will not download would keep the
		// thread retrying the same one forever.
		pthread_mutex_lock(&prefetch_lock);
		if (prefetch_want_id == id) {
			prefetch_want_id = 0;
		}
		bool superseded = prefetch_want_id != 0 && prefetch_want_id != id;
		pthread_mutex_unlock(&prefetch_lock);

		// Belt and braces: if the request changed while the download was
		// already starting (the abandon passed an instant earlier and missed
		// it), what was just started is a zombie nobody else will stop. It is
		// stopped here, the only place that knows it exists.
		if (ok && superseded && tidalcache_downloading_id() == id) {
			fprintf(stderr, "tidal: %ld had started but is no longer needed, dropping it\n", id);
			// Only Tidal's, not everyone's: this is not a user request but
			// cleanup of a Tidal download that started and is no longer
			// needed. Abandoning everything here would also discard the
			// track another service is downloading to play right now.
			tidalcache_abandon_all();
		}

		if (ok && !superseded) {
			// If playback was waiting for exactly this, it can resume now.
			gui_post(resume_if_waiting_async, NULL);
		}
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// the worker thread
// ---------------------------------------------------------------------------

// What is shown on screen while waiting for the code, and what the worker saw
// at the last poll. Written by the worker, read by the GUI inside the async:
// the same rule as the result_* buffers.
static tidal_login_t login;
static tidal_login_state_t login_result;

static void *worker_main(void *arg) {
	(void)arg;
	// The page waits on these requests behind its spinner: a service that
	// does not answer is said in seconds (streamerr.h).
	http_set_thread_timeout_cap(STREAM_UI_TIMEOUT_SECS);

	for (;;) {
		pthread_mutex_lock(&job_lock);
		while (!have_pending) {
			pthread_cond_wait(&job_ready, &job_lock);
		}

		// The previous result must have been taken, or it would be overwritten.
		// Two seconds of patience and then proceed anyway: see the comment on
		// result_pending.
		if (result_pending) {
			struct timespec until;
			struct timeval now;
			gettimeofday(&now, NULL);
			until.tv_sec = now.tv_sec + 2;
			until.tv_nsec = now.tv_usec * 1000;
			while (result_pending) {
				if (pthread_cond_timedwait(&result_taken, &job_lock, &until) == ETIMEDOUT) {
					break;
				}
			}
		}

		job_t job = pending;
		have_pending = false;
		pthread_mutex_unlock(&job_lock);

		result_error[0] = '\0';
		result_class = STREAM_ERR_NONE;
		result_kind = job.kind;
		result_featured_gone = -1;
		streamerr_begin();

		// The length of the list currently on screen, saved before touching
		// anything.
		//
		// The result_* buffers back the visible list: fill_list() draws the
		// rows from them, and list_tap() looks up what to do when one is
		// tapped. A failing job never calls fill_list() -- job_done_cb() shows
		// the error and returns -- so the previous rows stay on screen and the
		// count has to be put back, or `index >= result_count` would reject
		// every tap on them. Failing requests do not write into the arrays
		// (they return -1 before touching them, see tidal.c), so restoring the
		// count is enough to leave the list as it was.
		int kept_count = result_count;
		char kept_title[sizeof(result_title)];
		snprintf(kept_title, sizeof(kept_title), "%s", result_title);

		// Where this page starts. 0 is a new list; anything higher continues
		// the existing one, and only the tail of the array is written.
		int off = job.offset;
		if (off < 0 || off >= TIDAL_MAX_HELD) {
			off = 0;
		}
		int want = TIDAL_MAX_HELD - off;
		if (want > TIDAL_PAGE_LIMIT) {
			want = TIDAL_PAGE_LIMIT;
		}

		// Playback does not clear the list: what has to play is exactly the
		// track held in it, plus the ones after it to prefetch. (Clearing here
		// makes do_play() a no-op: it finds an empty list and returns
		// silently.) Nor does an extra page clear it: the old rows stay on
		// screen and usable while the new ones download.
		if (job_is_list(job.kind) && off == 0) {
			result_count = 0;
			result_title[0] = '\0';
		}
		result_list_serial = job.list_serial;

		int n = 0;
		switch (job.kind) {
		case JOB_LOGIN_BEGIN:
			memset(&login, 0, sizeof(login));
			if (!tidal_login_begin(&login)) {
				// Tidal's own words unless the network or the service is at
				// fault (streamerr.h).
				result_class = streamerr_describe("tidal", tidal_last_error(), result_error, sizeof(result_error));
				if (!streamerr_is_transport(result_class)) {
					snprintf(result_error, sizeof(result_error), "%s", tidal_last_error());
				}
			}
			break;
		case JOB_LOGIN_POLL:
			login_result = tidal_login_poll(&login);
			// Only this thread knows why: tidal_last_error() is per-thread and
			// would come back empty from the GUI.
			if (login_result == TIDAL_LOGIN_ERROR) {
				snprintf(result_error, sizeof(result_error), "%s", tidal_last_error());
			}
			break;

		case JOB_SEARCH_TRACKS:
			n = tidal_search_tracks(job.text, off, result_tracks + off, want);
			snprintf(result_title, sizeof(result_title), "%.150s", job.text);
			break;
		case JOB_SEARCH_ALBUMS:
			n = tidal_search_albums(job.text, off, result_albums + off, want);
			snprintf(result_title, sizeof(result_title), "%.150s", job.text);
			break;
		case JOB_SEARCH_ARTISTS:
			n = tidal_search_artists(job.text, off, result_artists + off, want);
			snprintf(result_title, sizeof(result_title), "%.150s", job.text);
			break;

		case JOB_ALBUM_TRACKS:
			n = tidal_album_tracks(job.id_text, off, result_tracks + off, want);
			snprintf(result_title, sizeof(result_title), "%.150s", job.text);
			break;
		case JOB_ARTIST_ALBUMS:
			n = tidal_artist_albums(job.id, off, result_albums + off, want);
			snprintf(result_title, sizeof(result_title), "%.150s", job.text);
			break;
		case JOB_PLAYLIST_TRACKS:
			// The only list requested by text rather than by number: Tidal
			// playlists are named by UUID.
			n = tidal_playlist_tracks(job.id_text, off, result_tracks + off, want);
			snprintf(result_title, sizeof(result_title), "%.150s", job.text);
			break;

		case JOB_FAV_TRACKS:
			n = tidal_favorite_tracks(off, result_tracks + off, want);
			snprintf(result_title, sizeof(result_title), "%s", tr("favourite_tracks"));
			break;
		case JOB_FAV_ALBUMS:
			n = tidal_favorite_albums(off, result_albums + off, want);
			snprintf(result_title, sizeof(result_title), "%s", tr("favourite_albums"));
			break;
		case JOB_USER_PLAYLISTS:
			n = tidal_user_playlists(off, result_playlists + off, want);
			snprintf(result_title, sizeof(result_title), "%s", tr("my_playlists"));
			break;
		case JOB_FEATURED:
			n = tidal_featured_albums(job.text2, off, result_albums + off, want);
			// The showcase is the only request that can fail without anything
			// being wrong: the endpoint it needs is no longer confirmed by
			// anyone (tidal.c explains at length). An error here is not shown
			// -- the row is simply removed -- and must not even reach the
			// generic check below.
			if (n <= 0) {
				result_featured_gone = job.index;
				n = 0;
			}
			snprintf(result_title, sizeof(result_title), "%.150s", job.text);
			break;

		case JOB_PLAY:
			do_play(job.index);
			break;

		case JOB_QUEUE_ALBUM:
			// Into a buffer of its own, handed over with the answer: the list
			// on screen stays as it is.
			result_queue_tracks = malloc(TIDAL_MAX_HELD * sizeof(*result_queue_tracks));
			n = result_queue_tracks ? tidal_album_tracks(job.id_text, 0, result_queue_tracks, TIDAL_MAX_HELD) : -1;
			result_queue_count = n > 0 ? n : 0;
			result_queue_where = job.index;
			snprintf(result_queue_what, sizeof(result_queue_what), "%s", job.text);
			break;

		default:
			break;
		}

		if (n < 0) {
			result_class = streamerr_describe("tidal", tidal_last_error(), result_error, sizeof(result_error));
			n = 0;
		}
		if (job_is_list(job.kind)) {
			// A job that will draw nothing -- an error, or a vanished showcase
			// -- restores what it saved: the list on screen is still the
			// previous one, and these numbers must keep describing it. Zero
			// results without an error is a real answer ("no results") and
			// stands.
			// A first page that failed is not one of these any more: the
			// error is shown in place of the list (streamerror.h), so the
			// rows go and the failed list's title stands.
			if ((result_error[0] && off > 0) || result_featured_gone >= 0) {
				result_count = kept_count;
				snprintf(result_title, sizeof(result_title), "%s", kept_title);
			} else {
				result_from = off;
				result_count = result_error[0] ? 0 : off + n;
			}
		}

		pthread_mutex_lock(&job_lock);
		result_pending = true;
		pthread_mutex_unlock(&job_lock);
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
// going back
//
// All Tidal lists live on one reused screen: search results, an album's
// tracks, an artist's albums, favourites. The screen manager always sees the
// same screen and so has nothing to stack -- opening an album from the results
// and going back would return to the search page, skipping the results.
//
// The page keeps its own history: a stack of jobs. The chevron (and the
// gesture, which goes through the same function) consults this first; only
// when it is empty does back actually leave the page.
// ---------------------------------------------------------------------------

#define LIST_HISTORY_DEPTH 8
static job_t list_history[LIST_HISTORY_DEPTH];
static int list_history_len;
static job_t current_list_job;
static bool have_current_list;

// The list job in flight, and which direction it is going.
//
// The history moves when the result arrives, not when the request starts: a
// request that never answers is not a step, since job_done_cb() with an error
// returns without fill_list() and without switch_screen() and the previous list
// stays on screen. Pushing at request time would leave current_list_job naming
// a page nobody ever saw, and the whole stack one step out.
static job_t inflight_job;
static bool inflight_valid;
static bool inflight_is_back; // going back consumes a step instead of adding one

static bool job_is_list(job_kind_t kind) {
	return kind != JOB_PLAY && kind != JOB_LOGIN_BEGIN && kind != JOB_LOGIN_POLL && kind != JOB_NONE &&
		   kind != JOB_QUEUE_ALBUM;
}

// The job put nothing on screen: navigation stays exactly as it was, and the
// next tap starts from the list currently displayed.
static void list_nav_drop(void) { inflight_valid = false; }

// The list has been drawn: now, and only now, the history moves. Call before
// switch_screen(), because it looks at which screen is active to tell whether
// the page is being entered from outside.
static void list_nav_commit(void) {
	if (!inflight_valid) {
		return;
	}
	inflight_valid = false;

	if (inflight_is_back) {
		// The step had only been peeked at: it is removed now that what it
		// named is really on screen.
		if (list_history_len > 0) {
			list_history_len--;
		}
	} else {
		// Re-reading the same list (after removing a favourite, say) is not a
		// step forward: the whole job is compared, not only the kind, or two
		// albums in a row would not stack.
		bool same = have_current_list && current_list_job.kind == inflight_job.kind &&
					current_list_job.id == inflight_job.id &&
					strcmp(current_list_job.id_text, inflight_job.id_text) == 0 &&
					strcmp(current_list_job.text, inflight_job.text) == 0;
		if (have_current_list && !same && lv_screen_active() == list_screen && list_history_len < LIST_HISTORY_DEPTH) {
			list_history[list_history_len++] = current_list_job;
		} else if (lv_screen_active() != list_screen) {
			list_history_len = 0; // entering from outside: the history restarts
		}
	}

	current_list_job = inflight_job;
	have_current_list = true;
}

// Preview for the back swipe: is there a step to consume inside the page? It
// moves nothing -- it only stops another screen's page being drawn under the
// finger.
static bool list_back_has_step(void) { return list_history_len > 0; }

static bool list_back_guard(void) {
	if (list_history_len == 0) {
		have_current_list = false;
		return false; // nothing underneath: leave the page
	}
	// The step below is only peeked at; list_nav_commit() consumes it once
	// that list has actually come back. Removing it here and then failing to
	// reload would lose that step back forever.
	job_t previous = list_history[list_history_len - 1];
	previous.list_serial = ++list_serial;
	inflight_job = previous;
	inflight_valid = true;
	inflight_is_back = true;
	page_more = false;
	page_loading = false;
	wait_begin();
	start_worker();
	submit(&previous);
	return true;
}

static void run(const job_t *job_in, const char *waiting_text) {
	(void)waiting_text;
	job_t copy = *job_in;
	job_t *job = &copy;
	// A job opening another list while one is already open: the previous one
	// has to be remembered -- but when the new one is visible, not now.
	if (job_is_list(job->kind)) {
		job->list_serial = ++list_serial;
		inflight_job = *job;
		inflight_valid = true;
		inflight_is_back = false;
		// A new list is starting: the old one's pagination must not fire
		// while waiting.
		page_more = false;
		page_loading = false;
		wait_begin();
	}

	start_worker();
	submit(job);
}

// The error panel's button: the failed list again, from the start.
static void retry_list(void) {
	if (have_current_list) {
		run(&current_list_job, "loading");
	}
}

static void open_login_cb(lv_event_t *e);

// Or, when the session was refused, the sign-in page.
static void error_sign_in(void) { open_login_cb(NULL); }

// Another page when the scroll nears the bottom, like the radio. No veil and
// no history -- it is the same list continuing, not a new one (inflight is
// deliberately left off so list_nav_commit() does not count the continuation
// as a step forward), and the existing rows stay usable meanwhile.
static void maybe_load_more(void) {
	if (page_loading || !page_more) {
		return;
	}
	if (lv_screen_active() != list_screen || !have_current_list || !job_is_list(current_list_job.kind)) {
		return;
	}
	// Less than one screen from the bottom is enough (svclist says when): the
	// request takes a moment, and reaching the last row only to find the list
	// stops there is exactly what this replaces.
	page_loading = true;
	job_t job = current_list_job;
	job.offset = shown_count;
	job.list_serial = list_serial;
	start_worker();
	submit(&job);
}

// ---------------------------------------------------------------------------
// taps
// ---------------------------------------------------------------------------

static void list_tap(lv_obj_t *row, void *user) {
	(void)row;
	int index = (int)(intptr_t)user;
	if (index < 0 || index >= shown_count) {
		return;
	}

	job_t job = {0};

	if (kind_is_tracks(list_kind)) {
		if (!result_tracks[index].streamable) {
			toast_error(tr("stream_not_in_subscription"));
			return;
		}
		job.kind = JOB_PLAY;
		job.index = index;
		run(&job, NULL);
		return;
	}
	if (kind_is_albums(list_kind)) {
		job.kind = JOB_ALBUM_TRACKS;
		snprintf(job.id_text, sizeof(job.id_text), "%s", result_albums[index].id_text);
		snprintf(job.text, sizeof(job.text), "%s", result_albums[index].title);
		run(&job, "loading");
		return;
	}
	if (list_kind == JOB_SEARCH_ARTISTS) {
		job.kind = JOB_ARTIST_ALBUMS;
		job.id = result_artists[index].id;
		snprintf(job.text, sizeof(job.text), "%s", result_artists[index].name);
		run(&job, "loading");
		return;
	}
	if (list_kind == JOB_USER_PLAYLISTS) {
		job.kind = JOB_PLAYLIST_TRACKS;
		snprintf(job.id_text, sizeof(job.id_text), "%s", result_playlists[index].id);
		snprintf(job.text, sizeof(job.text), "%s", result_playlists[index].name);
		run(&job, "loading");
	}
}

static void list_swipe_next(lv_obj_t *row, void *user) {
	(void)row;
	queue_entry((int)(intptr_t)user, PLAYLIST_QUEUE_NEXT);
}

static void list_swipe_end(lv_obj_t *row, void *user) {
	(void)row;
	queue_entry((int)(intptr_t)user, PLAYLIST_QUEUE_END);
}

static void list_star(lv_obj_t *row, void *user) {
	(void)row;
	int index = (int)(intptr_t)user;
	if (index >= 0 && index < shown_count && kind_is_tracks(list_kind)) {
		const tidal_track_t *t = &result_tracks[index];
		star_set(t->id, NULL, !tidalsync_is_favorite(t->id), t->title);
	}
}

// ---------------------------------------------------------------------------
// search
// ---------------------------------------------------------------------------

static lv_obj_t *search_field;
static lv_obj_t *search_clear_btn; // the x that empties it
static keyboard_t *search_keyboard;
static int search_kind_index; // 0 tracks, 1 albums, 2 artists
static lv_obj_t *search_pills[3];

static void paint_search_pills(void) {
	for (int i = 0; i < 3; i++) {
		sx_pill_set_active(search_pills[i], i == search_kind_index);
	}
}

static void search_pill_cb(lv_event_t *e) {
	search_kind_index = (int)(intptr_t)lv_event_get_user_data(e);
	paint_search_pills();
}

// The x is only visible when there is something to clear.
static void refresh_search_clear(void) {
	if (!search_clear_btn) {
		return;
	}
	const char *text = lv_textarea_get_text(search_field);
	if (text && text[0]) {
		lv_obj_set_hidden(search_clear_btn, false);
	} else {
		lv_obj_set_hidden(search_clear_btn, true);
	}
}

static void search_field_changed_cb(lv_event_t *e) {
	(void)e;
	refresh_search_clear();
}

static void search_clear_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(search_field, "");
	keyboard_reset(search_keyboard);
	lv_obj_add_state(search_field, LV_STATE_FOCUSED);
	lv_obj_send_event(search_field, LV_EVENT_FOCUSED, NULL);
	refresh_search_clear();
}

static void search_accept_cb(lv_event_t *e) {
	(void)e;
	const char *text = lv_textarea_get_text(search_field);
	if (!text || !*text) {
		return;
	}

	job_t job = {0};
	job.kind = search_kind_index == 1 ? JOB_SEARCH_ALBUMS
									  : (search_kind_index == 2 ? JOB_SEARCH_ARTISTS : JOB_SEARCH_TRACKS);
	snprintf(job.text, sizeof(job.text), "%s", text);
	run(&job, "searching");
}

static void open_search_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(search_field, "");
	keyboard_reset(search_keyboard);
	lv_obj_add_state(search_field, LV_STATE_FOCUSED);
	lv_obj_send_event(search_field, LV_EVENT_FOCUSED, NULL);
	refresh_search_clear();
	switch_screen(search_screen);
}

static void build_search_page(gui_config_t *cfg) {
	search_screen = lv_obj_create(NULL);
	lv_obj_add_style(search_screen, &theme_style_screen, 0);

	settingsrow_title(search_screen, cfg, "tidal_search_tidal");
	int top = settingsrow_content_top(cfg);

	search_field = lv_textarea_create(search_screen);
	lv_textarea_set_one_line(search_field, true);
	lv_textarea_set_placeholder_text(search_field, tr("artist_album_or_track"));
	// The handoff's text field: 60 px, the surface of a card, its radius.
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
	lv_obj_add_event_cb(search_field, search_field_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

	// The x on the right edge of the field, as in local search: clears the
	// query with one tap. Hidden while the field is empty.
	search_clear_btn = lv_btn_create(search_screen);
	lv_obj_set_size(search_clear_btn, 56, 56);
	lv_obj_align(search_clear_btn, LV_ALIGN_TOP_RIGHT, -SX_BODY_PAD_L - 2, top + 2);
	lv_obj_set_style_bg_opa(search_clear_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(search_clear_btn, 0, 0);
	lv_obj_set_style_border_width(search_clear_btn, 0, 0);
	lv_obj_set_style_pad_all(search_clear_btn, 0, 0);
	lv_obj_set_hidden(search_clear_btn, true);
	lv_obj_add_event_cb(search_clear_btn, search_clear_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *search_clear_icon = lv_image_create(search_clear_btn);
	lv_image_set_src(search_clear_icon, &icon_clear);
	lv_obj_add_style(search_clear_icon, &theme_style_icon, 0);
	lv_obj_set_style_image_opa(search_clear_icon, LV_OPA_70, 0);
	lv_obj_center(search_clear_icon);

	// Room for it, so a long query does not run underneath.
	lv_obj_set_style_pad_right(search_field, 60, 0);

	// What to look for: the handoff's filter pills under the field.
	static const char *const KINDS[] = {"tracks_2", "albums", "artists"};
	lv_obj_t *pills = sx_box(search_screen);
	lv_obj_set_size(pills, cfg->screen_width - 2 * SX_BODY_PAD_L, LV_SIZE_CONTENT);
	lv_obj_align(pills, LV_ALIGN_TOP_LEFT, SX_BODY_PAD_L, top + 74);
	lv_obj_set_flex_flow(pills, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(pills, 10, 0);
	for (int i = 0; i < 3; i++) {
		search_pills[i] = sx_pill(pills, tr(KINDS[i]), i, search_pill_cb);
	}
	paint_search_pills();

	search_keyboard = keyboard_create(search_screen, cfg->screen_width, 316, search_field, NULL, "ok",
									  search_accept_cb, NULL);
	switcher_attach_back_gesture(search_screen);
}

// ---------------------------------------------------------------------------
// login
//
// Nothing is typed here: Tidal closed direct login for third-party programs,
// and what remains is the device-code flow (see tidal.h). The player asks for
// a code, shows it in large type, and the user enters it on a phone; meanwhile
// Tidal is polled every few seconds to see whether anyone has.
//
// The polls are made by the worker, one at a time, and paced by an LVGL timer
// on the GUI thread: a sleep-and-retry loop inside a single job would hold the
// worker for the whole life of the code, which is minutes.
//
// The polling stops when the page is left, or it would be one request every two
// seconds forever, and sooner or later a login succeeding by itself on a page
// nobody is looking at.
// ---------------------------------------------------------------------------

static lv_obj_t *login_hint;
// The login QR. 200 px on a 480 px screen: large enough for a camera to read
// it from half a metre, small enough to leave room for the written code below.
#define LOGIN_QR_SIZE 200
// The quiet zone, the white border around it. The standard asks for four
// modules; here it is a fixed border, which works better at this size.
#define LOGIN_QR_QUIET 10

static lv_obj_t *login_qr;
static lv_obj_t *login_url;
static lv_obj_t *login_code;
static lv_obj_t *login_spin;
static lv_obj_t *login_message;
static lv_obj_t *login_retry;

static lv_timer_t *login_timer;
static int login_elapsed;	// seconds since the code arrived
static bool login_active;	// the page is open and the flow is running
static bool login_polling;	// a poll is in flight: never two at once

static void login_set_visible(lv_obj_t *widget, bool visible) {
	if (!widget) {
		return;
	}
	if (visible) {
		lv_obj_set_hidden(widget, false);
	} else {
		lv_obj_set_hidden(widget, true);
	}
}

static void login_stop(void) {
	login_active = false;
	login_polling = false;
	if (login_timer) {
		lv_timer_delete(login_timer);
		login_timer = NULL;
	}
}

// The flow ended without signing in: say what happened and leave a way to
// retry. Without the button the only way out would be to go back and re-enter,
// which on an expired code is what nine users in ten want to do.
static void login_ended(const char *message) {
	login_stop();
	lv_label_set_text(login_hint, "");
	lv_label_set_text(login_message, message && message[0] ? message : tr("tidal_sign_in_failed"));
	login_set_visible(login_qr, false);
	login_set_visible(login_url, false);
	login_set_visible(login_code, false);
	login_set_visible(login_spin, false);
	login_set_visible(login_message, true);
	login_set_visible(login_retry, true);
}

static void login_failed(const char *why) {
	if (!login_active) {
		return; // the page is gone: nobody left here to tell
	}
	login_ended(why);
}

static void login_poll_result(void) {
	login_polling = false;
	// A successful answer counts even if the page stopped waiting meanwhile:
	// the tokens are already written and the user is signed in, and ignoring it
	// would show "code expired" to someone who typed the code in time.
	if (!login_active && login_result != TIDAL_LOGIN_OK) {
		return;
	}
	switch (login_result) {
	case TIDAL_LOGIN_PENDING:
		break; // nobody has typed it yet: keep polling
	case TIDAL_LOGIN_OK:
		login_stop();
		rebuild_home();
		if (lv_screen_active() == login_screen) {
			switch_screen(tidal_screen);
		}
		toast_success("signed_in");
		break;
	case TIDAL_LOGIN_EXPIRED:
		login_ended(tr("tidal_the_code_has_expired"));
		break;
	case TIDAL_LOGIN_ERROR:
	default:
		// Only reached when the worker could not say why: when it can, the
		// answer goes through result_error and login_failed().
		login_ended(NULL);
		break;
	}
}

static void login_tick_cb(lv_timer_t *timer) {
	(void)timer;
	if (!login_active) {
		return;
	}
	login_elapsed++;

	// The code has a lifetime after which Tidal stops recognising it; polling
	// past that point is pure traffic.
	if (login.expires_secs > 0 && login_elapsed >= login.expires_secs) {
		login_ended(tr("tidal_the_code_has_expired"));
		return;
	}

	int interval = login.interval_secs > 0 ? login.interval_secs : 2;
	if (login_elapsed % interval != 0) {
		return;
	}
	if (login_polling) {
		return; // the previous poll has not come back yet
	}

	login_polling = true;
	job_t job = {0};
	job.kind = JOB_LOGIN_POLL;
	run(&job, NULL);
}

// The code arrived: show it, and start the countdown from here.
static void login_code_arrived(void) {
	if (!login_active) {
		return;
	}
	lv_label_set_text(login_code, login.user_code);
	login_set_visible(login_code, true);

	// The QR first: if it generates, the written URL is unnecessary and the
	// wording changes accordingly -- scan, not type.
	bool qr_ok = login_qr && login.verification_url[0] &&
				 lv_qrcode_update(login_qr, login.verification_url, (uint32_t)strlen(login.verification_url)) ==
					 LV_RESULT_OK;

	login_set_visible(login_qr, qr_ok);
	login_set_visible(login_url, !qr_ok);
	if (qr_ok) {
		lv_label_set_text(login_hint, tr("tidal_login_qr_note"));
	} else {
		lv_label_set_text(login_hint, tr("tidal_login_address_note"));
		lv_label_set_text(login_url, login.verification_url);
	}
	login_set_visible(login_spin, true);
	login_set_visible(login_message, false);
	login_set_visible(login_retry, false);

	login_elapsed = 0;
	if (!login_timer) {
		login_timer = lv_timer_create(login_tick_cb, 1000, NULL);
	}
}

static void login_start(void) {
	login_stop();
	login_active = true;
	login_elapsed = 0;

	lv_label_set_text(login_hint, tr("tidal_asking_code"));
	lv_label_set_text(login_url, "");
	lv_label_set_text(login_code, "");
	login_set_visible(login_url, false);
	login_set_visible(login_code, false);
	login_set_visible(login_spin, true);
	login_set_visible(login_message, false);
	login_set_visible(login_retry, false);

	job_t job = {0};
	job.kind = JOB_LOGIN_BEGIN;
	run(&job, NULL);
}

static void login_retry_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	login_start();
}

// The page is leaving: the polling stops here, however it was left (chevron,
// gesture, or a successful login).
static void login_unloaded_cb(lv_event_t *e) {
	(void)e;
	login_stop();
}

static void paint_login(void) {
	if (!login_url) {
		return;
	}
	lv_obj_set_style_text_color(login_url, theme()->accent, 0);
	lv_obj_set_style_text_color(login_code, theme()->text_primary, 0);
	lv_obj_set_style_bg_color(login_retry, theme()->accent, 0);
	lv_obj_set_style_text_color(lv_obj_get_child(login_retry, 0), lv_color_white(), 0);
}

static void build_login_page(gui_config_t *cfg) {
	login_screen = lv_obj_create(NULL);
	lv_obj_add_style(login_screen, &theme_style_screen, 0);

	settingsrow_title(login_screen, cfg, "tidal_sign_in_to_tidal");
	int top = settingsrow_content_top(cfg);

	// A single centred column: there is nothing to touch here except the retry
	// button, and everything on it has to be readable from a distance with a
	// phone in hand.
	lv_obj_t *box = lv_obj_create(login_screen);
	lv_obj_set_size(box, cfg->screen_width - 2 * cfg->padding, LV_SIZE_CONTENT);
	lv_obj_align(box, LV_ALIGN_TOP_MID, 0, top + 20);
	lv_obj_set_style_bg_opa(box, 0, 0);
	lv_obj_set_style_border_width(box, 0, 0);
	lv_obj_set_style_pad_all(box, 0, 0);
	lv_obj_set_style_pad_row(box, 22, 0);
	lv_obj_set_scrollable(box, false);
	lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	login_hint = lv_label_create(box);
	lv_label_set_long_mode(login_hint, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(login_hint, lv_pct(100));
	lv_obj_set_style_text_align(login_hint, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(login_hint, &theme_style_text, 0);
	lv_obj_set_style_text_font(login_hint, &font_ui_20, 0);
	lv_label_set_text(login_hint, "");

	// The QR, which is the URL. It encodes verificationUriComplete, the one
	// that already carries the code, so the phone lands on the filled-in page
	// without anything being typed.
	//
	// Fixed black and white, not theme colours: a QR is an optical target, and
	// drawing it in the accent colour on a dark background gives a contrast a
	// camera may not resolve. The white border is not decoration: the quiet
	// zone is part of the standard, and without it many readers will not lock
	// onto the code.
	login_qr = lv_qrcode_create(box);
	lv_qrcode_set_size(login_qr, LOGIN_QR_SIZE);
	lv_qrcode_set_dark_color(login_qr, lv_color_black());
	lv_qrcode_set_light_color(login_qr, lv_color_white());
	lv_obj_set_style_border_color(login_qr, lv_color_white(), 0);
	lv_obj_set_style_border_width(login_qr, LOGIN_QR_QUIET, 0);
	lv_obj_set_style_bg_color(login_qr, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(login_qr, LV_OPA_COVER, 0);

	// The written URL stays as a fallback, shown only when the QR could not be
	// generated. A login flow that cannot start because a library refused,
	// with no other way in, would be a dead end.
	login_url = lv_label_create(box);
	lv_label_set_long_mode(login_url, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(login_url, lv_pct(100));
	lv_obj_set_style_text_align(login_url, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(login_url, &font_ui_26, 0);
	lv_label_set_text(login_url, "");

	// The code below, bold and on one line: the fallback for anyone who cannot
	// scan the QR, read from a distance while typing on a phone. LONG_CLIP and
	// not WRAP -- five letters broken in half are worse than five small
	// letters -- and the extra letter spacing keeps adjacent characters from
	// being confused while copying.
	login_code = lv_label_create(box);
	lv_label_set_long_mode(login_code, LV_LABEL_LONG_CLIP);
	lv_obj_set_style_text_font(login_code, &font_ui_36_bold, 0);
	lv_obj_set_style_text_letter_space(login_code, 5, 0);
	lv_label_set_text(login_code, "");

	login_spin = spinner_create(box, &icon_loader_big);

	login_message = lv_label_create(box);
	lv_label_set_long_mode(login_message, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(login_message, lv_pct(100));
	lv_obj_set_style_text_align(login_message, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(login_message, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(login_message, &font_ui_20, 0);
	lv_label_set_text(login_message, "");

	login_retry = lv_btn_create(box);
	lv_obj_set_size(login_retry, LV_SIZE_CONTENT, 56);
	lv_obj_set_style_pad_hor(login_retry, 22, 0);
	lv_obj_set_style_radius(login_retry, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(login_retry, 0, 0);
	lv_obj_set_style_border_width(login_retry, 0, 0);
	lv_obj_add_event_cb(login_retry, login_retry_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *retry_label = lv_label_create(login_retry);
	lv_label_set_text(retry_label, tr("tidal_ask_for_a_new_code"));
	lv_obj_set_style_text_font(retry_label, &font_ui_20, 0);
	lv_obj_center(retry_label);

	login_set_visible(login_url, false);
	login_set_visible(login_code, false);
	login_set_visible(login_spin, false);
	login_set_visible(login_message, false);
	login_set_visible(login_retry, false);

	lv_obj_t *note = lv_label_create(login_screen);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, cfg->screen_width - 2 * cfg->padding);
	lv_obj_align(note, LV_ALIGN_BOTTOM_MID, 0, -cfg->padding - 10);
	lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_18, 0);
	lv_label_set_text(note, tr("tidal_login_privacy_note"));

	paint_login();
	theme_register_refresh(paint_login);
	lv_obj_add_event_cb(login_screen, login_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(login_screen);
}

static void open_login_cb(lv_event_t *e) {
	(void)e;
	switch_screen(login_screen);
	login_start();
}

// ---------------------------------------------------------------------------
// the main page
// ---------------------------------------------------------------------------

static lv_obj_t *home_container;
static lv_obj_t *search_btn;

// The showcases Tidal offers, with the short name tidal.c expects.
static const char *const FEATURED_TYPES[3] = {"new", "top", "recommended"};
static const char *const FEATURED_NAMES[3] = {"new_releases", "tidal_most_streamed", "tidal_recommended_for_you"};

// The showcases this session found empty. A showcase that answered with
// nothing is not offered again: the endpoint behind it is the only one in all
// of tidal.c that no other program still uses, and a card that leads to the
// same nothing every time is worse than no card.
//
// Not permanent, though: this is cleared on every return to the main page. From
// here the difference between "Tidal retired this showcase" and "the Wi-Fi
// dropped a packet" is invisible, so a single bad moment must not cost a card
// until the player is restarted.
static bool featured_hidden[3];

static const int QUALITY_VALUES[4] = {TIDAL_QUALITY_LOW, TIDAL_QUALITY_HIGH, TIDAL_QUALITY_LOSSLESS,
									  TIDAL_QUALITY_HIRES};

// The perceived quality first, then the format underneath: "High" alone is not
// distinguishable from "Hi-res", and "AAC 320" alone tells nobody it is lossy.
// These go through tr(): half of each is a translatable word.
static const char *const QUALITY_NAMES[4] = {"tidal_low_aac_96", "tidal_high_aac_320", "tidal_cd_flac_16_44",
											 "tidal_hi_res_flac_24"};
static lv_obj_t *quality_pills[4];

// The magnifier, shown only when signed in: otherwise there is nothing to
// search.
static void update_corner_buttons(void) {
	if (search_btn) {
		lv_obj_set_hidden(search_btn, !(tidal_configured() && tidal_logged_in()));
	}
}

// ---------------------------------------------------------------------------
// the quality and the account, on the page itself
//
// What used to sit behind the gear on a page of its own: the quality as pills,
// and the sign-out, on the account's card at the bottom of the main page.
// ---------------------------------------------------------------------------

static void quality_refresh(void) {
	int current = tidal_get_quality();
	for (int i = 0; i < 4; i++) {
		if (quality_pills[i]) {
			sx_pill_set_active(quality_pills[i], QUALITY_VALUES[i] == current);
		}
	}
}

static void quality_pick_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	tidal_set_quality((int)(intptr_t)lv_event_get_user_data(e));
	quality_refresh();
}

static void do_logout(void *user) {
	(void)user;
	tidal_logout();
	// The remembered favourites and playlists belonged to that account; under
	// another user they would be lies.
	tidalsync_forget();
	// And the tracks downloaded for that account are of no use to anyone.
	tidalcache_clear();
	// Not the covers: that directory is shared with Qobuz (see qobuzart.h),
	// and clearing it here would force a service unrelated to this sign-out to
	// download its own again.
	rebuild_home();
	toast_success("signed_out");
}

static void logout_cb(lv_event_t *e) {
	(void)e;
	// Just the question. The downloaded tracks do go, but they are transient
	// files the user never put there and does not know they have: mentioning
	// it here would make them weigh a decision over something that is not
	// theirs.
	char message[160];
	snprintf(message, sizeof(message), tr("sign_out_confirm_note"), tidal_display_name());
	confirm_show("tidal_sign_out_of_tidal", message, "leave", do_logout, NULL);
}

static void simple_job_cb(lv_event_t *e) {
	job_t job = {0};
	job.kind = (job_kind_t)(intptr_t)lv_event_get_user_data(e);
	run(&job, "loading");
}

static void featured_job_cb(lv_event_t *e) {
	int slot = (int)(intptr_t)lv_event_get_user_data(e);
	if (slot < 0 || slot >= 3) {
		return;
	}
	job_t job = {0};
	job.kind = JOB_FEATURED;
	job.index = slot;
	snprintf(job.text2, sizeof(job.text2), "%s", FEATURED_TYPES[slot]);
	snprintf(job.text, sizeof(job.text), "%s", tr(FEATURED_NAMES[slot]));
	run(&job, "loading");
}

// Called on returning to the main page: every showcase is tried again. It
// costs three requests that almost always succeed, and removes the case where
// a row disappears forever because of a one-second disconnection.
static void featured_reset(void) {
	for (int i = 0; i < 3; i++) {
		featured_hidden[i] = false;
	}
}

static void featured_gone(int slot) {
	if (slot < 0 || slot >= 3) {
		return;
	}
	featured_hidden[slot] = true;
	rebuild_home();
	// The row disappears and nothing else happens: no warning, because the
	// showcases are a bonus and an apology screen where a row of covers should
	// be is worse than one row fewer. The next entry to the page retries; see
	// the comment on featured_hidden.
}

// The "Wi-Fi settings" row of the no-network notice, as on the transfer page:
// it leads where the problem is fixed.
static void open_wifi_settings_cb(lv_event_t *e) {
	(void)e;
	switch_screen(wifisettings_screen);
}

// Whether the network is really there. Tidal has nothing to show without it:
// every list comes from the server, and with no IP address each request ends
// in a generic error that does not tell the user what they need to know.
static bool wifi_is_connected(void) {
#ifdef HOST_BUILD
	// The simulator runs on a machine that has a network but no wlan0; without
	// this the page could not be tested.
	return true;
#else
	wifi_status_t status;
	wifi_get_status(&status);
	return status.state == WIFI_STATE_CONNECTED && status.ip[0] != '\0';
#endif
}

static void rebuild_home(void) {
	lv_obj_clean(home_container);
	for (int i = 0; i < 4; i++) {
		quality_pills[i] = NULL;
	}
	update_corner_buttons();

	if (!tidal_configured()) {
		svcui_note(home_container, tr("api_keys_unavailable"));
		return;
	}

	// The network comes before the login, and not the other way round: signing
	// in is a conversation with Tidal's servers, so with no network the "Sign
	// in" card could only fail. The notice says what is actually missing.
	if (!wifi_is_connected()) {
		svcui_note(home_container, tr("enable_wi_fi_first"));
		svcui_nav_card(home_container, tr("wi_fi_settings"), NULL, open_wifi_settings_cb, NULL);
		return;
	}

	if (!tidal_logged_in()) {
		svcui_nav_card(home_container, tr("sign_in"), NULL, open_login_cb, NULL);
		return;
	}

	// No "Search" card: it is the magnifier in the corner, where it sits on
	// every other searchable page.
	svcui_nav_card(home_container, tr("favourite_tracks"), NULL, simple_job_cb, (void *)(intptr_t)JOB_FAV_TRACKS);
	svcui_nav_card(home_container, tr("favourite_albums"), NULL, simple_job_cb, (void *)(intptr_t)JOB_FAV_ALBUMS);
	svcui_nav_card(home_container, tr("my_playlists"), NULL, simple_job_cb, (void *)(intptr_t)JOB_USER_PLAYLISTS);
	for (int i = 0; i < 3; i++) {
		if (!featured_hidden[i]) {
			svcui_nav_card(home_container, tr(FEATURED_NAMES[i]), NULL, featured_job_cb, (void *)(intptr_t)i);
		}
	}

	lv_obj_t *quality = sx_card_create(home_container);
	sx_card_title(quality, tr("audio_quality"));
	lv_obj_t *pills = sx_card_pills(quality);
	for (int i = 0; i < 4; i++) {
		quality_pills[i] = sx_pill(pills, tr(QUALITY_NAMES[i]), QUALITY_VALUES[i], quality_pick_cb);
	}
	quality_refresh();

	// The account: who is signed in, and the way out.
	lv_obj_t *account = sx_card_create(home_container);
	sx_card_title(account, tidal_display_name());
	sx_card_subtitle(account, tr("tidal"), false);
	sx_card_action(account, tr("leave"), logout_cb, NULL);
}

// "Show album" in the player, for a Tidal track: opens the album's track list
// as if reached from search. The job is the usual JOB_ALBUM_TRACKS.
bool tidalpage_open_album(const char *album_id, const char *title) {
	if (!album_id || !album_id[0] || !tidal_logged_in()) {
		return false;
	}
	job_t job = {0};
	job.kind = JOB_ALBUM_TRACKS;
	snprintf(job.id_text, sizeof(job.id_text), "%s", album_id);
	snprintf(job.text, sizeof(job.text), "%s", title && title[0] ? title : tr("tidal"));
	run(&job, "loading");
	return true;
}

static void home_loaded_cb(lv_event_t *e) {
	(void)e;
	// The card may have been inserted or removed since the page was built, and
	// without a card there is nowhere to download to.
	tidalcache_set_root(storage_sd_root());
	qobuzart_set_root(storage_sd_root());

	// Every showcase is tried again: the one that did not answer last time may
	// well have been a one-second disconnection.
	featured_reset();

	// The page is rebuilt on every open, not once at startup. Everything it
	// depends on can have changed while elsewhere: the network, the keys, and
	// -- more so than on Qobuz -- the login, because a Tidal token lasts a
	// week and the automatic refresh may have failed.
	rebuild_home();
}

// ---------------------------------------------------------------------------

static void build_list_page(gui_config_t *cfg) {
	list_screen = lv_obj_create(NULL);
	lv_obj_add_style(list_screen, &theme_style_screen, 0);
	lv_obj_add_event_cb(list_screen, list_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(list_screen, list_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	list_title = settingsrow_title(list_screen, cfg, "tidal");
	int top = settingsrow_content_top(cfg);

	static const sx_row_handlers_t handlers = {
		.on_tap = list_tap,
		.on_long_press = list_hold,
		.on_swipe_next = list_swipe_next,
		.on_swipe_end = list_swipe_end,
		.on_star = list_star,
	};
	static const svclist_cbs_t cbs = {.bind = list_bind, .near_end = maybe_load_more, .visible = list_visible_cb};
	list = svclist_create(list_screen, &handlers, &cbs, ROW_POOL, false);
	lv_obj_t *view = svclist_obj(list);
	lv_obj_set_size(view, cfg->screen_width - 2 * SX_BODY_PAD_L, cfg->screen_height - top);
	lv_obj_set_pos(view, SX_BODY_PAD_L, top);

	list_empty = lv_label_create(list_screen);
	lv_label_set_text(list_empty, tr("no_results"));
	lv_obj_set_style_text_align(list_empty, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(list_empty, &sx_style_text2, 0);
	lv_obj_set_style_text_font(list_empty, &font_ui_24, 0);
	lv_obj_align(list_empty, LV_ALIGN_TOP_MID, 0, top + 100);
	lv_obj_set_hidden(list_empty, true);

	list_error = streamerror_create(list_screen);
	lv_obj_set_width(list_error, cfg->screen_width - 2 * SX_BODY_PAD_L);
	lv_obj_align(list_error, LV_ALIGN_TOP_MID, 0, top);
	streamerror_set_service(list_error, tr("tidal"));
	list_spin = svcui_spinner(list_screen);

	switcher_attach_back_gesture(list_screen);
	switcher_set_back_guard(list_screen, list_back_guard);
	switcher_set_back_guard_peek(list_screen, list_back_has_step);
	// The leftward drag that pulls in the player, as on every other browsable
	// page.
	player_sheet_attach_drag(list_screen, true);
}

// "Show album" from the player, as the provider registry asks for it: the
// screen back leads to, or NULL when the album could not be opened.
static void *provider_open_album(const char *album_id, const char *title) {
	return tidalpage_open_album(album_id, title) ? tidal_list_screen : NULL;
}

void tidalpage_init(gui_config_t *cfg) {
	// What only the GUI can tell the rest of the program about Tidal: its mark
	// over the artwork and how to open one of its albums (provider.h).
	const stream_provider_gui_t provider_gui = {.badge = &icon_tidal_badge, .open_album = provider_open_album};
	stream_provider_attach_gui("tidal", &provider_gui);

	tidal_screen = lv_obj_create(NULL);
	lv_obj_add_style(tidal_screen, &theme_style_screen, 0);
	lv_obj_t *title = settingsrow_title(tidal_screen, cfg, "tidal");
	home_container = svcui_column(tidal_screen, cfg);

	// The magnifier top right. The title has to be told the corner is taken,
	// or its text runs underneath it.
	settingsrow_title_corner_slots(title, cfg, 1);
	search_btn = svcui_header_button(tidal_screen, cfg, 0, &icon_search, open_search_cb, NULL);

	build_list_page(cfg);
	build_search_page(cfg);
	build_login_page(cfg);
	tidalcache_set_slow_start_cb(on_slow_start);

	// "Can this file play right now?". Added, not set: device_state keeps a
	// list of these precisely because there are two services, so the Qobuz one
	// sits alongside without either overriding the other. A path that is not
	// a Tidal one returns true immediately.
	device_state_add_prepare_cb(tidalpage_prepare_track);

	// The queue watchdog: tells the prefetch thread how far the ready tracks
	// reach. One second is enough -- these are tracks, not frames.
	lv_timer_create(queue_watch_cb, 1000, NULL);
	// Created paused: the first cover request resumes it.
	art_timer = lv_timer_create(art_poll_cb, 120, NULL);
	lv_timer_pause(art_timer);
	art_drop_timer = lv_timer_create(art_drop_cb, ART_DROP_DELAY_MS, NULL);
	lv_timer_pause(art_drop_timer);
	qobuzart_set_root(storage_sd_root());
	for (int i = 0; i < QOBUZART_SLOTS; i++) {
		slot_owner[i] = -1;
	}

	tidalcache_set_root(storage_sd_root());
	rebuild_home();

	lv_obj_add_event_cb(tidal_screen, home_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(tidal_screen);
	player_sheet_attach_drag(tidal_screen, true);
	player_sheet_attach_drag(home_container, true);
}

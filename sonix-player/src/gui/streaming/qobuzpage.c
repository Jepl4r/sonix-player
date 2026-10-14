#include "qobuzpage.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <stdlib.h>

#include "src/gui/shell/confirm.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
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
#include "src/gui/shell/touch.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/system/decode/growfile.h"
#include "src/system/playback/device_state.h"
#include "src/system/core/lang.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/qobuz.h"
#include "src/system/streaming/qobuzcache.h"
#include "src/system/streaming/provider.h"
#include "src/system/streaming/streamerr.h"
#include "src/system/net/http.h"
#include "src/system/streaming/streamturn.h"
#include "src/system/streaming/qobuzsync.h"
#include "src/system/device/system.h"
#include "src/system/core/utils.h"
#include "src/system/net/wifi.h"
#include "src/system/net/wifitransfer.h"

// ---------------------------------------------------------------------------
// Qobuz front end.
//
// Everything that talks to the network BLOCKS (see qobuz.h), so none of it can
// run on the GUI thread: there is a single worker with a single slot for the
// job to do, and results come back through gui_post() -- the purpose-built
// bridge (lv_async_call is NOT safe from another thread: see gui.h).
//
// One slot rather than a queue: touching a second entry while the first is
// still loading means the second is what matters. The first is dropped, not
// queued.
// ---------------------------------------------------------------------------

#define QOBUZ_PAGE_LIMIT 50
// Maximum entries held: four pages, as for radio stations. Reaching the bottom
// asks for another fifty; past this cap the list really stops, because every
// entry is static memory on a device with 64 MB in total.
#define QOBUZ_MAX_HELD (4 * QOBUZ_PAGE_LIMIT)


static lv_obj_t *login_screen;
lv_obj_t *qobuz_list_screen;
#define list_screen qobuz_list_screen
static lv_obj_t *search_screen;

// Global rather than hidden behind a function: the Streaming grid wants the
// address of the variable (grid_entry_t.target), as for every other page.
lv_obj_t *qobuz_screen;

// ---------------------------------------------------------------------------
// the worker
// ---------------------------------------------------------------------------

typedef enum {
	JOB_NONE = 0,
	JOB_LOGIN,
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
	char text[256];	   // search terms, or user name
	char text2[128];   // password
	char id_text[40];  // album identifier
	long id;		   // numeric identifier
	int index;		   // which track of the list playback starts from
	int offset;		   // first entry of this page: 0 = new list
	unsigned list_serial; // list jobs: list_serial when asked
} job_t;

static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_ready = PTHREAD_COND_INITIALIZER;
static job_t pending;
static bool have_pending;
static bool worker_started;

// Job serial number: when it changes, a late result belongs to a request that
// no longer matters and is discarded.
static unsigned job_serial;

// What came back. Only the worker writes here, and only while the GUI is not
// reading (it reads inside the async, which runs after the worker is done).
static qobuz_track_t result_tracks[QOBUZ_MAX_HELD];
static qobuz_album_t result_albums[QOBUZ_MAX_HELD];
static qobuz_artist_t result_artists[QOBUZ_MAX_HELD];
static qobuz_playlist_t result_playlists[QOBUZ_MAX_HELD];
static int result_count;
static int result_from; // where the last page received starts (0 = new list)
static job_kind_t result_kind;
static char result_error[256];
static char result_title[160];
static stream_err_t result_class; // what kind of failure result_error is

// What the list is showing right now, so a row tap knows what to do.
static job_kind_t list_kind;

// Raised by each new list asked for (GUI thread): an answer carrying an older
// number belongs to a list the user has since left, and is dropped.
static unsigned list_serial;

// Pagination, as for radio: the last page came back full, so there may be
// another -- and one request at a time.
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
// 2.0 has no veils (the handoff: no full-screen dimming). A list on its way
// opens the list page at once, under its title, with the spinner where the
// rows will be: the page stays usable, and back goes back. Signing in, which
// leaves nothing else to do on its page, holds the busy toast until Qobuz has
// answered. Playback waits for nothing: the track starts while it is still
// downloading (see qobuzcache_start).
// ---------------------------------------------------------------------------

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
// Undo away. The mirror of favourite tracks flips before Qobuz answers
// (qobuzsync.h); a refusal is said, and the rows follow the mirror again.
// ---------------------------------------------------------------------------

static void list_refresh_async(void *user) {
	(void)user;
	svclist_rebind(list);
}

static void refused_async(void *user) {
	char *why = user;
	toast_error(why && why[0] ? why : tr("qobuz_change_refused"));
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
		qobuzsync_album_favorite(star_album_id, on, sync_done, NULL);
	} else {
		qobuzsync_favorite_toggle(star_track_id, on, sync_done, NULL);
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
// A Qobuz track goes into the queue by its cache path -- the file when it is
// on the card, the name it will have otherwise -- and becomes known to the
// prefetch, which fetches it when playback nears it as it does for a list
// played from here (qobuz_prepare_track()). An album's tracks are asked for
// first.
// ---------------------------------------------------------------------------

// The whole queued list: needed by the prefetch thread, and needed by
// on-demand requests -- when playback reaches a track that has not downloaded
// yet, this is where what to ask Qobuz for is found. The list played, and
// what was queued from the lists since. Under its lock: the prefetch thread
// reads it.
static qobuz_track_t queued_tracks[QOBUZ_MAX_HELD];
static int queued_count;
static pthread_mutex_t queued_lock = PTHREAD_MUTEX_INITIALIZER;

// Makes `t` known to the prefetch (the oldest known track gives way when the
// list is full), and writes its tags beside the cache name, so the queue
// shows its title. Its path into `path`.
static void queued_add(const qobuz_track_t *t, char *path, size_t size) {
	const char *expected = qobuz_expected_mime();
	if (!qobuzcache_find(t->id, path, size)) {
		qobuzcache_path(t->id, expected, path, size);
	}
	qobuzcache_write_sidecars(t->id, expected, t->title, t->artist, t->album, t->album_id, t->track_number, NULL);
	pthread_mutex_lock(&queued_lock);
	bool known = false;
	for (int i = 0; i < queued_count && !known; i++) {
		known = queued_tracks[i].id == t->id;
	}
	if (!known) {
		if (queued_count == QOBUZ_MAX_HELD) {
			memmove(queued_tracks, queued_tracks + 1, (QOBUZ_MAX_HELD - 1) * sizeof(*queued_tracks));
			queued_count--;
		}
		queued_tracks[queued_count++] = *t;
	}
	pthread_mutex_unlock(&queued_lock);
}

static void queue_tracks(const qobuz_track_t *tracks, int count, playlist_queue_where_t where, const char *what) {
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

static void queue_album(const qobuz_album_t *a, playlist_queue_where_t where) {
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
	const qobuz_track_t *t = &result_tracks[menu_index];
	job_t job = {0};
	job.kind = JOB_ALBUM_TRACKS;
	snprintf(job.id_text, sizeof(job.id_text), "%s", t->album_id);
	snprintf(job.text, sizeof(job.text), "%s", t->album[0] ? t->album : tr("qobuz"));
	run(&job, "loading");
}

// Deleting a playlist on Qobuz cannot be undone: asked first, in red.
static long delete_playlist_id;

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
	qobuzsync_playlist_remove(delete_playlist_id, playlist_deleted, NULL);
}

static void sheet_playlist_delete(void *user) {
	(void)user;
	if (!menu_valid() || list_kind != JOB_USER_PLAYLISTS) {
		return;
	}
	delete_playlist_id = result_playlists[menu_index].id;
	char message[QOBUZ_TITLE_MAX + 64];
	snprintf(message, sizeof(message), tr("qobuz_will_be_deleted_from_your_qobuz"), result_playlists[menu_index].name);
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
	static char title[QOBUZ_TITLE_MAX];
	if (kind_is_tracks(list_kind) || kind_is_albums(list_kind)) {
		items[n++] = (sx_sheet_item_t){.label = tr("queue_play_next"), .icon = &icon_list_start, .action = sheet_queue,
									   .user = (void *)(intptr_t)PLAYLIST_QUEUE_NEXT};
		items[n++] = (sx_sheet_item_t){.label = tr("queue_add_to_end"), .icon = &icon_list_plus_small,
									   .action = sheet_queue, .user = (void *)(intptr_t)PLAYLIST_QUEUE_END};
	}
	if (kind_is_tracks(list_kind)) {
		const qobuz_track_t *t = &result_tracks[index];
		if (!t->streamable) {
			n = 0; // nothing of the queue for a track that will not play
		}
		bool fav = qobuzsync_is_favorite(t->id);
		items[n++] = (sx_sheet_item_t){.label = tr(fav ? "remove_from_favourites" : "medialist_add_to_favourites"),
									   .icon = fav ? &icon_star_filled : &icon_star,
									   .action = sheet_star,
									   .user = fav ? NULL : (void *)1};
		if (t->album_id[0] && list_kind != JOB_ALBUM_TRACKS) {
			items[n++] = (sx_sheet_item_t){.label = tr("show_album"), .icon = &icon_chevron_right, .action = sheet_album};
		}
		snprintf(title, sizeof(title), "%s", t->title);
	} else if (kind_is_albums(list_kind)) {
		// Qobuz says whether an album is a favourite only in the list of
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
static cover_image_t row_images[QOBUZ_MAX_HELD];
static bool row_has_image[QOBUZ_MAX_HELD];

// This entry already asked for its cover and did not get one: dead URL, image
// that fails to decode, host that does not answer. Without this one broken
// entry would be asked for forever and keep a slot from those that can be
// served.
static bool row_art_failed[QOBUZ_MAX_HELD];

// The token that claims the cover slots: only its address matters, never its
// value. See qobuzart.h.
static const char qobuz_art_owner;

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
	qobuzart_release(&qobuz_art_owner, slot);
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
		if (entry < 0 || entry >= QOBUZ_MAX_HELD) {
			continue;
		}
		cover_image_t image;
		bool finished = false;
		if (!qobuzart_take(&qobuz_art_owner, slot, &image, &finished)) {
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
	for (int entry = from; entry <= to && entry < QOBUZ_MAX_HELD; entry++) {
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
		qobuzart_request(&qobuz_art_owner, free_slot, url, thumb_px());
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
	for (int entry = 0; entry < QOBUZ_MAX_HELD; entry++) {
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
	for (int entry = 0; entry < QOBUZ_MAX_HELD; entry++) {
		row_has_image[entry] = false;
	}
	// The rows let go of the pictures before they are freed.
	svclist_rebind(list);
	for (int entry = 0; entry < QOBUZ_MAX_HELD; entry++) {
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
		const qobuz_track_t *t = &result_tracks[index];
		if (!t->streamable) {
			snprintf(out, size, "%s \xC2\xB7 %s", t->artist, tr("not_included"));
		} else {
			snprintf(out, size, "%s", t->artist);
		}
	} else if (kind_is_albums(list_kind)) {
		const qobuz_album_t *a = &result_albums[index];
		snprintf(out, size, "%s%s%s", a->artist, a->released[0] ? " \xC2\xB7 " : "", a->released);
	} else if (list_kind == JOB_SEARCH_ARTISTS) {
		const qobuz_artist_t *a = &result_artists[index];
		if (a->album_count > 0) {
			snprintf(out, size, tr(a->album_count == 1 ? "subsonic_one_album" : "subsonic_albums_n"), a->album_count);
		}
	} else if (list_kind == JOB_USER_PLAYLISTS) {
		const qobuz_playlist_t *p = &result_playlists[index];
		snprintf(out, size, "%s \xC2\xB7 %d", p->owner, p->track_count);
	}
}

// The track playing now, as a Qobuz id, for the playing row.
static long playing_id;

static bool playing_update(void) {
	char path[512];
	long id = 0;
	if (playlist_current_path(path, sizeof(path)) && qobuzcache_owns(path)) {
		id = qobuzcache_track_id(path);
	}
	if (id == playing_id) {
		return false;
	}
	playing_id = id;
	return true;
}

// "· 24/96", "· 24/44.1": a hi-res track's resolution, the handoff's accent
// badge.
static void hires_badge(const qobuz_track_t *t, char *out, size_t size) {
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
	const lv_image_dsc_t *image = index < QOBUZ_MAX_HELD && row_has_image[index] ? &row_images[index].dsc : NULL;
	sx_row_set_playing(row, false);
	lv_obj_set_style_opa(row, LV_OPA_COVER, 0);
	if (kind_is_tracks(list_kind)) {
		const qobuz_track_t *t = &result_tracks[index];
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
		sx_row_set_starred(row, qobuzsync_is_favorite(t->id));
		sx_row_set_swipes(row, t->streamable, t->streamable);
		sx_row_set_playing(row, playing_id != 0 && playing_id == t->id);
		// A track the subscription does not cover stays in the list, faint:
		// hiding it would leave a gap in the album with no explanation.
		if (!t->streamable) {
			lv_obj_set_style_opa(row, LV_OPA_50, 0);
		}
	} else if (kind_is_albums(list_kind)) {
		const qobuz_album_t *a = &result_albums[index];
		sx_row_set_title(row, a->title);
		sx_row_set_subtitle(row, detail, NULL, NULL);
		sx_row_set_thumb(row, image, svcui_tone(a->id_text));
		sx_row_set_trail(row, SX_ROW_TRAIL_CHEVRON);
		sx_row_set_swipes(row, true, true);
	} else if (list_kind == JOB_SEARCH_ARTISTS) {
		const qobuz_artist_t *a = &result_artists[index];
		sx_row_set_title(row, a->name);
		sx_row_set_subtitle(row, detail, NULL, NULL);
		sx_row_set_thumb(row, image, svcui_tone(a->name));
		sx_row_set_trail(row, SX_ROW_TRAIL_CHEVRON);
		sx_row_set_swipes(row, false, false);
	} else if (list_kind == JOB_USER_PLAYLISTS) {
		const qobuz_playlist_t *p = &result_playlists[index];
		sx_row_set_title(row, p->name);
		sx_row_set_subtitle(row, detail, NULL, NULL);
		sx_row_set_thumb(row, image, svcui_tone(p->name));
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
		qobuzsync_favorites_refresh(favourites_known);
	}
}

// ---------------------------------------------------------------------------
// what comes back from the worker, on the GUI thread
// ---------------------------------------------------------------------------

static bool job_is_list(job_kind_t kind);
static void retry_list(void);
static void error_sign_in(void);

// One job's answer, handed to the GUI thread whole through gui_post's user
// pointer, which then owns it. The worker used to post a bare "done" and the
// GUI read the shared result_* fields -- which the next job could already be
// overwriting by then. The lists' entries stay in the shared arrays: the rows
// are emptied before a new list is asked for (run()), and a further page only
// writes past the rows shown.
typedef struct {
	job_kind_t kind;
	char error[256];
	stream_err_t error_class;
	char title[160];
	int from, count;
	unsigned list_serial;
	// JOB_QUEUE_ALBUM: where, the album's title, and its tracks (owned).
	int where;
	char what[256];
	qobuz_track_t *tracks;
} answer_t;

static void job_done_cb(void *user) {
	answer_t *a = user;
	if (a->kind == JOB_LOGIN) {
		sx_toast_busy_end();
	}
	if (a->kind == JOB_QUEUE_ALBUM) {
		if (a->error[0] || a->count <= 0) {
			toast_error(a->error[0] ? a->error : tr("playlist_cannot_add_the_tracks"));
		} else {
			queue_tracks(a->tracks, a->count, (playlist_queue_where_t)a->where, a->what);
		}
		free(a->tracks);
		free(a);
		return;
	}
	// A list the user has since left (or asked for again): another answer is
	// on its way, and this one would only flash past.
	if (job_is_list(a->kind) && a->list_serial != list_serial) {
		free(a);
		return;
	}

	if (a->error[0]) {
		if (job_is_list(a->kind) && a->from == 0) {
			// Said where the list would have been, with a way to ask again,
			// rather than in a popup over whatever page is open.
			page_loading = false;
			page_more = false;
			lv_obj_set_hidden(list_spin, true);
			lv_obj_set_hidden(list_empty, true);
			lv_label_set_text(list_title, a->title[0] ? a->title : lv_label_get_text(list_title));
			streamerror_show(list_error, a->error_class, a->error, retry_list, error_sign_in);
		} else if (job_is_list(a->kind)) {
			// A further page: the rows already there stay, and scrolling does
			// not ask again by itself.
			page_loading = false;
			page_more = false;
			toast_error(a->error);
		} else {
			gui_notify_popup(a->error);
		}
		free(a);
		return;
	}
	if (job_is_list(a->kind) && a->from == 0) {
		streamerror_hide(list_error);
	}

	switch (a->kind) {
	case JOB_LOGIN:
		rebuild_home();
		switch_screen(qobuz_screen);
		toast_success("signed_in");
		break;

	case JOB_PLAY:
		// Nothing: the music started long ago (play_started_async). This point
		// is reached much later -- after the next track has been prefetched
		// too, which takes minutes.
		break;

	default:
		fill_list(a->kind, a->from, a->count, a->title);
		// A full last page probably means there is another. A short one means
		// the list really ended, and nothing more is requested.
		page_loading = false;
		page_more = (a->count - a->from == QOBUZ_PAGE_LIMIT) && a->count < QOBUZ_MAX_HELD;
		break;
	}
	free(a);
}

// ---------------------------------------------------------------------------
// playback
//
// The tapped track downloads and starts by itself. The ones after it, in the
// same list, download while it plays and are queued as they arrive: an album
// plays straight through without waiting for the whole thing to land before the
// first note.
// ---------------------------------------------------------------------------

// How many tracks to prefetch. Two and not four: a hi-res track is around
// 190 MB, and prefetching four fills the cache (and the card) for a queue
// nobody may listen to.
#define PREFETCH_AHEAD 2

// Says the network is slower than the track and a large chunk has to arrive
// before playback can start, rather than leaving the screen frozen. Posted to
// the GUI thread, and a toast that goes away on its own rather than a blocking
// veil -- the rest of the interface stays usable while it downloads.
static void slow_start_async(void *user) {
	int seconds = (int)(intptr_t)user;
	char message[128];
	snprintf(message, sizeof(message), tr("stream_slow_connection"), seconds);
	gui_notify_popup(message);
}

static void on_slow_start(int seconds) { gui_post(slow_start_async, (void *)(intptr_t)seconds); }

// The player opens as soon as the music starts, not at the end of the job:
// after the start the worker prefetches the next track, which can take minutes,
// and waiting for that would leave the list on screen with music playing.
// Set when playback stopped on a track that had not arrived yet: when the file
// lands, it resumes from where it stopped.
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
	if (qobuzcache_owns(path) && !track_is_playable(path)) {
		// A different track arrived, not this one. Logged: when the resume does
		// not fire, this is the place to look.
		fprintf(stderr, "qobuz: something arrived but the queue is waiting for '%s', which is not ready\n", path);
		return;
	}
	waiting_for_track = false;
	fprintf(stderr, "qobuz: restarting the queue from slot %d ('%s')\n", index, path);
	device_state_play_queue_index(index);
	player_refresh_now_playing();
}

// True when the file can already be played: fully downloaded, or growing right
// now. When it is false the caller stops playback, which then resumes on its
// own once the track has landed (see resume_if_waiting_async).
static bool track_is_playable(const char *path) {
	long id = qobuzcache_track_id(path);
	// Fully downloaded, under this name or under another container.
	if (qobuzcache_find(id, NULL, 0)) {
		return true;
	}
	// Or downloading RIGHT NOW: the decoder can read a growing file, which is
	// how the first note plays without waiting for the end.
	//
	// Two questions, not one: growfile compares paths, the cache compares the
	// identifier. Both are asked, and a disagreement is logged.
	bool growing = growfile_is_growing(path);
	bool downloading = id != 0 && qobuzcache_downloading_id() == id;
	if (growing != downloading) {
		fprintf(stderr, "qobuz: '%s' -- growfile says %d, the cache says %d: THEY DISAGREE\n", path, growing,
				downloading);
	}
	return growing || downloading;
}

// The "downloading" notice MUST go through gui_post.
//
// qobuz_prepare_track is called by device_state when a file is about to start,
// and device_state calls it from whatever thread it happens to be on: the GUI
// thread when a finger picked the track, but also this page's WORKER when
// do_play() started the queue. LVGL is not reentrant and has no locks
// (LV_USE_OS is off): drawing a popup from there is the failure class that
// shows up as an occasional crash with no pattern.
static void downloading_note_async(void *user) {
	(void)user;
	gui_notify_popup("downloading");
}

static bool qobuz_prepare_track(const char *path) {
	if (!qobuzcache_owns(path)) {
		waiting_for_track = false;
		return true; // not a Qobuz file
	}
	if (track_is_playable(path)) {
		waiting_for_track = false;
		return true;
	}
	// Not there. The reason goes to the log in full: when this decision is
	// wrong the player spins in circles, and without this line the device logs
	// cost days.
	fprintf(stderr, "qobuz: '%s' not ready (id=%ld, downloading=%ld): stopping and requesting it\n", path,
			qobuzcache_track_id(path), qobuzcache_downloading_id());

	// Whatever was playing is stopped FIRST, and not out of politeness: the
	// previous track is reading a file that is still downloading, and it is one
	// of those prefetch_urgent() is about to abandon. Pulling the file out from
	// under the audio thread makes it report "track finished", the queue
	// advances by itself onto another missing track, and the loop starts over.
	// Stopped, there is nothing to pull out from under anyone.
	//
	// The network is needed, and needed NOW: the Wi-Fi idle parking is told to
	// hold before playback is even stopped, or the radio goes down in the gap
	// (see qobuzcache_network_wanted()).
	qobuzcache_set_network_wanted(true);
	waiting_for_track = true;
	device_state_stop();
	prefetch_urgent(path);
	gui_post(downloading_note_async, NULL);
	return false;
}

// A track's cover arrives with the track, so it can land an instant AFTER the
// player has already looked for one. If it belongs to the track being played,
// the player must be told, otherwise the grey note stays until the next track.
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
// prefetching, on a thread of its own
//
// Not on the worker that serves taps: prefetching sits in qobuzcache_wait_idle()
// for the whole download, which on a real network is minutes, and for all that
// time tapping another entry would do nothing. The worker always answers, and
// this thread checks the wanted id before each step, so as soon as another track
// is started it notices it is no longer needed and gives up.
// ---------------------------------------------------------------------------

static pthread_mutex_t prefetch_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t prefetch_ready = PTHREAD_COND_INITIALIZER;

// Which queued track must be downloaded now, as a Qobuz IDENTIFIER.
// 0 = nothing to do. Written by the queue watchdog on the GUI thread, the only
// one that knows where playback is and which files already exist.
//
// The identifier and not the position: with shuffle on the play queue has an
// order of its own, and position N of the queue is not position N of the list
// the queue came from. An identifier means the same track in any order.
static long prefetch_want_id;
static bool prefetch_started;

// The last track that did NOT download, and when it was tried. Keeps the thread
// from charging at the same identifier once a second when Qobuz says no: it
// waits a moment and retries.
static long prefetch_failed_id;
static uint32_t prefetch_failed_ms;
#define PREFETCH_RETRY_MS 4000

static void *prefetch_main(void *arg);

// There is deliberately no serial check here. Every job of this page increments
// the job serial, browsing included, so it says nothing about what is playing.
// Whether a download is still worth doing is decided by looking the track up in
// queued_tracks -- the check just below.

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

// The track needed NOW -- prev was pressed, or the queue reached one that was
// not prefetched.
//
// Two things, both of which remove seconds of waiting after a key press: the
// thread is asked immediately instead of waiting for the watchdog tick, and the
// running download is abandoned -- only one downloads at a time, and letting it
// finish means waiting a whole track before this one starts.
//
// Two things not done: never abandon while music is still playing (the caller
// stops first, see qobuz_prepare_track -- the file the audio thread is reading
// is one of those being abandoned, and pulling it away mid-read makes it report
// "track finished"), and never abandon when the download in progress is already
// the wanted track.
static void prefetch_urgent(const char *wanted_path) {
	long wanted = qobuzcache_track_id(wanted_path ? wanted_path : "");
	if (wanted == 0) {
		return; // not a cache track: there is nothing to download
	}

	// The name stem (without extension): Qobuz may have sent a container other
	// than the expected one, in which case the file downloading for this track
	// is named differently.
	char stem[512];
	snprintf(stem, sizeof(stem), "%s", wanted_path ? wanted_path : "");
	char *dot = strrchr(stem, '.');
	if (dot) {
		*(dot + 1) = '\0'; // "<folder>/12345678."
	}
	// "Is this exact one already downloading?", asked both ways: by path to
	// growfile and by identifier to the cache. Abandoning the download of the
	// track being waited for is the most expensive mistake of all -- it restarts
	// from zero every time and never finishes.
	bool already_ours = (stem[0] && growfile_prefix_is_growing(stem)) || qobuzcache_downloading_id() == wanted;
	if (!already_ours) {
		streamturn_abandon_all();
	} else {
		fprintf(stderr, "qobuz: %ld is already downloading, not dropping it\n", wanted);
	}

	qobuzcache_set_network_wanted(true);

	pthread_mutex_lock(&prefetch_lock);
	prefetch_want_id = wanted;
	pthread_cond_broadcast(&prefetch_ready);
	pthread_mutex_unlock(&prefetch_lock);
}

// Prefetches one track of the list while another plays: signed URL, download,
// and the sidecar files (tags and cover) so that everything is ready by the
// time its turn comes.
static bool prepare_track(const qobuz_track_t *track, char *path, size_t size) {
	char url[QOBUZ_URL_MAX];
	char mime[64];
	if (!qobuz_track_url(track->id, url, sizeof(url), mime, sizeof(mime), NULL)) {
		return false;
	}

	// Between getting the URL and starting the download the request may ALREADY
	// have changed: getFileUrl costs hundreds of milliseconds of real network,
	// and inside that window a next abandons everything and asks for another
	// track. A download that STARTS after that abandon can no longer be
	// stopped by anyone: it holds the single download slot for a whole file,
	// and the track the user is looking at waits behind it.
	pthread_mutex_lock(&prefetch_lock);
	bool superseded = prefetch_want_id != 0 && prefetch_want_id != track->id;
	pthread_mutex_unlock(&prefetch_lock);
	if (superseded) {
		fprintf(stderr, "qobuz: %ld no longer needed, not even starting it\n", track->id);
		return false;
	}

	if (!qobuzcache_start(track->id, url, mime, track->duration, path, size)) {
		return false;
	}

	// The REAL name against the one the queue guessed. If they differ, the whole
	// queue points at a file that does not exist -- exactly the kind of thing
	// only this log line finds.
	char guessed[512];
	qobuzcache_path(track->id, qobuz_expected_mime(), guessed, sizeof(guessed));
	if (strcmp(guessed, path) != 0) {
		fprintf(stderr, "qobuz: WARNING real name '%s' but the queue expects '%s'\n", path, guessed);
	}

	qobuzcache_write_sidecars(track->id, mime, track->title, track->artist, track->album, track->album_id,
							  track->track_number, track->cover);
	// Prefetched tracks count too: they are files taking up card space, which is
	// what the periodic cleanup has to keep in check.
	qobuzcache_note_played(path);

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

	const qobuz_track_t *track = &result_tracks[index];
	if (!track->streamable) {
		snprintf(result_error, sizeof(result_error), "%s", tr("stream_not_in_subscription"));
		return;
	}

	// Whatever was downloading for another track is no longer needed: the
	// bandwidth belongs to this one.
	streamturn_abandon_all();

	char url[QOBUZ_URL_MAX];
	char mime[64];
	if (!qobuz_track_url(track->id, url, sizeof(url), mime, sizeof(mime), NULL)) {
		const char *why = qobuz_last_error();
		result_class = streamerr_describe("qobuz", why && why[0] ? why : tr("download_failed"), result_error,
										  sizeof(result_error));
		return;
	}

	char path[512];
	if (!qobuzcache_start(track->id, url, mime, track->duration, path, sizeof(path))) {
		// The cache now reports what went wrong: no card, no network, card
		// full. The generic "download failed" is left only for the case where
		// even it does not know.
		const char *why = qobuzcache_last_error();
		result_class = streamerr_describe("qobuz", why && why[0] ? why : tr("download_failed"), result_error,
										  sizeof(result_error));
		return;
	}

	qobuzcache_note_played(path);

	// Tags BEFORE starting playback: that is when device_state reads the
	// metadata, and a moment later would be too late.
	qobuzcache_write_sidecars(track->id, mime, track->title, track->artist, track->album, track->album_id,
							  track->track_number, NULL);

	// --- the queue: the WHOLE list, not only what has downloaded ------------
	//
	// Every track's path is known BEFORE asking Qobuz for it: the file name is
	// the identifier plus the extension, and the extension follows the chosen
	// quality (qobuz_expected_mime). So the whole queue is built at once and the
	// files land underneath -- the thread prefetches the ones ahead, and if
	// playback reaches a missing one first, device_state asks for it and it is
	// downloaded there and then.
	static char paths[QOBUZ_MAX_HELD][512];
	static const char *path_ptr[QOBUZ_MAX_HELD];
	const char *expected = qobuz_expected_mime();

	pthread_mutex_lock(&queued_lock);
	queued_count = 0;
	int start = 0;
	for (int i = 0; i < result_count && queued_count < QOBUZ_MAX_HELD; i++) {
		if (!result_tracks[i].streamable) {
			continue; // unplayable: it does not go in the queue
		}
		if (i == index) {
			start = queued_count;
			// The tapped one has already downloaded: it is queued under its REAL
			// name, which with an unexpected container may differ from the
			// predicted one.
			snprintf(paths[queued_count], sizeof(paths[0]), "%s", path);
		} else {
			qobuzcache_path(result_tracks[i].id, expected, paths[queued_count], sizeof(paths[0]));
		}
		path_ptr[queued_count] = paths[queued_count];
		queued_tracks[queued_count] = result_tracks[i];
		queued_count++;
	}
	pthread_mutex_unlock(&queued_lock);

	if (queued_count == 0) {
		return;
	}

	// Tags for the WHOLE queue, immediately. They are text files of a few dozen
	// bytes and cost nothing (no covers here: those are downloads, and arrive
	// with the track). Without them the Queue page would show the file name,
	// which is the track identifier, instead of the title for everything not
	// yet downloaded.
	for (int i = 0; i < queued_count; i++) {
		if (i == start) {
			continue; // already written, and under the real name
		}
		const qobuz_track_t *t = &queued_tracks[i];
		qobuzcache_write_sidecars(t->id, expected, t->title, t->artist, t->album, t->album_id, t->track_number, NULL);
	}

	device_state_play_list(path_ptr, queued_count, start);

	// The cover last, with the music already playing: it is another download and
	// not worth delaying the first note for. It deliberately comes before the
	// player is opened, so the image is there when the page appears.
	qobuzcache_write_sidecars(track->id, mime, track->title, track->artist, track->album, track->album_id,
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
//   1. tells the cache which files it must not evict (those still to be
//      played): a fully downloaded track is no longer protected by growfile,
//      and deleting it while queued stops playback halfway through an album;
//   2. tells the thread WHICH track to download now: the first missing one
//      between the current track and the next two. It is the same question
//      whether prefetching ahead or playback has reached a track that is not
//      there yet -- in that case the first missing one is exactly the one being
//      waited for, and gets served first on its own.
static void queue_watch_cb(lv_timer_t *timer) {
	(void)timer;
	// The playing row follows the track, which moves on by itself; only
	// while the list is on screen.
	if (list && shown_count > 0 && lv_screen_active() == list_screen && playing_update()) {
		svclist_rebind(list);
	}
	// A queue that is a library list holds no cache files, and walking it here
	// would be a database read per entry, once a second, for nothing. The
	// bookkeeping below still has to run: something was very likely being
	// waited for when the user started that list, and the flag keeping Wi-Fi
	// awake is only ever lowered here.
	bool library_queue = playlist_is_library_backed();
	if (library_queue) {
		qobuzcache_set_protected(NULL, 0);
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

	static char queued[QOBUZCACHE_PROTECTED_MAX][512];
	static const char *queued_ptr[QOBUZCACHE_PROTECTED_MAX];
	int protect = 0;
	for (int i = current; !library_queue && i >= 0 && i < count && protect < QOBUZCACHE_PROTECTED_MAX; i++) {
		if (playlist_path_at(i, queued[protect], sizeof(queued[0]))) {
			queued_ptr[protect] = queued[protect];
			protect++;
		}
	}
	if (!library_queue) {
		qobuzcache_set_protected(queued_ptr, protect);
	}

	// The first missing one in the window, `current` included: if that one is
	// missing, it is what playback is waiting for.
	//
	// The identifier is read from the PATH the queue gives for that position --
	// the queue is what knows its own order, shuffle included.
	// `queued_tracks[i]` for the same `i` may be an entirely different track.
	long want_id = 0;
	if (!library_queue && !transfer_on && current >= 0 && queued_count > 0) {
		for (int i = current; i <= current + PREFETCH_AHEAD && i < count && want_id == 0; i++) {
			char path[512];
			if (!playlist_path_at(i, path, sizeof(path)) || !qobuzcache_owns(path)) {
				continue;
			}
			long id = qobuzcache_track_id(path);
			if (id != 0 && !qobuzcache_find(id, NULL, 0)) {
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

	// The wait only holds while the queue really points at a Qobuz track. If
	// something else has been started meanwhile the flag would stay raised
	// forever: nothing consumes it, and it keeps Wi-Fi awake.
	if (waiting_for_track) {
		int index = playlist_current_index();
		char wanted[512];
		if (index >= 0 && playlist_path_at(index, wanted, sizeof(wanted)) && !qobuzcache_owns(wanted)) {
			waiting_for_track = false;
		}
	}

	// This is the only place that recomputes the flag keeping Wi-Fi awake, once
	// a second. Those that raise it (prefetch_urgent, qobuz_prepare_track) do so
	// immediately so as not to leave a second-long gap; lowering it is this
	// pass's job.
	qobuzcache_set_network_wanted(want_id != 0 || waiting_for_track);
}

static void *prefetch_main(void *arg) {
	(void)arg;
	thread_be_background("qobuz prefetch");

	for (;;) {
		pthread_mutex_lock(&prefetch_lock);
		while (prefetch_want_id == 0) {
			pthread_cond_wait(&prefetch_ready, &prefetch_lock);
		}
		long id = prefetch_want_id;
		pthread_mutex_unlock(&prefetch_lock);

		// A track that just failed is not retried at once: the watchdog would
		// re-queue it every second and this would knock on Qobuz sixty times a
		// minute to hear the same answer.
		if (id == prefetch_failed_id && lv_tick_get() - prefetch_failed_ms < PREFETCH_RETRY_MS) {
			usleep(300 * 1000);
			continue;
		}

		// From identifier to track: the data (title, duration, cover) is taken
		// from the queued list by SEARCHING, not by trusting a position.
		int index = -1;
		qobuz_track_t track = {0};
		pthread_mutex_lock(&queued_lock);
		for (int i = 0; i < queued_count && index < 0; i++) {
			if (queued_tracks[i].id == id) {
				index = i;
				track = queued_tracks[i];
			}
		}
		pthread_mutex_unlock(&queued_lock);

		if (index < 0) {
			// Not in the queue being played: wait for the watchdog to recompute
			// instead of downloading for the previous queue.
			fprintf(stderr, "qobuz: %ld is not in the list we know, not downloading it\n", id);
			prefetch_failed_id = id;
			prefetch_failed_ms = lv_tick_get();
			pthread_mutex_lock(&prefetch_lock);
			if (prefetch_want_id == id) {
				prefetch_want_id = 0;
			}
			pthread_mutex_unlock(&prefetch_lock);
			continue;
		}

		qobuzcache_wait_idle();

		// While waiting for the slot the request may have changed: prev is
		// pressed and another track is needed now. Without this check the one
		// chosen BEFORE the wait was downloaded, and whoever pressed waited a
		// whole extra track.
		pthread_mutex_lock(&prefetch_lock);
		bool moved_on = prefetch_want_id != id;
		pthread_mutex_unlock(&prefetch_lock);
		if (moved_on) {
			continue;
		}

		char next_path[512];
		bool ok = prepare_track(&track, next_path, sizeof(next_path));
		if (!ok) {
			fprintf(stderr, "qobuz: %ld does not download: %s\n", track.id, qobuzcache_last_error());
			prefetch_failed_id = id;
			prefetch_failed_ms = lv_tick_get();
		} else if (prefetch_failed_id == id) {
			prefetch_failed_id = 0;
		}

		// However it went, this request has been served: the watchdog will
		// recompute on its next pass. Without this, a track that will not
		// download would keep the thread retrying it forever.
		pthread_mutex_lock(&prefetch_lock);
		if (prefetch_want_id == id) {
			prefetch_want_id = 0;
		}
		bool superseded = prefetch_want_id != 0 && prefetch_want_id != id;
		pthread_mutex_unlock(&prefetch_lock);

		// Belt as well as braces: if the request changed while THE DOWNLOAD was
		// already starting (the abandon passed an instant before this start and
		// did not see it), what was just started is a zombie nobody will stop.
		// It is stopped here, this being the only place that knows it exists.
		if (ok && superseded && qobuzcache_downloading_id() == id) {
			fprintf(stderr, "qobuz: %ld had started but is no longer needed, dropping it\n", id);
			// ONLY the Qobuz one, not everyone's: this is not a user request,
			// it is cleanup of a Qobuz download that started and is no longer
			// needed. Dropping everything from here would also throw away the
			// track another service is downloading to play right now.
			qobuzcache_abandon_all();
		}

		if (ok && !superseded) {
			// If playback was waiting for exactly this one, it can resume now.
			gui_post(resume_if_waiting_async, NULL);
		}
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// the worker thread
// ---------------------------------------------------------------------------

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
		job_t job = pending;
		have_pending = false;
		pthread_mutex_unlock(&job_lock);

		result_error[0] = '\0';
		result_class = STREAM_ERR_NONE;
		result_kind = job.kind;
		streamerr_begin();

		// Where this page starts. 0 is a new list; anything higher continues the
		// one already held, and the tail of the array is the only part written.
		int off = job.offset;
		if (off < 0 || off >= QOBUZ_MAX_HELD) {
			off = 0;
		}
		int want = QOBUZ_MAX_HELD - off;
		if (want > QOBUZ_PAGE_LIMIT) {
			want = QOBUZ_PAGE_LIMIT;
		}

		// Playback does NOT clear the list: what has to play is precisely the
		// track held in it, plus the ones after it to prefetch. (Clearing here
		// makes do_play() a no-op: it finds an empty list and returns
		// silently.) Nor does an extra page clear it: the old rows stay on
		// screen and usable while the new ones download.
		if (job_is_list(job.kind) && off == 0) {
			result_count = 0;
			result_title[0] = '\0';
		}
		qobuz_track_t *album_tracks = NULL; // JOB_QUEUE_ALBUM's

		int n = 0;
		switch (job.kind) {
		case JOB_LOGIN:
			if (!qobuz_login(job.text, job.text2)) {
				// A wrong password is best said in Qobuz's words; only a
				// network or service fault takes the common sentence.
				result_class = streamerr_describe("qobuz", qobuz_last_error(), result_error, sizeof(result_error));
				if (!streamerr_is_transport(result_class)) {
					snprintf(result_error, sizeof(result_error), "%s", qobuz_last_error());
				}
			}
			break;

		case JOB_SEARCH_TRACKS:
			n = qobuz_search_tracks(job.text, off, result_tracks + off, want);
			snprintf(result_title, sizeof(result_title), "%.150s", job.text);
			break;
		case JOB_SEARCH_ALBUMS:
			n = qobuz_search_albums(job.text, off, result_albums + off, want);
			snprintf(result_title, sizeof(result_title), "%.150s", job.text);
			break;
		case JOB_SEARCH_ARTISTS:
			n = qobuz_search_artists(job.text, off, result_artists + off, want);
			snprintf(result_title, sizeof(result_title), "%.150s", job.text);
			break;

		case JOB_ALBUM_TRACKS:
			n = qobuz_album_tracks(job.id_text, off, result_tracks + off, want);
			snprintf(result_title, sizeof(result_title), "%.150s", job.text);
			break;
		case JOB_ARTIST_ALBUMS:
			n = qobuz_artist_albums(job.id, off, result_albums + off, want);
			snprintf(result_title, sizeof(result_title), "%.150s", job.text);
			break;
		case JOB_PLAYLIST_TRACKS:
			n = qobuz_playlist_tracks(job.id, off, result_tracks + off, want);
			snprintf(result_title, sizeof(result_title), "%.150s", job.text);
			break;

		case JOB_FAV_TRACKS:
			n = qobuz_favorite_tracks(off, result_tracks + off, want);
			snprintf(result_title, sizeof(result_title), "%s", tr("favourite_tracks"));
			break;
		case JOB_FAV_ALBUMS:
			n = qobuz_favorite_albums(off, result_albums + off, want);
			snprintf(result_title, sizeof(result_title), "%s", tr("favourite_albums"));
			break;
		case JOB_USER_PLAYLISTS:
			n = qobuz_user_playlists(off, result_playlists + off, want);
			snprintf(result_title, sizeof(result_title), "%s", tr("my_playlists"));
			break;
		case JOB_FEATURED:
			n = qobuz_featured_albums("new-releases", off, result_albums + off, want);
			snprintf(result_title, sizeof(result_title), "%s", tr("new_releases"));
			break;

		case JOB_PLAY:
			do_play(job.index);
			break;

		case JOB_QUEUE_ALBUM:
			album_tracks = malloc(QOBUZ_MAX_HELD * sizeof(*album_tracks));
			n = album_tracks ? qobuz_album_tracks(job.id_text, 0, album_tracks, QOBUZ_MAX_HELD) : -1;
			break;

		default:
			break;
		}

		if (n < 0) {
			result_class = streamerr_describe("qobuz", qobuz_last_error(), result_error, sizeof(result_error));
			n = 0;
		}
		if (job_is_list(job.kind)) {
			result_from = off;
			result_count = off + n;
		}

		// The answer, whole, for the GUI thread (see answer_t).
		answer_t *a = calloc(1, sizeof(*a));
		if (!a) {
			free(album_tracks);
			continue; // no memory even for this: the spinner stays, which is the truth
		}
		a->kind = job.kind;
		snprintf(a->error, sizeof(a->error), "%s", result_error);
		a->error_class = result_class;
		snprintf(a->title, sizeof(a->title), "%s", result_title);
		a->from = result_from;
		a->count = job_is_list(job.kind) ? result_count : n;
		a->list_serial = job.list_serial;
		a->where = job.index;
		snprintf(a->what, sizeof(a->what), "%s", job.text);
		a->tracks = album_tracks;
		gui_post(job_done_cb, a);
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
// Every Qobuz list lives on ONE reused page: search results, album tracks,
// artist albums, favourites. The screen manager always sees the same screen, so
// it has nothing to stack: opening an album from the results and then going
// back would land on the search page, skipping the results.
//
// The page keeps its own history: a stack of jobs. The chevron (and the
// gesture, which goes through the same function) consults it first; only when
// it is empty does back really leave the page.
// ---------------------------------------------------------------------------

#define LIST_HISTORY_DEPTH 8
static job_t list_history[LIST_HISTORY_DEPTH];
static int list_history_len;
static job_t current_list_job;
static bool have_current_list;

static bool job_is_list(job_kind_t kind) {
	return kind != JOB_PLAY && kind != JOB_LOGIN && kind != JOB_NONE && kind != JOB_QUEUE_ALBUM;
}

// A list's title, known before it comes: the list page opens under it at once.
static const char *job_title(const job_t *job) {
	switch (job->kind) {
	case JOB_FAV_TRACKS:
		return tr("favourite_tracks");
	case JOB_FAV_ALBUMS:
		return tr("favourite_albums");
	case JOB_USER_PLAYLISTS:
		return tr("my_playlists");
	case JOB_FEATURED:
		return tr("new_releases");
	default:
		return job->text;
	}
}

// The list page as a list is asked for: its title, no rows -- they would read
// the arrays the worker is about to fill -- and the spinner where they will
// be. The serial makes any answer still on its way for another list stale.
static void list_expect(job_t *job) {
	list_serial++;
	job->list_serial = list_serial;
	art_forget_rows();
	streamerror_hide(list_error);
	lv_obj_set_hidden(list_empty, true);
	lv_obj_set_hidden(list_spin, false);
	lv_label_set_text(list_title, job_title(job));
}

// Peek for the back-swipe guard: is there a step to consume inside the page?
// It changes nothing -- it only keeps another screen from being drawn under the
// finger.
static bool list_back_has_step(void) { return list_history_len > 0; }

static bool list_back_guard(void) {
	if (list_history_len == 0) {
		have_current_list = false;
		return false; // nothing underneath: leave the page
	}
	job_t previous = list_history[--list_history_len];
	// Re-submitted without stacking: this IS the step back.
	page_more = false;
	page_loading = false;
	list_expect(&previous);
	have_current_list = true;
	current_list_job = previous;
	start_worker();
	submit(&previous);
	return true;
}

static void run(const job_t *job_in, const char *waiting_text) {
	(void)waiting_text;
	job_t copy = *job_in;
	job_t *job = &copy;
	// A job that opens another list while a list is already open: the previous
	// one must be remembered, otherwise the step back skips it.
	if (job_is_list(job->kind)) {
		// Re-reading the SAME list is not a step forward: the whole job is
		// compared, not just its kind, otherwise two albums in a row would not
		// stack.
		bool same = have_current_list && current_list_job.kind == job->kind && current_list_job.id == job->id &&
					strcmp(current_list_job.id_text, job->id_text) == 0 &&
					strcmp(current_list_job.text, job->text) == 0;
		bool on_list = lv_screen_active() == list_screen;
		if (have_current_list && !same && on_list && list_history_len < LIST_HISTORY_DEPTH) {
			list_history[list_history_len++] = current_list_job;
		} else if (!on_list) {
			list_history_len = 0; // entering from outside: the history restarts
		}
		// A new list is starting: the old one's pagination must not fire while
		// waiting for it.
		page_more = false;
		page_loading = false;
		list_expect(job);
		current_list_job = *job;
		have_current_list = true;
		if (!on_list) {
			switch_screen(list_screen);
		}
	}
	if (job->kind == JOB_LOGIN) {
		// Nothing else to do on the sign-in page meanwhile: the busy toast
		// holds it until Qobuz has answered.
		sx_toast_busy(tr("sign_in"), false);
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

// Another page when scrolling nears the bottom, as for radio. No veil and no
// history entry -- it is the SAME list continuing, not a new one, and the rows
// already there stay usable meanwhile.
static void maybe_load_more(void) {
	if (page_loading || !page_more) {
		return;
	}
	if (lv_screen_active() != list_screen || !have_current_list || !job_is_list(current_list_job.kind)) {
		return;
	}
	// Within one screen of the bottom is close enough (svclist says when):
	// the request takes a moment, and reaching the last row only to find the
	// list stops there is exactly what this replaces.
	page_loading = true;
	job_t job = current_list_job;
	job.offset = shown_count;
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
		job.id = result_playlists[index].id;
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
		const qobuz_track_t *t = &result_tracks[index];
		star_set(t->id, NULL, !qobuzsync_is_favorite(t->id), t->title);
	}
}

// ---------------------------------------------------------------------------
// search
// ---------------------------------------------------------------------------

// The handoff's text field: 60 px, the surface of a card, its radius.
static void field_style(lv_obj_t *field, gui_config_t *cfg, int y) {
	lv_obj_set_size(field, cfg->screen_width - 2 * SX_BODY_PAD_L, 60);
	lv_obj_set_scrollbar_mode(field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(field, LV_ALIGN_TOP_LEFT, SX_BODY_PAD_L, y);
	lv_obj_add_style(field, &sx_style_surface, 0);
	lv_obj_add_style(field, &sx_style_text, 0);
	lv_obj_set_style_radius(field, SX_CARD_R, 0);
	lv_obj_set_style_border_width(field, 0, 0);
	lv_obj_set_style_shadow_width(field, 0, 0);
	lv_obj_set_style_pad_ver(field, 14, 0);
	lv_obj_set_style_pad_hor(field, SX_CARD_PAD_H, 0);
}

static lv_obj_t *search_field;
static lv_obj_t *search_clear_btn; // the x that empties the field
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

// The x shows only when there is something to clear.
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

	settingsrow_title(search_screen, cfg, "qobuz_search_qobuz");
	int top = settingsrow_content_top(cfg);

	search_field = lv_textarea_create(search_screen);
	lv_textarea_set_one_line(search_field, true);
	lv_textarea_set_placeholder_text(search_field, tr("artist_album_or_track"));
	field_style(search_field, cfg, top);
	lv_obj_set_style_text_font(search_field, &font_ui_24, 0);
	keyboard_style_caret(search_field);
	lv_obj_add_event_cb(search_field, search_field_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

	// The x on the right edge of the field, as in the local search: clears the
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
// sign in
// ---------------------------------------------------------------------------

static lv_obj_t *user_field;
static lv_obj_t *password_field;
static lv_obj_t *password_eye_icon;
static keyboard_t *login_keyboard;

static void password_eye_refresh(void) {
	bool hidden = lv_textarea_get_password_mode(password_field);
	lv_image_set_src(password_eye_icon, hidden ? &icon_eye : &icon_eye_off);
	// The tight kerning applies only while the dots are shown.
	keyboard_refresh_password(password_field);
}

static void password_eye_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_password_mode(password_field, !lv_textarea_get_password_mode(password_field));
	password_eye_refresh();
}

// The keyboard writes into one of the two fields: the last one touched.
static void focus_field(lv_obj_t *field) {
	lv_obj_t *other = field == user_field ? password_field : user_field;
	keyboard_set_field(login_keyboard, field);
	lv_obj_add_state(field, LV_STATE_FOCUSED);
	lv_obj_remove_state(other, LV_STATE_FOCUSED);
	// And the caret: on here, off on the other. The focused state alone is not
	// enough -- LVGL blinks every textarea's caret from creation, and two
	// carets were visible at once.
	keyboard_show_caret(field, true);
	keyboard_show_caret(other, false);
}

static void field_focus_cb(lv_event_t *e) { focus_field(lv_event_get_target(e)); }

static void login_accept_cb(lv_event_t *e) {
	(void)e;
	const char *user = lv_textarea_get_text(user_field);
	const char *password = lv_textarea_get_text(password_field);
	if (!user || !*user || !password || !*password) {
		gui_notify_popup("credentials_required");
		return;
	}

	job_t job = {0};
	job.kind = JOB_LOGIN;
	snprintf(job.text, sizeof(job.text), "%s", user);
	snprintf(job.text2, sizeof(job.text2), "%s", password);
	run(&job, "qobuz_sign_in_to_qobuz");

	// The password does not stay in the field a second longer than needed.
	lv_textarea_set_text(password_field, "");
}

static lv_obj_t *make_field(lv_obj_t *parent, gui_config_t *cfg, const char *placeholder, int y) {
	lv_obj_t *field = lv_textarea_create(parent);
	lv_textarea_set_one_line(field, true);
	lv_textarea_set_placeholder_text(field, tr(placeholder));
	field_style(field, cfg, y);
	lv_obj_set_style_text_font(field, &font_ui_24, 0);
	keyboard_style_caret(field);
	lv_obj_add_event_cb(field, field_focus_cb, LV_EVENT_CLICKED, NULL);
	return field;
}

static void build_login_page(gui_config_t *cfg) {
	login_screen = lv_obj_create(NULL);
	lv_obj_add_style(login_screen, &theme_style_screen, 0);

	settingsrow_title(login_screen, cfg, "qobuz_sign_in_to_qobuz");
	int top = settingsrow_content_top(cfg);

	user_field = make_field(login_screen, cfg, "qobuz_username_or_email", top);
	password_field = make_field(login_screen, cfg, "password", top + 74);

	// The password as on any modern system: dots from the first keystroke, and
	// the eye to reveal the whole of it. Zero show time means the character is
	// never readable, not even for an instant.
	keyboard_style_password(password_field, 0);
	lv_obj_set_style_pad_right(password_field, 60, 0);

	lv_obj_t *eye = lv_btn_create(login_screen);
	lv_obj_set_size(eye, 56, 56);
	lv_obj_align(eye, LV_ALIGN_TOP_RIGHT, -SX_BODY_PAD_L - 2, top + 74 + 2);
	lv_obj_set_style_bg_opa(eye, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(eye, 0, 0);
	lv_obj_set_style_border_width(eye, 0, 0);
	lv_obj_add_event_cb(eye, password_eye_cb, LV_EVENT_CLICKED, NULL);

	password_eye_icon = lv_image_create(eye);
	lv_obj_add_style(password_eye_icon, &theme_style_icon, 0);
	lv_obj_center(password_eye_icon);
	password_eye_refresh();

	lv_obj_t *note = lv_label_create(login_screen);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, cfg->screen_width - 2 * cfg->padding);
	lv_obj_align(note, LV_ALIGN_TOP_LEFT, cfg->padding, top + 150);
	lv_obj_add_style(note, &sx_style_text2, 0);
	lv_obj_set_style_text_font(note, &font_ui_20, 0);
	lv_label_set_text(note, tr("qobuz_login_note"));

	login_keyboard = keyboard_create(login_screen, cfg->screen_width, 316, user_field, NULL, "ok", login_accept_cb,
									 NULL);
	switcher_attach_back_gesture(login_screen);
}

static void open_login_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(user_field, "");
	lv_textarea_set_text(password_field, "");
	lv_textarea_set_password_mode(password_field, true);
	password_eye_refresh();
	keyboard_reset(login_keyboard);
	focus_field(user_field);
	switch_screen(login_screen);
}

// ---------------------------------------------------------------------------
// the main page
//
// As the other services' pages in 2.0: the sections as cards with chevrons,
// the quality as pills on the page itself (it was a page of its own behind a
// gear), and the account at the bottom with its Sign out. The search stays in
// the header.
// ---------------------------------------------------------------------------

static lv_obj_t *home_container;
static lv_obj_t *search_btn;

static const int QUALITY_VALUES[4] = {QOBUZ_FORMAT_MP3, QOBUZ_FORMAT_CD, QOBUZ_FORMAT_HIRES96,
									  QOBUZ_FORMAT_HIRES192};

// Full names rather than shorthand: "24/192" alone does not tell anyone that
// a FLAC is behind it. Nearly symbols, but not quite: the decimal separator is
// locale-dependent, so these go through tr() like everything else.
static const char *const QUALITY_NAMES[4] = {"qobuz_mp3_320_kbps", "qobuz_flac_16_44_1", "qobuz_flac_24_96",
											 "qobuz_flac_24_192"};
static lv_obj_t *quality_pills[4];

// The magnifier appears only once signed in: before that there is nothing to
// search.
static void update_corner_buttons(void) {
	if (search_btn) {
		lv_obj_set_hidden(search_btn, !(qobuz_configured() && qobuz_logged_in()));
	}
}

static void quality_refresh(void) {
	int current = qobuz_get_format();
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
	qobuz_set_format((int)(intptr_t)lv_event_get_user_data(e));
	quality_refresh();
}

static void do_logout(void *user) {
	(void)user;
	qobuz_logout();
	// The remembered favourites and playlists belonged to that account: under
	// another user they would be lies.
	qobuzsync_forget();
	// And the tracks downloaded for that account are of no use to anyone, nor
	// the covers of its lists.
	qobuzcache_clear();
	qobuzart_clear();
	rebuild_home();
	toast_success("signed_out");
}

static void logout_cb(lv_event_t *e) {
	(void)e;
	// Only the question. The downloaded tracks do go away, but they are
	// transient files the user never put there and does not know they have:
	// mentioning it here would make them weigh a decision that is not theirs.
	char message[160];
	snprintf(message, sizeof(message), tr("sign_out_confirm_note"), qobuz_display_name());
	confirm_show("qobuz_sign_out_of_qobuz", message, "leave", do_logout, NULL);
}

static void simple_job_cb(lv_event_t *e) {
	job_t job = {0};
	job.kind = (job_kind_t)(intptr_t)lv_event_get_user_data(e);
	run(&job, "loading");
}

// The "Wi-Fi settings" card of the no-network notice, as on the transfer page:
// it leads to where the problem is fixed.
static void open_wifi_settings_cb(lv_event_t *e) {
	(void)e;
	switch_screen(wifisettings_screen);
}

// Whether the network is actually up. Qobuz has nothing to show without it:
// every list comes from the server, and with no IP address each request ends in
// a generic error that does not tell the user the one thing they need to know.
static bool wifi_is_connected(void) {
#ifdef HOST_BUILD
	// The simulator runs on a machine that has a network but no wlan0: without
	// this the page would not be testable.
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

	if (!qobuz_configured()) {
		svcui_note(home_container, tr("api_keys_unavailable"));
		return;
	}

	// The network comes before the login, and not the other way round: signing
	// in is a conversation with Qobuz's servers, so with no network the "Sign
	// in" card could only fail. The notice says what is actually missing.
	if (!wifi_is_connected()) {
		svcui_note(home_container, tr("enable_wi_fi_first"));
		svcui_nav_card(home_container, tr("wi_fi_settings"), NULL, open_wifi_settings_cb, NULL);
		return;
	}

	if (!qobuz_logged_in()) {
		svcui_nav_card(home_container, tr("sign_in"), NULL, open_login_cb, NULL);
		return;
	}

	// No "Search" card: it is the magnifier in the corner, where it sits on
	// every other searchable page.
	svcui_nav_card(home_container, tr("favourite_tracks"), NULL, simple_job_cb, (void *)(intptr_t)JOB_FAV_TRACKS);
	svcui_nav_card(home_container, tr("favourite_albums"), NULL, simple_job_cb, (void *)(intptr_t)JOB_FAV_ALBUMS);
	svcui_nav_card(home_container, tr("my_playlists"), NULL, simple_job_cb, (void *)(intptr_t)JOB_USER_PLAYLISTS);
	svcui_nav_card(home_container, tr("new_releases"), NULL, simple_job_cb, (void *)(intptr_t)JOB_FEATURED);

	lv_obj_t *quality = sx_card_create(home_container);
	sx_card_title(quality, tr("audio_quality"));
	lv_obj_t *pills = sx_card_pills(quality);
	for (int i = 0; i < 4; i++) {
		quality_pills[i] = sx_pill(pills, tr(QUALITY_NAMES[i]), QUALITY_VALUES[i], quality_pick_cb);
	}
	quality_refresh();

	// The account: who is signed in, and the way out.
	lv_obj_t *account = sx_card_create(home_container);
	sx_card_title(account, qobuz_display_name());
	sx_card_subtitle(account, tr("qobuz"), false);
	sx_card_action(account, tr("leave"), logout_cb, NULL);
}

// "Show album" in the player, for a Qobuz track: opens the album's track list
// as if reached from a search. The job is the usual JOB_ALBUM_TRACKS.
bool qobuzpage_open_album(const char *album_id, const char *title) {
	if (!album_id || !album_id[0] || !qobuz_logged_in()) {
		return false;
	}
	job_t job = {0};
	job.kind = JOB_ALBUM_TRACKS;
	snprintf(job.id_text, sizeof(job.id_text), "%s", album_id);
	snprintf(job.text, sizeof(job.text), "%s", title && title[0] ? title : tr("qobuz"));
	run(&job, "loading");
	return true;
}

static void home_loaded_cb(lv_event_t *e) {
	(void)e;
	// The card may have been inserted or removed since the page was built, and
	// without a card there is nowhere to download to.
	qobuzcache_set_root(storage_sd_root());
	qobuzart_set_root(storage_sd_root());

	// The page is rebuilt on every open, not once at startup: everything it
	// depends on -- the network, the sign-in, the app keys -- can change while
	// the user is elsewhere, so it is all re-read here.
	rebuild_home();
}

// ---------------------------------------------------------------------------

static void build_list_page(gui_config_t *cfg) {
	list_screen = lv_obj_create(NULL);
	lv_obj_add_style(list_screen, &theme_style_screen, 0);
	lv_obj_add_event_cb(list_screen, list_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(list_screen, list_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	list_title = settingsrow_title(list_screen, cfg, "qobuz");
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
	streamerror_set_service(list_error, tr("qobuz"));
	list_spin = svcui_spinner(list_screen);

	switcher_attach_back_gesture(list_screen);
	switcher_set_back_guard(list_screen, list_back_guard);
	switcher_set_back_guard_peek(list_screen, list_back_has_step);
	// The leftward drag that pulls in the player sheet, as on every other
	// browsable page.
	player_sheet_attach_drag(list_screen, true);
}

// "Show album" from the player, as the provider registry asks for it: the
// screen back leads to, or NULL when the album could not be opened.
static void *provider_open_album(const char *album_id, const char *title) {
	return qobuzpage_open_album(album_id, title) ? qobuz_list_screen : NULL;
}

void qobuzpage_init(gui_config_t *cfg) {
	// What only the GUI can tell the rest of the program about Qobuz: its mark
	// over the artwork and how to open one of its albums (provider.h).
	const stream_provider_gui_t provider_gui = {.badge = &icon_qobuz_badge, .open_album = provider_open_album};
	stream_provider_attach_gui("qobuz", &provider_gui);

	qobuz_screen = lv_obj_create(NULL);
	lv_obj_add_style(qobuz_screen, &theme_style_screen, 0);
	lv_obj_t *title = settingsrow_title(qobuz_screen, cfg, "qobuz");
	home_container = svcui_column(qobuz_screen, cfg);

	// The magnifier top right. The title must be told the corner is taken,
	// otherwise its text runs under it.
	settingsrow_title_corner_slots(title, cfg, 1);
	search_btn = svcui_header_button(qobuz_screen, cfg, 0, &icon_search, open_search_cb, NULL);

	build_list_page(cfg);
	build_search_page(cfg);
	build_login_page(cfg);
	qobuzcache_set_slow_start_cb(on_slow_start);
	device_state_add_prepare_cb(qobuz_prepare_track);

	// The queue watchdog: tells the prefetch thread which track is missing next.
	// One second is enough -- this is about tracks, not frames.
	lv_timer_create(queue_watch_cb, 1000, NULL);
	// Created paused: the first cover request starts it.
	art_timer = lv_timer_create(art_poll_cb, 120, NULL);
	lv_timer_pause(art_timer);
	art_drop_timer = lv_timer_create(art_drop_cb, ART_DROP_DELAY_MS, NULL);
	lv_timer_pause(art_drop_timer);
	qobuzart_set_root(storage_sd_root());
	for (int i = 0; i < QOBUZART_SLOTS; i++) {
		slot_owner[i] = -1;
	}

	qobuzcache_set_root(storage_sd_root());
	rebuild_home();

	lv_obj_add_event_cb(qobuz_screen, home_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(qobuz_screen);
	player_sheet_attach_drag(qobuz_screen, true);
	player_sheet_attach_drag(home_container, true);
}

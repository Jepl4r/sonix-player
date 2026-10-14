#include "subsonicpage.h"

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
#include "src/system/net/http.h"
#include "src/system/streaming/provider.h"
#include "src/system/streaming/streamerr.h"
#include "src/system/streaming/streamturn.h"
#include "src/system/streaming/subsonic.h"
#include "src/system/streaming/subsoniccache.h"
#include "src/system/streaming/subsonicsync.h"
#include "src/system/device/system.h"
#include "src/system/core/config.h"
#include "src/system/core/utils.h"
#include "src/system/net/wifi.h"
#include "src/system/net/wifitransfer.h"

// ---------------------------------------------------------------------------
// Subsonic front end: the user's own server (Navidrome, Gonic, ...).
//
// Built on the Qobuz page's skeleton (qobuzpage.c), whose comments explain the
// machinery -- the single-slot worker, the busy veil, the reused list page with
// its own back history, the thumbnails, playback that starts while the track is
// still downloading and the prefetch thread that fetches the next ones. What is
// different here is what is browsed (artists, album lists, playlists, starred
// items, all from subsonic.h) and the identity of a track: a string id, whose
// cache file is named after a key (see subsoniccache.h).
// ---------------------------------------------------------------------------

#define SUB_PAGE_LIMIT 50
// Maximum entries held: four pages, as for radio stations. Reaching the bottom
// asks for another fifty; past this cap the list really stops, because every
// entry is static memory on a device with 64 MB in total.
#define SUB_MAX_HELD (4 * SUB_PAGE_LIMIT)


static lv_obj_t *login_screen;
lv_obj_t *subsonic_list_screen;
#define list_screen subsonic_list_screen
static lv_obj_t *search_screen;

// Global rather than hidden behind a function: the Streaming grid wants the
// address of the variable (grid_entry_t.target), as for every other page.
lv_obj_t *subsonic_screen;

// ---------------------------------------------------------------------------
// the worker
// ---------------------------------------------------------------------------

typedef enum {
	JOB_NONE = 0,
	JOB_LOGIN,
	JOB_SEARCH_TRACKS,
	JOB_SEARCH_ALBUMS,
	JOB_SEARCH_ARTISTS,
	JOB_ARTISTS,
	JOB_ALBUM_TRACKS,
	JOB_ARTIST_ALBUMS,
	JOB_PLAYLIST_TRACKS,
	JOB_STARRED_TRACKS,
	JOB_STARRED_ALBUMS,
	JOB_PLAYLISTS,
	JOB_ALBUM_LIST, // getAlbumList2, the type in `text2`
	JOB_RANDOM_TRACKS, // getRandomSongs: "Random 50", one draw, no paging
	JOB_PLAY,
	// Not lists: they leave the list on screen alone.
	JOB_STATUS,		 // the server card's line: a timed ping and the size of the library
	JOB_QUEUE_ALBUM, // an album's tracks for the queue (`index`: where)
} job_kind_t;

typedef struct {
	job_kind_t kind;
	char text[256];					// search terms, user name, or the list's title
	char text2[128];				// password, or the album list type ("newest")
	char server[SUBSONIC_SERVER_MAX]; // sign-in only
	char id_text[SUBSONIC_ID_MAX];	// album, artist or playlist id
	int index;						// which track of the list playback starts from
	int offset;						// first entry of this page: 0 = new list
	unsigned list_serial;			// list jobs: list_serial when asked
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
static subsonic_track_t result_tracks[SUB_MAX_HELD];
static subsonic_album_t result_albums[SUB_MAX_HELD];
static subsonic_artist_t result_artists[SUB_MAX_HELD];
static subsonic_playlist_t result_playlists[SUB_MAX_HELD];
static int result_count;
static int result_from; // where the last page received starts (0 = new list)
static job_kind_t result_kind;
static char result_error[256];
static stream_err_t result_class; // what kind of failure result_error is
static char result_title[160];

// What the list is showing right now, so a row tap knows what to do.
static job_kind_t list_kind;

// Raised by each new list asked for (GUI thread): an answer carrying an older
// number belongs to a list the user has since left, and is dropped.
static unsigned list_serial;
// The server card's figures (worker only; they reach the GUI in the answer).
static bool status_ok;
static int status_tracks, status_ms;

// Pagination, as for radio: the last page came back full, so there may be
// another -- and one request at a time.
static bool page_more;
static bool page_loading;

// When the server card's line was last asked for (GUI thread; see status_ask).
static uint32_t status_asked_ms;

static void submit(const job_t *job) {
	pthread_mutex_lock(&job_lock);
	// The server card's question, pushed out by a tap before the worker took
	// it: asked again at the next visit rather than a minute later.
	if (have_pending && pending.kind == JOB_STATUS && job->kind != JOB_STATUS) {
		status_asked_ms = 0;
	}
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
// leaves nothing else to do on its page, holds the busy toast until the server
// has answered. Playback waits for nothing: the track starts while it is
// still downloading (see subsoniccache_start).
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// the result list
//
// 2.0 rows over the list (svclist.h): a pool of rows rides the scroll, so two
// hundred entries cost the rows of a screen and a half, not two hundred
// widgets of seven objects. A track: its cover (a plaque in the album's tone
// until it comes), the artist, the format when it is not the common case, its
// length, the star. An album: the cover, "artist · year", the chevron. An
// artist: a round plaque with its letter. A playlist: its cover.
//
// The gestures are the handoff's matrix: a tap plays a track (the page stays,
// the mini player comes up) or opens the rest; a swipe right puts a track or a
// whole album next, left at the end of the queue; the star stars a track at
// once, with Undo; a hold brings up the sheet of a track or an album.
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
		   kind == JOB_STARRED_TRACKS || kind == JOB_RANDOM_TRACKS;
}

static bool kind_is_albums(job_kind_t kind) {
	return kind == JOB_SEARCH_ALBUMS || kind == JOB_STARRED_ALBUMS || kind == JOB_ARTIST_ALBUMS ||
		   kind == JOB_ALBUM_LIST;
}

static bool kind_is_artists(job_kind_t kind) { return kind == JOB_SEARCH_ARTISTS || kind == JOB_ARTISTS; }

static void run(const job_t *job, const char *waiting_text);

// ---------------------------------------------------------------------------
// favourites: the star, and the sheet's line
//
// Changed at once and offered back in the toast (the handoff's principle 6):
// nothing asks first any more -- a star taken off by mistake is one tap on
// Undo away. The mirror of starred songs flips before the server answers
// (subsonicsync.h); a refusal is said, and the rows follow the mirror again.
// ---------------------------------------------------------------------------

static void list_refresh_async(void *user) {
	(void)user;
	svclist_rebind(list);
}

static void refused_async(void *user) {
	char *why = user;
	toast_error(why && why[0] ? why : tr("subsonic_change_refused"));
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
static char star_id[SUBSONIC_ID_MAX];
static bool star_album;
static bool star_on;

static void star_apply(bool on) {
	star_on = on;
	if (star_album) {
		subsonicsync_album_favorite(star_id, on, sync_done, NULL);
	} else {
		subsonicsync_favorite_toggle(star_id, on, sync_done, NULL);
	}
	svclist_rebind(list);
}

static void star_undo(void *user) {
	(void)user;
	star_apply(!star_on);
}

static void star_set(const char *id, bool album, bool on, const char *what) {
	snprintf(star_id, sizeof(star_id), "%s", id);
	star_album = album;
	star_apply(on);
	svcui_star_toast(on, "svc_fav_added", "svc_fav_removed", what, star_undo, NULL);
}

// ---------------------------------------------------------------------------
// "Play next" and "Add to end of queue"
//
// A Subsonic track goes into the queue by its cache path -- the file when it
// is on the card, the name it will have otherwise -- and becomes known to the
// prefetch, which fetches it when playback nears it as it does for a list
// played from here (subsonic_prepare_track()). An album's tracks are asked for
// first.
// ---------------------------------------------------------------------------

typedef struct {
	subsonic_track_t track;
	char key[SUBSONICCACHE_KEY_LEN];
} queued_t;

static queued_t *queued_tracks; // SUB_MAX_HELD entries, allocated on first use
static int queued_count;
static pthread_mutex_t queued_lock = PTHREAD_MUTEX_INITIALIZER;

static bool queued_ensure(void) {
	if (!queued_tracks) {
		queued_tracks = calloc(SUB_MAX_HELD, sizeof(*queued_tracks));
	}
	return queued_tracks != NULL;
}

// Makes `t` known to the prefetch (the oldest known track gives way when the
// list is full), and writes its tags beside the cache name, so the queue
// shows its title. Its path into `path`.
static void queued_add(const subsonic_track_t *t, char *path, size_t size) {
	char key[SUBSONICCACHE_KEY_LEN];
	subsonic_track_key(t->id, key, sizeof(key));
	const char *suffix = subsonic_expected_suffix(t);
	if (!subsoniccache_find(key, path, size)) {
		subsoniccache_path(key, suffix, path, size);
	}
	subsoniccache_write_sidecars(key, suffix, t->id, t->title, t->artist, t->album, t->album_id, t->track_number,
								 NULL);
	pthread_mutex_lock(&queued_lock);
	bool known = false;
	for (int i = 0; i < queued_count && !known; i++) {
		known = strcmp(queued_tracks[i].key, key) == 0;
	}
	if (!known) {
		if (queued_count == SUB_MAX_HELD) {
			memmove(queued_tracks, queued_tracks + 1, (SUB_MAX_HELD - 1) * sizeof(*queued_tracks));
			queued_count--;
		}
		queued_tracks[queued_count].track = *t;
		snprintf(queued_tracks[queued_count].key, sizeof(queued_tracks[0].key), "%s", key);
		queued_count++;
	}
	pthread_mutex_unlock(&queued_lock);
}

static void queue_tracks(const subsonic_track_t *tracks, int count, playlist_queue_where_t where, const char *what) {
	if (count <= 0 || !queued_ensure()) {
		gui_notify_popup("playlist_cannot_add_the_tracks");
		return;
	}
	char *block = malloc((size_t)count * 512);
	const char **paths = malloc((size_t)count * sizeof(*paths));
	if (!block || !paths) {
		free(block);
		free(paths);
		gui_notify_popup("playlist_cannot_add_the_tracks");
		return;
	}
	for (int i = 0; i < count; i++) {
		queued_add(&tracks[i], block + (size_t)i * 512, 512);
		paths[i] = block + (size_t)i * 512;
	}
	svcui_queue(paths, count, where, what);
	free(paths);
	free(block);
}

// An album's tracks for the queue are fetched on the worker into a buffer of
// their own, handed over with the answer (the list on screen stays as it is).
static void queue_album(const subsonic_album_t *a, playlist_queue_where_t where) {
	job_t job = {0};
	job.kind = JOB_QUEUE_ALBUM;
	job.index = (int)where;
	snprintf(job.id_text, sizeof(job.id_text), "%s", a->id);
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
		star_set(result_tracks[menu_index].id, false, on, result_tracks[menu_index].title);
	} else {
		star_set(result_albums[menu_index].id, true, on, result_albums[menu_index].title);
	}
}

static void sheet_album(void *user) {
	(void)user;
	if (!menu_valid() || !kind_is_tracks(list_kind)) {
		return;
	}
	const subsonic_track_t *t = &result_tracks[menu_index];
	job_t job = {0};
	job.kind = JOB_ALBUM_TRACKS;
	snprintf(job.id_text, sizeof(job.id_text), "%s", t->album_id);
	snprintf(job.text, sizeof(job.text), "%s", t->album[0] ? t->album : tr("subsonic"));
	run(&job, "loading");
}

static void row_detail(int index, char *out, size_t size);

static void list_hold(lv_obj_t *row, void *user) {
	(void)row;
	int index = (int)(intptr_t)user;
	if (index < 0 || index >= shown_count || (!kind_is_tracks(list_kind) && !kind_is_albums(list_kind))) {
		return;
	}
	menu_index = index;
	menu_kind = list_kind;
	sx_sheet_item_t items[5];
	memset(items, 0, sizeof(items));
	int n = 0;
	items[n++] = (sx_sheet_item_t){.label = tr("queue_play_next"), .icon = &icon_list_start, .action = sheet_queue,
								   .user = (void *)(intptr_t)PLAYLIST_QUEUE_NEXT};
	items[n++] = (sx_sheet_item_t){.label = tr("queue_add_to_end"), .icon = &icon_list_plus_small,
								   .action = sheet_queue, .user = (void *)(intptr_t)PLAYLIST_QUEUE_END};
	static char title[SUBSONIC_TITLE_MAX];
	if (kind_is_tracks(list_kind)) {
		const subsonic_track_t *t = &result_tracks[index];
		bool fav = subsonicsync_is_favorite(t->id);
		items[n++] = (sx_sheet_item_t){.label = tr(fav ? "remove_from_favourites" : "medialist_add_to_favourites"),
									   .icon = fav ? &icon_star_filled : &icon_star,
									   .action = sheet_star,
									   .user = fav ? NULL : (void *)1};
		if (t->album_id[0] && list_kind != JOB_ALBUM_TRACKS) {
			items[n++] = (sx_sheet_item_t){.label = tr("show_album"), .icon = &icon_chevron_right, .action = sheet_album};
		}
		snprintf(title, sizeof(title), "%s", t->title);
	} else {
		// The server says whether an album is starred only in the list of
		// starred ones: there it is offered for removal, elsewhere for adding.
		bool fav = list_kind == JOB_STARRED_ALBUMS;
		items[n++] = (sx_sheet_item_t){.label = tr(fav ? "remove_from_favourites" : "medialist_add_to_favourites"),
									   .icon = fav ? &icon_star_filled : &icon_star,
									   .action = sheet_star,
									   .user = fav ? NULL : (void *)1};
		snprintf(title, sizeof(title), "%s", result_albums[index].title);
	}
	static char subtitle[400];
	row_detail(index, subtitle, sizeof(subtitle));
	sx_sheet_menu(title, subtitle, items, n);
}

// Side of a row's cover as fetched: the lead's own, so nothing is scaled.
static int thumb_px(void) { return sx_row_height(false) > SX_ROW_H ? SX_ROW_THUMB_BIG : SX_ROW_THUMB; }

// What getCoverArt is asked for for the player. The server scales it, so the
// row's sixty pixels do not mean fetching the original two-megabyte scan.
#define PLAYER_COVER_PX 600

// Row cover art.
//
// The loader has twelve slots and a list can hold far more entries than that,
// so slots are a shared pool and not assigned seats: they go to the entries in
// view and move to new ones as the list scrolls. The pictures are kept by
// entry, so a row scrolled away and back finds its cover still there.
static cover_image_t row_images[SUB_MAX_HELD];
static bool row_has_image[SUB_MAX_HELD];

// This entry already asked for its cover and did not get one: dead URL, image
// that fails to decode, host that does not answer. Without this one broken
// entry would be asked for forever and keep a slot from those that can be
// served.
static bool row_art_failed[SUB_MAX_HELD];

// The token that claims the cover slots: only its address matters, never its
// value. See qobuzart.h.
static const char subsonic_art_owner;

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
	qobuzart_release(&subsonic_art_owner, slot);
	slot_owner[slot] = -1;
}

// The cover id of entry `index` of the list on screen, "" for none.
static const char *entry_cover_id(int index) {
	if (index < 0 || index >= shown_count) {
		return "";
	}
	if (kind_is_tracks(list_kind)) {
		return result_tracks[index].cover_id;
	}
	if (kind_is_albums(list_kind)) {
		return result_albums[index].cover_id;
	}
	if (list_kind == JOB_PLAYLISTS) {
		return result_playlists[index].cover_id;
	}
	return ""; // an artist: its letter
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
		if (entry < 0 || entry >= SUB_MAX_HELD) {
			continue;
		}
		cover_image_t image;
		bool finished = false;
		if (!qobuzart_take(&subsonic_art_owner, slot, &image, &finished)) {
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
	char url[SUBSONIC_URL_MAX];
	for (int entry = from; entry <= to && entry < SUB_MAX_HELD; entry++) {
		const char *cover = entry_cover_id(entry);
		if (!cover[0] || row_has_image[entry] || row_art_failed[entry]) {
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
		subsonic_cover_url(cover, thumb_px(), url, sizeof(url));
		slot_owner[free_slot] = entry;
		art_wake();
		qobuzart_request(&subsonic_art_owner, free_slot, url, thumb_px());
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
	for (int entry = 0; entry < SUB_MAX_HELD; entry++) {
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
	for (int entry = 0; entry < SUB_MAX_HELD; entry++) {
		row_has_image[entry] = false;
	}
	// The rows let go of the pictures before they are freed.
	svclist_rebind(list);
	for (int entry = 0; entry < SUB_MAX_HELD; entry++) {
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
		const subsonic_track_t *t = &result_tracks[index];
		snprintf(out, size, "%s", t->artist);
	} else if (kind_is_albums(list_kind)) {
		const subsonic_album_t *a = &result_albums[index];
		if (a->year > 0) {
			snprintf(out, size, "%s \xC2\xB7 %d", a->artist, a->year);
		} else {
			snprintf(out, size, "%s", a->artist);
		}
	} else if (kind_is_artists(list_kind)) {
		const subsonic_artist_t *a = &result_artists[index];
		if (a->album_count > 0) {
			snprintf(out, size, tr(a->album_count == 1 ? "subsonic_one_album" : "subsonic_albums_n"), a->album_count);
		}
	} else if (list_kind == JOB_PLAYLISTS) {
		const subsonic_playlist_t *p = &result_playlists[index];
		snprintf(out, size, "%s \xC2\xB7 %d", p->owner, p->song_count);
	}
}

// The first character of `text` (a whole UTF-8 one), for a letter plaque.
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

// The track playing now, as a cache key, for the playing row.
static char playing_key[SUBSONICCACHE_KEY_LEN];

static bool playing_update(void) {
	char path[512], key[SUBSONICCACHE_KEY_LEN] = "";
	if (playlist_current_path(path, sizeof(path)) && subsoniccache_owns(path)) {
		subsoniccache_path_key(path, key, sizeof(key));
	}
	if (strcmp(key, playing_key) == 0) {
		return false;
	}
	snprintf(playing_key, sizeof(playing_key), "%s", key);
	return true;
}

static void list_bind(lv_obj_t *row, int index) {
	if (index < 0 || index >= shown_count) {
		return;
	}
	char detail[400];
	row_detail(index, detail, sizeof(detail));
	const lv_image_dsc_t *image = index < SUB_MAX_HELD && row_has_image[index] ? &row_images[index].dsc : NULL;
	sx_row_set_playing(row, false);
	if (kind_is_tracks(list_kind)) {
		const subsonic_track_t *t = &result_tracks[index];
		char length[16], format[16] = "";
		duration_text(t->duration, length, sizeof(length));
		// The format is worth a word when it is not the common case: a FLAC on
		// a list of FLACs says nothing, an Opus among them does.
		if (t->suffix[0] && strcmp(t->suffix, "flac") != 0 && strcmp(t->suffix, "mp3") != 0) {
			snprintf(format, sizeof(format), "\xC2\xB7 %s", t->suffix);
		}
		sx_row_set_title(row, t->title);
		sx_row_set_subtitle(row, detail, format[0] ? format : NULL, length);
		if (list_kind == JOB_ALBUM_TRACKS && t->track_number > 0) {
			sx_row_set_number(row, t->track_number);
		} else {
			sx_row_set_thumb(row, image, svcui_tone(t->album_id[0] ? t->album_id : t->title));
		}
		sx_row_set_trail(row, SX_ROW_TRAIL_STAR);
		sx_row_set_starred(row, subsonicsync_is_favorite(t->id));
		sx_row_set_swipes(row, true, true);
		if (playing_key[0]) {
			char key[SUBSONICCACHE_KEY_LEN];
			subsonic_track_key(t->id, key, sizeof(key));
			sx_row_set_playing(row, strcmp(key, playing_key) == 0);
		}
	} else if (kind_is_albums(list_kind)) {
		const subsonic_album_t *a = &result_albums[index];
		sx_row_set_title(row, a->title);
		sx_row_set_subtitle(row, detail, NULL, NULL);
		sx_row_set_thumb(row, image, svcui_tone(a->id));
		sx_row_set_trail(row, SX_ROW_TRAIL_CHEVRON);
		sx_row_set_swipes(row, true, true);
	} else if (kind_is_artists(list_kind)) {
		const subsonic_artist_t *a = &result_artists[index];
		char letter[8];
		first_char(a->name, letter, sizeof(letter));
		sx_row_set_title(row, a->name);
		sx_row_set_subtitle(row, detail, NULL, NULL);
		sx_row_set_letter(row, letter, svcui_tone(a->name), true);
		sx_row_set_trail(row, SX_ROW_TRAIL_CHEVRON);
		sx_row_set_swipes(row, false, false);
	} else if (list_kind == JOB_PLAYLISTS) {
		const subsonic_playlist_t *p = &result_playlists[index];
		sx_row_set_title(row, p->name);
		sx_row_set_subtitle(row, detail, NULL, NULL);
		sx_row_set_thumb(row, image, svcui_tone(p->id));
		sx_row_set_trail(row, SX_ROW_TRAIL_CHEVRON);
		sx_row_set_swipes(row, false, false);
	}
}

static void list_visible_cb(int first, int last) { art_request_range(first, last); }

// `from` is the first entry new: 0 a new list, a higher value a page added at
// the bottom -- the rows already there and the scroll stay.
static void favourites_known(void) { gui_post(list_refresh_async, NULL); }

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
	// The stars read the mirror of starred songs: asked for when not known yet,
	// the rows told when it comes.
	if (kind_is_tracks(list_kind)) {
		subsonicsync_favorites_refresh(favourites_known);
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
// GUI read the shared result_* fields -- which the next job was already
// overwriting by then (the server card's ping right after a list, say). The
// lists' entries stay in the shared arrays: the rows are emptied before a new
// list is asked for (run()), and a further page only writes past the rows
// shown.
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
	subsonic_track_t *tracks;
	// JOB_STATUS
	bool status_ok;
	int status_tracks, status_ms;
} answer_t;

static void status_shown(const answer_t *a);

static void job_done_cb(void *user) {
	answer_t *a = user;
	if (a->kind == JOB_LOGIN) {
		sx_toast_busy_end();
	}
	if (a->kind == JOB_STATUS) {
		status_shown(a);
		free(a);
		return;
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
			// not ask again by itself -- against a server that is down that
			// would be a request per scroll event.
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
		// The favourites mirror belongs to the account just signed in to.
		subsonicsync_forget();
		rebuild_home();
		switch_screen(subsonic_screen);
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
		// the list really ended, and nothing more is requested. A random draw
		// is one draw.
		page_loading = false;
		page_more = a->kind != JOB_RANDOM_TRACKS && (a->count - a->from == SUB_PAGE_LIMIT) && a->count < SUB_MAX_HELD;
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
	if (subsoniccache_owns(path) && !track_is_playable(path)) {
		// A different track arrived, not this one. Logged: when the resume does
		// not fire, this is the place to look.
		fprintf(stderr, "subsonic: something arrived but the queue is waiting for '%s', which is not ready\n", path);
		return;
	}
	waiting_for_track = false;
	fprintf(stderr, "subsonic: restarting the queue from slot %d ('%s')\n", index, path);
	device_state_play_queue_index(index);
	player_refresh_now_playing();
}

// True when the file can already be played: fully downloaded, or growing right
// now. When it is false the caller stops playback, which then resumes on its
// own once the track has landed (see resume_if_waiting_async).
static bool track_is_playable(const char *path) {
	char key[SUBSONICCACHE_KEY_LEN];
	if (!subsoniccache_path_key(path, key, sizeof(key))) {
		return false;
	}
	if (subsoniccache_find(key, NULL, 0)) {
		return true;
	}
	// Or downloading right now: the decoder reads a growing file. Asked both
	// ways, as on the Qobuz page, and a disagreement is logged.
	bool growing = growfile_is_growing(path);
	bool downloading = subsoniccache_is_downloading(key);
	if (growing != downloading) {
		fprintf(stderr, "subsonic: '%s' -- growfile says %d, the cache says %d: THEY DISAGREE\n", path, growing,
				downloading);
	}
	return growing || downloading;
}

// Must go through gui_post: see the Qobuz page -- device_state calls the
// prepare hook from whichever thread started the track.
static void downloading_note_async(void *user) {
	(void)user;
	gui_notify_popup("downloading");
}

static void prefetch_urgent(const char *wanted_path);

static bool subsonic_prepare_track(const char *path) {
	if (!subsoniccache_owns(path)) {
		waiting_for_track = false;
		return true; // not a Subsonic file
	}
	if (track_is_playable(path)) {
		waiting_for_track = false;
		return true;
	}
	fprintf(stderr, "subsonic: '%s' not ready: stopping and requesting it\n", path);
	// Stop first, then fetch: the Qobuz page says why at length.
	subsoniccache_set_network_wanted(true);
	waiting_for_track = true;
	device_state_stop();
	prefetch_urgent(path);
	gui_post(downloading_note_async, NULL);
	return false;
}

// A track's cover arrives with the track, possibly an instant after the player
// looked for one: tell it.
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
// prefetching, on a thread of its own (see the Qobuz page)
//
// The identity throughout is the cache key, which the queue's paths carry in
// their file names: no sidecar has to be read to know which track a queue
// position is.
// ---------------------------------------------------------------------------

static pthread_mutex_t prefetch_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t prefetch_ready = PTHREAD_COND_INITIALIZER;

// Which queued track must be downloaded now, by key. Empty = nothing to do.
static char prefetch_want[SUBSONICCACHE_KEY_LEN];
static bool prefetch_started;

static char prefetch_failed[SUBSONICCACHE_KEY_LEN];
static uint32_t prefetch_failed_ms;
#define PREFETCH_RETRY_MS 4000

static void *prefetch_main(void *arg);

static void prefetch_submit(void) {
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

// The tracks the prefetch knows (queued_tracks, with the lists' code above):
// the list played, and what was queued from the lists since.

static void prefetch_want_set(const char *key) {
	pthread_mutex_lock(&prefetch_lock);
	if (strcmp(prefetch_want, key) != 0) {
		snprintf(prefetch_want, sizeof(prefetch_want), "%s", key);
		pthread_cond_broadcast(&prefetch_ready);
	}
	pthread_mutex_unlock(&prefetch_lock);
}

static void prefetch_urgent(const char *wanted_path) {
	char wanted[SUBSONICCACHE_KEY_LEN];
	if (!subsoniccache_path_key(wanted_path, wanted, sizeof(wanted))) {
		return;
	}
	if (!subsoniccache_is_downloading(wanted)) {
		streamturn_abandon_all();
	} else {
		fprintf(stderr, "subsonic: %s is already downloading, not dropping it\n", wanted);
	}
	subsoniccache_set_network_wanted(true);
	prefetch_want_set(wanted);
	prefetch_submit();
}

// Downloads one track of the list and writes its sidecars (tags and cover), so
// it is ready by the time its turn comes.
static bool prepare_track(const queued_t *q, char *path, size_t size) {
	char url[SUBSONIC_URL_MAX];
	subsonic_stream_url(q->track.id, url, sizeof(url));

	pthread_mutex_lock(&prefetch_lock);
	bool superseded = prefetch_want[0] && strcmp(prefetch_want, q->key) != 0;
	pthread_mutex_unlock(&prefetch_lock);
	if (superseded) {
		fprintf(stderr, "subsonic: %s no longer needed, not even starting it\n", q->key);
		return false;
	}

	const char *suffix = subsonic_expected_suffix(&q->track);
	if (!subsoniccache_start(q->key, url, suffix, q->track.duration, path, size)) {
		return false;
	}
	char cover[SUBSONIC_URL_MAX];
	subsonic_cover_url(q->track.cover_id, PLAYER_COVER_PX, cover, sizeof(cover));
	subsoniccache_write_sidecars(q->key, suffix, q->track.id, q->track.title, q->track.artist, q->track.album,
								 q->track.album_id, q->track.track_number, cover);
	subsoniccache_note_played(path);

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
	if (!queued_ensure()) {
		snprintf(result_error, sizeof(result_error), "%s", tr("out_of_memory"));
		return;
	}

	// Whatever was downloading for another track is no longer needed.
	streamturn_abandon_all();

	const subsonic_track_t *track = &result_tracks[index];
	char key[SUBSONICCACHE_KEY_LEN];
	subsonic_track_key(track->id, key, sizeof(key));
	const char *suffix = subsonic_expected_suffix(track);
	char url[SUBSONIC_URL_MAX];
	subsonic_stream_url(track->id, url, sizeof(url));

	char path[512];
	if (!subsoniccache_start(key, url, suffix, track->duration, path, sizeof(path))) {
		const char *why = subsoniccache_last_error();
		result_class = streamerr_describe("subsonic", why && why[0] ? why : tr("download_failed"), result_error,
										  sizeof(result_error));
		return;
	}
	subsoniccache_note_played(path);

	// The queue: the whole list, every path known before asking (the key and
	// the expected suffix name it), the files landing underneath.
	static char paths[SUB_MAX_HELD][512];
	static const char *path_ptr[SUB_MAX_HELD];

	pthread_mutex_lock(&queued_lock);
	queued_count = 0;
	int start = 0;
	for (int i = 0; i < result_count && queued_count < SUB_MAX_HELD; i++) {
		queued_t *q = &queued_tracks[queued_count];
		q->track = result_tracks[i];
		subsonic_track_key(q->track.id, q->key, sizeof(q->key));
		const char *s = subsonic_expected_suffix(&q->track);
		if (i == index) {
			start = queued_count;
			snprintf(paths[queued_count], sizeof(paths[0]), "%s", path);
		} else {
			// Already on the card from an earlier listen, perhaps under another
			// suffix (the quality changed since): that file plays, no download.
			if (!subsoniccache_find(q->key, paths[queued_count], sizeof(paths[0]))) {
				subsoniccache_path(q->key, s, paths[queued_count], sizeof(paths[0]));
			}
		}
		// Tags for the whole queue at once, a few dozen bytes each: the Queue
		// page shows titles instead of hashes. No covers: those are downloads.
		subsoniccache_write_sidecars(q->key, s, q->track.id, q->track.title, q->track.artist, q->track.album,
									 q->track.album_id, q->track.track_number, NULL);
		path_ptr[queued_count] = paths[queued_count];
		queued_count++;
	}
	pthread_mutex_unlock(&queued_lock);
	if (queued_count == 0) {
		return;
	}

	device_state_play_list(path_ptr, queued_count, start);

	// The cover last, with the music already playing.
	char cover[SUBSONIC_URL_MAX];
	subsonic_cover_url(track->cover_id, PLAYER_COVER_PX, cover, sizeof(cover));
	subsoniccache_write_sidecars(key, suffix, track->id, track->title, track->artist, track->album, track->album_id,
								 track->track_number, cover);

	gui_post(play_started_async, NULL);
	prefetch_submit();
}

// ---------------------------------------------------------------------------
// scrobbling
//
// Read off the queue watchdog's once-a-second pass rather than hooked into the
// audio path: "now playing" when a Subsonic track becomes current, the play
// count once half of it (or four minutes) has been heard -- the rule
// Last.fm and every Subsonic client follow. Nothing runs when what plays is not
// from this cache.
// ---------------------------------------------------------------------------

static char scrobble_path[512];
static bool scrobble_submitted;

static void scrobble_tick(void) {
	device_state_t state;
	device_state_get(&state);
	if (state.live || !state.current_file[0] || !subsoniccache_owns(state.current_file)) {
		scrobble_path[0] = '\0';
		return;
	}
	char id[SUBSONIC_ID_MAX];
	if (strcmp(scrobble_path, state.current_file) != 0) {
		if (!track_is_playable(state.current_file) || !subsoniccache_song_id(state.current_file, id, sizeof(id))) {
			return;
		}
		snprintf(scrobble_path, sizeof(scrobble_path), "%s", state.current_file);
		scrobble_submitted = false;
		subsonicsync_scrobble(id, false);
		return;
	}
	if (scrobble_submitted || state.progress_total_secs <= 0) {
		return;
	}
	double heard = state.progress_current_secs;
	if (heard >= state.progress_total_secs / 2 || heard >= 240) {
		if (subsoniccache_song_id(state.current_file, id, sizeof(id))) {
			subsonicsync_scrobble(id, true);
		}
		scrobble_submitted = true;
	}
}

// ---------------------------------------------------------------------------

// The queue watchdog, once a second on the GUI thread: protects the queued
// files from the prune and tells the prefetch thread which one to fetch next.
// See the Qobuz page for the reasoning; only the identity differs.
static void queue_watch_cb(lv_timer_t *timer) {
	(void)timer;
	// The playing row follows the track, which moves on by itself; only
	// while the list is on screen.
	if (list && shown_count > 0 && lv_screen_active() == list_screen && playing_update()) {
		svclist_rebind(list);
	}
	// Never signed in and nothing of ours ever queued: nothing to watch, and
	// this tick costs one comparison.
	if (queued_count == 0 && !waiting_for_track && !subsonic_logged_in()) {
		return;
	}
	scrobble_tick();

	bool library_queue = playlist_is_library_backed();
	if (library_queue) {
		subsoniccache_set_protected(NULL, 0);
		waiting_for_track = false;
	}
	bool transfer_on = wifitransfer_get_enabled();
	if (transfer_on) {
		waiting_for_track = false;
	}

	int count = playlist_count();
	int current = playlist_current_index();

	static char queued[SUBSONICCACHE_PROTECTED_MAX][512];
	static const char *queued_ptr[SUBSONICCACHE_PROTECTED_MAX];
	int protect = 0;
	for (int i = current; !library_queue && i >= 0 && i < count && protect < SUBSONICCACHE_PROTECTED_MAX; i++) {
		if (playlist_path_at(i, queued[protect], sizeof(queued[0])) && subsoniccache_owns(queued[protect])) {
			queued_ptr[protect] = queued[protect];
			protect++;
		}
	}
	if (!library_queue) {
		subsoniccache_set_protected(queued_ptr, protect);
	}

	char want[SUBSONICCACHE_KEY_LEN] = "";
	if (!library_queue && !transfer_on && current >= 0 && queued_count > 0) {
		for (int i = current; i <= current + PREFETCH_AHEAD && i < count && !want[0]; i++) {
			char path[512];
			char key[SUBSONICCACHE_KEY_LEN];
			if (!playlist_path_at(i, path, sizeof(path)) || !subsoniccache_path_key(path, key, sizeof(key))) {
				continue;
			}
			if (!subsoniccache_find(key, NULL, 0)) {
				snprintf(want, sizeof(want), "%s", key);
			}
		}
	}
	prefetch_want_set(want);

	if (waiting_for_track) {
		int index = playlist_current_index();
		char wanted[512];
		if (index >= 0 && playlist_path_at(index, wanted, sizeof(wanted)) && !subsoniccache_owns(wanted)) {
			waiting_for_track = false;
		}
	}
	subsoniccache_set_network_wanted(want[0] != '\0' || waiting_for_track);
}

static void *prefetch_main(void *arg) {
	(void)arg;
	thread_be_background("subsonic prefetch");

	for (;;) {
		pthread_mutex_lock(&prefetch_lock);
		while (!prefetch_want[0]) {
			pthread_cond_wait(&prefetch_ready, &prefetch_lock);
		}
		char key[SUBSONICCACHE_KEY_LEN];
		snprintf(key, sizeof(key), "%s", prefetch_want);
		pthread_mutex_unlock(&prefetch_lock);

		if (strcmp(key, prefetch_failed) == 0 && lv_tick_get() - prefetch_failed_ms < PREFETCH_RETRY_MS) {
			usleep(300 * 1000);
			continue;
		}

		int index = -1;
		queued_t q = {0};
		pthread_mutex_lock(&queued_lock);
		for (int i = 0; i < queued_count && index < 0; i++) {
			if (strcmp(queued_tracks[i].key, key) == 0) {
				index = i;
				q = queued_tracks[i];
			}
		}
		pthread_mutex_unlock(&queued_lock);
		if (index < 0) {
			fprintf(stderr, "subsonic: %s is not in the list we know, not downloading it\n", key);
			snprintf(prefetch_failed, sizeof(prefetch_failed), "%s", key);
			prefetch_failed_ms = lv_tick_get();
			pthread_mutex_lock(&prefetch_lock);
			if (strcmp(prefetch_want, key) == 0) {
				prefetch_want[0] = '\0';
			}
			pthread_mutex_unlock(&prefetch_lock);
			continue;
		}

		subsoniccache_wait_idle();

		pthread_mutex_lock(&prefetch_lock);
		bool moved_on = strcmp(prefetch_want, key) != 0;
		pthread_mutex_unlock(&prefetch_lock);
		if (moved_on) {
			continue;
		}

		char next_path[512];
		bool ok = prepare_track(&q, next_path, sizeof(next_path));
		if (!ok) {
			fprintf(stderr, "subsonic: %s does not download: %s\n", key, subsoniccache_last_error());
			snprintf(prefetch_failed, sizeof(prefetch_failed), "%s", key);
			prefetch_failed_ms = lv_tick_get();
		} else if (strcmp(prefetch_failed, key) == 0) {
			prefetch_failed[0] = '\0';
		}

		pthread_mutex_lock(&prefetch_lock);
		if (strcmp(prefetch_want, key) == 0) {
			prefetch_want[0] = '\0';
		}
		bool superseded = prefetch_want[0] && strcmp(prefetch_want, key) != 0;
		pthread_mutex_unlock(&prefetch_lock);

		if (ok && superseded && subsoniccache_is_downloading(key)) {
			fprintf(stderr, "subsonic: %s had started but is no longer needed, dropping it\n", key);
			subsoniccache_abandon_all();
		}
		if (ok && !superseded) {
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
	// The page waits on these requests behind its spinner: a server that does
	// not answer is said in seconds, not after twenty (streamerr.h).
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
		if (off < 0 || off >= SUB_MAX_HELD) {
			off = 0;
		}
		int want = SUB_MAX_HELD - off;
		if (want > SUB_PAGE_LIMIT) {
			want = SUB_PAGE_LIMIT;
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
		subsonic_track_t *album_tracks = NULL; // JOB_QUEUE_ALBUM's

		int n = 0;
		switch (job.kind) {
		case JOB_LOGIN:
			if (!subsonic_login(job.server, job.text, job.text2)) {
				// A wrong password is best said in the server's words; only a
				// network or server fault takes the common sentence.
				result_class = streamerr_describe("subsonic", subsonic_last_error(), result_error, sizeof(result_error));
				if (!streamerr_is_transport(result_class)) {
					snprintf(result_error, sizeof(result_error), "%s", subsonic_last_error());
				}
			}
			// The password does not outlive the attempt, even in this copy.
			memset(job.text2, 0, sizeof(job.text2));
			break;

		case JOB_SEARCH_TRACKS:
			n = subsonic_search_tracks(job.text, off, result_tracks + off, want);
			snprintf(result_title, sizeof(result_title), "%.150s", job.text);
			break;
		case JOB_SEARCH_ALBUMS:
			n = subsonic_search_albums(job.text, off, result_albums + off, want);
			snprintf(result_title, sizeof(result_title), "%.150s", job.text);
			break;
		case JOB_SEARCH_ARTISTS:
			n = subsonic_search_artists(job.text, off, result_artists + off, want);
			snprintf(result_title, sizeof(result_title), "%.150s", job.text);
			break;

		case JOB_ARTISTS:
			n = subsonic_artists(off, result_artists + off, want);
			snprintf(result_title, sizeof(result_title), "%s", tr("artists"));
			break;
		case JOB_ALBUM_TRACKS:
			n = subsonic_album_tracks(job.id_text, off, result_tracks + off, want);
			snprintf(result_title, sizeof(result_title), "%.150s", job.text);
			break;
		case JOB_ARTIST_ALBUMS:
			n = subsonic_artist_albums(job.id_text, off, result_albums + off, want);
			snprintf(result_title, sizeof(result_title), "%.150s", job.text);
			break;
		case JOB_PLAYLIST_TRACKS:
			n = subsonic_playlist_tracks(job.id_text, off, result_tracks + off, want);
			snprintf(result_title, sizeof(result_title), "%.150s", job.text);
			break;
		case JOB_ALBUM_LIST:
			n = subsonic_album_list(job.text2, off, result_albums + off, want);
			snprintf(result_title, sizeof(result_title), "%.150s", job.text);
			break;

		case JOB_STARRED_TRACKS:
			n = subsonic_starred_tracks(off, result_tracks + off, want);
			snprintf(result_title, sizeof(result_title), "%s", tr("favourite_tracks"));
			break;
		case JOB_STARRED_ALBUMS:
			n = subsonic_starred_albums(off, result_albums + off, want);
			snprintf(result_title, sizeof(result_title), "%s", tr("favourite_albums"));
			break;
		case JOB_PLAYLISTS:
			n = subsonic_playlists(off, result_playlists + off, want);
			snprintf(result_title, sizeof(result_title), "%s", tr("playlists"));
			break;

		case JOB_RANDOM_TRACKS:
			n = subsonic_random_tracks(result_tracks, want);
			snprintf(result_title, sizeof(result_title), "%s", tr("subsonic_random_50"));
			break;

		case JOB_PLAY:
			do_play(job.index);
			break;

		case JOB_STATUS:
			status_ok = subsonic_status(&status_tracks, &status_ms);
			break;

		case JOB_QUEUE_ALBUM:
			album_tracks = malloc(SUB_MAX_HELD * sizeof(*album_tracks));
			n = album_tracks ? subsonic_album_tracks(job.id_text, 0, album_tracks, SUB_MAX_HELD) : -1;
			break;

		default:
			break;
		}

		if (n < 0) {
			result_class = streamerr_describe("subsonic", subsonic_last_error(), result_error, sizeof(result_error));
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
		a->status_ok = status_ok;
		a->status_tracks = status_tracks;
		a->status_ms = status_ms;
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
// Every Subsonic list lives on ONE reused page: search results, album tracks,
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
	return kind != JOB_PLAY && kind != JOB_LOGIN && kind != JOB_NONE && kind != JOB_STATUS && kind != JOB_QUEUE_ALBUM;
}

// A list's title, known before it comes: the list page opens under it at once.
static const char *job_title(const job_t *job) {
	switch (job->kind) {
	case JOB_ARTISTS:
		return tr("artists");
	case JOB_STARRED_TRACKS:
		return tr("favourite_tracks");
	case JOB_STARRED_ALBUMS:
		return tr("favourite_albums");
	case JOB_PLAYLISTS:
		return tr("playlists");
	case JOB_RANDOM_TRACKS:
		return tr("subsonic_random_50");
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
		bool same = have_current_list && current_list_job.kind == job->kind &&
					strcmp(current_list_job.id_text, job->id_text) == 0 &&
					strcmp(current_list_job.text, job->text) == 0 && strcmp(current_list_job.text2, job->text2) == 0;
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
		// holds it until the server has answered.
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

// Or, when the server refused the credentials, the sign-in page.
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
		job.kind = JOB_PLAY;
		job.index = index;
		run(&job, NULL);
		return;
	}
	if (kind_is_albums(list_kind)) {
		job.kind = JOB_ALBUM_TRACKS;
		snprintf(job.id_text, sizeof(job.id_text), "%s", result_albums[index].id);
		snprintf(job.text, sizeof(job.text), "%s", result_albums[index].title);
		run(&job, "loading");
		return;
	}
	if (kind_is_artists(list_kind)) {
		job.kind = JOB_ARTIST_ALBUMS;
		snprintf(job.id_text, sizeof(job.id_text), "%s", result_artists[index].id);
		snprintf(job.text, sizeof(job.text), "%s", result_artists[index].name);
		run(&job, "loading");
		return;
	}
	if (list_kind == JOB_PLAYLISTS) {
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
		const subsonic_track_t *t = &result_tracks[index];
		star_set(t->id, false, !subsonicsync_is_favorite(t->id), t->title);
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

	settingsrow_title(search_screen, cfg, "subsonic_search");
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

static lv_obj_t *server_field;
static lv_obj_t *user_field;
static lv_obj_t *password_field;
static lv_obj_t *password_eye_icon;
static keyboard_t *login_keyboard;

static void password_eye_refresh(void) {
	bool hidden = lv_textarea_get_password_mode(password_field);
	lv_image_set_src(password_eye_icon, hidden ? &icon_eye : &icon_eye_off);
	keyboard_refresh_password(password_field);
}

static void password_eye_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_password_mode(password_field, !lv_textarea_get_password_mode(password_field));
	password_eye_refresh();
}

// The keyboard writes into one of the three fields: the last one touched. Only
// that one has a caret (LVGL blinks every textarea's caret otherwise).
static void focus_field(lv_obj_t *field) {
	lv_obj_t *const fields[] = {server_field, user_field, password_field};
	keyboard_set_field(login_keyboard, field);
	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
		if (fields[i] == field) {
			lv_obj_add_state(fields[i], LV_STATE_FOCUSED);
			keyboard_show_caret(fields[i], true);
		} else {
			lv_obj_remove_state(fields[i], LV_STATE_FOCUSED);
			keyboard_show_caret(fields[i], false);
		}
	}
}

static void field_focus_cb(lv_event_t *e) { focus_field(lv_event_get_target(e)); }

// "OK" walks down the form -- server, user, password -- and only the last one
// signs in: three fields and one keyboard, and no reaching for the next field
// over the keys.
static void login_accept_cb(lv_event_t *e) {
	(void)e;
	const char *server = lv_textarea_get_text(server_field);
	const char *user = lv_textarea_get_text(user_field);
	const char *password = lv_textarea_get_text(password_field);
	if (lv_obj_has_state(server_field, LV_STATE_FOCUSED) && server && *server && (!user || !*user)) {
		focus_field(user_field);
		return;
	}
	if (lv_obj_has_state(user_field, LV_STATE_FOCUSED) && user && *user && (!password || !*password)) {
		focus_field(password_field);
		return;
	}
	if (!server || !*server || !user || !*user || !password || !*password) {
		gui_notify_popup("credentials_required");
		return;
	}

	job_t job = {0};
	job.kind = JOB_LOGIN;
	snprintf(job.server, sizeof(job.server), "%s", server);
	snprintf(job.text, sizeof(job.text), "%s", user);
	snprintf(job.text2, sizeof(job.text2), "%s", password);
	run(&job, "sign_in");
	memset(job.text2, 0, sizeof(job.text2));

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

	settingsrow_title(login_screen, cfg, "subsonic_sign_in");
	int top = settingsrow_content_top(cfg);

	server_field = make_field(login_screen, cfg, "subsonic_server_address", top);
	user_field = make_field(login_screen, cfg, "subsonic_user_name", top + 74);
	password_field = make_field(login_screen, cfg, "password", top + 148);

	keyboard_style_password(password_field, 0);
	lv_obj_set_style_pad_right(password_field, 60, 0);

	lv_obj_t *eye = lv_btn_create(login_screen);
	lv_obj_set_size(eye, 56, 56);
	lv_obj_align(eye, LV_ALIGN_TOP_RIGHT, -SX_BODY_PAD_L - 2, top + 148 + 2);
	lv_obj_set_style_bg_opa(eye, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(eye, 0, 0);
	lv_obj_set_style_border_width(eye, 0, 0);
	lv_obj_add_event_cb(eye, password_eye_cb, LV_EVENT_CLICKED, NULL);

	password_eye_icon = lv_image_create(eye);
	lv_obj_add_style(password_eye_icon, &theme_style_icon, 0);
	lv_obj_center(password_eye_icon);
	password_eye_refresh();

	// What the address looks like, below the fields rather than in the
	// placeholder, where it was cut off.
	lv_obj_t *note = lv_label_create(login_screen);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, cfg->screen_width - 2 * cfg->padding);
	lv_obj_align(note, LV_ALIGN_TOP_LEFT, cfg->padding, top + 222);
	lv_obj_add_style(note, &sx_style_text2, 0);
	lv_obj_set_style_text_font(note, &font_ui_20, 0);
	lv_label_set_text(note, tr("subsonic_login_example"));

	login_keyboard = keyboard_create(login_screen, cfg->screen_width, 316, server_field, NULL, "ok", login_accept_cb,
									 NULL);
	switcher_attach_back_gesture(login_screen);
}

static void open_login_cb(lv_event_t *e) {
	(void)e;
	// The server is kept across a sign-out: signing back in is mostly typing
	// the password again.
	char server[SUBSONIC_SERVER_MAX];
	subsonic_server(server, sizeof(server));
	if (!server[0]) {
		snprintf(server, sizeof(server), "%s", config_get("subsonic", "server", ""));
	}
	lv_textarea_set_text(server_field, server);
	lv_textarea_set_text(user_field, config_get("subsonic", "user", ""));
	lv_textarea_set_text(password_field, "");
	lv_textarea_set_password_mode(password_field, true);
	password_eye_refresh();
	keyboard_reset(login_keyboard);
	focus_field(server[0] ? (lv_textarea_get_text(user_field)[0] ? password_field : user_field) : server_field);
	switch_screen(login_screen);
}

// ---------------------------------------------------------------------------
// the main page
//
// The handoff's page: the server as the first card -- its address, and in the
// accent's mono line whether it answers, how many tracks it holds and how
// fast it answered ("Connected · 2 840 tracks · 38 ms") -- a tap on it opens
// the account's page; the sections as cards with chevrons; the stream's
// quality as pills. The search stays in the header.
// ---------------------------------------------------------------------------

static lv_obj_t *home_container;
static lv_obj_t *search_btn;
static lv_obj_t *server_card;
static gui_config_t *home_cfg;

// The quality pills: the original file, or a transcode for a slow network.
static const char *const QUALITY_NAMES[SUBSONIC_QUALITY_COUNT] = {"subsonic_original", "qobuz_mp3_320_kbps",
																	"subsonic_mp3_192", "subsonic_opus_128"};
static lv_obj_t *quality_pills[SUBSONIC_QUALITY_COUNT];

// The magnifier appears only once signed in.
static void update_corner_buttons(void) {
	if (search_btn) {
		lv_obj_set_hidden(search_btn, !subsonic_logged_in());
	}
}

// ---------------------------------------------------------------------------
// the server card's line
//
// Asked when the page is shown, at most once a minute: a ping and the scan
// status, on the worker, a fraction of a second against a server at home.
// Nothing runs while the page is away.
// ---------------------------------------------------------------------------

#define STATUS_EVERY_MS 60000
static bool status_known;
static bool status_last_ok;
static int status_last_tracks, status_last_ms;

// 2840 -> "2 840", the thin gaps of the prototype (a no-break space here).
static void group_digits(int n, char *out, size_t size) {
	char digits[16];
	snprintf(digits, sizeof(digits), "%d", n);
	size_t len = strlen(digits), used = 0;
	for (size_t i = 0; i < len && used + 4 < size; i++) {
		if (i > 0 && (len - i) % 3 == 0) {
			out[used++] = '\xC2';
			out[used++] = '\xA0';
		}
		out[used++] = digits[i];
	}
	out[used] = '\0';
}

static void server_card_paint(void) {
	if (!server_card) {
		return;
	}
	char server[SUBSONIC_SERVER_MAX];
	subsonic_server(server, sizeof(server));
	// The host, as the user knows it: the scheme and the slash said nothing.
	const char *host = strstr(server, "://");
	host = host ? host + 3 : server;
	char shown[SUBSONIC_SERVER_MAX];
	snprintf(shown, sizeof(shown), "%s", host);
	size_t len = strlen(shown);
	if (len > 0 && shown[len - 1] == '/') {
		shown[len - 1] = '\0';
	}
	sx_card_title(server_card, shown);
	char line[160];
	if (!status_known) {
		snprintf(line, sizeof(line), "%s", tr("subsonic_status_checking"));
	} else if (!status_last_ok) {
		snprintf(line, sizeof(line), "%s", tr("subsonic_status_offline"));
	} else if (status_last_tracks >= 0) {
		char count[24];
		group_digits(status_last_tracks, count, sizeof(count));
		lv_snprintf(line, sizeof(line), tr("subsonic_status_line"), count, status_last_ms);
	} else {
		lv_snprintf(line, sizeof(line), tr("subsonic_status_ms"), status_last_ms);
	}
	sx_card_subtitle(server_card, line, true);
}

static void status_shown(const answer_t *a) {
	status_known = true;
	status_last_ok = a->status_ok;
	status_last_tracks = a->status_tracks;
	status_last_ms = a->status_ms;
	server_card_paint();
}

static void status_ask(void) {
	if (!subsonic_logged_in()) {
		return;
	}
	uint32_t now = lv_tick_get();
	if (status_asked_ms && now - status_asked_ms < STATUS_EVERY_MS) {
		return;
	}
	status_asked_ms = now;
	job_t job = {0};
	job.kind = JOB_STATUS;
	run(&job, NULL);
}

// ---------------------------------------------------------------------------
// the account's page: who and where, the cache, signing out
// ---------------------------------------------------------------------------

static lv_obj_t *ssettings_screen;
static lv_obj_t *account_card;

static void quality_refresh(void) {
	int current = subsonic_get_quality();
	for (int i = 0; i < SUBSONIC_QUALITY_COUNT; i++) {
		if (quality_pills[i]) {
			sx_pill_set_active(quality_pills[i], i == current);
		}
	}
}

static void quality_pick_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	subsonic_set_quality((int)(intptr_t)lv_event_get_user_data(e));
	quality_refresh();
}

static void do_logout(void *user) {
	(void)user;
	subsonic_logout();
	subsonicsync_forget();
	status_known = false;
	status_asked_ms = 0;
	// The cached tracks stay: they are the user's own music and may be wanted
	// offline, and a different account on the same server shares them. They
	// can be emptied from this page.
	qobuzart_clear();
	rebuild_home();
	switch_screen(subsonic_screen);
	toast_success("signed_out");
}

static void logout_cb(lv_event_t *e) {
	(void)e;
	char name[SUBSONIC_SERVER_MAX + SUBSONIC_NAME_MAX];
	subsonic_display_name(name, sizeof(name));
	char message[sizeof(name) + 96];
	snprintf(message, sizeof(message), tr("sign_out_confirm_note"), name);
	confirm_show("subsonic_sign_out", message, "leave", do_logout, NULL);
}

static void do_clear_cache(void *user) {
	(void)user;
	subsoniccache_clear();
	toast_success("subsonic_cache_cleared");
}

// Clearing the cache cannot be undone: the one change 2.0 still confirms, in
// the sheet with the action in red.
static void clear_cache_cb(lv_event_t *e) {
	(void)e;
	char message[96];
	snprintf(message, sizeof(message), tr("subsonic_cache_size"), (int)(subsoniccache_bytes() / (1024 * 1024)));
	confirm_show("subsonic_clear_cache", message, "remove", do_clear_cache, NULL);
}

static void open_settings_cb(lv_event_t *e) {
	(void)e;
	char name[SUBSONIC_SERVER_MAX + SUBSONIC_NAME_MAX];
	subsonic_display_name(name, sizeof(name));
	sx_card_title(account_card, name);
	switch_screen(ssettings_screen);
}

static void build_settings_page(gui_config_t *cfg) {
	ssettings_screen = lv_obj_create(NULL);
	lv_obj_add_style(ssettings_screen, &theme_style_screen, 0);
	settingsrow_title(ssettings_screen, cfg, "subsonic_settings");
	lv_obj_t *column = svcui_column(ssettings_screen, cfg);

	// Who and where, so a user with two servers knows which one this is; and
	// the way out.
	account_card = sx_card_create(column);
	sx_card_title(account_card, "");
	sx_card_subtitle(account_card, tr("subsonic"), false);
	sx_card_action(account_card, tr("leave"), logout_cb, NULL);

	svcui_nav_card(column, tr("subsonic_clear_cache"), NULL, clear_cache_cb, NULL);
	switcher_attach_back_gesture(ssettings_screen);
}

// ---------------------------------------------------------------------------
// the sections
// ---------------------------------------------------------------------------

static void simple_job_cb(lv_event_t *e) {
	job_t job = {0};
	job.kind = (job_kind_t)(intptr_t)lv_event_get_user_data(e);
	run(&job, "loading");
}

// The album lists getAlbumList2 serves, one card each: the type the server
// wants and the card's (translated) name, which is also the list's title.
typedef struct {
	const char *type;
	const char *label;
} album_list_t;

// The first is the handoff's "Albums": all of them, A to Z. The others are
// the server's own selections, under their heading.
static const album_list_t ALBUM_LISTS[] = {
	{"alphabeticalByName", "albums"},
	{"newest", "subsonic_recently_added"},
	{"recent", "subsonic_recently_played"},
	{"frequent", "subsonic_most_played"},
	{"random", "subsonic_random_albums"},
};

static void album_list_cb(lv_event_t *e) {
	const album_list_t *list = lv_event_get_user_data(e);
	job_t job = {0};
	job.kind = JOB_ALBUM_LIST;
	snprintf(job.text2, sizeof(job.text2), "%s", list->type);
	snprintf(job.text, sizeof(job.text), "%s", tr(list->label));
	run(&job, "loading");
}

static void open_wifi_settings_cb(lv_event_t *e) {
	(void)e;
	switch_screen(wifisettings_screen);
}

static bool wifi_is_connected(void) {
#ifdef HOST_BUILD
	return true;
#else
	wifi_status_t status;
	wifi_get_status(&status);
	return status.state == WIFI_STATE_CONNECTED && status.ip[0] != '\0';
#endif
}

static void rebuild_home(void) {
	lv_obj_clean(home_container);
	server_card = NULL;
	for (int i = 0; i < SUBSONIC_QUALITY_COUNT; i++) {
		quality_pills[i] = NULL;
	}
	update_corner_buttons();

	// The network first: signing in is a conversation with the server. A home
	// server on the same Wi-Fi is the usual case, but it is still the network.
	if (!wifi_is_connected()) {
		svcui_note(home_container, tr("enable_wi_fi_first"));
		svcui_nav_card(home_container, tr("wi_fi_settings"), NULL, open_wifi_settings_cb, NULL);
		return;
	}

	if (!subsonic_logged_in()) {
		svcui_nav_card(home_container, tr("sign_in"), NULL, open_login_cb, NULL);
		svcui_note(home_container, tr("subsonic_login_note"));
		return;
	}

	server_card = svcui_nav_card(home_container, "", NULL, open_settings_cb, NULL);
	server_card_paint();

	svcui_nav_card(home_container, tr(ALBUM_LISTS[0].label), NULL, album_list_cb, (void *)&ALBUM_LISTS[0]);
	svcui_nav_card(home_container, tr("artists"), NULL, simple_job_cb, (void *)(intptr_t)JOB_ARTISTS);
	svcui_nav_card(home_container, tr("playlists"), NULL, simple_job_cb, (void *)(intptr_t)JOB_PLAYLISTS);
	svcui_nav_card(home_container, tr("favourite_tracks"), NULL, simple_job_cb, (void *)(intptr_t)JOB_STARRED_TRACKS);
	svcui_nav_card(home_container, tr("favourite_albums"), NULL, simple_job_cb, (void *)(intptr_t)JOB_STARRED_ALBUMS);
	svcui_nav_card(home_container, tr("subsonic_random_50"), NULL, simple_job_cb, (void *)(intptr_t)JOB_RANDOM_TRACKS);

	svcui_group(home_container, tr("subsonic_selections"));
	for (size_t i = 1; i < sizeof(ALBUM_LISTS) / sizeof(ALBUM_LISTS[0]); i++) {
		svcui_nav_card(home_container, tr(ALBUM_LISTS[i].label), NULL, album_list_cb, (void *)&ALBUM_LISTS[i]);
	}

	// The stream's quality: the original file, or a transcode for a slow
	// network.
	lv_obj_t *quality = sx_card_create(home_container);
	sx_card_title(quality, tr("subsonic_stream_quality"));
	lv_obj_t *pills = sx_card_pills(quality);
	for (int i = 0; i < SUBSONIC_QUALITY_COUNT; i++) {
		quality_pills[i] = sx_pill(pills, tr(QUALITY_NAMES[i]), i, quality_pick_cb);
	}
	lv_obj_t *note = lv_label_create(quality);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &sx_style_text2, 0);
	lv_obj_set_style_text_font(note, &font_ui_20, 0);
	lv_obj_set_style_pad_bottom(note, 8, 0);
	lv_label_set_text(note, tr("subsonic_quality_note"));
	quality_refresh();
}

bool subsonicpage_open_album(const char *album_id, const char *title) {
	if (!album_id || !album_id[0] || !subsonic_logged_in()) {
		return false;
	}
	job_t job = {0};
	job.kind = JOB_ALBUM_TRACKS;
	snprintf(job.id_text, sizeof(job.id_text), "%s", album_id);
	snprintf(job.text, sizeof(job.text), "%s", title && title[0] ? title : tr("subsonic"));
	run(&job, "loading");
	return true;
}

static void home_loaded_cb(lv_event_t *e) {
	(void)e;
	subsoniccache_set_root(storage_sd_root());
	qobuzart_set_root(storage_sd_root());
	rebuild_home();
	status_ask();
}

// ---------------------------------------------------------------------------

static void build_list_page(gui_config_t *cfg) {
	list_screen = lv_obj_create(NULL);
	lv_obj_add_style(list_screen, &theme_style_screen, 0);
	lv_obj_add_event_cb(list_screen, list_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(list_screen, list_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	list_title = settingsrow_title(list_screen, cfg, "subsonic");
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
	streamerror_set_service(list_error, tr("subsonic"));
	list_spin = svcui_spinner(list_screen);

	switcher_attach_back_gesture(list_screen);
	switcher_set_back_guard(list_screen, list_back_guard);
	switcher_set_back_guard_peek(list_screen, list_back_has_step);
	player_sheet_attach_drag(list_screen, true);
}

static void *provider_open_album(const char *album_id, const char *title) {
	return subsonicpage_open_album(album_id, title) ? subsonic_list_screen : NULL;
}

void subsonicpage_init(gui_config_t *cfg) {
	// The mark over the artwork and "Show album" for the rest of the program
	// (provider.h).
	const stream_provider_gui_t provider_gui = {.badge = &icon_subsonic_badge, .open_album = provider_open_album};
	stream_provider_attach_gui("subsonic", &provider_gui);
	home_cfg = cfg;

	subsonic_screen = lv_obj_create(NULL);
	lv_obj_add_style(subsonic_screen, &theme_style_screen, 0);
	lv_obj_t *title = settingsrow_title(subsonic_screen, cfg, "subsonic");
	home_container = svcui_column(subsonic_screen, cfg);

	// The search in the header; the account's page is the server card.
	settingsrow_title_corner_slots(title, cfg, 1);
	search_btn = svcui_header_button(subsonic_screen, cfg, 0, &icon_search, open_search_cb, NULL);

	build_list_page(cfg);
	build_search_page(cfg);
	build_settings_page(cfg);
	build_login_page(cfg);
	subsoniccache_set_slow_start_cb(on_slow_start);
	device_state_add_prepare_cb(subsonic_prepare_track);

	lv_timer_create(queue_watch_cb, 1000, NULL);
	art_timer = lv_timer_create(art_poll_cb, 120, NULL);
	lv_timer_pause(art_timer);
	art_drop_timer = lv_timer_create(art_drop_cb, ART_DROP_DELAY_MS, NULL);
	lv_timer_pause(art_drop_timer);
	qobuzart_set_root(storage_sd_root());
	for (int i = 0; i < QOBUZART_SLOTS; i++) {
		slot_owner[i] = -1;
	}

	subsoniccache_set_root(storage_sd_root());
	rebuild_home();

	lv_obj_add_event_cb(subsonic_screen, home_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(subsonic_screen);
	player_sheet_attach_drag(subsonic_screen, true);
	player_sheet_attach_drag(home_container, true);
}

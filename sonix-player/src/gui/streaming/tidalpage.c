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
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/spinner.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/system/decode/growfile.h"
#include "src/system/playback/device_state.h"
#include "src/system/core/lang.h"
#include "src/system/playback/playlist.h"
#include "src/system/device/system.h"
#include "src/system/streaming/tidal.h"
#include "src/system/streaming/tidalcache.h"
#include "src/system/streaming/streamturn.h"
#include "src/system/streaming/tidalsync.h"
#include "src/system/core/utils.h"
#include "src/system/net/wifi.h"

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

// The showcase that came back empty, as a slot among the three featured
// entries; -1 when it worked.
static int result_featured_gone = -1;

// What the list currently shows, so a tap on a row knows what to do.
static job_kind_t list_kind;

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
// the busy veil
//
// Only for requests that lead to a list, and only because it stops a second
// entry being tapped while the first is arriving. It lasts as long as one HTTP
// request.
//
// Neither playback (the track starts while it downloads, see tidalcache_start)
// nor login raises it; login has its own spinner on the code page, which the
// veil would hide.
// ---------------------------------------------------------------------------

// A spinner in the middle of the screen, no text: understood without reading
// and needing no translation. The string callers pass is ignored.
static lv_obj_t *busy_layer;

static void busy_hide(void) {
	if (busy_layer) {
		lv_obj_add_flag(busy_layer, LV_OBJ_FLAG_HIDDEN);
	}
}

static void busy_show(const char *text) {
	(void)text;
	if (!busy_layer) {
		return;
	}
	lv_obj_remove_flag(busy_layer, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(busy_layer);
}

static void build_busy_layer(void) {
	busy_layer = lv_obj_create(lv_layer_sys());
	lv_obj_set_size(busy_layer, lv_pct(100), lv_pct(100));
	lv_obj_set_pos(busy_layer, 0, 0);
	lv_obj_set_style_bg_color(busy_layer, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(busy_layer, LV_OPA_50, 0);
	lv_obj_set_style_border_width(busy_layer, 0, 0);
	lv_obj_set_style_radius(busy_layer, 0, 0);
	lv_obj_remove_flag(busy_layer, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(busy_layer, LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_flex_flow(busy_layer, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(busy_layer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	// White rather than the theme colour: the veil under it is 50% black over
	// any page, so the background here is dark in both themes.
	lv_obj_t *spin = spinner_create(busy_layer, &icon_loader_big);
	lv_obj_set_style_image_recolor(spin, lv_color_white(), 0);
	lv_obj_set_style_image_recolor_opa(spin, LV_OPA_COVER, 0);
}

// ---------------------------------------------------------------------------
// the result list
// ---------------------------------------------------------------------------

static lv_obj_t *list_container;
static lv_obj_t *list_title;
static lv_obj_t *list_empty;

static void rebuild_home(void);
static void row_clicked_cb(lv_event_t *e);
static void login_code_arrived(void);
static void login_poll_result(void);
static void login_failed(const char *why);
static void featured_gone(int slot);

// The list history lives at the end of the file, but job_done_cb() above
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

// ---------------------------------------------------------------------------
// the per-row menu
//
// Not every list has something to offer on a row, and those that do offer
// different things: search results add to favourites, the favourites list
// removes, "My playlists" deletes. Three actions, two of them irreversible and
// therefore behind a confirmation.
// ---------------------------------------------------------------------------

static void run(const job_t *job, const char *waiting_text);

// A short notice when something is added to favourites: the list on screen
// does not change, so without a word nothing would be visible.
static void added_toast_async(void *user) {
	(void)user;
	toast_success("added_to_favourites");
}

typedef enum {
	ROW_MENU_NONE = 0,
	ROW_MENU_TRACK_ADD,	   // "add to favourites" on a track
	ROW_MENU_TRACK_REMOVE, // "remove from favourites" on a track
	ROW_MENU_ALBUM_ADD,
	ROW_MENU_ALBUM_REMOVE,
	ROW_MENU_PLAYLIST_DELETE,
} row_menu_t;

static row_menu_t row_menu_kind(job_kind_t kind) {
	switch (kind) {
	case JOB_SEARCH_TRACKS:
	case JOB_ALBUM_TRACKS:
	case JOB_PLAYLIST_TRACKS:
		return ROW_MENU_TRACK_ADD;
	case JOB_FAV_TRACKS:
		return ROW_MENU_TRACK_REMOVE;
	case JOB_SEARCH_ALBUMS:
	case JOB_ARTIST_ALBUMS:
	case JOB_FEATURED:
		return ROW_MENU_ALBUM_ADD;
	case JOB_FAV_ALBUMS:
		return ROW_MENU_ALBUM_REMOVE;
	case JOB_USER_PLAYLISTS:
		return ROW_MENU_PLAYLIST_DELETE;
	default:
		return ROW_MENU_NONE;
	}
}

// The row the menu was opened on. Only one: the menu is modal.
static int menu_index = -1;

static char sync_message[192];

// Re-reads the list on screen: after removing a favourite or deleting a
// playlist, what is displayed is no longer true.
static void reload_current_list(void) {
	job_t job = {0};
	job.kind = list_kind;
	switch (list_kind) {
	case JOB_FAV_TRACKS:
	case JOB_FAV_ALBUMS:
	case JOB_USER_PLAYLISTS:
		run(&job, "loading");
		break;
	default:
		break;
	}
}

static void sync_result_async(void *user) {
	bool reload = (bool)(intptr_t)user;
	if (sync_message[0]) {
		gui_notify_popup(sync_message);
		return;
	}
	if (reload) {
		reload_current_list();
	}
}

static void row_sync_done(bool ok, const char *error, void *user) {
	snprintf(sync_message, sizeof(sync_message), "%s",
			 ok ? "" : (error && error[0] ? error : tr("tidal_change_refused")));
	gui_post(sync_result_async, user);
}

// Adding to favourites does not change the list on screen (this happens from
// search results), so only success is reported.
static void row_added_done(bool ok, const char *error, void *user) {
	(void)user;
	snprintf(sync_message, sizeof(sync_message), "%s",
			 ok ? "" : (error && error[0] ? error : tr("tidal_change_refused")));
	gui_post(sync_result_async, (void *)(intptr_t)0);
	if (ok) {
		gui_post(added_toast_async, NULL);
	}
}

static void do_track_add(void *user) {
	(void)user;
	if (menu_index < 0 || menu_index >= result_count) {
		return;
	}
	long id = result_tracks[menu_index].id;
	tidalsync_favorite_toggle(id, true, row_added_done, NULL);
}

static void do_track_remove(void *user) {
	(void)user;
	if (menu_index < 0 || menu_index >= result_count) {
		return;
	}
	tidalsync_favorite_toggle(result_tracks[menu_index].id, false, row_sync_done, (void *)(intptr_t)1);
}

static void do_album_add(void *user) {
	(void)user;
	if (menu_index < 0 || menu_index >= result_count) {
		return;
	}
	tidalsync_album_favorite(result_albums[menu_index].id_text, true, row_added_done, NULL);
}

static void do_album_remove(void *user) {
	(void)user;
	if (menu_index < 0 || menu_index >= result_count) {
		return;
	}
	tidalsync_album_favorite(result_albums[menu_index].id_text, false, row_sync_done, (void *)(intptr_t)1);
}

static void do_playlist_delete(void *user) {
	(void)user;
	if (menu_index < 0 || menu_index >= result_count) {
		return;
	}
	tidalsync_playlist_remove(result_playlists[menu_index].id, row_sync_done, (void *)(intptr_t)1);
}

// The two destructive actions ask first: an accidental tap on the three dots
// must not delete a playlist.
static void ask_track_remove(void *user) {
	(void)user;
	if (menu_index < 0 || menu_index >= result_count) {
		return;
	}
	char message[TIDAL_TITLE_MAX + 64];
	snprintf(message, sizeof(message), tr("will_no_longer_be_one_of"),
			 result_tracks[menu_index].title);
	confirm_show("remove_from_favourites_2", message, "remove", do_track_remove, NULL);
}

static void ask_album_remove(void *user) {
	(void)user;
	if (menu_index < 0 || menu_index >= result_count) {
		return;
	}
	char message[TIDAL_TITLE_MAX + 64];
	snprintf(message, sizeof(message), tr("will_no_longer_be_one_of_2"),
			 result_albums[menu_index].title);
	confirm_show("remove_from_favourites_2", message, "remove", do_album_remove, NULL);
}

static void ask_playlist_delete(void *user) {
	(void)user;
	if (menu_index < 0 || menu_index >= result_count) {
		return;
	}
	char message[TIDAL_TITLE_MAX + 64];
	snprintf(message, sizeof(message), tr("tidal_delete_playlist_confirm_note"),
			 result_playlists[menu_index].name);
	confirm_show("delete_the_playlist", message, "delete", do_playlist_delete, NULL);
}

static void row_menu_cb(lv_event_t *e) {
	lv_event_stop_bubbling(e); // the tap belongs to the menu, not the row
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}

	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= result_count) {
		return;
	}
	menu_index = index;

	popover_item_t item;
	memset(&item, 0, sizeof(item));

	switch (row_menu_kind(list_kind)) {
	case ROW_MENU_TRACK_ADD:
		// A track already in favourites is offered for removal, not for
		// adding again.
		if (tidalsync_is_favorite(result_tracks[index].id)) {
			item.label = "remove_from_favourites";
			item.action = ask_track_remove;
		} else {
			item.label = "medialist_add_to_favourites";
			item.action = do_track_add;
		}
		break;
	case ROW_MENU_TRACK_REMOVE:
		item.label = "remove_from_favourites";
		item.action = ask_track_remove;
		break;
	case ROW_MENU_ALBUM_ADD:
		item.label = "medialist_add_to_favourites";
		item.action = do_album_add;
		break;
	case ROW_MENU_ALBUM_REMOVE:
		item.label = "remove_from_favourites";
		item.action = ask_album_remove;
		break;
	case ROW_MENU_PLAYLIST_DELETE:
		item.label = "delete_playlist";
		item.action = ask_playlist_delete;
		break;
	default:
		return;
	}

	popover_show(lv_event_get_current_target(e), &item, 1);
}

// Side length of a row thumbnail.
#define ROW_THUMB 60

// Row cover art.
//
// The loader has twelve slots and a list can hold up to TIDAL_MAX_HELD rows, so
// slots are a shared pool, not an assigned seat: they go to the rows that are
// visible and move to new ones on scroll.
//
// The loader is the Qobuz one, which is fine: it names cached images by the
// MD5 of the URL, so the two services cannot collide even by accident (see
// qobuzart.h). The URLs here are built by tidal.c from a UUID and are short,
// well within QOBUZART_URL_MAX.
static lv_obj_t *row_thumbs[TIDAL_MAX_HELD];
static char row_cover_url[TIDAL_MAX_HELD][QOBUZART_URL_MAX];
static cover_image_t row_images[TIDAL_MAX_HELD];
static bool row_has_image[TIDAL_MAX_HELD];

// This row already asked for its cover and did not get one: dead URL, image
// that will not decode, host that does not answer. The reload pass restarts
// every time a slot frees up, so without this one broken row would be asked for
// forever and keep a slot from the rows that can be served.
static bool row_art_failed[TIDAL_MAX_HELD];

// The token claiming the art slots for this page: only its address matters,
// never its value. See qobuzart.h.
static const char tidal_art_owner;

// Which row owns each slot right now, -1 = free.
static int slot_owner[QOBUZART_SLOTS];

// Timer that collects covers as they arrive. It lives exactly as long as the
// requests do: resumed when a slot is taken (art_wake()) and pausing itself
// when the last one frees, so it costs nothing when no art is in flight.
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

// Finished thumbnails are collected here periodically, in the same shape as
// the local cover loader: the thread works, the list draws, and whatever is
// ready appears on the next pass.
static void art_request_visible(void);

static void art_poll_cb(lv_timer_t *timer) {
	bool freed = false;
	if (!art_any_pending()) {
		lv_timer_pause(timer); // nothing in flight: art_wake() resumes it
		return;
	}

	for (int slot = 0; slot < QOBUZART_SLOTS; slot++) {
		int row = slot_owner[slot];
		if (row < 0 || row >= TIDAL_MAX_HELD || !row_thumbs[row]) {
			continue;
		}
		cover_image_t image;
		bool finished = false;
		if (!qobuzart_take(&tidal_art_owner, slot, &image, &finished)) {
			if (finished) {
				row_art_failed[row] = true; // failed: never ask again
				freed = true;
				slot_owner[slot] = -1; // nothing to show: slot back to the pool
			}
			continue;
		}

		cover_free(&row_images[row]);
		row_images[row] = image;
		row_has_image[row] = true;
		// The grey note placeholder was recoloured by the theme; a real cover
		// must keep its own colours or it becomes a solid square.
		lv_obj_remove_style(row_thumbs[row], &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(row_thumbs[row], LV_OPA_TRANSP, 0);
		lv_obj_set_style_radius(row_thumbs[row], 6, 0);
		lv_obj_set_style_clip_corner(row_thumbs[row], true, 0);
		lv_image_set_src(row_thumbs[row], &row_images[row].dsc);

		// Served: the slot returns to the pool for the next row.
		slot_owner[slot] = -1;
		freed = true;
	}

	// A freed slot must be handed out again immediately. There are more rows
	// than slots, so art_request_visible() serves what it can and returns;
	// without this pass the rows beyond the twelfth would only be asked for
	// again on the next scroll, and never at all if the last scroll happened
	// while every slot was busy.
	if (freed) {
		art_request_visible();
	}

}

// Requests covers for the visible rows plus a margin above and below, so slow
// scrolling finds them already there. Called on fill and on every scroll.
static void art_request_visible(void) {
	if (!list_container) {
		return;
	}

	int32_t view_top = lv_obj_get_scroll_y(list_container);
	int32_t view_height = lv_obj_get_height(list_container);
	int32_t margin = view_height / 2;

	uint32_t children = lv_obj_get_child_count(list_container);
	for (uint32_t c = 0; c < children && c < TIDAL_MAX_HELD; c++) {
		int row = (int)c;
		if (!row_thumbs[row] || row_has_image[row] || row_art_failed[row] || !row_cover_url[row][0]) {
			continue;
		}

		lv_obj_t *widget = lv_obj_get_child(list_container, (int32_t)c);
		int32_t y = lv_obj_get_y(widget);
		int32_t h = lv_obj_get_height(widget);
		if (y + h < view_top - margin || y > view_top + view_height + margin) {
			continue; // too far away to be worth a request
		}

		// Already queued for this row?
		bool queued = false;
		for (int slot = 0; slot < QOBUZART_SLOTS && !queued; slot++) {
			queued = slot_owner[slot] == row;
		}
		if (queued) {
			continue;
		}

		int free_slot = -1;
		for (int slot = 0; slot < QOBUZART_SLOTS && free_slot < 0; slot++) {
			if (slot_owner[slot] < 0) {
				free_slot = slot;
			}
		}
		if (free_slot < 0) {
			return; // all busy: the rest on the next pass
		}

		slot_owner[free_slot] = row;
		art_wake();
		qobuzart_request(&tidal_art_owner, free_slot, row_cover_url[row], ROW_THUMB);
	}
}

static void maybe_load_more(void);

static void list_scrolled_cb(lv_event_t *e) {
	(void)e;
	art_request_visible();
	maybe_load_more();
}

// The rows all go at once (lv_obj_clean): the pointers have to be forgotten
// first, or the thumbnail pass writes to dead objects.
static void art_forget_rows(void) {
	for (int slot = 0; slot < QOBUZART_SLOTS; slot++) {
		art_release_slot(slot);
	}
	for (int row = 0; row < TIDAL_MAX_HELD; row++) {
		row_thumbs[row] = NULL;
		row_cover_url[row][0] = '\0';
		row_has_image[row] = false;
		row_art_failed[row] = false;
		cover_free(&row_images[row]);
	}
}

// Gives the decoded thumbnails back without taking the list apart.
//
// Not art_forget_rows(): that one is only safe because an lv_obj_clean()
// follows it immediately, so the widgets pointing at the freed pixels are
// destroyed before anything can draw them. Here the rows survive -- coming back
// to the page finds the list where it was left -- so every widget goes back to
// the grey note first, with the note's styles on and the photograph's off.
//
// row_art_failed is deliberately NOT cleared: a cover that would not download
// still will not, and clearing it would send the same failing requests again at
// every visit.
static void art_drop_pictures(void) {
	for (int slot = 0; slot < QOBUZART_SLOTS; slot++) {
		art_release_slot(slot);
	}
	for (int row = 0; row < TIDAL_MAX_HELD; row++) {
		if (row_thumbs[row] && row_has_image[row]) {
			lv_image_set_src(row_thumbs[row], &icon_music2);
			lv_obj_add_style(row_thumbs[row], &theme_style_icon, 0);
			lv_obj_set_style_image_recolor_opa(row_thumbs[row], LV_OPA_COVER, 0);
			lv_obj_set_style_clip_corner(row_thumbs[row], false, 0);
			lv_obj_set_style_radius(row_thumbs[row], 0, 0);
		}
		row_has_image[row] = false;
		cover_free(&row_images[row]);
	}
}

// Two hundred rows of 60x60 is 1.4 MB, held until the list is rebuilt, so the
// pictures are given back a while after the page is left. Not the moment it is
// left: tapping a track goes to the player and coming straight back is the
// ordinary way to use this page, and freeing on the way out would re-read and
// re-decode a dozen pictures every time a track is played.
#define ART_DROP_DELAY_MS 20000

static lv_timer_t *art_drop_timer;

static void art_drop_cb(lv_timer_t *timer) {
	lv_timer_pause(timer);
	art_drop_pictures();

	// And back to the kernel, not merely back to the arena. A thumbnail is
	// 7200 bytes, well under the 96 kB mmap threshold main.c sets, so the
	// thumbnails come out of the heap rather than out of their own mappings
	// and freeing them alone leaves the arena holding the space.
	malloc_trim(0);
}

static void list_loaded_cb(lv_event_t *e) {
	(void)e;
	if (art_drop_timer) {
		lv_timer_pause(art_drop_timer); // back before the wait ran out
	}
	if (!list_container) {
		return; // the screen exists before the page inside it does
	}
	// Whatever was given back while the page was away is asked for again. Free
	// when nothing was dropped: every row still holding its picture is skipped.
	art_wake();
	lv_obj_update_layout(list_container);
	art_request_visible();
}

static void list_unloaded_cb(lv_event_t *e) {
	(void)e;
	if (art_drop_timer) {
		lv_timer_reset(art_drop_timer);
		lv_timer_resume(art_drop_timer);
	}
}

// A list row: name on top, detail below, and where needed the three dots on
// the right.
//
// The two labels form a flex column with a declared gap, each exactly one line
// high: that is what forces an ellipsis as the only outcome for text that is
// too long, instead of letting it wrap over the subtitle.
static lv_obj_t *make_row(const char *name, const char *detail, const char *cover_url, bool dimmed, int index) {
	lv_obj_t *row = lv_btn_create(list_container);
	lv_obj_set_size(row, lv_pct(100), 88);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, 12, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, 16, 0);
	lv_obj_set_style_pad_ver(row, 0, 0);
	lv_obj_set_style_pad_column(row, 8, 0);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	// The gestures (back, and pulling in the player) often start on a row, and
	// a button keeps the press to itself: without this neither drag works on
	// the lists.
	lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

	// The cover, where there is a URL for it. Starts as the grey note and
	// becomes the real image once the thread has downloaded it, so the list is
	// readable at once without waiting for the network.
	if (cover_url && cover_url[0] && index < TIDAL_MAX_HELD) {
		lv_obj_t *thumb = lv_image_create(row);
		lv_obj_set_size(thumb, ROW_THUMB, ROW_THUMB);
		lv_image_set_inner_align(thumb, LV_IMAGE_ALIGN_CENTER);
		lv_image_set_src(thumb, &icon_music2);
		lv_obj_add_style(thumb, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(thumb, LV_OPA_COVER, 0);
		lv_obj_add_flag(thumb, LV_OBJ_FLAG_EVENT_BUBBLE);
		row_thumbs[index] = thumb;
		snprintf(row_cover_url[index], sizeof(row_cover_url[0]), "%s", cover_url);
	}

	lv_obj_t *texts = lv_obj_create(row);
	lv_obj_set_flex_grow(texts, 1);
	lv_obj_set_height(texts, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(texts, 0, 0);
	lv_obj_set_style_border_width(texts, 0, 0);
	lv_obj_set_style_pad_all(texts, 0, 0);
	lv_obj_set_style_pad_row(texts, 6, 0); // breathing room between title and subtitle
	lv_obj_remove_flag(texts, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(texts, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(texts, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_obj_t *label = lv_label_create(texts);
	lv_label_set_text(label, name);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_set_height(label, lv_font_get_line_height(&font_ui_22));
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);

	// A track the subscription does not cover stays in the list but reads as
	// unplayable: hiding it would leave a gap in the album with no explanation.
	if (dimmed) {
		lv_obj_set_style_text_color(label, theme()->text_secondary, 0);
	}

	if (detail && detail[0]) {
		lv_obj_t *sub = lv_label_create(texts);
		lv_label_set_text(sub, detail);
		lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
		lv_obj_set_width(sub, lv_pct(100));
		lv_obj_set_height(sub, lv_font_get_line_height(&font_ui_18));
		lv_obj_add_style(sub, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(sub, &font_ui_18, 0);
	}

	// The three dots, where the list has something to offer on a single entry.
	if (row_menu_kind(result_kind) != ROW_MENU_NONE) {
		lv_obj_t *menu_btn = lv_btn_create(row);
		lv_obj_set_size(menu_btn, 44, 44);
		lv_obj_set_style_bg_opa(menu_btn, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(menu_btn, 0, 0);
		lv_obj_set_style_shadow_width(menu_btn, 0, 0);
		lv_obj_set_style_pad_all(menu_btn, 0, 0);
		lv_obj_add_event_cb(menu_btn, row_menu_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

		lv_obj_t *dots = lv_image_create(menu_btn);
		lv_image_set_src(dots, &icon_ellipsis_vertical);
		lv_obj_add_style(dots, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(dots, LV_OPA_COVER, 0);
		lv_obj_center(dots);
	}

	return row;
}

// `from` is the first row to create: 0 rebuilds everything, a higher value
// appends the new rows at the bottom without touching the others -- which is
// what keeps the scroll position steady when the next page arrives.
static void fill_list(int from) {
	if (from == 0) {
		art_forget_rows();
		lv_obj_clean(list_container);
	}
	list_kind = result_kind;

	for (int i = from; i < result_count; i++) {
		char detail[400];

		switch (result_kind) {
		case JOB_SEARCH_TRACKS:
		case JOB_ALBUM_TRACKS:
		case JOB_PLAYLIST_TRACKS:
		case JOB_FAV_TRACKS: {
			const tidal_track_t *t = &result_tracks[i];
			char length[16];
			duration_text(t->duration, length, sizeof(length));
			if (!t->streamable) {
				snprintf(detail, sizeof(detail), "%s \xC2\xB7 %s", t->artist, tr("not_included"));
			} else if (t->hires && t->sample_rate > 0) {
				snprintf(detail, sizeof(detail), "%s \xC2\xB7 %d bit %d kHz \xC2\xB7 %s", t->artist, t->bit_depth,
						 t->sample_rate / 1000, length);
			} else {
				snprintf(detail, sizeof(detail), "%s \xC2\xB7 %s", t->artist, length);
			}
			make_row(t->title, detail, t->cover, !t->streamable, i);
			break;
		}
		case JOB_SEARCH_ALBUMS:
		case JOB_FAV_ALBUMS:
		case JOB_ARTIST_ALBUMS:
		case JOB_FEATURED: {
			const tidal_album_t *a = &result_albums[i];
			snprintf(detail, sizeof(detail), "%s%s%s", a->artist, a->released[0] ? " \xC2\xB7 " : "", a->released);
			make_row(a->title, detail, a->cover, false, i);
			break;
		}
		case JOB_SEARCH_ARTISTS: {
			const tidal_artist_t *a = &result_artists[i];
			snprintf(detail, sizeof(detail), "%d album", a->album_count);
			make_row(a->name, a->album_count > 0 ? detail : "", a->image, false, i);
			break;
		}
		case JOB_USER_PLAYLISTS: {
			const tidal_playlist_t *p = &result_playlists[i];
			snprintf(detail, sizeof(detail), "%s \xC2\xB7 %d", p->owner, p->track_count);
			make_row(p->name, detail, p->image, false, i);
			break;
		}
		default:
			break;
		}
	}

	// Covers for the visible rows. Must run once the rows exist and LVGL has
	// computed the layout, because this reads coordinates.
	lv_obj_update_layout(list_container);
	art_request_visible();

	if (result_count == 0) {
		lv_obj_remove_flag(list_empty, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(list_empty, LV_OBJ_FLAG_HIDDEN);
	}

	lv_label_set_text(list_title, result_title);
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

static void job_done_cb(void *user) {
	(void)user;
	// Before reading any result_*: from here on the worker is held and nothing
	// writes underneath. It is released on the way out, in job_result_taken().
	busy_hide();

	if (result_error[0]) {
		// Login has its own page to report on. A transient popup would leave
		// the spinner turning on a code that will never arrive, with no way to
		// retry.
		if (result_kind == JOB_LOGIN_BEGIN || result_kind == JOB_LOGIN_POLL) {
			login_failed(result_error);
			job_result_taken();
			return;
		}
		// This returns without fill_list() and without switch_screen(): the
		// previous list stays on screen, so the history must stay as it was.
		// list_nav_commit() pushes the step, and only for a page actually
		// shown.
		if (job_is_list(result_kind)) {
			list_nav_drop();
		}
		gui_notify_popup(result_error);
		page_loading = false;
		job_result_taken();
		return;
	}

	switch (result_kind) {
	case JOB_LOGIN_BEGIN:
		login_code_arrived();
		break;

	case JOB_LOGIN_POLL:
		login_poll_result();
		break;

	case JOB_PLAY:
		// Nothing: the worker already opened the player the moment the music
		// started. This runs much later -- after the next track has also been
		// prefetched, which is minutes -- and reopening it now would tear away
		// whatever page the user has since opened.
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
		fill_list(result_from);
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
		fill_list(result_from);
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

static void play_started_async(void *user) {
	(void)user;
	player_refresh_now_playing();
	switch_screen(player_screen);
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

// The whole queued list: used by the prefetch thread, and used for on-demand
// fetches -- when playback reaches a track that has not downloaded yet, this
// is where to find what to ask Tidal for.
static tidal_track_t queued_tracks[TIDAL_MAX_HELD];
static int queued_count;

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
		snprintf(result_error, sizeof(result_error), "%s", why && why[0] ? why : tr("download_failed"));
		free(stream);
		return;
	}

	char path[512];
	if (!tidalcache_start(track->id, stream, track->duration, path, sizeof(path))) {
		// The cache says what went wrong: no card, no network, card full. The
		// generic "download failed" is only for when even it does not know.
		const char *why = tidalcache_last_error();
		snprintf(result_error, sizeof(result_error), "%s", why && why[0] ? why : tr("download_failed"));
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
	if (!library_queue && current >= 0 && queued_count > 0) {
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
		for (int i = 0; i < queued_count && index < 0; i++) {
			if (queued_tracks[i].id == id) {
				index = i;
			}
		}

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

		tidal_track_t track = queued_tracks[index];
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
		result_kind = job.kind;
		result_featured_gone = -1;

		// The length of the list currently on screen, saved before touching
		// anything.
		//
		// The result_* buffers back the visible list: fill_list() draws the
		// rows from them, and row_clicked_cb() looks up what to do when one is
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
		if (job.kind != JOB_PLAY && off == 0) {
			result_count = 0;
			result_title[0] = '\0';
		}

		int n = 0;
		switch (job.kind) {
		case JOB_LOGIN_BEGIN:
			memset(&login, 0, sizeof(login));
			if (!tidal_login_begin(&login)) {
				snprintf(result_error, sizeof(result_error), "%s", tidal_last_error());
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

		default:
			break;
		}

		if (n < 0) {
			snprintf(result_error, sizeof(result_error), "%s", tidal_last_error());
			n = 0;
		}
		if (job.kind != JOB_PLAY) {
			// A job that will draw nothing -- an error, or a vanished showcase
			// -- restores what it saved: the list on screen is still the
			// previous one, and these numbers must keep describing it. Zero
			// results without an error is a real answer ("no results") and
			// stands.
			if (result_error[0] || result_featured_gone >= 0) {
				result_count = kept_count;
				snprintf(result_title, sizeof(result_title), "%s", kept_title);
			} else {
				result_from = off;
				result_count = off + n;
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
	return kind != JOB_PLAY && kind != JOB_LOGIN_BEGIN && kind != JOB_LOGIN_POLL && kind != JOB_NONE;
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
	inflight_job = previous;
	inflight_valid = true;
	inflight_is_back = true;
	page_more = false;
	page_loading = false;
	start_worker();
	busy_show("loading");
	submit(&previous);
	return true;
}

static void run(const job_t *job, const char *waiting_text) {
	// A job opening another list while one is already open: the previous one
	// has to be remembered -- but when the new one is visible, not now.
	if (job_is_list(job->kind)) {
		inflight_job = *job;
		inflight_valid = true;
		inflight_is_back = false;
	}

	// A new list is starting: the old one's pagination must not fire while
	// waiting.
	if (job_is_list(job->kind)) {
		page_more = false;
		page_loading = false;
	}

	start_worker();
	// No veil for playback (it starts immediately) or for login, which has its
	// own spinner on the code page.
	if (job->kind != JOB_PLAY && job->kind != JOB_LOGIN_BEGIN && job->kind != JOB_LOGIN_POLL) {
		busy_show(waiting_text);
	}
	submit(job);
}

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
	// A sideways drag (back, or pulling in the player) moves this list too:
	// that is not a request for more rows.
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	// Less than one screen from the bottom is enough: the request takes a
	// moment, and reaching the last row only to find the list stops there is
	// exactly what this replaces.
	if (lv_obj_get_scroll_bottom(list_container) > lv_obj_get_height(list_container)) {
		return;
	}
	page_loading = true;
	job_t job = current_list_job;
	job.offset = result_count;
	start_worker();
	submit(&job);
}

// ---------------------------------------------------------------------------
// taps
// ---------------------------------------------------------------------------

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
	case JOB_SEARCH_TRACKS:
	case JOB_ALBUM_TRACKS:
	case JOB_PLAYLIST_TRACKS:
	case JOB_FAV_TRACKS:
		job.kind = JOB_PLAY;
		job.index = index;
		run(&job, NULL);
		return;

	case JOB_SEARCH_ALBUMS:
	case JOB_FAV_ALBUMS:
	case JOB_ARTIST_ALBUMS:
	case JOB_FEATURED:
		job.kind = JOB_ALBUM_TRACKS;
		snprintf(job.id_text, sizeof(job.id_text), "%s", result_albums[index].id_text);
		snprintf(job.text, sizeof(job.text), "%s", result_albums[index].title);
		run(&job, "loading");
		return;

	case JOB_SEARCH_ARTISTS:
		job.kind = JOB_ARTIST_ALBUMS;
		job.id = result_artists[index].id;
		snprintf(job.text, sizeof(job.text), "%s", result_artists[index].name);
		run(&job, "loading");
		return;

	case JOB_USER_PLAYLISTS:
		job.kind = JOB_PLAYLIST_TRACKS;
		snprintf(job.id_text, sizeof(job.id_text), "%s", result_playlists[index].id);
		snprintf(job.text, sizeof(job.text), "%s", result_playlists[index].name);
		run(&job, "loading");
		return;

	default:
		return;
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
		bool on = i == search_kind_index;
		lv_obj_set_style_bg_color(search_pills[i], on ? theme()->accent : theme()->surface_pressed, 0);
		lv_obj_set_style_text_color(lv_obj_get_child(search_pills[i], 0), on ? lv_color_white() : theme()->text_primary,
									0);
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
		lv_obj_remove_flag(search_clear_btn, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(search_clear_btn, LV_OBJ_FLAG_HIDDEN);
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
	lv_obj_set_size(search_field, cfg->screen_width - 2 * cfg->padding, 62);
	lv_obj_set_scrollbar_mode(search_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(search_field, LV_ALIGN_TOP_LEFT, cfg->padding, top);
	lv_obj_add_style(search_field, &theme_style_card, 0);
	lv_obj_set_style_radius(search_field, 12, 0);
	lv_obj_set_style_border_width(search_field, 0, 0);
	lv_obj_set_style_shadow_width(search_field, 0, 0);
	lv_obj_set_style_pad_all(search_field, 14, 0);
	lv_obj_set_style_text_font(search_field, &font_ui_24, 0);
	keyboard_style_caret(search_field);
	lv_obj_add_event_cb(search_field, search_field_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

	// The x on the right edge of the field, as in local search: clears the
	// query with one tap. Hidden while the field is empty.
	search_clear_btn = lv_btn_create(search_screen);
	lv_obj_set_size(search_clear_btn, 56, 56);
	lv_obj_align(search_clear_btn, LV_ALIGN_TOP_RIGHT, -cfg->padding - 4, top + (62 - 56) / 2);
	lv_obj_set_style_bg_opa(search_clear_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(search_clear_btn, 0, 0);
	lv_obj_set_style_border_width(search_clear_btn, 0, 0);
	lv_obj_set_style_pad_all(search_clear_btn, 0, 0);
	lv_obj_add_flag(search_clear_btn, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_event_cb(search_clear_btn, search_clear_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *search_clear_icon = lv_image_create(search_clear_btn);
	lv_image_set_src(search_clear_icon, &icon_clear);
	lv_obj_add_style(search_clear_icon, &theme_style_icon, 0);
	lv_obj_set_style_image_opa(search_clear_icon, LV_OPA_70, 0);
	lv_obj_center(search_clear_icon);

	// Room for it, so a long query does not run underneath.
	lv_obj_set_style_pad_right(search_field, 60, 0);

	static const char *const KINDS[] = {"tracks_2", "albums", "artists"};
	for (int i = 0; i < 3; i++) {
		lv_obj_t *pill = lv_btn_create(search_screen);
		lv_obj_set_size(pill, 140, 52);
		lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_shadow_width(pill, 0, 0);
		lv_obj_set_style_border_width(pill, 0, 0);
		lv_obj_align(pill, LV_ALIGN_TOP_LEFT, cfg->padding + i * 150, top + 78);
		lv_obj_add_event_cb(pill, search_pill_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

		lv_obj_t *label = lv_label_create(pill);
		lv_label_set_text(label, tr(KINDS[i]));
		lv_obj_set_style_text_font(label, &font_ui_20, 0);
		lv_obj_center(label);
		search_pills[i] = pill;
	}
	paint_search_pills();

	search_keyboard = keyboard_create(search_screen, cfg->screen_width, 316, search_field, NULL, "ok",
									  search_accept_cb, NULL);
	switcher_attach_back_gesture(search_screen);
	theme_register_refresh(paint_search_pills);
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
		lv_obj_remove_flag(widget, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(widget, LV_OBJ_FLAG_HIDDEN);
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
	lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
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
static lv_obj_t *settings_btn;
static lv_obj_t *search_btn;

// The showcases Tidal offers, with the short name tidal.c expects.
static const char *const FEATURED_TYPES[3] = {"new", "top", "recommended"};
static const char *const FEATURED_NAMES[3] = {"new_releases", "tidal_most_streamed", "tidal_recommended_for_you"};

// The showcases this session found empty. A showcase that answered with
// nothing is not offered again: the endpoint behind it is the only one in all
// of tidal.c that no other program still uses, and a row that leads to the
// same nothing every time is worse than no row.
//
// Not permanent, though: this is cleared on every return to the main page. From
// here the difference between "Tidal retired this showcase" and "the Wi-Fi
// dropped a packet" is invisible, so a single bad moment must not cost a row
// until the player is restarted.
static bool featured_hidden[3];

static const int QUALITY_VALUES[4] = {TIDAL_QUALITY_LOW, TIDAL_QUALITY_HIGH, TIDAL_QUALITY_LOSSLESS,
									  TIDAL_QUALITY_HIRES};

// The perceived quality first, then the format underneath: "High" alone is not
// distinguishable from "Hi-res", and "AAC 320" alone tells nobody it is lossy.
static const char *const QUALITY_NAMES[4] = {"tidal_low_aac_96", "tidal_high_aac_320", "tidal_cd_flac_16_44",
											 "tidal_hi_res_flac_24"};

// The two corner buttons: the gear (Tidal settings) and the magnifier. Shown
// only when signed in, because otherwise there is nothing to configure and
// nothing to search.
static void update_corner_buttons(void) {
	bool visible = tidal_configured() && tidal_logged_in();
	lv_obj_t *const buttons[] = {settings_btn, search_btn};
	for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
		if (!buttons[i]) {
			continue;
		}
		if (visible) {
			lv_obj_remove_flag(buttons[i], LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_add_flag(buttons[i], LV_OBJ_FLAG_HIDDEN);
		}
	}
}

// Same shape as the Music page corner buttons: 56 px, transparent, aligned top
// right and stepped leftwards one by one.
static lv_obj_t *corner_button(gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph, lv_event_cb_t cb) {
	lv_obj_t *button = lv_btn_create(tidal_screen);
	lv_obj_set_size(button, 56, 56);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -cfg->padding - slot * (56 + 6), cfg->padding + cfg->top_bar_height);

	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, glyph);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_center(icon);

	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);
	return button;
}

// ---------------------------------------------------------------------------
// Tidal settings
//
// The corner holds the gear (as on the Music page) and the magnifier; this
// page holds what is done once a year: quality as pills, like every other
// choice between alternatives in this interface, and sign-out as a row.
// ---------------------------------------------------------------------------

static lv_obj_t *tsettings_screen;
static lv_obj_t *quality_pills[4];

static void quality_refresh(void) {
	int current = tidal_get_quality();
	for (int i = 0; i < 4; i++) {
		if (!quality_pills[i]) {
			continue;
		}
		bool on = QUALITY_VALUES[i] == current;
		lv_obj_set_style_bg_color(quality_pills[i], on ? theme()->accent : theme()->surface_pressed, 0);
		lv_obj_set_style_text_color(lv_obj_get_child(quality_pills[i], 0), on ? lv_color_white() : theme()->text_primary,
									0);
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

static void open_settings_cb(lv_event_t *e) {
	(void)e;
	quality_refresh();
	switch_screen(tsettings_screen);
}

static void build_settings_page(gui_config_t *cfg) {
	tsettings_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(tsettings_screen, cfg, "tidal_settings");

	// Quality: a card with the name on top and the four pills below, exactly
	// like "DSD output" or "Replay gain" on the Music page.
	lv_obj_t *card = lv_obj_create(container);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 12, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, 20, 0);
	lv_obj_set_style_pad_row(card, 18, 0);
	lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_obj_t *name = lv_label_create(card);
	lv_label_set_text(name, tr("audio_quality"));
	lv_obj_add_style(name, &theme_style_text, 0);
	lv_obj_set_style_text_font(name, &font_ui_24, 0);

	lv_obj_t *pills = lv_obj_create(card);
	lv_obj_set_size(pills, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(pills, 0, 0);
	lv_obj_set_style_border_width(pills, 0, 0);
	lv_obj_set_style_pad_all(pills, 0, 0);
	lv_obj_set_style_pad_gap(pills, 12, 0);
	lv_obj_remove_flag(pills, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(pills, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_set_flex_flow(pills, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(pills, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

	for (int i = 0; i < 4; i++) {
		lv_obj_t *btn = lv_btn_create(pills);
		lv_obj_set_size(btn, LV_SIZE_CONTENT, 56);
		lv_obj_set_style_pad_hor(btn, 22, 0);
		lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_shadow_width(btn, 0, 0);
		lv_obj_set_style_border_width(btn, 0, 0);
		lv_obj_add_event_cb(btn, quality_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)QUALITY_VALUES[i]);

		// These names go through tr(): half of each is a translatable word,
		// not an acronym.
		lv_obj_t *label = lv_label_create(btn);
		lv_label_set_text(label, tr(QUALITY_NAMES[i]));
		lv_obj_set_style_text_font(label, &font_ui_22, 0);
		lv_obj_center(label);

		quality_pills[i] = btn;
	}

	settingsrow_action(container, "leave", logout_cb, NULL);

	quality_refresh();
	theme_register_refresh(quality_refresh);
	switcher_attach_back_gesture(tsettings_screen);
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

	if (!tidal_configured()) {
		update_corner_buttons();
		lv_obj_t *note = lv_label_create(home_container);
		lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(note, lv_pct(100));
		lv_obj_add_style(note, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(note, &font_ui_20, 0);
		// Not an "error": this is fixable, and the text says how. The second
		// sentence exists because Tidal revokes keys now and then, leaving the
		// file present but worthless.
		lv_label_set_text(note, tr("tidal_not_configured"));
		return;
	}

	// The network comes before the login, and not the other way round: signing
	// in is a conversation with Tidal's servers, so with no network the "Sign
	// in" row could only fail. The notice says what is actually missing.
	if (!wifi_is_connected()) {
		update_corner_buttons();
		// The same notice as the Wi-Fi transfer page: centred message, with a
		// "Wi-Fi settings" row below that leads where it is fixed.
		lv_obj_t *note = lv_label_create(home_container);
		lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(note, lv_pct(100));
		lv_obj_add_style(note, &theme_style_text, 0); // normal colour: it is an instruction
		lv_obj_set_style_text_font(note, &font_ui_22, 0);
		lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_pad_hor(note, 4, 0);
		lv_obj_set_style_pad_top(note, 14, 0);
		lv_label_set_text(note, tr("enable_wi_fi_first"));
		lv_obj_t *row = settingsrow_add(home_container, "wi_fi_settings", NULL, open_wifi_settings_cb, NULL);
		lv_obj_set_style_margin_top(row, 16, 0);
		return;
	}

	if (!tidal_logged_in()) {
		update_corner_buttons();
		settingsrow_add(home_container, "sign_in", NULL, open_login_cb, NULL);
		return;
	}

	// No "Search" row: it is the magnifier in the corner, where it sits on
	// every other searchable page.
	settingsrow_add(home_container, "favourite_tracks", NULL, simple_job_cb, (void *)(intptr_t)JOB_FAV_TRACKS);
	settingsrow_add(home_container, "favourite_albums", NULL, simple_job_cb, (void *)(intptr_t)JOB_FAV_ALBUMS);
	settingsrow_add(home_container, "my_playlists", NULL, simple_job_cb, (void *)(intptr_t)JOB_USER_PLAYLISTS);
	for (int i = 0; i < 3; i++) {
		if (featured_hidden[i]) {
			continue;
		}
		settingsrow_add(home_container, FEATURED_NAMES[i], NULL, featured_job_cb, (void *)(intptr_t)i);
	}

	update_corner_buttons();
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
	lv_obj_add_event_cb(list_screen, list_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(list_screen, list_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	list_container = settingsrow_page(list_screen, cfg, "tidal");
	list_title = settingsrow_page_title(list_screen);

	list_empty = lv_label_create(list_screen);
	lv_label_set_text(list_empty, tr("no_results"));
	lv_obj_set_style_text_align(list_empty, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(list_empty, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(list_empty, &font_ui_24, 0);
	lv_obj_align(list_empty, LV_ALIGN_TOP_MID, 0, settingsrow_content_top(cfg) + 100);
	lv_obj_add_flag(list_empty, LV_OBJ_FLAG_HIDDEN);

	lv_obj_add_event_cb(list_container, list_scrolled_cb, LV_EVENT_SCROLL, NULL);
	switcher_attach_back_gesture(list_screen);
	switcher_set_back_guard(list_screen, list_back_guard);
	switcher_set_back_guard_peek(list_screen, list_back_has_step);
	// The leftward drag that pulls in the player, as on every other browsable
	// page.
	player_sheet_attach_drag(list_screen, true);
	player_sheet_attach_drag(list_container, true);
}

void tidalpage_init(gui_config_t *cfg) {
	tidal_screen = lv_obj_create(NULL);
	home_container = settingsrow_page(tidal_screen, cfg, "tidal");

	// Two buttons top right, like the Music page gear. The title has to be
	// told the corner is taken, or its text runs underneath them.
	settingsrow_title_corner_slots(settingsrow_page_title(tidal_screen), cfg, 2);
	settings_btn = corner_button(cfg, 0, &icon_music_settings, open_settings_cb);
	search_btn = corner_button(cfg, 1, &icon_search, open_search_cb);

	build_list_page(cfg);
	build_search_page(cfg);
	build_settings_page(cfg);
	build_login_page(cfg);
	build_busy_layer();
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

	tidalcache_set_root(storage_sd_root());
	rebuild_home();

	lv_obj_add_event_cb(tidal_screen, home_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(tidal_screen);
	player_sheet_attach_drag(tidal_screen, true);
	player_sheet_attach_drag(home_container, true);
}

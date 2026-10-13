#include "subsonicpage.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>

#include "src/gui/shell/confirm.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/popover.h"
#include "src/gui/streaming/qobuzart.h"
#include "src/gui/streaming/streamerror.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/spinner.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
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
	JOB_PLAY,
} job_kind_t;

typedef struct {
	job_kind_t kind;
	char text[256];					// search terms, user name, or the list's title
	char text2[128];				// password, or the album list type ("newest")
	char server[SUBSONIC_SERVER_MAX]; // sign-in only
	char id_text[SUBSONIC_ID_MAX];	// album, artist or playlist id
	int index;						// which track of the list playback starts from
	int offset;						// first entry of this page: 0 = new list
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
// the busy veil
//
// Only for requests that lead to a list, and only because it blocks touching a
// second entry while the first is still arriving. It lasts as long as one HTTP
// request.
//
// Playback does NOT raise it: the track starts while it is still downloading
// (see subsoniccache_start), so there is no wait to show, and a veil raised here
// would stay up for the whole prefetch of the following tracks.
// ---------------------------------------------------------------------------

// The wait indicator: a spinner in the middle of the screen, no text --
// understood without reading and needing no translation. The text callers pass
// is ignored.
static lv_obj_t *busy_layer;

static void busy_hide(void) {
	if (busy_layer) {
		lv_obj_set_hidden(busy_layer, true);
	}
}

static void busy_show(const char *text) {
	(void)text;
	if (!busy_layer) {
		return;
	}
	lv_obj_set_hidden(busy_layer, false);
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
	lv_obj_set_scrollable(busy_layer, false);
	lv_obj_set_hidden(busy_layer, true);
	lv_obj_set_flex_flow(busy_layer, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(busy_layer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	// White rather than the theme colour: the veil underneath is 50% black over
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
// A list that could not be loaded: what went wrong, and "Retry" (streamerror.h).
static lv_obj_t *list_error;

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

// ---------------------------------------------------------------------------
// the per-row menu
//
// Not every list has something to offer on a row, and those that do offer
// different things: search results add to favourites, the favourites list
// removes, "My playlists" deletes. Three actions, two of them irreversible --
// so those ask for confirmation.
// ---------------------------------------------------------------------------

static void run(const job_t *job, const char *waiting_text);

// A short toast when something was added to favourites: the list on screen does
// not change, so without a word nothing would appear to happen.
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
} row_menu_t;

static row_menu_t row_menu_kind(job_kind_t kind) {
	switch (kind) {
	case JOB_SEARCH_TRACKS:
	case JOB_ALBUM_TRACKS:
	case JOB_PLAYLIST_TRACKS:
		return ROW_MENU_TRACK_ADD;
	case JOB_STARRED_TRACKS:
		return ROW_MENU_TRACK_REMOVE;
	case JOB_SEARCH_ALBUMS:
	case JOB_ARTIST_ALBUMS:
	case JOB_ALBUM_LIST:
		return ROW_MENU_ALBUM_ADD;
	case JOB_STARRED_ALBUMS:
		return ROW_MENU_ALBUM_REMOVE;
	default:
		// Playlists carry no menu: deleting one on the server from a music
		// player is a step nobody asked for.
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
	case JOB_STARRED_TRACKS:
	case JOB_STARRED_ALBUMS:
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
			 ok ? "" : (error && error[0] ? error : tr("subsonic_change_refused")));
	gui_post(sync_result_async, user);
}

// Adding to favourites does not change the list on screen (the user is among
// search results), so a success toast is all that is reported.
static void row_added_done(bool ok, const char *error, void *user) {
	(void)user;
	snprintf(sync_message, sizeof(sync_message), "%s",
			 ok ? "" : (error && error[0] ? error : tr("subsonic_change_refused")));
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
	subsonicsync_favorite_toggle(result_tracks[menu_index].id, true, row_added_done, NULL);
}

static void do_track_remove(void *user) {
	(void)user;
	if (menu_index < 0 || menu_index >= result_count) {
		return;
	}
	subsonicsync_favorite_toggle(result_tracks[menu_index].id, false, row_sync_done, (void *)(intptr_t)1);
}

static void do_album_add(void *user) {
	(void)user;
	if (menu_index < 0 || menu_index >= result_count) {
		return;
	}
	subsonicsync_album_favorite(result_albums[menu_index].id, true, row_added_done, NULL);
}

static void do_album_remove(void *user) {
	(void)user;
	if (menu_index < 0 || menu_index >= result_count) {
		return;
	}
	subsonicsync_album_favorite(result_albums[menu_index].id, false, row_sync_done, (void *)(intptr_t)1);
}

// The two destructive actions go through a confirmation: an accidental tap on
// the ellipsis must not delete a playlist.
static void ask_track_remove(void *user) {
	(void)user;
	if (menu_index < 0 || menu_index >= result_count) {
		return;
	}
	char message[SUBSONIC_TITLE_MAX + 64];
	snprintf(message, sizeof(message), tr("will_no_longer_be_one_of"),
			 result_tracks[menu_index].title);
	confirm_show("remove_from_favourites_2", message, "remove", do_track_remove, NULL);
}

static void ask_album_remove(void *user) {
	(void)user;
	if (menu_index < 0 || menu_index >= result_count) {
		return;
	}
	char message[SUBSONIC_TITLE_MAX + 64];
	snprintf(message, sizeof(message), tr("will_no_longer_be_one_of_2"),
			 result_albums[menu_index].title);
	confirm_show("remove_from_favourites_2", message, "remove", do_album_remove, NULL);
}

static void row_menu_cb(lv_event_t *e) {
	lv_event_stop_bubbling(e); // the tap belongs to the menu, not to the row
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
		// A track already among the favourites is not offered for adding again:
		// it is offered for removal.
		if (subsonicsync_is_favorite(result_tracks[index].id)) {
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
	default:
		return;
	}

	popover_show(lv_event_get_current_target(e), &item, 1);
}

// Side of a row thumbnail, in pixels.
#define ROW_THUMB 60

// What getCoverArt is asked for, for a list row and for the player. The server
// scales it, so the row's sixty pixels do not mean fetching the original
// two-megabyte scan.
#define LIST_COVER_PX 300
#define PLAYER_COVER_PX 600

// Row cover art.
//
// The loader has twelve slots and a list can hold far more rows than that, so
// slots are a shared pool and not assigned seats: they go to the rows currently
// visible and move to new ones as the list scrolls.
static lv_obj_t *row_thumbs[SUB_MAX_HELD];
static char row_cover_url[SUB_MAX_HELD][QOBUZART_URL_MAX];
static cover_image_t row_images[SUB_MAX_HELD];
static bool row_has_image[SUB_MAX_HELD];

// This row already asked for its cover and did not get one: dead URL, image
// that fails to decode, host that does not answer. The reload pass restarts
// every time a slot frees up, so without this one broken row would be asked for
// forever and keep a slot from the rows that can be served.
static bool row_art_failed[SUB_MAX_HELD];

// The token that claims the cover slots: only its address matters, never its
// value. See qobuzart.h.
static const char subsonic_art_owner;

// Which row occupies each slot right now, -1 = free.
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

// Finished thumbnails are collected here periodically: same shape as the local
// cover loader -- the thread works, the list draws, and whatever is ready shows
// up on the next pass.
static void art_request_visible(void);

static void art_poll_cb(lv_timer_t *timer) {
	bool freed = false;
	if (!art_any_pending()) {
		lv_timer_pause(timer); // nothing in flight: art_wake() restarts it
		return;
	}

	for (int slot = 0; slot < QOBUZART_SLOTS; slot++) {
		int row = slot_owner[slot];
		if (row < 0 || row >= SUB_MAX_HELD || !row_thumbs[row]) {
			continue;
		}
		cover_image_t image;
		bool finished = false;
		if (!qobuzart_take(&subsonic_art_owner, slot, &image, &finished)) {
			if (finished) {
				row_art_failed[row] = true; // failed: never requested again
				freed = true;
				slot_owner[slot] = -1; // nothing to show: the slot goes back
			}
			continue;
		}

		cover_free(&row_images[row]);
		row_images[row] = image;
		row_has_image[row] = true;
		// The grey note placeholder was recoloured by the theme: a real cover
		// must keep its own colours, otherwise it turns into a flat square.
		lv_obj_remove_style(row_thumbs[row], &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(row_thumbs[row], LV_OPA_TRANSP, 0);
		lv_obj_set_style_radius(row_thumbs[row], 6, 0);
		lv_obj_set_style_clip_corner(row_thumbs[row], true, 0);
		lv_image_set_src(row_thumbs[row], &row_images[row].dsc);

		// Served: the slot goes back to the pool for the next row.
		slot_owner[slot] = -1;
		freed = true;
	}

	// A slot that just freed up must be handed out immediately.
	//
	// There are more rows than slots, so art_request_visible() serves what it
	// can and returns; without this pass the rows beyond the twelfth would only
	// be asked for again on the next scroll, and never at all if the last
	// scroll happened while every slot was busy.
	if (freed) {
		art_request_visible();
	}

}

// Requests covers for the visible rows, plus some margin above and below so
// slow scrolling finds them already there. Called on fill and on every scroll.
static void art_request_visible(void) {
	if (!list_container) {
		return;
	}

	int32_t view_top = lv_obj_get_scroll_y(list_container);
	int32_t view_height = lv_obj_get_height(list_container);
	int32_t margin = view_height / 2;

	uint32_t children = lv_obj_get_child_count(list_container);
	for (uint32_t c = 0; c < children && c < SUB_MAX_HELD; c++) {
		int row = (int)c;
		if (!row_thumbs[row] || row_has_image[row] || row_art_failed[row] || !row_cover_url[row][0]) {
			continue;
		}

		lv_obj_t *widget = lv_obj_get_child(list_container, (int32_t)c);
		int32_t y = lv_obj_get_y(widget);
		int32_t h = lv_obj_get_height(widget);
		if (y + h < view_top - margin || y > view_top + view_height + margin) {
			continue; // too far off screen to be worth a request
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
		qobuzart_request(&subsonic_art_owner, free_slot, row_cover_url[row], ROW_THUMB);
	}
}

static void maybe_load_more(void);

static void list_scrolled_cb(lv_event_t *e) {
	(void)e;
	art_request_visible();
	maybe_load_more();
}

// The rows all go away at once (lv_obj_clean): the pointers must be forgotten
// BEFORE that, or the thumbnail pass writes to dead objects.
static void art_forget_rows(void) {
	for (int slot = 0; slot < QOBUZART_SLOTS; slot++) {
		art_release_slot(slot);
	}
	for (int row = 0; row < SUB_MAX_HELD; row++) {
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
// follows it immediately, so the widgets still pointing at the freed pixels are
// destroyed before anything can draw them. Here the rows have to survive --
// coming back to the page must find the list where it was left -- so every
// widget goes back to the grey note first, with the styles the note needs and
// the ones the photograph needed taken off again.
//
// row_art_failed is deliberately NOT cleared: a cover that would not download
// still will not, and forgetting that would send the same failing requests
// again at every visit.
static void art_drop_pictures(void) {
	for (int slot = 0; slot < QOBUZART_SLOTS; slot++) {
		art_release_slot(slot);
	}
	for (int row = 0; row < SUB_MAX_HELD; row++) {
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
// left: tapping a row goes to the player and coming straight back is the
// ordinary way to use this page, and freeing on the way out would re-read and
// re-decode a dozen pictures every time something is played.
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

// One list row: name on top, detail below, and -- where applicable -- the
// ellipsis menu on the right.
//
// The two labels form a flex column with a declared gap, each exactly one line
// high: that is what makes the ellipsis the only possible outcome for text that
// is too long, instead of letting it wrap over the subtitle.
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
	lv_obj_set_scrollable(row, false);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	// Gestures (back, and pulling in the player sheet) often start on a row,
	// and a button keeps the press to itself: without this neither drag works
	// on these lists.
	lv_obj_set_event_bubble(row, true);
	lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

	// The cover, where there is a URL for one. It starts as the grey note and
	// becomes the real image once the thread has downloaded it: the list is
	// readable immediately, without waiting for the network.
	if (cover_url && cover_url[0] && index < SUB_MAX_HELD) {
		lv_obj_t *thumb = lv_image_create(row);
		lv_obj_set_size(thumb, ROW_THUMB, ROW_THUMB);
		lv_image_set_inner_align(thumb, LV_IMAGE_ALIGN_CENTER);
		lv_image_set_src(thumb, &icon_music2);
		lv_obj_add_style(thumb, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(thumb, LV_OPA_COVER, 0);
		lv_obj_set_event_bubble(thumb, true);
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

	// A track the subscription does not cover stays in the list but is visibly
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

	// The ellipsis button, where the list has something to offer per entry.
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
// APPENDS the new rows at the bottom without touching the others -- which is
// what keeps the scroll position steady when the next page arrives.
static void fill_list(int from) {
	if (from == 0) {
		art_forget_rows();
		lv_obj_clean(list_container);
	}
	list_kind = result_kind;

	for (int i = from; i < result_count; i++) {
		char detail[400];

		char cover[SUBSONIC_URL_MAX];

		switch (result_kind) {
		case JOB_SEARCH_TRACKS:
		case JOB_ALBUM_TRACKS:
		case JOB_PLAYLIST_TRACKS:
		case JOB_STARRED_TRACKS: {
			const subsonic_track_t *t = &result_tracks[i];
			char length[16];
			duration_text(t->duration, length, sizeof(length));
			// The format is worth a word when it is not the common case: a FLAC
			// on a list of FLACs says nothing, an Opus among them does.
			if (t->suffix[0] && strcmp(t->suffix, "flac") != 0 && strcmp(t->suffix, "mp3") != 0) {
				snprintf(detail, sizeof(detail), "%s \xC2\xB7 %s \xC2\xB7 %s", t->artist, t->suffix, length);
			} else {
				snprintf(detail, sizeof(detail), "%s \xC2\xB7 %s", t->artist, length);
			}
			subsonic_cover_url(t->cover_id, LIST_COVER_PX, cover, sizeof(cover));
			make_row(t->title, detail, cover, false, i);
			break;
		}
		case JOB_SEARCH_ALBUMS:
		case JOB_STARRED_ALBUMS:
		case JOB_ARTIST_ALBUMS:
		case JOB_ALBUM_LIST: {
			const subsonic_album_t *a = &result_albums[i];
			if (a->year > 0) {
				snprintf(detail, sizeof(detail), "%s \xC2\xB7 %d", a->artist, a->year);
			} else {
				snprintf(detail, sizeof(detail), "%s", a->artist);
			}
			subsonic_cover_url(a->cover_id, LIST_COVER_PX, cover, sizeof(cover));
			make_row(a->title, detail, cover, false, i);
			break;
		}
		case JOB_SEARCH_ARTISTS:
		case JOB_ARTISTS: {
			const subsonic_artist_t *a = &result_artists[i];
			snprintf(detail, sizeof(detail), tr(a->album_count == 1 ? "subsonic_one_album" : "subsonic_albums_n"),
					 a->album_count);
			subsonic_cover_url(a->cover_id, LIST_COVER_PX, cover, sizeof(cover));
			make_row(a->name, a->album_count > 0 ? detail : "", cover, false, i);
			break;
		}
		case JOB_PLAYLISTS: {
			const subsonic_playlist_t *p = &result_playlists[i];
			snprintf(detail, sizeof(detail), "%s \xC2\xB7 %d", p->owner, p->song_count);
			subsonic_cover_url(p->cover_id, LIST_COVER_PX, cover, sizeof(cover));
			make_row(p->name, detail, cover, false, i);
			break;
		}
		default:
			break;
		}
	}

	// Covers for the visible rows. This runs after the rows exist and after
	// LVGL has computed the layout, because it reads their coordinates.
	lv_obj_update_layout(list_container);
	art_request_visible();

	if (result_count == 0) {
		lv_obj_set_hidden(list_empty, false);
	} else {
		lv_obj_set_hidden(list_empty, true);
	}

	lv_label_set_text(list_title, result_title);
}

// ---------------------------------------------------------------------------
// what comes back from the worker, on the GUI thread
// ---------------------------------------------------------------------------

static bool job_is_list(job_kind_t kind);
static void retry_list(void);
static void error_sign_in(void);

static void job_done_cb(void *user) {
	(void)user;
	busy_hide();

	if (result_error[0]) {
		page_loading = false;
		if (job_is_list(result_kind) && result_from == 0) {
			// Said where the list would have been, with a way to ask again,
			// rather than in a popup over whatever page was open: the list
			// page opens (under the list's own title) and its back leads
			// where it would have.
			fill_list(0);
			lv_obj_set_hidden(list_empty, true);
			page_more = false;
			streamerror_show(list_error, result_class, result_error, retry_list, error_sign_in);
			if (lv_screen_active() != list_screen) {
				switch_screen(list_screen);
			}
			return;
		}
		if (job_is_list(result_kind)) {
			// A further page: the rows already there stay, and scrolling does
			// not ask again by itself -- against a server that is down that
			// would be a request per scroll event.
			page_more = false;
			toast_error(result_error);
			return;
		}
		gui_notify_popup(result_error);
		return;
	}
	if (job_is_list(result_kind) && result_from == 0) {
		streamerror_hide(list_error);
	}

	switch (result_kind) {
	case JOB_LOGIN:
		// The favourites mirror belongs to the account just signed in to.
		subsonicsync_forget();
		rebuild_home();
		switch_screen(subsonic_screen);
		toast_success("signed_in");
		break;

	case JOB_PLAY:
		// Nothing: the worker already opened the player the instant the music
		// started. This point is reached much later -- after the next track has
		// been prefetched too, which takes minutes -- and reopening it now
		// would tear away whatever page the user has opened meanwhile.
		break;

	default:
		fill_list(result_from);
		// A full last page probably means there is another. A short one means
		// the list really ended, and nothing more is requested.
		page_loading = false;
		page_more = (result_count - result_from == SUB_PAGE_LIMIT) && result_count < SUB_MAX_HELD;
		if (lv_screen_active() != list_screen && result_from == 0) {
			switch_screen(list_screen);
		}
		break;
	}
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

static void play_started_async(void *user) {
	(void)user;
	player_refresh_now_playing();
	switch_screen(player_screen);
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

// The whole queued list, with each track's key beside it.
typedef struct {
	subsonic_track_t track;
	char key[SUBSONICCACHE_KEY_LEN];
} queued_t;

static queued_t *queued_tracks; // SUB_MAX_HELD entries, allocated on first play
static int queued_count;

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
	if (!queued_tracks) {
		queued_tracks = calloc(SUB_MAX_HELD, sizeof(*queued_tracks));
		if (!queued_tracks) {
			snprintf(result_error, sizeof(result_error), "%s", tr("out_of_memory"));
			return;
		}
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
		for (int i = 0; i < queued_count && index < 0; i++) {
			if (strcmp(queued_tracks[i].key, key) == 0) {
				index = i;
			}
		}
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

		queued_t q = queued_tracks[index];
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
		if (job.kind != JOB_PLAY && off == 0) {
			result_count = 0;
			result_title[0] = '\0';
		}

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

		case JOB_PLAY:
			do_play(job.index);
			break;

		default:
			break;
		}

		if (n < 0) {
			result_class = streamerr_describe("subsonic", subsonic_last_error(), result_error, sizeof(result_error));
			n = 0;
		}
		if (job.kind != JOB_PLAY) {
			result_from = off;
			result_count = off + n;
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

static bool job_is_list(job_kind_t kind) { return kind != JOB_PLAY && kind != JOB_LOGIN && kind != JOB_NONE; }

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
	have_current_list = true;
	current_list_job = previous;
	page_more = false;
	page_loading = false;
	start_worker();
	busy_show("loading");
	submit(&previous);
	return true;
}

static void run(const job_t *job, const char *waiting_text) {
	// A job that opens another list while a list is already open: the previous
	// one must be remembered, otherwise the step back skips it.
	if (job_is_list(job->kind)) {
		// Re-reading the SAME list (after removing a favourite, say) is not a
		// step forward: the whole job is compared, not just its kind, otherwise
		// two albums in a row would not stack.
		bool same = have_current_list && current_list_job.kind == job->kind &&
					strcmp(current_list_job.id_text, job->id_text) == 0 &&
					strcmp(current_list_job.text, job->text) == 0 && strcmp(current_list_job.text2, job->text2) == 0;
		if (have_current_list && !same && lv_screen_active() == list_screen && list_history_len < LIST_HISTORY_DEPTH) {
			list_history[list_history_len++] = current_list_job;
		} else if (lv_screen_active() != list_screen) {
			list_history_len = 0; // entering from outside: the history restarts
		}
		current_list_job = *job;
		have_current_list = true;
	}

	// A new list is starting: the old one's pagination must not fire while
	// waiting for it.
	if (job_is_list(job->kind)) {
		page_more = false;
		page_loading = false;
	}

	start_worker();
	// No veil for playback: it starts right away and there is nothing to wait
	// for.
	if (job->kind != JOB_PLAY) {
		busy_show(waiting_text);
	}
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
	// A sideways drag (back, or pulling in the player sheet) also moves this
	// list: that is not a request for more rows.
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	// Within one screen of the bottom is close enough: the request takes a
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
	case JOB_STARRED_TRACKS:
		job.kind = JOB_PLAY;
		job.index = index;
		run(&job, NULL);
		return;

	case JOB_SEARCH_ALBUMS:
	case JOB_STARRED_ALBUMS:
	case JOB_ARTIST_ALBUMS:
	case JOB_ALBUM_LIST:
		job.kind = JOB_ALBUM_TRACKS;
		snprintf(job.id_text, sizeof(job.id_text), "%s", result_albums[index].id);
		snprintf(job.text, sizeof(job.text), "%s", result_albums[index].title);
		run(&job, "loading");
		return;

	case JOB_SEARCH_ARTISTS:
	case JOB_ARTISTS:
		job.kind = JOB_ARTIST_ALBUMS;
		snprintf(job.id_text, sizeof(job.id_text), "%s", result_artists[index].id);
		snprintf(job.text, sizeof(job.text), "%s", result_artists[index].name);
		run(&job, "loading");
		return;

	case JOB_PLAYLISTS:
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
static lv_obj_t *search_clear_btn; // the x that empties the field
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

	// The x on the right edge of the field, as in the local search: clears the
	// query with one tap. Hidden while the field is empty.
	search_clear_btn = lv_btn_create(search_screen);
	lv_obj_set_size(search_clear_btn, 56, 56);
	lv_obj_align(search_clear_btn, LV_ALIGN_TOP_RIGHT, -cfg->padding - 4, top + (62 - 56) / 2);
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
	lv_obj_set_size(field, cfg->screen_width - 2 * cfg->padding, 62);
	lv_obj_set_scrollbar_mode(field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(field, LV_ALIGN_TOP_LEFT, cfg->padding, y);
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
	lv_obj_align(eye, LV_ALIGN_TOP_RIGHT, -cfg->padding - 4, top + 148 + 3);
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
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_18, 0);
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
// ---------------------------------------------------------------------------

static lv_obj_t *home_container;
static lv_obj_t *settings_btn;
static lv_obj_t *search_btn;

// The quality pills: the original file, or a transcode for a slow network.
static const char *const QUALITY_NAMES[SUBSONIC_QUALITY_COUNT] = {"subsonic_original", "qobuz_mp3_320_kbps",
																	"subsonic_mp3_192", "subsonic_opus_128"};

// The gear and the magnifier appear only once signed in.
static void update_corner_buttons(void) {
	bool visible = subsonic_logged_in();
	lv_obj_t *const buttons[] = {settings_btn, search_btn};
	for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
		if (buttons[i]) {
			lv_obj_set_hidden(buttons[i], !visible);
		}
	}
}

static lv_obj_t *corner_button(gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph, lv_event_cb_t cb) {
	lv_obj_t *button = lv_btn_create(subsonic_screen);
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
// settings: quality, the account, the cache
// ---------------------------------------------------------------------------

static lv_obj_t *ssettings_screen;
static lv_obj_t *ssettings_container;
static lv_obj_t *quality_pills[SUBSONIC_QUALITY_COUNT];
static lv_obj_t *account_label;

static void quality_refresh(void) {
	int current = subsonic_get_quality();
	for (int i = 0; i < SUBSONIC_QUALITY_COUNT; i++) {
		if (!quality_pills[i]) {
			continue;
		}
		bool on = i == current;
		lv_obj_set_style_bg_color(quality_pills[i], on ? theme()->accent : theme()->surface_pressed, 0);
		lv_obj_set_style_text_color(lv_obj_get_child(quality_pills[i], 0), on ? lv_color_white() : theme()->text_primary,
									0);
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

static void clear_cache_cb(lv_event_t *e) {
	(void)e;
	char message[96];
	snprintf(message, sizeof(message), tr("subsonic_cache_size"), (int)(subsoniccache_bytes() / (1024 * 1024)));
	confirm_show("subsonic_clear_cache", message, "remove", do_clear_cache, NULL);
}

static void open_settings_cb(lv_event_t *e) {
	(void)e;
	quality_refresh();
	char name[SUBSONIC_SERVER_MAX + SUBSONIC_NAME_MAX];
	subsonic_display_name(name, sizeof(name));
	lv_label_set_text(account_label, name);
	switch_screen(ssettings_screen);
}

static void build_settings_page(gui_config_t *cfg) {
	ssettings_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(ssettings_screen, cfg, "subsonic_settings");
	ssettings_container = container;

	// Who and where, so a user with two servers knows which one this is.
	account_label = lv_label_create(container);
	lv_label_set_long_mode(account_label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(account_label, lv_pct(100));
	lv_obj_add_style(account_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(account_label, &font_ui_20, 0);

	lv_obj_t *card = lv_obj_create(container);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 12, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, 20, 0);
	lv_obj_set_style_pad_row(card, 18, 0);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_event_bubble(card, true);
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
	lv_obj_set_scrollable(pills, false);
	lv_obj_set_event_bubble(pills, true);
	lv_obj_set_flex_flow(pills, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(pills, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

	for (int i = 0; i < SUBSONIC_QUALITY_COUNT; i++) {
		lv_obj_t *btn = lv_btn_create(pills);
		lv_obj_set_size(btn, LV_SIZE_CONTENT, 56);
		lv_obj_set_style_pad_hor(btn, 22, 0);
		lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_shadow_width(btn, 0, 0);
		lv_obj_set_style_border_width(btn, 0, 0);
		lv_obj_add_event_cb(btn, quality_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

		lv_obj_t *label = lv_label_create(btn);
		lv_label_set_text(label, tr(QUALITY_NAMES[i]));
		lv_obj_set_style_text_font(label, &font_ui_22, 0);
		lv_obj_center(label);
		quality_pills[i] = btn;
	}

	lv_obj_t *note = lv_label_create(card);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_18, 0);
	lv_label_set_text(note, tr("subsonic_quality_note"));

	settingsrow_add(container, "subsonic_clear_cache", NULL, clear_cache_cb, NULL);
	settingsrow_action(container, "leave", logout_cb, NULL);

	quality_refresh();
	theme_register_refresh(quality_refresh);
	switcher_attach_back_gesture(ssettings_screen);
}

static void simple_job_cb(lv_event_t *e) {
	job_t job = {0};
	job.kind = (job_kind_t)(intptr_t)lv_event_get_user_data(e);
	run(&job, "loading");
}

// The album lists getAlbumList2 serves, one home row each: the type the server
// wants and the row's (translated) name, which is also the list's title.
typedef struct {
	const char *type;
	const char *label;
} album_list_t;

static const album_list_t ALBUM_LISTS[] = {
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

	// The network first: signing in is a conversation with the server. A home
	// server on the same Wi-Fi is the usual case, but it is still the network.
	if (!wifi_is_connected()) {
		update_corner_buttons();
		lv_obj_t *note = lv_label_create(home_container);
		lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(note, lv_pct(100));
		lv_obj_add_style(note, &theme_style_text, 0);
		lv_obj_set_style_text_font(note, &font_ui_22, 0);
		lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_pad_hor(note, 4, 0);
		lv_obj_set_style_pad_top(note, 14, 0);
		lv_label_set_text(note, tr("enable_wi_fi_first"));
		lv_obj_t *row = settingsrow_add(home_container, "wi_fi_settings", NULL, open_wifi_settings_cb, NULL);
		lv_obj_set_style_margin_top(row, 16, 0);
		return;
	}

	if (!subsonic_logged_in()) {
		update_corner_buttons();
		settingsrow_add(home_container, "sign_in", NULL, open_login_cb, NULL);
		lv_obj_t *note = lv_label_create(home_container);
		lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(note, lv_pct(100));
		lv_obj_add_style(note, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(note, &font_ui_18, 0);
		lv_obj_set_style_pad_top(note, 10, 0);
		lv_label_set_text(note, tr("subsonic_login_note"));
		return;
	}

	settingsrow_add(home_container, "artists", NULL, simple_job_cb, (void *)(intptr_t)JOB_ARTISTS);
	for (size_t i = 0; i < sizeof(ALBUM_LISTS) / sizeof(ALBUM_LISTS[0]); i++) {
		settingsrow_add(home_container, ALBUM_LISTS[i].label, NULL, album_list_cb, (void *)&ALBUM_LISTS[i]);
	}
	settingsrow_add(home_container, "playlists", NULL, simple_job_cb, (void *)(intptr_t)JOB_PLAYLISTS);
	settingsrow_add(home_container, "favourite_albums", NULL, simple_job_cb, (void *)(intptr_t)JOB_STARRED_ALBUMS);
	settingsrow_add(home_container, "favourite_tracks", NULL, simple_job_cb, (void *)(intptr_t)JOB_STARRED_TRACKS);

	update_corner_buttons();
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
}

// ---------------------------------------------------------------------------

static void build_list_page(gui_config_t *cfg) {
	list_screen = lv_obj_create(NULL);
	lv_obj_add_event_cb(list_screen, list_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(list_screen, list_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	list_container = settingsrow_page(list_screen, cfg, "subsonic");
	list_title = settingsrow_page_title(list_screen);

	list_empty = lv_label_create(list_screen);
	lv_label_set_text(list_empty, tr("no_results"));
	lv_obj_set_style_text_align(list_empty, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(list_empty, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(list_empty, &font_ui_24, 0);
	lv_obj_align(list_empty, LV_ALIGN_TOP_MID, 0, settingsrow_content_top(cfg) + 100);
	lv_obj_set_hidden(list_empty, true);

	list_error = streamerror_create(list_screen);
	lv_obj_set_width(list_error, cfg->screen_width - 2 * cfg->padding);
	lv_obj_align(list_error, LV_ALIGN_TOP_MID, 0, settingsrow_content_top(cfg) + 20);

	lv_obj_add_event_cb(list_container, list_scrolled_cb, LV_EVENT_SCROLL, NULL);
	switcher_attach_back_gesture(list_screen);
	switcher_set_back_guard(list_screen, list_back_guard);
	switcher_set_back_guard_peek(list_screen, list_back_has_step);
	player_sheet_attach_drag(list_screen, true);
	player_sheet_attach_drag(list_container, true);
}

static void *provider_open_album(const char *album_id, const char *title) {
	return subsonicpage_open_album(album_id, title) ? subsonic_list_screen : NULL;
}

void subsonicpage_init(gui_config_t *cfg) {
	// The mark over the artwork and "Show album" for the rest of the program
	// (provider.h).
	const stream_provider_gui_t provider_gui = {.badge = &icon_subsonic_badge, .open_album = provider_open_album};
	stream_provider_attach_gui("subsonic", &provider_gui);

	subsonic_screen = lv_obj_create(NULL);
	home_container = settingsrow_page(subsonic_screen, cfg, "subsonic");

	settingsrow_title_corner_slots(settingsrow_page_title(subsonic_screen), cfg, 2);
	settings_btn = corner_button(cfg, 0, &icon_music_settings, open_settings_cb);
	search_btn = corner_button(cfg, 1, &icon_search, open_search_cb);

	build_list_page(cfg);
	build_search_page(cfg);
	build_settings_page(cfg);
	build_login_page(cfg);
	build_busy_layer();
	subsoniccache_set_slow_start_cb(on_slow_start);
	device_state_add_prepare_cb(subsonic_prepare_track);

	lv_timer_create(queue_watch_cb, 1000, NULL);
	art_timer = lv_timer_create(art_poll_cb, 120, NULL);
	lv_timer_pause(art_timer);
	art_drop_timer = lv_timer_create(art_drop_cb, ART_DROP_DELAY_MS, NULL);
	lv_timer_pause(art_drop_timer);
	qobuzart_set_root(storage_sd_root());

	subsoniccache_set_root(storage_sd_root());
	rebuild_home();

	lv_obj_add_event_cb(subsonic_screen, home_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(subsonic_screen);
	player_sheet_attach_drag(subsonic_screen, true);
	player_sheet_attach_drag(home_container, true);
}

#include "qobuzpage.h"

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
#include "src/system/streaming/qobuz.h"
#include "src/system/streaming/qobuzcache.h"
#include "src/system/streaming/streamturn.h"
#include "src/system/streaming/qobuzsync.h"
#include "src/system/device/system.h"
#include "src/system/core/utils.h"
#include "src/system/net/wifi.h"

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
} job_kind_t;

typedef struct {
	job_kind_t kind;
	char text[256];	   // search terms, or user name
	char text2[128];   // password
	char id_text[40];  // album identifier
	long id;		   // numeric identifier
	int index;		   // which track of the list playback starts from
	int offset;		   // first entry of this page: 0 = new list
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
// (see qobuzcache_start), so there is no wait to show, and a veil raised here
// would stay up for the whole prefetch of the following tracks.
// ---------------------------------------------------------------------------

// The wait indicator: a spinner in the middle of the screen, no text --
// understood without reading and needing no translation. The text callers pass
// is ignored.
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
			 ok ? "" : (error && error[0] ? error : tr("qobuz_change_refused")));
	gui_post(sync_result_async, user);
}

// Adding to favourites does not change the list on screen (the user is among
// search results), so a success toast is all that is reported.
static void row_added_done(bool ok, const char *error, void *user) {
	(void)user;
	snprintf(sync_message, sizeof(sync_message), "%s",
			 ok ? "" : (error && error[0] ? error : tr("qobuz_change_refused")));
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
	qobuzsync_favorite_toggle(id, true, row_added_done, NULL);
}

static void do_track_remove(void *user) {
	(void)user;
	if (menu_index < 0 || menu_index >= result_count) {
		return;
	}
	qobuzsync_favorite_toggle(result_tracks[menu_index].id, false, row_sync_done, (void *)(intptr_t)1);
}

static void do_album_add(void *user) {
	(void)user;
	if (menu_index < 0 || menu_index >= result_count) {
		return;
	}
	qobuzsync_album_favorite(result_albums[menu_index].id_text, true, row_added_done, NULL);
}

static void do_album_remove(void *user) {
	(void)user;
	if (menu_index < 0 || menu_index >= result_count) {
		return;
	}
	qobuzsync_album_favorite(result_albums[menu_index].id_text, false, row_sync_done, (void *)(intptr_t)1);
}

static void do_playlist_delete(void *user) {
	(void)user;
	if (menu_index < 0 || menu_index >= result_count) {
		return;
	}
	qobuzsync_playlist_remove(result_playlists[menu_index].id, row_sync_done, (void *)(intptr_t)1);
}

// The two destructive actions go through a confirmation: an accidental tap on
// the ellipsis must not delete a playlist.
static void ask_track_remove(void *user) {
	(void)user;
	if (menu_index < 0 || menu_index >= result_count) {
		return;
	}
	char message[QOBUZ_TITLE_MAX + 64];
	snprintf(message, sizeof(message), tr("will_no_longer_be_one_of"),
			 result_tracks[menu_index].title);
	confirm_show("remove_from_favourites_2", message, "remove", do_track_remove, NULL);
}

static void ask_album_remove(void *user) {
	(void)user;
	if (menu_index < 0 || menu_index >= result_count) {
		return;
	}
	char message[QOBUZ_TITLE_MAX + 64];
	snprintf(message, sizeof(message), tr("will_no_longer_be_one_of_2"),
			 result_albums[menu_index].title);
	confirm_show("remove_from_favourites_2", message, "remove", do_album_remove, NULL);
}

static void ask_playlist_delete(void *user) {
	(void)user;
	if (menu_index < 0 || menu_index >= result_count) {
		return;
	}
	char message[QOBUZ_TITLE_MAX + 64];
	snprintf(message, sizeof(message), tr("qobuz_will_be_deleted_from_your_qobuz"),
			 result_playlists[menu_index].name);
	confirm_show("delete_the_playlist", message, "delete", do_playlist_delete, NULL);
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
		if (qobuzsync_is_favorite(result_tracks[index].id)) {
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

// Side of a row thumbnail, in pixels.
#define ROW_THUMB 60

// Row cover art.
//
// The loader has twelve slots and a list can hold far more rows than that, so
// slots are a shared pool and not assigned seats: they go to the rows currently
// visible and move to new ones as the list scrolls.
static lv_obj_t *row_thumbs[QOBUZ_MAX_HELD];
static char row_cover_url[QOBUZ_MAX_HELD][QOBUZART_URL_MAX];
static cover_image_t row_images[QOBUZ_MAX_HELD];
static bool row_has_image[QOBUZ_MAX_HELD];

// This row already asked for its cover and did not get one: dead URL, image
// that fails to decode, host that does not answer. The reload pass restarts
// every time a slot frees up, so without this one broken row would be asked for
// forever and keep a slot from the rows that can be served.
static bool row_art_failed[QOBUZ_MAX_HELD];

// The token that claims the cover slots: only its address matters, never its
// value. See qobuzart.h.
static const char qobuz_art_owner;

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
	qobuzart_release(&qobuz_art_owner, slot);
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
		if (row < 0 || row >= QOBUZ_MAX_HELD || !row_thumbs[row]) {
			continue;
		}
		cover_image_t image;
		bool finished = false;
		if (!qobuzart_take(&qobuz_art_owner, slot, &image, &finished)) {
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
	for (uint32_t c = 0; c < children && c < QOBUZ_MAX_HELD; c++) {
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
		qobuzart_request(&qobuz_art_owner, free_slot, row_cover_url[row], ROW_THUMB);
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
	for (int row = 0; row < QOBUZ_MAX_HELD; row++) {
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
	for (int row = 0; row < QOBUZ_MAX_HELD; row++) {
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
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	// Gestures (back, and pulling in the player sheet) often start on a row,
	// and a button keeps the press to itself: without this neither drag works
	// on these lists.
	lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

	// The cover, where there is a URL for one. It starts as the grey note and
	// becomes the real image once the thread has downloaded it: the list is
	// readable immediately, without waiting for the network.
	if (cover_url && cover_url[0] && index < QOBUZ_MAX_HELD) {
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

		switch (result_kind) {
		case JOB_SEARCH_TRACKS:
		case JOB_ALBUM_TRACKS:
		case JOB_PLAYLIST_TRACKS:
		case JOB_FAV_TRACKS: {
			const qobuz_track_t *t = &result_tracks[i];
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
			const qobuz_album_t *a = &result_albums[i];
			snprintf(detail, sizeof(detail), "%s%s%s", a->artist, a->released[0] ? " \xC2\xB7 " : "", a->released);
			make_row(a->title, detail, a->cover, false, i);
			break;
		}
		case JOB_SEARCH_ARTISTS: {
			const qobuz_artist_t *a = &result_artists[i];
			snprintf(detail, sizeof(detail), "%d album", a->album_count);
			make_row(a->name, a->album_count > 0 ? detail : "", a->image, false, i);
			break;
		}
		case JOB_USER_PLAYLISTS: {
			const qobuz_playlist_t *p = &result_playlists[i];
			snprintf(detail, sizeof(detail), "%s \xC2\xB7 %d", p->owner, p->track_count);
			make_row(p->name, detail, p->image, false, i);
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
		lv_obj_remove_flag(list_empty, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(list_empty, LV_OBJ_FLAG_HIDDEN);
	}

	lv_label_set_text(list_title, result_title);
}

// ---------------------------------------------------------------------------
// what comes back from the worker, on the GUI thread
// ---------------------------------------------------------------------------

static void job_done_cb(void *user) {
	(void)user;
	busy_hide();

	if (result_error[0]) {
		gui_notify_popup(result_error);
		page_loading = false;
		return;
	}

	switch (result_kind) {
	case JOB_LOGIN:
		rebuild_home();
		switch_screen(qobuz_screen);
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
		page_more = (result_count - result_from == QOBUZ_PAGE_LIMIT) && result_count < QOBUZ_MAX_HELD;
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

static void play_started_async(void *user) {
	(void)user;
	player_refresh_now_playing();
	switch_screen(player_screen);
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

// The whole queued list: needed by the prefetch thread, and needed by on-demand
// requests -- when playback reaches a track that has not downloaded yet, this
// is where what to ask Qobuz for is found.
static qobuz_track_t queued_tracks[QOBUZ_MAX_HELD];
static int queued_count;

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
		snprintf(result_error, sizeof(result_error), "%s", why && why[0] ? why : tr("download_failed"));
		return;
	}

	char path[512];
	if (!qobuzcache_start(track->id, url, mime, track->duration, path, sizeof(path))) {
		// The cache now reports what went wrong: no card, no network, card
		// full. The generic "download failed" is left only for the case where
		// even it does not know.
		const char *why = qobuzcache_last_error();
		snprintf(result_error, sizeof(result_error), "%s", why && why[0] ? why : tr("download_failed"));
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
	if (!library_queue && current >= 0 && queued_count > 0) {
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
		for (int i = 0; i < queued_count && index < 0; i++) {
			if (queued_tracks[i].id == id) {
				index = i;
			}
		}

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

		qobuz_track_t track = queued_tracks[index];
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
		if (job.kind != JOB_PLAY && off == 0) {
			result_count = 0;
			result_title[0] = '\0';
		}

		int n = 0;
		switch (job.kind) {
		case JOB_LOGIN:
			if (!qobuz_login(job.text, job.text2)) {
				snprintf(result_error, sizeof(result_error), "%s", qobuz_last_error());
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

		default:
			break;
		}

		if (n < 0) {
			snprintf(result_error, sizeof(result_error), "%s", qobuz_last_error());
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
		bool same = have_current_list && current_list_job.kind == job->kind && current_list_job.id == job->id &&
					strcmp(current_list_job.id_text, job->id_text) == 0 &&
					strcmp(current_list_job.text, job->text) == 0;
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
		job.id = result_playlists[index].id;
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

	settingsrow_title(search_screen, cfg, "qobuz_search_qobuz");
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

	settingsrow_title(login_screen, cfg, "qobuz_sign_in_to_qobuz");
	int top = settingsrow_content_top(cfg);

	user_field = make_field(login_screen, cfg, "qobuz_username_or_email", top);
	password_field = make_field(login_screen, cfg, "password", top + 78);

	// The password as on any modern system: dots from the first keystroke, and
	// the eye to reveal the whole of it. Zero show time means the character is
	// never readable, not even for an instant.
	keyboard_style_password(password_field, 0);
	lv_obj_set_style_pad_right(password_field, 60, 0);

	lv_obj_t *eye = lv_btn_create(login_screen);
	lv_obj_set_size(eye, 56, 56);
	lv_obj_align(eye, LV_ALIGN_TOP_RIGHT, -cfg->padding - 4, top + 78 + 3);
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
	lv_obj_align(note, LV_ALIGN_TOP_LEFT, cfg->padding, top + 156);
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_18, 0);
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
// ---------------------------------------------------------------------------

static lv_obj_t *home_container;
static lv_obj_t *settings_btn;
static lv_obj_t *search_btn;

static const int QUALITY_VALUES[4] = {QOBUZ_FORMAT_MP3, QOBUZ_FORMAT_CD, QOBUZ_FORMAT_HIRES96,
									  QOBUZ_FORMAT_HIRES192};

// Full names rather than shorthand: there is room in the menu, and "24/192"
// alone does not tell anyone that a FLAC is behind it.
static const char *const QUALITY_NAMES[4] = {"qobuz_mp3_320_kbps", "qobuz_flac_16_44_1", "qobuz_flac_24_96", "qobuz_flac_24_192"};

// The two corner buttons: the gear (which opens the Qobuz settings) and the
// magnifier. They appear only once signed in, because before that there is
// nothing to configure and nothing to search.
static void update_corner_buttons(void) {
	bool visible = qobuz_configured() && qobuz_logged_in();
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

// Same shape as the Music corner buttons: 56 px, transparent, aligned top
// right and stepped leftwards one slot at a time.
static lv_obj_t *corner_button(gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph, lv_event_cb_t cb) {
	lv_obj_t *button = lv_btn_create(qobuz_screen);
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
// Qobuz settings
//
// The corner holds the gear (as on the Music page) and the magnifier; this page
// holds what is done once a year: quality as pills, like every other multiple
// choice in this interface, and sign-out as a row.
// ---------------------------------------------------------------------------

static lv_obj_t *qsettings_screen;
static lv_obj_t *quality_pills[4];

static void quality_refresh(void) {
	int current = qobuz_get_format();
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

static void open_settings_cb(lv_event_t *e) {
	(void)e;
	quality_refresh();
	switch_screen(qsettings_screen);
}

static void build_settings_page(gui_config_t *cfg) {
	qsettings_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(qsettings_screen, cfg, "qobuz_settings");

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

		// Nearly symbols, but not quite: the decimal separator is
		// locale-dependent ("FLAC 16/44.1" in English), so these go through
		// tr() like everything else.
		lv_obj_t *label = lv_label_create(btn);
		lv_label_set_text(label, tr(QUALITY_NAMES[i]));
		lv_obj_set_style_text_font(label, &font_ui_22, 0);
		lv_obj_center(label);

		quality_pills[i] = btn;
	}

	settingsrow_action(container, "leave", logout_cb, NULL);

	quality_refresh();
	theme_register_refresh(quality_refresh);
	switcher_attach_back_gesture(qsettings_screen);
}

static void simple_job_cb(lv_event_t *e) {
	job_t job = {0};
	job.kind = (job_kind_t)(intptr_t)lv_event_get_user_data(e);
	run(&job, "loading");
}

// The "Wi-Fi settings" row of the no-network notice, as on the transfer page:
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

	if (!qobuz_configured()) {
		update_corner_buttons();
		lv_obj_t *note = lv_label_create(home_container);
		lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(note, lv_pct(100));
		lv_obj_add_style(note, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(note, &font_ui_20, 0);
		// Not an "error": it is fixable, and the line says how.
		lv_label_set_text(note, tr("qobuz_not_configured"));
		return;
	}

	// The network comes before the login, and not the other way round: signing
	// in is a conversation with Qobuz's servers, so with no network the "Sign
	// in" row could only fail. The notice says what is actually missing.
	if (!wifi_is_connected()) {
		update_corner_buttons();
		// The same notice as the Wi-Fi transfer page: centred message, with the
		// "Wi-Fi settings" row below leading to where it is fixed.
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

	if (!qobuz_logged_in()) {
		update_corner_buttons();
		settingsrow_add(home_container, "sign_in", NULL, open_login_cb, NULL);
		return;
	}

	// No "Search" row: it is the magnifier in the corner, where it sits on
	// every other searchable page.
	settingsrow_add(home_container, "favourite_tracks", NULL, simple_job_cb, (void *)(intptr_t)JOB_FAV_TRACKS);
	settingsrow_add(home_container, "favourite_albums", NULL, simple_job_cb, (void *)(intptr_t)JOB_FAV_ALBUMS);
	settingsrow_add(home_container, "my_playlists", NULL, simple_job_cb, (void *)(intptr_t)JOB_USER_PLAYLISTS);
	settingsrow_add(home_container, "new_releases", NULL, simple_job_cb, (void *)(intptr_t)JOB_FEATURED);

	update_corner_buttons();
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
	lv_obj_add_event_cb(list_screen, list_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(list_screen, list_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	list_container = settingsrow_page(list_screen, cfg, "qobuz");
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
	// The leftward drag that pulls in the player sheet, as on every other
	// browsable page.
	player_sheet_attach_drag(list_screen, true);
	player_sheet_attach_drag(list_container, true);
}

void qobuzpage_init(gui_config_t *cfg) {


	qobuz_screen = lv_obj_create(NULL);
	home_container = settingsrow_page(qobuz_screen, cfg, "qobuz");

	// Two buttons top right, like the Music gear: settings and search. The title
	// must be told the corner is taken, otherwise its text runs under them.
	settingsrow_title_corner_slots(settingsrow_page_title(qobuz_screen), cfg, 2);
	settings_btn = corner_button(cfg, 0, &icon_music_settings, open_settings_cb);
	search_btn = corner_button(cfg, 1, &icon_search, open_search_cb);

	build_list_page(cfg);
	build_search_page(cfg);
	build_settings_page(cfg);
	build_login_page(cfg);
	build_busy_layer();
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

	qobuzcache_set_root(storage_sd_root());
	rebuild_home();

	lv_obj_add_event_cb(qobuz_screen, home_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(qobuz_screen);
	player_sheet_attach_drag(qobuz_screen, true);
	player_sheet_attach_drag(home_container, true);
}

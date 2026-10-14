#include "search.h"

#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/library/listacts.h"
#include "src/gui/library/medialist.h"
#include "src/gui/library/rowart.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/sx_card.h"
#include "src/gui/shell/sx_frame.h"
#include "src/gui/shell/sx_row.h"
#include "src/gui/shell/sx_sheet.h"
#include "src/gui/shell/sx_styles.h"
#include "src/gui/shell/sx_tokens.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/library/covertone_watch.h"
#include "src/system/library/library.h"
#include "src/system/library/playlists.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"

lv_obj_t *search_screen;

// ---------------------------------------------------------------------------
// The search page (UI 2.0)
//
// Under the header: the field (60 px, round, ⌕ at its left and ✕ at its right
// while it holds something). The query is typed in full and run by the
// keyboard's search key. Then, in one scrolling list: a card of filter pills
// -- All, Tracks · N, Albums · N, Artists · N -- and the hits grouped under
// "TRACKS · N", "ALBUMS · N", "ARTISTS · N" (All), or only the group picked.
// With no query the recent searches stand there instead, as pills that run
// again; a query with no hits gets a state saying so, with "Clear".
//
// The rows are the library lists' rows and do what they do (the handoff's
// gesture matrix): a track plays at a tap, with the track hits as the queue,
// and the page stays; pulled right it plays next, pulled left it goes to the
// end, its star stars it, held it brings up its sheet. An album opens, takes
// the same swipes and has its sheet; an artist only opens.
//
// A group holds row ids, not rows (library_index_t): a query that matches most
// of the card costs four bytes a hit. What is drawn is a pool of rows that
// follows the scroll, bound to the hits under it, and three group headings.
// Thumbnails come from the covers' cache for the rows on screen (rowart.h).
// ---------------------------------------------------------------------------

#define FIELD_H 60
#define FIELD_GAP 8
#define FIELD_ICON_W 56	 // the ⌕ at the field's left
#define FIELD_CLEAR_W 60 // the ✕ at its right
#define SEARCH_KEYBOARD_H 316	 // tall enough for honest fingertip-sized keys
#define GROUP_H 44		 // a group's heading above its rows
#define RESULTS_BOTTOM 12
#define RECENT_MAX 6
#define STATE_DISC 84
#define STATE_BUTTON_H 64

// Rows on screen at once on the tallest panel, plus one beyond each edge.
#define POOL 12

// How often the rows' pictures are looked for while any are missing, and how
// many card reads one pass may make (the library lists' figures).
#define THUMB_POLL_MS 80
#define THUMB_LOADS_PER_PASS 3

// Same wait as the streaming pages, and for the same reason: a hit opens an
// album and coming straight back must not cost a dozen reads off the card.
#define ART_DROP_DELAY_MS 20000

typedef enum {
	CAT_TRACKS = 0,
	CAT_ALBUMS,
	CAT_ARTISTS,
	CAT_COUNT,
} category_t;

// The pills: everything, or one group.
#define FILTER_ALL CAT_COUNT

static const library_list_t CAT_KIND[CAT_COUNT] = {LIBRARY_LIST_TRACKS, LIBRARY_LIST_ALBUMS, LIBRARY_LIST_ARTISTS};
static const char *const CAT_PILL[CAT_COUNT] = {"search_tracks_2", "albums", "artists"};
static const char *const CAT_HEAD[CAT_COUNT] = {"lists_head_tracks", "lists_head_albums", "lists_head_artists"};

static gui_config_t *page_cfg;
static lv_obj_t *field;
static lv_obj_t *field_icon;
static lv_obj_t *clear_btn;
static keyboard_t *keyboard;
static int content_top; // under the field
static int pitch;		// a row and the gap under it, for the Large rows setting at the last search

// The results: one scroller, a body as tall as every hit shown, the pills
// card at its top, the headings and the pool on it.
static lv_obj_t *results;
static lv_obj_t *body;
static lv_obj_t *pills_card;
static lv_obj_t *pill[CAT_COUNT + 1];
static lv_obj_t *heading[CAT_COUNT];

// No query: the recent searches. No hits: the state.
static lv_obj_t *recent_box;
static lv_obj_t *recent_pills;
static lv_obj_t *nothing_box;

// The query the results answer, the hits of each group and which are shown.
static char searched[256];
static library_index_t *cat_ix[CAT_COUNT];
static int cat_count[CAT_COUNT];
static int filter = FILTER_ALL;

// Where each group shown starts on the body (its heading), -1 for one not
// shown; and where the groups start (under the pills card).
static int group_top[CAT_COUNT];
static int groups_top;

typedef struct {
	lv_obj_t *row;
	int cat; // -1: unbound
	int index;
	char name[256];	 // a track's title, an album's value, an artist
	char path[512];	 // a track's file
	char artist[256];
	rowart_t art;
	bool picture_shown;
} row_t;

static row_t rows[POOL];
static lv_timer_t *thumb_timer;
static lv_timer_t *art_drop_timer;
static char playing_path[512];
static bool art_dropped; // the pictures went while the page was away

static void run_search(const char *query, bool keep_place);
static void window_update(void);
static void recent_show(void);

// ---------------------------------------------------------------------------
// Recent searches ([library] search_recent_0..5, newest first)
// ---------------------------------------------------------------------------

static char recent[RECENT_MAX][128];
static int recent_count = -1; // -1 until read

static void recent_load(void) {
	if (recent_count >= 0) {
		return;
	}
	recent_count = 0;
	for (int i = 0; i < RECENT_MAX; i++) {
		char key[24];
		snprintf(key, sizeof(key), "search_recent_%d", i);
		const char *q = config_get("library", key, "");
		if (q && q[0]) {
			snprintf(recent[recent_count++], sizeof(recent[0]), "%s", q);
		}
	}
}

static void recent_save(void) {
	for (int i = 0; i < RECENT_MAX; i++) {
		char key[24];
		snprintf(key, sizeof(key), "search_recent_%d", i);
		config_set("library", key, i < recent_count ? recent[i] : "");
	}
	config_save();
}

// The query to the front of the list, once.
static void recent_note(const char *query) {
	recent_load();
	if (!query || !query[0] || strlen(query) >= sizeof(recent[0])) {
		return;
	}
	if (recent_count > 0 && strcmp(recent[0], query) == 0) {
		return; // already first: nothing to write
	}
	int at = recent_count < RECENT_MAX ? recent_count : RECENT_MAX - 1;
	for (int i = 0; i < recent_count; i++) {
		if (strcmp(recent[i], query) == 0) {
			at = i;
			break;
		}
	}
	if (at == recent_count) {
		recent_count++;
	}
	for (int i = at; i > 0; i--) {
		memcpy(recent[i], recent[i - 1], sizeof(recent[0]));
	}
	snprintf(recent[0], sizeof(recent[0]), "%s", query);
	recent_save();
}

// ---------------------------------------------------------------------------
// Where things are on the body
// ---------------------------------------------------------------------------

static bool group_shown(int cat) { return cat_count[cat] > 0 && (filter == FILTER_ALL || filter == cat); }

static int rows_top(int cat) { return group_top[cat] + GROUP_H; }

// Lays the groups out under the pills card and sizes the body to hold them.
static void layout_groups(void) {
	lv_obj_update_layout(pills_card);
	groups_top = lv_obj_get_height(pills_card) + SX_CARD_GAP;
	int y = groups_top;
	for (int c = 0; c < CAT_COUNT; c++) {
		if (!group_shown(c)) {
			group_top[c] = -1;
			lv_obj_set_hidden(heading[c], true);
			continue;
		}
		group_top[c] = y;
		char text[96];
		snprintf(text, sizeof(text), cat_count[c] >= LIBRARY_SEARCH_MAX ? "%s \xC2\xB7 %d+" : "%s \xC2\xB7 %d",
				 tr(CAT_HEAD[c]), cat_count[c]);
		lv_label_set_text(heading[c], text);
		lv_obj_set_y(heading[c], y + GROUP_H - lv_font_get_line_height(&font_ui_17) - 8);
		lv_obj_set_hidden(heading[c], false);
		y += GROUP_H + cat_count[c] * pitch;
	}
	lv_obj_set_height(body, y);
}

// ---------------------------------------------------------------------------
// The rows
// ---------------------------------------------------------------------------

static row_t *row_live(void *user) {
	row_t *r = user;
	return r && r->cat >= 0 && r->cat < CAT_COUNT && r->index < cat_count[r->cat] ? r : NULL;
}

static void row_mark_playing(row_t *r) {
	sx_row_set_playing(r->row, r->cat == CAT_TRACKS && playing_path[0] && strcmp(r->path, playing_path) == 0);
}

static void playing_refresh(void) {
	device_state_t state;
	device_state_get(&state);
	snprintf(playing_path, sizeof(playing_path), "%s", state.current_file);
}

void search_notify_now_playing(void) {
	if (!rows[0].row) {
		return;
	}
	playing_refresh();
	for (int i = 0; i < POOL; i++) {
		if (rows[i].cat >= 0) {
			row_mark_playing(&rows[i]);
		}
	}
}

static void row_unbind(row_t *r) {
	r->cat = -1;
	r->index = -1;
	r->name[0] = r->path[0] = r->artist[0] = '\0';
	lv_obj_set_hidden(r->row, true);
}

static void row_bind(row_t *r, int cat, int index, const char *name, const char *path, const char *artist) {
	r->cat = cat;
	r->index = index;
	snprintf(r->name, sizeof(r->name), "%s", name ? name : "");
	snprintf(r->path, sizeof(r->path), "%s", path && cat == CAT_TRACKS ? path : "");
	snprintf(r->artist, sizeof(r->artist), "%s", artist ? artist : "");

	if (lv_obj_get_style_height(r->row, LV_PART_MAIN) != pitch - SX_ROW_GAP) {
		sx_row_set_compact(r->row, false); // Large rows changed since it was made
	}
	lv_obj_set_y(r->row, rows_top(cat) + index * pitch);
	lv_obj_set_hidden(r->row, false);

	// An album comes named by its value (name and key, see library.h).
	char shown[256];
	library_album_title(r->name, shown, sizeof(shown));
	sx_row_set_title(r->row, shown);

	if (cat == CAT_ARTISTS) {
		char letter[8];
		listacts_first_letter(r->name, letter, sizeof(letter));
		sx_row_set_letter(r->row, letter, listacts_name_tone(r->name), true);
		sx_row_set_subtitle(r->row, "", NULL, NULL);
		sx_row_set_trail(r->row, SX_ROW_TRAIL_CHEVRON);
		sx_row_set_swipes(r->row, false, false);
		r->picture_shown = false;
	} else {
		if (cat == CAT_ALBUMS) {
			rowart_bind(&r->art, r->name);
		} else {
			char album[300];
			// Outside the window callback: this asks the index (rowart.h).
			if (!rowart_album_of_track(r->path, album, sizeof(album))) {
				album[0] = '\0';
			}
			rowart_bind(&r->art, album);
		}
		r->picture_shown = rowart_picture(&r->art) != NULL;
		sx_row_set_thumb(r->row, rowart_picture(&r->art), rowart_tone(&r->art));
		if (cat == CAT_TRACKS) {
			char badge[24], length[16], badge_text[32] = "";
			listacts_track_badge(r->path, badge, sizeof(badge), length, sizeof(length));
			if (badge[0]) {
				snprintf(badge_text, sizeof(badge_text), r->artist[0] ? "\xC2\xB7 %s" : "%s", badge);
			}
			sx_row_set_subtitle(r->row, r->artist, badge_text, length);
			sx_row_set_starred(r->row, library_fav_contains(r->path));
			sx_row_set_trail(r->row, SX_ROW_TRAIL_STAR);
		} else {
			sx_row_set_subtitle(r->row, r->artist, NULL, NULL);
			sx_row_set_trail(r->row, SX_ROW_TRAIL_CHEVRON);
		}
		sx_row_set_swipes(r->row, true, true);
	}
	row_mark_playing(r);
}

// One window of a group's hits, read out of the index.
typedef struct {
	char name[256];
	char path[512];
	char artist[256];
} hit_t;

typedef struct {
	hit_t *hits;
	int count;
	int max;
} window_ctx_t;

static bool window_cb(const char *name, const char *path, const char *artist, void *user) {
	window_ctx_t *ctx = user;
	if (ctx->count >= ctx->max) {
		return false;
	}
	hit_t *hit = &ctx->hits[ctx->count++];
	snprintf(hit->name, sizeof(hit->name), "%s", name ? name : "");
	snprintf(hit->path, sizeof(hit->path), "%s", path ? path : "");
	snprintf(hit->artist, sizeof(hit->artist), "%s", artist ? artist : "");
	return true;
}

static bool row_holds(int cat, int index) {
	for (int i = 0; i < POOL; i++) {
		if (rows[i].cat == cat && rows[i].index == index) {
			return true;
		}
	}
	return false;
}

// Binds the pool to the hits under the scroll position. A row keeps the hit it
// shows for as long as that hit stays in view, so scrolling by one row rebinds
// one row, not the pool.
static void window_update(void) {
	if (!searched[0] || lv_obj_is_hidden(results)) {
		return;
	}
	int y0 = (int)lv_obj_get_scroll_y(results) - pitch;
	int y1 = (int)lv_obj_get_scroll_y(results) + (int)lv_obj_get_height(results) + pitch;

	// What should be bound: per group, the range of hits under the view.
	int first[CAT_COUNT], last[CAT_COUNT];
	int wanted = 0;
	for (int c = 0; c < CAT_COUNT; c++) {
		first[c] = last[c] = 0;
		if (group_top[c] < 0) {
			continue;
		}
		int top = rows_top(c);
		int f = y0 > top ? (y0 - top) / pitch : 0;
		int l = y1 > top ? (y1 - top) / pitch + 1 : 0;
		if (l > cat_count[c]) {
			l = cat_count[c];
		}
		if (f >= l) {
			continue;
		}
		if (wanted + (l - f) > POOL) {
			l = f + (POOL - wanted);
		}
		first[c] = f;
		last[c] = l;
		wanted += l - f;
	}

	// The rows no longer in view are free.
	for (int i = 0; i < POOL; i++) {
		row_t *r = &rows[i];
		if (r->cat >= 0 && (r->index < first[r->cat] || r->index >= last[r->cat] || group_top[r->cat] < 0)) {
			row_unbind(r);
		}
	}

	// The hits that have no row yet get one, a group's window read at once.
	static hit_t hits[POOL];
	for (int c = 0; c < CAT_COUNT; c++) {
		if (last[c] <= first[c]) {
			continue;
		}
		bool missing = false;
		for (int k = first[c]; k < last[c] && !missing; k++) {
			missing = !row_holds(c, k);
		}
		if (!missing) {
			continue;
		}
		window_ctx_t ctx = {hits, 0, POOL};
		library_index_window(cat_ix[c], first[c], last[c] - first[c], window_cb, &ctx);
		if (ctx.count < last[c] - first[c]) {
			// The handle is from before a rescan or a card change: the hits
			// are asked for again, and the list comes back where it was.
			if (library_index_stale(cat_ix[c])) {
				run_search(searched, true);
			}
			return;
		}
		for (int k = first[c]; k < last[c]; k++) {
			if (row_holds(c, k)) {
				continue;
			}
			for (int i = 0; i < POOL; i++) {
				if (rows[i].cat < 0) {
					const hit_t *h = &hits[k - first[c]];
					row_bind(&rows[i], c, k, h->name, h->path, h->artist);
					break;
				}
			}
		}
	}
	lv_timer_resume(thumb_timer);
}

static void scroll_cb(lv_event_t *e) {
	(void)e;
	window_update();
}

// ---------------------------------------------------------------------------
// Pictures
// ---------------------------------------------------------------------------

static bool row_on_screen(const row_t *r) {
	if (r->cat < 0 || lv_obj_is_hidden(r->row)) {
		return false;
	}
	int scroll = (int)lv_obj_get_scroll_y(results);
	int top = rows_top(r->cat) + r->index * pitch;
	return top + pitch > scroll && top < scroll + (int)lv_obj_get_height(results);
}

static void thumbs_update(void) {
	if (lv_screen_active() != search_screen || !searched[0]) {
		return;
	}
	// Nothing touches the card while the list moves: the timer comes back for
	// it once it has settled.
	if (rowart_list_busy(results) || switcher_nav_anim_running()) {
		lv_timer_resume(thumb_timer);
		return;
	}
	int loads = 0;
	bool pending = false;
	for (int i = 0; i < POOL; i++) {
		row_t *r = &rows[i];
		if (r->cat == CAT_ARTISTS || !row_on_screen(r)) {
			continue;
		}
		if (!rowart_wanted(&r->art)) {
			if (!r->picture_shown && rowart_picture(&r->art)) {
				sx_row_set_thumb(r->row, rowart_picture(&r->art), rowart_tone(&r->art));
				r->picture_shown = true;
			}
			continue;
		}
		if (loads >= THUMB_LOADS_PER_PASS) {
			pending = true;
			continue;
		}
		loads++;
		if (rowart_load(&r->art)) {
			sx_row_set_thumb(r->row, rowart_picture(&r->art), rowart_tone(&r->art));
			r->picture_shown = rowart_picture(&r->art) != NULL;
		}
	}
	if (pending) {
		lv_timer_resume(thumb_timer);
	}
}

static void thumb_timer_cb(lv_timer_t *timer) {
	lv_timer_pause(timer); // thumbs_update resumes it while work is left
	thumbs_update();
}

static void covers_changed(void) {
	if (thumb_timer && lv_screen_active() == search_screen) {
		lv_timer_resume(thumb_timer);
	}
}

// Off the page for a while: the pictures' buffers go back, and the rows show
// their plaques until they are read again.
static void art_drop_cb(lv_timer_t *timer) {
	lv_timer_pause(timer);
	for (int i = 0; i < POOL; i++) {
		rowart_free(&rows[i].art);
		rows[i].picture_shown = false;
	}
	art_dropped = true; // the rows are bound again when the page comes back
	// And back to the kernel, not merely to the arena: the buffers are under
	// the mmap threshold main.c sets.
	malloc_trim(0);
}

// ---------------------------------------------------------------------------
// The gestures
// ---------------------------------------------------------------------------

static void row_tap_cb(lv_obj_t *obj, void *user) {
	(void)obj;
	row_t *r = row_live(user);
	if (!r) {
		return;
	}
	switch (r->cat) {
	case CAT_TRACKS: {
		// The queue is the whole list of track hits, so next and previous walk
		// the search the way they walk any library list. The page stays: the
		// mini player comes up with the track.
		library_index_t *ix = cat_ix[CAT_TRACKS];
		if (ix && library_index_stale(ix)) {
			run_search(searched, true);
			return;
		}
		if (ix && device_state_play_index(library_index_clone(ix), r->index)) {
			listacts_played();
		} else if (r->path[0]) {
			player_play_file(r->path);
		}
		break;
	}
	case CAT_ALBUMS:
		if (r->name[0]) {
			medialist_open(r->name, LIBRARY_LIST_TRACKS, LIBRARY_FILTER_ALBUM, r->name);
		}
		break;
	default:
		medialist_open_artist(r->name);
		break;
	}
}

static void row_queue(row_t *r, playlist_queue_where_t where) {
	char shown[256];
	library_album_title(r->name, shown, sizeof(shown));
	if (r->cat == CAT_TRACKS && r->path[0]) {
		const char *one = r->path;
		listacts_queue(&one, 1, where, shown);
	} else if (r->cat == CAT_ALBUMS) {
		listacts_queue_index(listacts_album_tracks(r->name), where, shown);
	}
}

static void row_next_cb(lv_obj_t *obj, void *user) {
	(void)obj;
	row_t *r = row_live(user);
	if (r) {
		row_queue(r, PLAYLIST_QUEUE_NEXT);
	}
}

static void row_end_cb(lv_obj_t *obj, void *user) {
	(void)obj;
	row_t *r = row_live(user);
	if (r) {
		row_queue(r, PLAYLIST_QUEUE_END);
	}
}

static void row_star_cb(lv_obj_t *obj, void *user) {
	row_t *r = row_live(user);
	if (r && r->cat == CAT_TRACKS && r->path[0]) {
		sx_row_set_starred(obj, listacts_star(r->path, r->name, r->artist));
	}
}

// "Remove from playlist" in a track's sheet, as before: the playlists that
// hold the track are gathered when the sheet opens; one is left at once,
// several are named in a second sheet.
#define MENU_PLAYLISTS_MAX 8
static char menu_path[512];
static char menu_playlists[MENU_PLAYLISTS_MAX][128];
static int menu_playlist_count;

static bool collect_playlist_cb(const char *name, void *user) {
	(void)user;
	if (playlists_has_track(name, menu_path)) {
		snprintf(menu_playlists[menu_playlist_count], sizeof(menu_playlists[0]), "%s", name);
		menu_playlist_count++;
	}
	return menu_playlist_count < MENU_PLAYLISTS_MAX;
}

static void remove_from(const char *playlist) {
	if (playlists_remove_track(playlist, menu_path)) {
		toast_success("medialist_track_removed");
	} else {
		gui_notify_popup("medialist_remove_failed");
	}
}

static void remove_one_cb(void *user) {
	int i = (int)(intptr_t)user;
	if (i >= 0 && i < menu_playlist_count) {
		remove_from(menu_playlists[i]);
	}
}

static void remove_cb(void *user) {
	(void)user;
	if (menu_playlist_count == 1) {
		remove_from(menu_playlists[0]);
		return;
	}
	sx_sheet_item_t items[MENU_PLAYLISTS_MAX];
	for (int i = 0; i < menu_playlist_count; i++) {
		items[i] = (sx_sheet_item_t){menu_playlists[i], &icon_list_music, false, false, remove_one_cb,
									 (void *)(intptr_t)i};
	}
	sx_sheet_menu(tr("medialist_remove_from_playlist"), NULL, items, menu_playlist_count);
}

static void row_hold_cb(lv_obj_t *obj, void *user) {
	(void)obj;
	row_t *r = row_live(user);
	if (!r) {
		return;
	}
	if (r->cat == CAT_TRACKS && r->path[0]) {
		snprintf(menu_path, sizeof(menu_path), "%s", r->path);
		menu_playlist_count = 0;
		playlists_for_each(collect_playlist_cb, NULL);
		sx_sheet_item_t extra[1];
		int n = 0;
		if (menu_playlist_count > 0) {
			extra[n++] =
				(sx_sheet_item_t){tr("medialist_remove_from_playlist"), &icon_list_music, false, true, remove_cb, NULL};
		}
		listacts_track_sheet(r->path, r->name, r->artist, NULL, NULL, NULL, extra, n);
	} else if (r->cat == CAT_ALBUMS) {
		listacts_album_sheet(r->name, r->artist[0] ? r->artist : NULL, NULL, NULL, 0);
	}
}

// A star changed elsewhere (a sheet, an undo): the stars on screen again.
static void lists_changed(void) {
	for (int i = 0; i < POOL; i++) {
		if (rows[i].cat == CAT_TRACKS) {
			sx_row_set_starred(rows[i].row, library_fav_contains(rows[i].path));
		}
	}
}

// ---------------------------------------------------------------------------
// The pills
// ---------------------------------------------------------------------------

static void pills_paint(void) {
	for (int i = 0; i <= CAT_COUNT; i++) {
		sx_pill_set_active(pill[i], i == filter);
	}
}

static void pill_text(int which, const char *text) { lv_label_set_text(lv_obj_get_child(pill[which], 0), text); }

static void pill_clicked_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	int which = (int)(intptr_t)lv_event_get_user_data(e);
	if (which == filter) {
		return;
	}
	filter = which;
	pills_paint();
	for (int i = 0; i < POOL; i++) {
		row_unbind(&rows[i]);
	}
	layout_groups();
	lv_obj_scroll_to_y(results, 0, LV_ANIM_OFF);
	window_update();
}

// ---------------------------------------------------------------------------
// Running the query
// ---------------------------------------------------------------------------

// The keyboard takes the bottom of the screen while it is up: the results stop
// where it starts (a full-height list with a keyboard's worth of padding would
// scroll its hits under the keys).
static void layout_results(void) {
	bool kb = keyboard_is_visible(keyboard);
	int height = (int)page_cfg->screen_height - content_top - (kb ? SEARCH_KEYBOARD_H : 0);
	if (height < 60) {
		height = 60;
	}
	lv_obj_set_height(results, height);
	// The two that do not scroll stop above the mini player, so the state's
	// middle is the middle of what can be seen.
	int still = height - (kb ? 0 : sx_frame_body_bottom());
	lv_obj_set_height(recent_box, still);
	lv_obj_set_height(nothing_box, still);
	lv_obj_update_layout(results); // the new height, before the window is worked out from it
	window_update();
}

static void keyboard_show(bool visible) {
	keyboard_set_visible(keyboard, visible);
	layout_results();
}

// The parts of the page under the field: the results, the recent searches, or
// the state saying nothing was found.
static void show_part(lv_obj_t *part) {
	lv_obj_set_hidden(results, part != results);
	lv_obj_set_hidden(recent_box, part != recent_box);
	lv_obj_set_hidden(nothing_box, part != nothing_box);
}

static void results_clear(void) {
	for (int i = 0; i < POOL; i++) {
		row_unbind(&rows[i]);
	}
	for (int i = 0; i < CAT_COUNT; i++) {
		library_index_close(cat_ix[i]);
		cat_ix[i] = NULL;
		cat_count[i] = 0;
		group_top[i] = -1;
	}
	searched[0] = '\0';
	lv_obj_set_height(body, 0);
	recent_show();
}

// Asks the library for all three groups. `keep_place` is for the same query
// asked again because the library changed under it: the filter and the scroll
// stay where they were.
static void run_search(const char *query, bool keep_place) {
	char wanted[sizeof(searched)];
	snprintf(wanted, sizeof(wanted), "%s", query ? query : "");
	int scroll = keep_place ? (int)lv_obj_get_scroll_y(results) : 0;

	results_clear();
	if (!wanted[0]) {
		return;
	}
	if (!library_is_open()) {
		gui_notify_no_card();
		return;
	}
	if (!keep_place) {
		filter = FILTER_ALL;
	}

	snprintf(searched, sizeof(searched), "%s", wanted);
	pitch = sx_row_height(false) + SX_ROW_GAP;
	playing_refresh();
	int total = 0;
	for (int i = 0; i < CAT_COUNT; i++) {
		cat_ix[i] = library_index_open(CAT_KIND[i], LIBRARY_FILTER_SEARCH, wanted, LIBRARY_ORDER_DEFAULT, false);
		cat_count[i] = library_index_count(cat_ix[i]);
		total += cat_count[i];
		// A group cut at the cap says so: the count is a floor, not a total.
		char text[96];
		snprintf(text, sizeof(text), cat_count[i] >= LIBRARY_SEARCH_MAX ? "%s \xC2\xB7 %d+" : "%s \xC2\xB7 %d",
				 tr(CAT_PILL[i]), cat_count[i]);
		pill_text(i, text);
		// A group with nothing in it has no pill: there is nothing to see.
		lv_obj_set_hidden(pill[i], cat_count[i] == 0);
	}

	if (total == 0) {
		show_part(nothing_box);
		return;
	}
	// Kept among the recent searches: only a query that found something is
	// worth running again.
	if (!keep_place) {
		recent_note(wanted);
	}
	if (filter != FILTER_ALL && cat_count[filter] == 0) {
		filter = FILTER_ALL;
	}
	pills_paint();
	show_part(results);
	layout_groups();
	lv_obj_update_layout(results);
	lv_obj_scroll_to_y(results, scroll, LV_ANIM_OFF);
	window_update();
}

static void refresh_clear_button(void) {
	const char *text = lv_textarea_get_text(field);
	lv_obj_set_hidden(clear_btn, !(text && text[0]));
}

static void clear_query(void) {
	lv_textarea_set_text(field, "");
	results_clear();
	refresh_clear_button();
	keyboard_show(true); // ready for the next query
}

static void clear_clicked_cb(lv_event_t *e) {
	(void)e;
	clear_query();
}

// The results answer the query they were run with: once the text says
// something else, they go, and the next press of the search key brings the new
// ones.
static void field_changed_cb(lv_event_t *e) {
	(void)e;
	refresh_clear_button();
	const char *text = lv_textarea_get_text(field);
	if (searched[0] && strcmp(text ? text : "", searched) != 0) {
		results_clear();
	}
}

static void kb_search_cb(lv_event_t *e) {
	(void)e;
	run_search(lv_textarea_get_text(field), false);
	keyboard_show(false); // the results get the whole page
}

static void field_clicked_cb(lv_event_t *e) {
	(void)e;
	keyboard_show(true);
}

// ---------------------------------------------------------------------------
// No query: the recent searches
// ---------------------------------------------------------------------------

static void recent_clicked_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	if (i < 0 || i >= recent_count) {
		return;
	}
	char query[128];
	snprintf(query, sizeof(query), "%s", recent[i]);
	lv_textarea_set_text(field, query); // clears nothing: no search stands
	refresh_clear_button();
	run_search(query, false);
	keyboard_show(false);
}

static void recent_show(void) {
	recent_load();
	lv_obj_clean(recent_pills);
	for (int i = 0; i < recent_count; i++) {
		lv_obj_t *p = sx_pill(recent_pills, recent[i], i, recent_clicked_cb);
		lv_obj_set_style_max_width(p, lv_pct(100), 0);
		lv_label_set_long_mode(lv_obj_get_child(p, 0), LV_LABEL_LONG_DOT);
	}
	// Nothing searched yet: nothing to show but the field.
	show_part(recent_count > 0 ? recent_box : NULL);
}

static lv_obj_t *recent_build(lv_obj_t *parent) {
	lv_obj_t *box = sx_box(parent);
	lv_obj_set_width(box, lv_pct(100));
	lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(box, 6, 0);
	lv_obj_set_scrollable(box, true);
	lv_obj_set_scroll_dir(box, LV_DIR_VER);
	lv_obj_set_style_pad_hor(box, page_cfg->padding, 0);

	lv_obj_t *head = sx_label(box, &font_ui_17, &sx_style_text2);
	lv_obj_set_style_text_letter_space(head, 2, 0);
	lv_obj_set_style_pad_left(head, 6, 0);
	lv_obj_set_style_pad_top(head, 12, 0);
	lv_label_set_text(head, tr("lists_head_recent"));

	lv_obj_t *card = sx_card_create(box);
	sx_card_title(card, tr("lists_recent_queries"));
	recent_pills = sx_card_pills(card);
	return box;
}

// ---------------------------------------------------------------------------
// No hits: the state
// ---------------------------------------------------------------------------

static void nothing_clear_cb(lv_event_t *e) {
	(void)e;
	clear_query();
}

static lv_obj_t *nothing_build(lv_obj_t *parent) {
	lv_obj_t *box = sx_box(parent);
	lv_obj_set_width(box, lv_pct(100));
	lv_obj_set_style_pad_hor(box, 2 * page_cfg->padding, 0);
	lv_obj_set_style_pad_bottom(box, 40, 0);
	lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(box, 14, 0);

	lv_obj_t *disc = sx_box(box);
	lv_obj_set_size(disc, STATE_DISC, STATE_DISC);
	lv_obj_add_style(disc, &sx_style_surface, 0);
	lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
	lv_obj_t *glyph = lv_image_create(disc);
	lv_image_set_src(glyph, &icon_search);
	lv_obj_add_style(glyph, &sx_style_icon, 0);
	lv_obj_center(glyph);

	lv_obj_t *title = sx_label(box, &font_ui_28_bold, &sx_style_text);
	lv_label_set_text(title, tr("lists_nothing_found"));

	lv_obj_t *text = lv_label_create(box);
	lv_obj_add_style(text, &sx_style_text2, 0);
	lv_obj_set_style_text_font(text, &font_ui_20, 0);
	lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(text, lv_pct(100));
	lv_label_set_text(text, tr("lists_nothing_found_text"));

	lv_obj_t *button = lv_btn_create(box);
	lv_obj_remove_style_all(button);
	lv_obj_add_style(button, &sx_style_accent, 0);
	lv_obj_add_style(button, &sx_style_press, LV_STATE_PRESSED);
	lv_obj_set_size(button, LV_SIZE_CONTENT, STATE_BUTTON_H);
	lv_obj_set_style_min_width(button, 240, 0);
	lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_pad_hor(button, 40, 0);
	lv_obj_set_style_margin_top(button, 10, 0);
	lv_obj_add_event_cb(button, nothing_clear_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *label = lv_label_create(button);
	lv_obj_set_style_text_font(label, &font_ui_24_bold, 0);
	lv_label_set_text(label, tr("lists_clear"));
	lv_obj_center(label);
	return box;
}

// ---------------------------------------------------------------------------
// The page
// ---------------------------------------------------------------------------

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	lv_timer_pause(art_drop_timer); // back before the wait ran out
	// Back from a hit the results are still there to pick another; a page with
	// nothing on it is a page to type in.
	if (!searched[0]) {
		recent_show();
		keyboard_show(true);
		return;
	}
	for (int i = 0; i < CAT_COUNT; i++) {
		if (cat_ix[i] && library_index_stale(cat_ix[i])) {
			run_search(searched, true);
			return;
		}
	}
	if (pitch != sx_row_height(false) + SX_ROW_GAP) {
		run_search(searched, true); // Large rows changed since
		return;
	}
	if (art_dropped) {
		art_dropped = false;
		for (int i = 0; i < POOL; i++) {
			row_unbind(&rows[i]);
		}
	}
	search_notify_now_playing();
	window_update(); // whatever was dropped is asked for again
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	lv_timer_pause(thumb_timer);
	lv_timer_reset(art_drop_timer);
	lv_timer_resume(art_drop_timer);
}

static void refresh_theme(void) {
	if (pill[0]) {
		pills_paint();
	}
}

void search_open(void) { switch_screen(search_screen); }

void search_init(gui_config_t *cfg) {
	page_cfg = cfg;
	search_screen = lv_obj_create(NULL);
	lv_obj_add_style(search_screen, &theme_style_screen, 0);
	settingsrow_title(search_screen, cfg, "search_2");

	int top = settingsrow_content_top(cfg);
	int w = (int)cfg->screen_width - 2 * cfg->padding;
	content_top = top + FIELD_H + FIELD_GAP;
	pitch = sx_row_height(false) + SX_ROW_GAP;

	field = lv_textarea_create(search_screen);
	lv_textarea_set_one_line(field, true);
	lv_textarea_set_placeholder_text(field, tr("lists_search_hint"));
	lv_obj_set_size(field, w, FIELD_H);
	lv_obj_set_scrollbar_mode(field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(field, LV_ALIGN_TOP_LEFT, cfg->padding, top);
	lv_obj_add_style(field, &sx_style_surface, 0);
	lv_obj_add_style(field, &sx_style_text, 0);
	lv_obj_set_style_radius(field, FIELD_H / 2, 0);
	lv_obj_set_style_border_width(field, 0, 0);
	lv_obj_set_style_shadow_width(field, 0, 0);
	lv_obj_set_style_pad_ver(field, (FIELD_H - lv_font_get_line_height(&font_ui_24)) / 2, 0);
	lv_obj_set_style_pad_left(field, FIELD_ICON_W, 0);
	lv_obj_set_style_pad_right(field, FIELD_CLEAR_W, 0);
	lv_obj_set_style_text_font(field, &font_ui_24, 0);
	lv_obj_add_style(field, &sx_style_text2, LV_PART_TEXTAREA_PLACEHOLDER);
	lv_obj_add_event_cb(field, field_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(field, field_clicked_cb, LV_EVENT_CLICKED, NULL);
	keyboard_style_caret(field);

	field_icon = lv_image_create(search_screen);
	lv_image_set_src(field_icon, &icon_search);
	lv_obj_add_style(field_icon, &sx_style_icon2, 0);
	lv_image_set_scale(field_icon, LV_SCALE_NONE * 3 / 4);
	lv_obj_align(field_icon, LV_ALIGN_TOP_LEFT, cfg->padding + 14, top + (FIELD_H - (int)icon_search.header.h) / 2);
	lv_obj_set_clickable(field_icon, false);

	// The clear button over the field's right edge: wipes the query and the
	// results in one tap. Hidden while the field is empty.
	clear_btn = lv_btn_create(search_screen);
	lv_obj_remove_style_all(clear_btn);
	lv_obj_set_size(clear_btn, FIELD_CLEAR_W, FIELD_H);
	lv_obj_align(clear_btn, LV_ALIGN_TOP_RIGHT, -cfg->padding, top);
	lv_obj_set_hidden(clear_btn, true);
	lv_obj_add_event_cb(clear_btn, clear_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *clear_icon = lv_image_create(clear_btn);
	lv_image_set_src(clear_icon, &icon_clear);
	lv_obj_add_style(clear_icon, &sx_style_icon2, 0);
	lv_obj_center(clear_icon);

	// The results: one scroller from under the field to the bottom (or the
	// keyboard), the rows placed by hand on a body as tall as every hit shown,
	// so the scroll and the fling cover them all while only the pool exists.
	results = sx_box(search_screen);
	lv_obj_set_width(results, cfg->screen_width);
	lv_obj_align(results, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_scrollable(results, true);
	lv_obj_set_scroll_dir(results, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(results, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_set_style_pad_hor(results, cfg->padding, 0);
	lv_obj_set_style_pad_bottom(results, RESULTS_BOTTOM, 0);
	lv_obj_add_event_cb(results, scroll_cb, LV_EVENT_SCROLL, NULL);
	lv_obj_set_hidden(results, true);

	body = sx_box(results);
	lv_obj_set_size(body, lv_pct(100), 0);

	pills_card = sx_card_create(body);
	lv_obj_set_style_pad_ver(pills_card, 14, 0);
	lv_obj_t *pills = sx_card_pills(pills_card);
	pill[FILTER_ALL] = sx_pill(pills, tr("lists_filter_all"), FILTER_ALL, pill_clicked_cb);
	for (int i = 0; i < CAT_COUNT; i++) {
		pill[i] = sx_pill(pills, tr(CAT_PILL[i]), i, pill_clicked_cb);
	}
	// The pill row comes first in a card with no head of its own.
	lv_obj_t *head = lv_obj_get_child(pills_card, 0);
	if (head && head != pills) {
		lv_obj_set_hidden(head, true);
	}
	pills_paint();

	for (int c = 0; c < CAT_COUNT; c++) {
		heading[c] = sx_label(body, &font_ui_17, &sx_style_text2);
		lv_obj_set_style_text_letter_space(heading[c], 2, 0);
		lv_obj_set_x(heading[c], 6);
		lv_obj_set_hidden(heading[c], true);
		group_top[c] = -1;
	}

	static const sx_row_handlers_t HANDLERS = {
		.on_tap = row_tap_cb,
		.on_long_press = row_hold_cb,
		.on_swipe_next = row_next_cb,
		.on_swipe_end = row_end_cb,
		.on_star = row_star_cb,
	};
	for (int i = 0; i < POOL; i++) {
		rows[i].row = sx_row_create(body, &HANDLERS, false);
		sx_row_set_user(rows[i].row, &rows[i]);
		rows[i].cat = -1;
		rows[i].index = -1;
		lv_obj_set_hidden(rows[i].row, true);
	}

	recent_box = recent_build(search_screen);
	lv_obj_align(recent_box, LV_ALIGN_TOP_LEFT, 0, content_top);
	nothing_box = nothing_build(search_screen);
	lv_obj_align(nothing_box, LV_ALIGN_TOP_LEFT, 0, content_top);
	show_part(NULL);

	keyboard = keyboard_create(search_screen, cfg->screen_width, SEARCH_KEYBOARD_H, field, &icon_search, NULL, kb_search_cb,
							   NULL);
	// The results reach the bottom of the screen once the keyboard is down:
	// room at their end for the mini player.
	sx_frame_reserve_bottom(results);
	sx_frame_on_body_change(layout_results);
	layout_results();

	thumb_timer = lv_timer_create(thumb_timer_cb, THUMB_POLL_MS, NULL);
	lv_timer_pause(thumb_timer);
	art_drop_timer = lv_timer_create(art_drop_cb, ART_DROP_DELAY_MS, NULL);
	lv_timer_pause(art_drop_timer);
	covertone_watch_add(covers_changed);
	listacts_on_change(lists_changed);

	theme_register_refresh(refresh_theme);
	lv_obj_add_event_cb(search_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(search_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(search_screen);
	player_sheet_attach_drag(search_screen, true);
}

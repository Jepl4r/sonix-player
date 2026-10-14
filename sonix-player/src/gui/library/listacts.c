#include "listacts.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "src/gui/library/medialist.h"
#include "src/gui/library/playlistpage.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/nowplaying/trackmenu.h"
#include "src/gui/settings/musicsettings.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/sx_toast.h"
#include "src/system/core/lang.h"
#include "src/system/playback/device_state.h"

// How many tracks a record or a selection may put into the queue at once: the
// same ceiling the selection had (a library queued whole is a library copied
// into memory).
#define QUEUE_MAX_TRACKS 20000

// ---------------------------------------------------------------------------
// Who wants to hear about a change
// ---------------------------------------------------------------------------

#define CHANGE_HOOKS_MAX 8
static void (*change_hooks[CHANGE_HOOKS_MAX])(void);
static int change_hook_count;

void listacts_on_change(void (*changed)(void)) {
	for (int i = 0; i < change_hook_count; i++) {
		if (change_hooks[i] == changed) {
			return;
		}
	}
	if (changed && change_hook_count < CHANGE_HOOKS_MAX) {
		change_hooks[change_hook_count++] = changed;
	}
}

static void changed(void) {
	for (int i = 0; i < change_hook_count; i++) {
		change_hooks[i]();
	}
}

// ---------------------------------------------------------------------------
// Collecting paths
// ---------------------------------------------------------------------------

typedef struct {
	char **paths;
	int count;
	int cap;
	int max;
} collect_t;

static bool collect_cb(const char *name, const char *path, const char *artist, void *user) {
	(void)name;
	(void)artist;
	collect_t *c = user;
	if (!path || !path[0]) {
		return true;
	}
	if (c->count >= c->max) {
		return false;
	}
	if (c->count == c->cap) {
		int cap = c->cap ? c->cap * 2 : 64;
		char **grown = realloc(c->paths, (size_t)cap * sizeof(*grown));
		if (!grown) {
			return false;
		}
		c->paths = grown;
		c->cap = cap;
	}
	char *copy = strdup(path);
	if (!copy) {
		return false;
	}
	c->paths[c->count++] = copy;
	return true;
}

int listacts_collect(library_index_t *ix, char ***paths_out, int max) {
	*paths_out = NULL;
	if (!ix) {
		return 0;
	}
	collect_t c = {NULL, 0, 0, max > 0 ? max : QUEUE_MAX_TRACKS};
	int total = library_index_count(ix);
	for (int first = 0; first < total && c.count < c.max; first += 256) {
		int want = total - first < 256 ? total - first : 256;
		int before = c.count;
		library_index_window(ix, first, want, collect_cb, &c);
		if (c.count == before) {
			break; // a stale handle hands out nothing: no endless loop
		}
	}
	*paths_out = c.paths;
	return c.count;
}

void listacts_paths_free(char **paths, int count) {
	for (int i = 0; i < count; i++) {
		free(paths[i]);
	}
	free(paths);
}

library_index_t *listacts_album_tracks(const char *album_value) {
	if (!album_value || !album_value[0]) {
		return NULL;
	}
	library_index_t *ix =
		library_index_open(LIBRARY_LIST_TRACKS, LIBRARY_FILTER_ALBUM, album_value, LIBRARY_ORDER_DEFAULT, false);
	if (ix && library_index_count(ix) > 0) {
		return ix;
	}
	library_index_close(ix);
	return NULL;
}

// ---------------------------------------------------------------------------
// The queue, with its undo
// ---------------------------------------------------------------------------

// The one addition that can still be taken back: the toast offering it is the
// latest one, and a newer toast drops the older offer.
static playlist_queue_undo_t queue_undo;

static void queue_undo_cb(void *user) {
	(void)user;
	if (playlist_queue_undo(&queue_undo)) {
		device_state_queue_changed();
		changed();
	} else {
		// Something else changed the queue in the meantime (a track came up,
		// another addition): taking these out now could take the wrong ones.
		sx_toast(tr("lists_undo_too_late"));
	}
	queue_undo.valid = false;
}

int listacts_queue(const char *const *paths, int count, playlist_queue_where_t where, const char *label) {
	if (!paths || count <= 0) {
		gui_notify_popup("playlist_cannot_add_the_tracks");
		return 0;
	}
	int queued = playlist_queue_paths_undoable(paths, count, where, &queue_undo);
	if (queued <= 0) {
		gui_notify_popup("playlist_cannot_add_the_tracks");
		return 0;
	}
	// Written down now: the on-disk copy is otherwise refreshed only when a
	// track is loaded, and a power-off before that loses the addition.
	device_state_queue_changed();

	char what[160];
	if (label && label[0]) {
		snprintf(what, sizeof(what), "%s", label);
	} else {
		snprintf(what, sizeof(what), tr("lists_n_tracks"), queued);
	}
	char text[256];
	snprintf(text, sizeof(text), tr(where == PLAYLIST_QUEUE_NEXT ? "lists_plays_next" : "lists_at_end"), what);
	if (queue_undo.valid) {
		sx_toast_undo(text, queue_undo_cb, NULL);
	} else {
		sx_toast(text); // the queue was made out of nothing: no queue to go back to
	}
	return queued;
}

int listacts_queue_index(library_index_t *ix, playlist_queue_where_t where, const char *label) {
	char **paths = NULL;
	int count = listacts_collect(ix, &paths, QUEUE_MAX_TRACKS);
	library_index_close(ix);
	int queued = listacts_queue((const char *const *)paths, count, where, label);
	listacts_paths_free(paths, count);
	return queued;
}

// ---------------------------------------------------------------------------
// Favourites, with their undo
// ---------------------------------------------------------------------------

static char star_path[512];
static char star_name[256];
static char star_artist[256];

static void star_undo_cb(void *user) {
	(void)user;
	if (star_path[0]) {
		library_fav_toggle(star_path, star_name, star_artist);
		changed();
	}
}

bool listacts_star(const char *path, const char *name, const char *artist) {
	if (!path || !path[0]) {
		return false;
	}
	snprintf(star_path, sizeof(star_path), "%s", path);
	snprintf(star_name, sizeof(star_name), "%s", name ? name : "");
	snprintf(star_artist, sizeof(star_artist), "%s", artist ? artist : "");
	bool on = library_fav_toggle(star_path, star_name, star_artist);
	char text[320];
	snprintf(text, sizeof(text), tr(on ? "lists_starred" : "lists_unstarred"), star_name);
	sx_toast_undo(text, star_undo_cb, NULL);
	changed();
	return on;
}

// ---------------------------------------------------------------------------
// Playing
// ---------------------------------------------------------------------------

void listacts_played(void) { player_refresh_now_playing(); }

static void set_shuffle(bool shuffled) {
	// Mode first: with shuffle already on, the queue deals its random order as
	// it is loaded, so it comes out shuffled in one step. Which of the two
	// shuffles is the "Continuous shuffle" option.
	playback_mode_t shuffle_mode =
		musicsettings_endless_shuffle() ? PLAYBACK_MODE_SHUFFLE_REPEAT : PLAYBACK_MODE_SHUFFLE;
	playlist_set_mode(shuffled ? shuffle_mode : PLAYBACK_MODE_NORMAL);
}

bool listacts_shuffle_all(void) {
	if (!library_is_open()) {
		gui_notify_no_card();
		return false;
	}
	library_index_t *ix = library_index_open(LIBRARY_LIST_TRACKS, LIBRARY_FILTER_NONE, NULL, LIBRARY_ORDER_DEFAULT, false);
	int count = library_index_count(ix);
	if (count <= 0) {
		library_index_close(ix);
		gui_notify_popup("library_empty_note");
		return false;
	}
	set_shuffle(true);
	int start = count > 1 ? (int)lv_rand(0, (uint32_t)(count - 1)) : 0;
	if (!device_state_play_index(ix, start)) {
		return false;
	}
	playlist_set_album("");
	listacts_played();
	sx_toast(tr("lists_shuffled_all"));
	return true;
}

void listacts_play_album(const char *album_value, bool shuffled) {
	library_index_t *ix = listacts_album_tracks(album_value);
	if (!ix) {
		return;
	}
	set_shuffle(shuffled);
	int count = library_index_count(ix);
	int start = shuffled && count > 1 ? (int)lv_rand(0, (uint32_t)(count - 1)) : 0;
	if (!device_state_play_index(ix, start)) {
		return;
	}
	// "Play albums back to back" needs to know which record is ending.
	playlist_set_album(album_value);
	listacts_played();
	if (shuffled) {
		char title[256];
		char text[320];
		library_album_title(album_value, title, sizeof(title));
		snprintf(text, sizeof(text), tr("lists_shuffled"), title);
		sx_toast(text);
	}
}

// ---------------------------------------------------------------------------
// The sheets
// ---------------------------------------------------------------------------

#define SHEET_ITEMS_MAX 16

// What the open sheet is about. Copies: the row it came from may be rebound
// before an item is picked.
static char sheet_path[512];
static char sheet_title[256];
static char sheet_artist[256];
static char sheet_subtitle[300];
static char sheet_album[300];

static void track_star_cb(void *user) {
	(void)user;
	listacts_star(sheet_path, sheet_title, sheet_artist);
}

static void track_queue_cb(void *user) {
	const char *path = sheet_path;
	listacts_queue(&path, 1, (playlist_queue_where_t)(intptr_t)user, sheet_title);
}

static void track_playlist_cb(void *user) {
	(void)user;
	playlistpage_add_track(sheet_path);
}

static void track_album_cb(void *user) {
	(void)user;
	if (sheet_album[0]) {
		medialist_open(sheet_album, LIBRARY_LIST_TRACKS, LIBRARY_FILTER_ALBUM, sheet_album);
	}
}

static void track_artist_cb(void *user) {
	(void)user;
	medialist_open_track_artist(sheet_path);
}

static void track_details_cb(void *user) {
	(void)user;
	trackmenu_details_open(sheet_path);
}

static int add_extra(sx_sheet_item_t *items, int n, const sx_sheet_item_t *extra, int extra_count) {
	for (int i = 0; i < extra_count && n < SHEET_ITEMS_MAX; i++) {
		items[n++] = extra[i];
	}
	return n;
}

void listacts_track_sheet(const char *path, const char *title, const char *artist, const char *subtitle,
						  const char *here_album, const char *here_artist, const sx_sheet_item_t *extra,
						  int extra_count) {
	if (!path || !path[0]) {
		return;
	}
	snprintf(sheet_path, sizeof(sheet_path), "%s", path);
	snprintf(sheet_title, sizeof(sheet_title), "%s", title ? title : "");
	snprintf(sheet_artist, sizeof(sheet_artist), "%s", artist ? artist : "");
	snprintf(sheet_subtitle, sizeof(sheet_subtitle), "%s", subtitle ? subtitle : sheet_artist);
	if (!library_track_album_value(sheet_path, sheet_album, sizeof(sheet_album))) {
		sheet_album[0] = '\0';
	}
	char page_artist[256] = "";
	bool by_album_artist = false;
	library_track_artist_page(sheet_path, page_artist, sizeof(page_artist), &by_album_artist);

	sx_sheet_item_t items[SHEET_ITEMS_MAX];
	int n = 0;
	bool fav = library_fav_contains(sheet_path);
	items[n++] = (sx_sheet_item_t){tr(fav ? "remove_from_favourites" : "medialist_add_to_favourites"),
								   fav ? &icon_star_filled : &icon_star, false, false, track_star_cb, NULL};
	items[n++] = (sx_sheet_item_t){tr("queue_play_next"), &icon_list_start, false, false, track_queue_cb,
								   (void *)(intptr_t)PLAYLIST_QUEUE_NEXT};
	items[n++] = (sx_sheet_item_t){tr("queue_add_to_end"), &icon_list_plus_small, false, false, track_queue_cb,
								   (void *)(intptr_t)PLAYLIST_QUEUE_END};
	items[n++] = (sx_sheet_item_t){tr("add_to_playlist"), &icon_list_music, false, false, track_playlist_cb, NULL};
	// Not the page already on screen: offering to open it is noise.
	if (sheet_album[0] && !(here_album && here_album[0] && library_album_same(here_album, sheet_album))) {
		items[n++] = (sx_sheet_item_t){tr("show_album"), &icon_album, false, false, track_album_cb, NULL};
	}
	if (page_artist[0] && !(here_artist && here_artist[0] && strcasecmp(here_artist, page_artist) == 0)) {
		items[n++] = (sx_sheet_item_t){tr("show_artist"), &icon_artist, false, false, track_artist_cb, NULL};
	}
	items[n++] = (sx_sheet_item_t){tr("details"), &icon_file, false, false, track_details_cb, NULL};
	n = add_extra(items, n, extra, extra_count);
	sx_sheet_menu(sheet_title, sheet_subtitle[0] ? sheet_subtitle : NULL, items, n);
}

static void album_play_cb(void *user) { listacts_play_album(sheet_album, user != NULL); }

static void album_queue_cb(void *user) {
	char title[256];
	library_album_title(sheet_album, title, sizeof(title));
	listacts_queue_index(listacts_album_tracks(sheet_album), (playlist_queue_where_t)(intptr_t)user, title);
}

static void album_paths_to(bool favourites) {
	char **paths = NULL;
	library_index_t *ix = listacts_album_tracks(sheet_album);
	int count = listacts_collect(ix, &paths, QUEUE_MAX_TRACKS);
	library_index_close(ix);
	if (count == 0) {
		gui_notify_popup(favourites ? "favourites_cannot_add_the_tracks" : "playlist_cannot_add_the_tracks");
	} else if (favourites) {
		playlistpage_add_favourites((const char *const *)paths, count);
	} else {
		playlistpage_add_tracks((const char *const *)paths, count);
	}
	listacts_paths_free(paths, count);
}

static void album_playlist_cb(void *user) {
	(void)user;
	album_paths_to(false);
}

static void album_fav_cb(void *user) {
	(void)user;
	album_paths_to(true);
}

static void album_artist_cb(void *user) {
	(void)user;
	medialist_open_artist(sheet_artist);
}

void listacts_album_sheet(const char *album_value, const char *artist, const char *subtitle,
						  const sx_sheet_item_t *extra, int extra_count) {
	if (!album_value || !album_value[0]) {
		return;
	}
	snprintf(sheet_album, sizeof(sheet_album), "%s", album_value);
	library_album_title(album_value, sheet_title, sizeof(sheet_title));
	snprintf(sheet_artist, sizeof(sheet_artist), "%s", artist ? artist : "");
	snprintf(sheet_subtitle, sizeof(sheet_subtitle), "%s", subtitle ? subtitle : sheet_artist);

	sx_sheet_item_t items[SHEET_ITEMS_MAX];
	int n = 0;
	items[n++] = (sx_sheet_item_t){tr("medialist_play"), &icon_play, false, false, album_play_cb, NULL};
	items[n++] = (sx_sheet_item_t){tr("medialist_play_in_random_order"), &icon_shuffle, false, false, album_play_cb,
								   (void *)(intptr_t)1};
	items[n++] = (sx_sheet_item_t){tr("queue_play_next"), &icon_list_start, false, false, album_queue_cb,
								   (void *)(intptr_t)PLAYLIST_QUEUE_NEXT};
	items[n++] = (sx_sheet_item_t){tr("queue_add_to_end"), &icon_list_plus_small, false, false, album_queue_cb,
								   (void *)(intptr_t)PLAYLIST_QUEUE_END};
	items[n++] = (sx_sheet_item_t){tr("add_to_playlist"), &icon_list_music, false, false, album_playlist_cb, NULL};
	items[n++] = (sx_sheet_item_t){tr("medialist_add_to_favourites"), &icon_star, false, false, album_fav_cb, NULL};
	if (sheet_artist[0]) {
		items[n++] = (sx_sheet_item_t){tr("show_artist"), &icon_artist, false, false, album_artist_cb, NULL};
	}
	n = add_extra(items, n, extra, extra_count);
	sx_sheet_menu(sheet_title, sheet_subtitle[0] ? sheet_subtitle : NULL, items, n);
}

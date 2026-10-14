#include "medialist.h"

#include <limits.h>

#include "src/system/library/playlists.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>

#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/library/listacts.h"
#include "src/gui/library/rowart.h"
#include "src/gui/library/search.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/settings/musicsettings.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/library/playlistpage.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/sx_card.h"
#include "src/gui/shell/sx_frame.h"
#include "src/gui/shell/sx_mini.h"
#include "src/gui/shell/sx_palette.h"
#include "src/gui/shell/sx_row.h"
#include "src/gui/shell/sx_sheet.h"
#include "src/gui/shell/sx_styles.h"
#include "src/gui/shell/sx_toast.h"
#include "src/gui/shell/sx_tokens.h"
#include "src/gui/nowplaying/trackmenu.h"
#include "src/system/core/config.h"
#include "src/system/library/covertone.h"
#include "src/system/library/covertone_watch.h"
#include "src/system/playback/device_state.h"
#include "src/system/core/lang.h"
#include "src/system/playback/playlist.h"
#include "src/system/device/power.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/topbar.h"
#include "src/gui/shell/touch.h"

lv_obj_t *medialist_screen;
lv_obj_t *medialist_tracks_screen;
lv_obj_t *medialist_albums_screen;

// The rows are the 2.0 list row (sx_row.h): 80 px, or 100 with "Large rows",
// SX_ROW_GAP apart, on the page itself rather than on cards. The height is
// taken by each list as it is opened and kept in its panel: row_pitch().
//
// The pool: the rows on screen at the tallest panel and the shortest row --
// (800 - 44) / 82, nine and a bit -- plus one above and one below.
#define ROW_POOL 12

// How many rows are held in RAM for a query-backed list: the twelve on screen
// plus enough either side that a flick does not fetch on every frame. At about
// 770 bytes a row this is 36 KB, whatever the size of the library.
#define WINDOW_ROWS 48

// A hard ceiling on how many rows are copied into RAM, for the one kind of list
// that still copies them: a playlist file handed in as an array of paths. A
// query-backed list has no ceiling -- it holds row ids and reads the rows a
// windowful at a time -- so a library is as large as the card allows.
#define MAX_ENTRIES 20000

// The buttons on the header row, beside the title: shuffle and sort on All
// tracks, sort (and search) on the name lists, shuffle on the lists that play
// as a whole -- and in selection mode its four. Four is the most that ever
// show at once. They are 56 px glyph buttons that the switcher turns into the
// header's 72 px round targets (touch_header).
#define CORNER_BUTTON_SIZE 56
#define CORNER_GAP 4
// How far above and below the shelf its buttons still take a press (touch.h):
// up to the status bar's margin and down to the list's.
#define CORNER_REACH 8
#define CORNER_MAX_BUTTONS 4

// How often the rows' pictures and counts are looked for while any are
// missing, and how many card reads one pass may make: a few, so a pass stays
// well inside a frame on the device (a 56 px thumbnail is a 6 KB read).
#define THUMB_POLL_MS 80
#define THUMB_LOADS_PER_PASS 3
#define COUNTS_PER_PASS 2

// The bin's red disc in selection mode.
#define DELETE_FAB_SIZE 72

// Inside a record of more than one disc, a heading above the first track of
// each: "Disc 2". This is the height it adds above that track's row.
#define DISC_HEAD_PITCH 44
// The gap between a heading and the row under it.
#define DISC_HEAD_BELOW 8
// More changes of disc than this is not a box set but tags in disarray, and a
// list of headings with a track or two under each reads worse than none.
#define DISC_HEAD_MAX 64
// An album with more tracks than this is somebody's "Unknown album" holding
// half the card: it gets neither headings nor track numbers.
#define ALBUM_PLACES_MAX 3000

// The album page (handoff, "Альбом"): under the status bar, on the album's
// own vertical gradient, the cover (240), the title (28 bold), the artist
// (24), a line of figures (20), and three buttons -- shuffle, a white "Play"
// pill and the album's sheet. It scrolls away with the rows. A floating 64 px
// back button stands over it, in place of the header row.
#define HEAD_COVER_SIZE COVERTONE_THUMB_LARGE
#define HEAD_GAP 12		   // between the head and the first row (or heading)
#define HEAD_TITLE_LINES 2
#define HEAD_BTN_W 72
#define HEAD_BTN_H 56
#define HEAD_PLAY_HEIGHT 60
#define HEAD_PLAY_PAD 52 // what the pill keeps either side of its word
#define HEAD_BTN_GAP 16
#define HEAD_BACK_SIZE 64
// The gradient's stops (the handoff's 0, 380 and 900 px) and the strip it is
// drawn from, tiled across the width: narrow enough to cost little, wide
// enough for the 4 x 4 dither to have both directions.
#define GRAD_STRIP_W 8
#define GRAD_Y1 380
#define GRAD_Y2 900

// The first-visit card on All tracks and Albums: what the swipes do. Gone for
// good once "Got it" is tapped ([ui] list_hint_seen).
#define HINT_GAP 8

// The A-Z lane down the right edge (handoff: SX_AZ_LANE_W, the rows' right
// margin SX_BODY_PAD_R becomes 32 beside it). It shows on the long
// alphabetical lists of SX_AZ_MIN_ROWS rows or more and not on a list by date.
#define AZ_RIGHT_PAD 32
#define AZ_MIN_ROWS 7

// ---------------------------------------------------------------------------
// The A-Z strip
//
// A column of letters down the right edge of the alphabetical lists, the way a
// phone's contacts do it: always there beside the rows, only the letters the
// list has, a press anywhere on it jumps to that letter, and sliding a finger
// down it walks the alphabet. The letter under the finger is drawn large in
// the middle of the screen, because a finger on the strip covers the strip.
//
// '#' comes first because that is where the list itself starts: digits and
// punctuation sort above A (see library_index_letter).
// ---------------------------------------------------------------------------
// '#', A to Z, and one at the end for everything the collation files BELOW Z:
// Cyrillic, Greek, kana, Hangul, CJK. Without it those rows sit under the strip
// with no letter pointing at them, which on a card with Russian tags reads as
// "the artist is not in the list at all".
//
// The names are library.c's: the handles count their rows into these same
// buckets, and two spellings of the same number is how they would drift apart.
#define INDEX_BUCKETS LIBRARY_INDEX_BUCKETS
#define INDEX_PAST_Z_SLOT LIBRARY_INDEX_PAST_Z_SLOT
#define INDEX_BAR_WIDTH SX_AZ_LANE_W
#define INDEX_HINT_MS 450	// the big letter outstays the finger by a moment
#define INDEX_ENGAGE_PX 6	// upward or downward movement that claims the press

typedef struct {
	char *name;
	char *path; // the track's file; on album rows a representative track
				// (cover source); NULL on the other name lists
} entry_t;

// One row of a query-backed list, as read back from the database. Fixed
// buffers rather than pointers: the whole point is that there are only ever
// WINDOW_ROWS of these, so a few kilobytes bought once beats an allocation per
// row bound.
typedef struct {
	char name[256];
	char path[512];
	char artist[128]; // a track's artist, an album's; empty on the other lists
	bool has_path;
} winrow_t;

typedef struct panel panel_t;

// One pooled row: the 2.0 row and what it is showing.
typedef struct {
	lv_obj_t *row; // sx_row
	panel_t *panel;
	int index;
	rowart_t art;	  // its cover's plaque and thumbnail
	bool picture_shown; // the thumbnail is on the row (not the plaque)
	// The count under an artist's, a genre's or a year's name ("Albums: 4"),
	// worked out after the row is drawn: -1 while it is still to come.
	int count;
} row_t;

// A list a panel showed before the one on it now, kept so the step back can
// open it again (see panel_back_guard).
typedef struct {
	library_list_t kind;
	library_filter_t filter;
	char value[256];
	char title[160];
	int scroll;
	// Replaced from the player's own menu: the step back reopens the player
	// over it, as leaving any page opened from there does.
	bool from_player;
} panel_step_t;

#define PANEL_BACK_DEPTH 6

// Everything one of the two screens owns.
struct panel {
	lv_obj_t *screen;
	lv_obj_t *title_label;
	lv_obj_t *list;	 // the scrollable viewport
	lv_obj_t *body;	 // fixed-height canvas the rows are placed on
	lv_obj_t *empty; // "nothing here" message
	lv_obj_t *corner;	   // the right-hand button strip on the title row
	lv_obj_t *reverse_btn; // Favourites: the list reversed (newest first)
	// Shuffle (⤮): the whole list at random at a tap; held, the sheet with
	// every way of starting it (in order, records at random, a track into the
	// queue, the whole list next or last).
	lv_obj_t *play_btn;
	lv_obj_t *sort_btn;	   // ⇅: the sort sheet
	lv_obj_t *search_btn;  // ⌕ on the artists' lists
	lv_obj_t *album_btn;   // tracks screen only: group an artist's tracks by album
	lv_obj_t *reorder_btn; // playlists only: put the list in hand-order mode
	bool reordering;	   // and whether it is in it now
	bool reorder_moved;	   // whether anything actually moved while it was
	gui_config_t *cfg;	 // kept for the heading, which is re-measured per list
	int row_h;			 // the rows' height for the list now open (sx_row_height())
	row_t rows[ROW_POOL];

	// A list is either a query -- which is held as a handle over its row ids,
	// four bytes a row, with the rows themselves read back a windowful at a
	// time -- or an arbitrary list of paths handed in by the caller, which has
	// no query behind it and is still kept whole. `from_paths` says which.
	library_index_t *ix;
	library_order_t list_order; // what the handle was opened with, to open it again
	bool list_desc;
	bool index_wanted; // whether this KIND of list has a strip at all
	winrow_t *window; // WINDOW_ROWS of them, allocated once with the panel
	int window_first; // index of window[0]; -1 when nothing is cached
	int window_count;

	// Path-backed lists only, with a count of its own: p->count below is the
	// length of the LIST, which for a query-backed one comes from the handle
	// and says nothing at all about this array.
	entry_t *entries;
	int entries_count;

	int count;

	bool is_tracks;		 // which flavour of row this screen shows
	library_list_t kind; // what the name list is listing (albums, artists...)

	// Enough to reload the very same list under a different order, which is
	// what the corner buttons do.
	library_filter_t filter;
	char filter_value[256];
	// Inside one album: the artist it is credited to, so a track is only
	// credited when its own artist is someone else.
	char album_artist[128];
	char title[160];
	bool from_paths; // filled by medialist_open_paths: not a query, cannot reorder
	// The playlist this list came from, empty when it came from anywhere else.
	// It is what lets the ellipsis offer to take a track out of the list it is
	// looking at, rather than to put it into another one.
	char playlist[NAME_MAX + 1];

	// Which list is loaded (kind|filter|value) and where it was scrolled to,
	// so reopening the same list puts the user back where they were.
	char signature[600];
	int saved_scroll;

	// The lists this one replaced on the same screen, newest last. One screen
	// shows them all in turn -- "Show album" from Favourites opens the album
	// where Favourites was -- so the switcher's history cannot step back
	// between them; the panel's back guard does, before the page is left.
	panel_step_t back[PANEL_BACK_DEPTH];
	int back_count;

	// Inside one album: what sits above the rows and between them. The rows
	// are still the list's rows one for one -- row i is track i of the queue a
	// tap starts -- and these only move them further down the page. See
	// row_y().
	int head_height;	  // the album head or the hint card, and the gap under it; 0 with neither
	lv_obj_t *hint;		  // the first-visit card about the swipes (All tracks, Albums)
	lv_obj_t *head;		  // the album page's head: cover, title, artist, figures, buttons
	lv_obj_t *head_cover;
	lv_obj_t *head_play_label;
	// The album page: on its own gradient and in its own hue while it is on
	// screen (sx_palette_push_hue), under the status bar, with a floating back
	// button in place of the header row.
	bool album_page;
	bool hue_pushed;
	float album_hue;
	float album_chroma; // < 0: the default for a hue from the title's hash
	int64_t album_id;
	lv_draw_buf_t *grad;
	lv_obj_t *back_float;
	// Behind the header row in selection mode on the album page: its buttons
	// need ground of their own over the rows scrolling by.
	lv_obj_t *head_bar;
	// The cover: the 56 px thumbnail stretched at once, then the 240 px one
	// read on the worker (covertone_thumb_async).
	uint8_t *head_small;
	lv_image_dsc_t head_small_dsc;
	lv_image_dsc_t head_large_dsc;
	bool head_has_large;
	uint32_t head_ticket;
	int *disc_first;	  // the first row of each disc, on a record of more than one
	uint16_t *disc_no;	  // ...and which disc that row begins
	lv_obj_t **disc_heads; // ...and the heading above it
	int disc_count;
	uint16_t *track_no;	  // each row's track number, in the head layout; NULL otherwise

	// The A-Z strip down the right edge and the letter it shows in the middle
	// of the screen while a finger is on it. index_row[] is the strip read from
	// top to bottom -- already turned round on a Z-A list -- and holds the row
	// each letter jumps to.
	lv_obj_t *index_bar;
	lv_obj_t *index_letters[INDEX_BUCKETS];
	lv_obj_t *index_hint;
	lv_obj_t *index_hint_label;
	int index_row[INDEX_BUCKETS];
	int index_bucket[INDEX_BUCKETS]; // which bucket each shown letter is
	int index_shown;				 // how many letters are shown
	bool index_enabled; // this list is one the strip makes sense on
	bool index_desc;	// the list runs Z-A, so the strip does too
	uint32_t index_hint_at;
	// Where a press on the strip started, and whether it has turned out to be
	// the strip's rather than the page's. Until it has, nothing moves.
	lv_point_t index_press;
	bool index_engaged;

	// Selection mode, on the lists panel_selectable() allows: "Select" in a
	// row's sheet starts it, taps (and holds) then choose rows, and the corner holds the way out
	// and "add to playlist" in place of its usual buttons. `corner_shown` is
	// which of those usual buttons to bring back, `corner_slots` how many.
	bool selecting;
	struct sel_item *sel;
	int sel_count;
	int sel_cap;
	lv_obj_t *sel_queue_btn;
	lv_obj_t *sel_fav_btn;
	lv_obj_t *sel_add_btn;
	lv_obj_t *sel_unfav_btn;  // favourites only
	lv_obj_t *sel_unlist_btn; // a playlist only
	lv_obj_t *sel_close_btn;
	// The red bin in the bottom corner, on the lists of tracks while
	// "Allow deleting tracks" is on (see sel_delete_cb).
	lv_obj_t *sel_delete_btn;
	unsigned corner_shown;
	int corner_slots;
};

// A chosen row: where it was in the list, and what it stands for -- a track's
// path, or the name of an album or an artist.
struct sel_item {
	int index;
	char *key;
};

static panel_t panel_names;
static panel_t panel_tracks;
// One artist's records: a name list like panel_names, on a screen of its own so
// it can sit between the artist list and an album's tracks.
static panel_t panel_artist_albums;

static lv_timer_t *thumb_timer;

// ---------------------------------------------------------------------------
// Ordering, remembered
//
// Per list kind, because "Z-A" is a decision about a particular list and not a
// mood: setting Albums to Z-A and finding Artists reversed too would be a
// surprise. Kept as one bitmask so it is one line of config rather than five.
// ---------------------------------------------------------------------------

static unsigned sort_desc_mask;
// Which lists run by when their files arrived rather than by name. Same shape
// as sort_desc_mask, and the two combine: "newest first" is by date, reversed.
static unsigned sort_added_mask;
// Which lists run by release year. Same shape again; a list is by year or by
// date or by name, never two of them, and the direction bit says which way.
static unsigned sort_year_mask;
// Which lists run by artist: only all the records can, filed under the artist
// each row shows, each artist's records by year. Same shape again, and like a
// year it is a list's order of its own -- never combined with the others.
static unsigned sort_artist_mask;
// The disc button on an artist's own lists: on, an artist opened as a track
// list -- from the search, or from the Artists list with Album view off --
// opens as their records instead.
static bool artist_records;

// Whether an artist opens as a list of their records. On by default: an artist
// with a dozen albums is a dozen rows this way and four hundred the other.
static bool album_view = true;

// Whether a track row wears its quality badge. Off by default: it is one more
// thing on every row, and a library of mp3s would wear the same badge all the
// way down.
static bool quality_badges;

// "Go to the current track": the five lists the Music page opens start at the
// row of whatever is playing. Off by default.
static bool go_to_current;

// "Allow deleting tracks": the bin among the selection's buttons on the lists
// of tracks. Off by default.
static bool allow_delete;

// "Show artist": the artist under each row's title, on the lists picked out by
// artist_lists (MEDIALIST_ARTIST_* bits). On by default, on all of them: a
// title alone says little in a list of tracks from many artists, and the
// line costs no room the row did not already have. Only a player that never
// touched the switch gets the default; one that turned it off keeps
// [library] show_artist = 0.
static bool show_artist;
static int artist_lists = MEDIALIST_ARTIST_ALL;

// "Alternative album view": an album's tracks open under a head with its cover,
// title, artist, genre and year, and the rows carry track numbers instead of
// covers. On by default since 1.2.4 -- inside an album every row's cover is
// the same picture, so the head says it once and the rows get the numbers
// back. A player whose config says album_head = 0 keeps the old page.
static bool album_head;

// Both are read on first use rather than in medialist_init(): the music
// settings page is built before it (see gui_init), so a switch built from these
// at init time would show the default rather than what the user chose, and then
// write that default back the first time it was touched.
static bool view_settings_loaded;

static void load_view_settings(void) {
	if (view_settings_loaded) {
		return;
	}
	view_settings_loaded = true;
	album_view = config_get_int("library", "album_view", 1) != 0;
	quality_badges = config_get_int("library", "quality_badges", 0) != 0;
	show_artist = config_get_int("library", "show_artist", 1) != 0;
	go_to_current = config_get_int("library", "go_to_current", 0) != 0;
	album_head = config_get_int("library", "album_head", 1) != 0;
	allow_delete = config_get_int("library", "allow_delete", 0) != 0;
	artist_lists = (int)config_get_int("library", "artist_lists", artist_lists);
}

bool medialist_album_view(void) {
	load_view_settings();
	return album_view;
}

void medialist_set_album_view(bool on) {
	load_view_settings();
	album_view = on;
	config_set_int("library", "album_view", on ? 1 : 0);
	config_save();
}

bool medialist_go_to_current(void) {
	load_view_settings();
	return go_to_current;
}

void medialist_set_go_to_current(bool on) {
	load_view_settings();
	go_to_current = on;
	config_set_int("library", "go_to_current", on ? 1 : 0);
	config_save();
}

bool medialist_allow_delete(void) {
	load_view_settings();
	return allow_delete;
}

void medialist_set_allow_delete(bool on) {
	load_view_settings();
	allow_delete = on;
	config_set_int("library", "allow_delete", on ? 1 : 0);
	config_save();
}

bool medialist_quality_badges(void) {
	load_view_settings();
	return quality_badges;
}

static bool sort_is_desc(library_list_t kind) { return (sort_desc_mask & (1u << (unsigned)kind)) != 0; }

static void sort_set_desc(library_list_t kind, bool desc) {
	if (desc) {
		sort_desc_mask |= 1u << (unsigned)kind;
	} else {
		sort_desc_mask &= ~(1u << (unsigned)kind);
	}
	config_set_int("library", "sort_desc", (long)sort_desc_mask);
	config_save();
}

static bool sort_is_added(library_list_t kind) { return (sort_added_mask & (1u << (unsigned)kind)) != 0; }

static void sort_set_added(library_list_t kind, bool added) {
	if (added) {
		sort_added_mask |= 1u << (unsigned)kind;
	} else {
		sort_added_mask &= ~(1u << (unsigned)kind);
	}
	config_set_int("library", "sort_added", (long)sort_added_mask);
	config_save();
}

static bool sort_is_year(library_list_t kind) { return (sort_year_mask & (1u << (unsigned)kind)) != 0; }

static void sort_set_year(library_list_t kind, bool year) {
	if (year) {
		sort_year_mask |= 1u << (unsigned)kind;
	} else {
		sort_year_mask &= ~(1u << (unsigned)kind);
	}
	config_set_int("library", "sort_year", (long)sort_year_mask);
	config_save();
}

static bool sort_is_artist(library_list_t kind) { return (sort_artist_mask & (1u << (unsigned)kind)) != 0; }

static void sort_set_artist(library_list_t kind, bool artist) {
	if (artist) {
		sort_artist_mask |= 1u << (unsigned)kind;
	} else {
		sort_artist_mask &= ~(1u << (unsigned)kind);
	}
	config_set_int("library", "sort_artist", (long)sort_artist_mask);
	config_save();
}

// The lists that can run by artist: all the records, where it is how a shelf of
// them is filed. A track list has its artist pages for that.
static bool sort_can_artist(library_list_t kind, library_filter_t filter) {
	return kind == LIBRARY_LIST_ALBUMS && filter == LIBRARY_FILTER_NONE;
}

// The lists that can run by date as well as by name: every track, every
// record, every artist and every album artist -- the last three by their
// newest file, so an artist with a new record comes up with it. Genres have no
// date worth going by, and a list inside one album or one artist is short
// enough, and ordered by disc, so that a date there would only scramble it.
static bool sort_can_date(library_list_t kind, library_filter_t filter) {
	return (kind == LIBRARY_LIST_TRACKS || kind == LIBRARY_LIST_ALBUMS || kind == LIBRARY_LIST_ARTISTS ||
			kind == LIBRARY_LIST_ALBUM_ARTISTS) &&
		   filter == LIBRARY_FILTER_NONE;
}

// The lists that can run by release year: every track and every record. An
// artist has no year of their own -- the first record and the last are both
// fair answers -- so the name lists stay out.
static bool sort_can_year(library_list_t kind, library_filter_t filter) {
	return (kind == LIBRARY_LIST_TRACKS || kind == LIBRARY_LIST_ALBUMS) && filter == LIBRARY_FILTER_NONE;
}

// The order a list is opened with, and whether the handle reads it backwards.
// Z-A and newest first are the ascending list read backwards; by year the
// database runs it either way itself, so the rows without a year stay at the
// end in both.
static void sort_order_for(library_list_t kind, library_filter_t filter, library_order_t *order, bool *desc) {
	bool reversed = sort_is_desc(kind);
	if (sort_can_artist(kind, filter) && sort_is_artist(kind)) {
		*order = LIBRARY_ORDER_ARTIST;
		*desc = false;
	} else if (sort_can_year(kind, filter) && sort_is_year(kind)) {
		*order = reversed ? LIBRARY_ORDER_YEAR_DESC : LIBRARY_ORDER_YEAR;
		*desc = false;
	} else if (sort_can_date(kind, filter) && sort_is_added(kind)) {
		*order = LIBRARY_ORDER_ADDED;
		*desc = reversed;
	} else {
		*order = LIBRARY_ORDER_DEFAULT;
		*desc = reversed;
	}
}

// ---------------------------------------------------------------------------
// model
// ---------------------------------------------------------------------------

static void model_clear(panel_t *p) {
	// Its own count, not p->count. They agree on a path-backed list and have
	// nothing to do with each other on a query-backed one, where entries is
	// NULL while p->count is a whole library -- which is a walk off the front
	// of a null pointer on the second list opened in the same panel.
	for (int i = 0; i < p->entries_count; i++) {
		free(p->entries[i].name);
		free(p->entries[i].path);
	}
	free(p->entries);
	p->entries = NULL;
	p->entries_count = 0;
	p->count = 0;

	library_index_close(p->ix);
	p->ix = NULL;
	p->window_first = -1;
	p->window_count = 0;
}

// ---------------------------------------------------------------------------
// Reading a row
//
// Every part of the page that draws or acts on a row goes through here, so
// neither backend is visible anywhere else. The strings belong to the panel and
// stay put only until the window next moves, which is why the callers that hold
// on to one copy it first.
// ---------------------------------------------------------------------------

static void index_rebuild(panel_t *p, bool enabled, bool descending);
static void row_drop_thumb(panel_t *p, row_t *row);
static void album_places_load(panel_t *p);
static bool panel_is_album(const panel_t *p);
static int body_height(const panel_t *p);
static int row_y(const panel_t *p, int index);
static int row_at_y(const panel_t *p, int y);

typedef struct {
	panel_t *panel;
	int filled;
} window_fill_t;

static bool window_fill_cb(const char *name, const char *path, const char *artist, void *user) {
	window_fill_t *fill = user;
	if (fill->filled >= WINDOW_ROWS) {
		return false;
	}
	winrow_t *row = &fill->panel->window[fill->filled++];
	snprintf(row->name, sizeof(row->name), "%s", name && name[0] ? name : tr("medialist_no_name"));
	row->has_path = path != NULL;
	snprintf(row->path, sizeof(row->path), "%s", path ? path : "");
	snprintf(row->artist, sizeof(row->artist), "%s", artist ? artist : "");
	return true;
}

// Row ids are only a promise until something rewrites the table, and a rescan
// or a card coming back rewrites all of them. The handle notices; this is what
// acts on it -- the same list, asked for again. Without it the page keeps a
// handle that hands out nothing and never recovers.
static void panel_refresh_stale(panel_t *p) {
	if (p->from_paths || !p->ix || !library_index_stale(p->ix)) {
		return;
	}

	library_index_t *fresh =
		library_index_open(p->kind, p->filter, p->filter_value, p->list_order, p->list_desc);
	library_index_close(p->ix);
	p->ix = fresh;
	p->count = library_index_count(fresh);
	p->window_first = -1;
	p->window_count = 0;

	// The rows underneath may have moved, so every pooled row has to be bound
	// again, and the letters have to be counted again. A row that comes back
	// with the same file keeps its picture (row_bind).
	for (int i = 0; i < ROW_POOL; i++) {
		p->rows[i].index = -2;
	}
	// The album may have gained or lost a disc, and its numbers came from the
	// rows that were there before.
	if (panel_is_album(p)) {
		album_places_load(p);
	}
	lv_obj_set_height(p->body, body_height(p));
	if (p->count == 0) {
		lv_obj_set_hidden(p->empty, false);
	} else {
		lv_obj_set_hidden(p->empty, true);
	}
	index_rebuild(p, p->index_wanted, p->index_desc);
}

// Brings `index` into the cached band, reading a windowful around it. The band
// is placed a few rows before the wanted one so that scrolling back up does not
// refetch on every row.
static bool window_cover(panel_t *p, int index) {
	if (!p->ix || index < 0 || index >= p->count) {
		return false;
	}
	if (!p->window) {
		// Bought once per panel and kept: two of these for the life of the
		// process is seventy kilobytes, against an allocation per row bound.
		p->window = calloc(WINDOW_ROWS, sizeof(*p->window));
		if (!p->window) {
			return false;
		}
	}
	if (p->window_first >= 0 && index >= p->window_first && index < p->window_first + p->window_count) {
		return true;
	}

	int first = index - ROW_POOL;
	if (first < 0) {
		first = 0;
	}
	if (first + WINDOW_ROWS > p->count) {
		first = p->count - WINDOW_ROWS;
	}
	if (first < 0) {
		first = 0;
	}

	window_fill_t fill = {.panel = p, .filled = 0};
	library_index_window(p->ix, first, WINDOW_ROWS, window_fill_cb, &fill);
	p->window_first = first;
	p->window_count = fill.filled;
	return index >= first && index < first + fill.filled;
}

// The name of a row, and its path when it has one. False when the row is not
// there -- a stale handle, or an index past the end.
static bool row_at(panel_t *p, int index, const char **name_out, const char **path_out) {
	if (index < 0 || index >= p->count) {
		return false;
	}
	if (p->from_paths) {
		if (name_out) {
			*name_out = p->entries[index].name;
		}
		if (path_out) {
			*path_out = p->entries[index].path;
		}
		return true;
	}
	if (!window_cover(p, index)) {
		return false;
	}
	const winrow_t *row = &p->window[index - p->window_first];
	if (name_out) {
		*name_out = row->name;
	}
	if (path_out) {
		*path_out = row->has_path ? row->path : NULL;
	}
	return true;
}

// The artist the database gave a row, or NULL: a list of paths handed in has
// none, and neither has a row whose artist tag is empty.
static const char *row_artist_at(panel_t *p, int index) {
	if (p->from_paths || index < 0 || index >= p->count || !window_cover(p, index)) {
		return NULL;
	}
	const char *artist = p->window[index - p->window_first].artist;
	return artist[0] ? artist : NULL;
}

// The same, copied out, for a caller that has to keep it across another read.
static bool row_path_copy(panel_t *p, int index, char *out, size_t size) {
	const char *path = NULL;
	if (!row_at(p, index, NULL, &path) || !path) {
		if (size > 0) {
			out[0] = '\0';
		}
		return false;
	}
	snprintf(out, size, "%s", path);
	return true;
}

typedef struct {
	panel_t *panel;
	int capacity;
} load_ctx_t;

static bool load_row_cb(const char *name, const char *path, const char *artist, void *user) {
	(void)artist;
	load_ctx_t *ctx = user;
	panel_t *p = ctx->panel;

	if (p->count >= MAX_ENTRIES) {
		return false;
	}

	if (p->count == ctx->capacity) {
		int grown = ctx->capacity ? ctx->capacity * 2 : 256;
		entry_t *bigger = realloc(p->entries, (size_t)grown * sizeof(entry_t));
		if (!bigger) {
			return false;
		}
		p->entries = bigger;
		ctx->capacity = grown;
	}

	p->entries[p->count].name = strdup(name && name[0] ? name : tr("medialist_no_name"));
	p->entries[p->count].path = path ? strdup(path) : NULL;
	if (!p->entries[p->count].name) {
		return false;
	}
	p->count++;
	p->entries_count = p->count;
	return true;
}

// ---------------------------------------------------------------------------
// rows, windowing and artwork (the browser's technique)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Where a row sits
//
// A row is row_pitch() below the one before it, and that is the whole of it on
// every list but one album's tracks, which may have the album head above the
// first row and a disc heading above the first track of each disc (and All
// tracks and Albums, which may have the hint card above theirs). Everything
// that turns a row into a height on the page, or a height back into a row,
// goes through these two, so the headings cannot drift from the rows.
// ---------------------------------------------------------------------------

// How many disc headings sit above row `index`, its own included.
static int disc_heads_through(const panel_t *p, int index) {
	int n = 0;
	while (n < p->disc_count && p->disc_first[n] <= index) {
		n++;
	}
	return n;
}

// One row and the gap under it. The height is the panel's own, taken when its
// list was opened (album_extras_clear()), so a list stays one shape while it is
// open even if "Large rows" is switched in the meantime.
static int row_pitch(const panel_t *p) { return (p->row_h > 0 ? p->row_h : SX_ROW_H) + SX_ROW_GAP; }

// The top of row `index` on the page. `index` may be p->count: the end of the
// last row, gap included.
static int row_y(const panel_t *p, int index) {
	return p->head_height + index * row_pitch(p) + disc_heads_through(p, index) * DISC_HEAD_PITCH;
}

// The row whose top is at or above `y`: the one a viewport starting at `y`
// opens on, or the one under a finger at `y`. Clamped to the list.
static int row_at_y(const panel_t *p, int y) {
	if (p->count <= 0) {
		return 0;
	}
	int index;
	if (p->disc_count == 0) {
		index = y < p->head_height ? 0 : (y - p->head_height) / row_pitch(p);
	} else {
		int lo = 0;
		int hi = p->count - 1;
		while (lo < hi) {
			int mid = lo + (hi - lo + 1) / 2;
			if (row_y(p, mid) <= y) {
				lo = mid;
			} else {
				hi = mid - 1;
			}
		}
		index = lo;
	}
	return index > p->count - 1 ? p->count - 1 : index;
}

// The height of everything the list scrolls over.
static int body_height(const panel_t *p) { return p->count > 0 ? row_y(p, p->count) : p->head_height + row_pitch(p); }

// One album's tracks, read from the index: the one list that can have a head
// and disc headings.
static bool panel_is_album(const panel_t *p) {
	return !p->from_paths && p->kind == LIBRARY_LIST_TRACKS && p->filter == LIBRARY_FILTER_ALBUM;
}

// What a row of this list is: the lead it wears, what its trailing slot is,
// which gestures it answers (the handoff's matrix).
typedef enum {
	RK_TRACK,  // thumbnail (or its number on the album page), star; every gesture
	RK_ALBUM,  // thumbnail, chevron; tap opens, swipes queue it, hold its sheet
	RK_PERSON, // round letter plaque, "Albums: N", chevron; tap opens
	RK_GENRE,  // square letter plaque, "Tracks: N", chevron; tap opens
	RK_YEAR,   // square plaque with the year's last two digits, "Albums: N"
} row_kind_t;

static row_kind_t panel_row_kind(const panel_t *p) {
	if (p->is_tracks) {
		return RK_TRACK;
	}
	switch (p->kind) {
	case LIBRARY_LIST_ALBUMS:
		return RK_ALBUM;
	case LIBRARY_LIST_GENRES:
		return RK_GENRE;
	case LIBRARY_LIST_YEARS:
		return RK_YEAR;
	default:
		return RK_PERSON;
	}
}

// The row loses its picture: the next bind works out the album again.
static void row_drop_thumb(panel_t *p, row_t *row) {
	(void)p;
	row->art.album_id = 0;
	row->art.has_picture = false;
	row->art.tone_known = false;
	row->picture_shown = false;
	row->count = -1;
}

// ---------------------------------------------------------------------------
// The now-playing mark
//
// The playing row (sx_row_set_playing): the surface under it, its title in
// the accent and the equaliser bars on its lead -- on whichever row the
// playing track is on, and, on the name lists, on the album, artist, genre or
// year that track belongs to, so walking into Albums shows which record is on.
//
// What is playing is cached here rather than asked per row: binding a row
// happens on every scroll step, and this turns that into four string compares
// against memory instead of a database query per row.
// ---------------------------------------------------------------------------

static char np_path[512];
static char np_album[256];
// The playing track's record as an Albums row names it -- the name and the key
// that tells same-named albums apart -- or empty when the file is not indexed.
static char np_album_value[300];
static char np_artist[256];
static char np_album_artist[256];
static char np_genre[128];
static char np_year[12];

static void np_cache_refresh(void) {
	device_state_t state;
	device_state_get(&state);

	snprintf(np_path, sizeof(np_path), "%s", state.current_file);
	snprintf(np_album, sizeof(np_album), "%s", state.metadata.album);
	snprintf(np_artist, sizeof(np_artist), "%s", state.metadata.artist);
	snprintf(np_album_artist, sizeof(np_album_artist), "%s", state.metadata.album_artist);
	snprintf(np_genre, sizeof(np_genre), "%s", state.metadata.genre);
	if (state.metadata.year > 0) {
		snprintf(np_year, sizeof(np_year), "%d", state.metadata.year);
	} else {
		np_year[0] = '\0';
	}
	if (!np_path[0] || !library_track_album_value(np_path, np_album_value, sizeof(np_album_value))) {
		np_album_value[0] = '\0';
	}

	// An album with no album-artist tag still belongs to its artist, which is
	// what the mark in Album artists is looked up by.
	if (!np_album_artist[0]) {
		snprintf(np_album_artist, sizeof(np_album_artist), "%s", np_artist);
	}
}

static bool entry_is_now_playing(panel_t *p, int index) {
	if (index < 0 || index >= p->count || !np_path[0]) {
		return false;
	}

	const char *name = NULL;
	const char *path = NULL;
	if (!row_at(p, index, &name, &path)) {
		return false;
	}

	// A track list: the file itself.
	if (p->is_tracks) {
		return path && strcmp(path, np_path) == 0;
	}

	// A name list: whichever of the playing track's tags this list is made of.
	if (!name || !name[0]) {
		return false;
	}
	switch (p->kind) {
	case LIBRARY_LIST_ALBUMS:
		// A row is named by album and key. The key decides when the index knows
		// the file; a track it does not know has only its tag, which matches
		// every album of that name.
		if (np_album_value[0]) {
			return strcmp(name, np_album_value) == 0;
		}
		return np_album[0] && library_album_same(name, np_album);
	case LIBRARY_LIST_ARTISTS:
		return np_artist[0] && strcmp(name, np_artist) == 0;
	case LIBRARY_LIST_ALBUM_ARTISTS:
		return np_album_artist[0] && strcmp(name, np_album_artist) == 0;
	case LIBRARY_LIST_GENRES:
		return np_genre[0] && strcmp(name, np_genre) == 0;
	case LIBRARY_LIST_YEARS:
		return np_year[0] && strcmp(name, np_year) == 0;
	default:
		return false;
	}
}

static void row_update_playmark(panel_t *p, row_t *row) {
	if (!row->row || row->index < 0) {
		return;
	}
	sx_row_set_playing(row->row, entry_is_now_playing(p, row->index));
}

static void panel_refresh_playmarks(panel_t *p) {
	for (int i = 0; i < ROW_POOL; i++) {
		row_update_playmark(p, &p->rows[i]);
	}
}

void medialist_notify_now_playing(void) {
	// Before the pages exist (the boot-time restore refreshes the player
	// before the GUI is finished) there is nothing to mark.
	if (!panel_names.rows[0].row) {
		return;
	}
	np_cache_refresh();
	panel_refresh_playmarks(&panel_names);
	panel_refresh_playmarks(&panel_tracks);
	panel_refresh_playmarks(&panel_artist_albums);
	search_notify_now_playing();
}

// ---------------------------------------------------------------------------
// The line under a row's title
// ---------------------------------------------------------------------------

// Whether this list is one "Show artist" is on for: all the tracks, the albums
// and the tracks inside one, a year's or a genre's albums and tracks, the
// favourites, or a playlist. Not an artist's own lists, where the name is the page's title
// already, and not the name lists, whose rows are artists or genres themselves.
static bool panel_shows_artist(const panel_t *p) {
	if (!show_artist || p->from_paths) {
		return false;
	}
	if ((p->kind == LIBRARY_LIST_TRACKS && p->filter == LIBRARY_FILTER_NONE) || p->kind == LIBRARY_LIST_MOST_PLAYED ||
		p->kind == LIBRARY_LIST_RECENTLY_PLAYED) {
		return (artist_lists & MEDIALIST_ARTIST_TRACKS) != 0;
	}
	if ((p->kind == LIBRARY_LIST_ALBUMS && (p->filter == LIBRARY_FILTER_NONE || p->filter == LIBRARY_FILTER_YEAR ||
											p->filter == LIBRARY_FILTER_RECENT)) ||
		(p->kind == LIBRARY_LIST_TRACKS && (p->filter == LIBRARY_FILTER_ALBUM || p->filter == LIBRARY_FILTER_YEAR))) {
		return (artist_lists & MEDIALIST_ARTIST_ALBUMS) != 0;
	}
	if ((p->kind == LIBRARY_LIST_TRACKS || p->kind == LIBRARY_LIST_ALBUMS) && p->filter == LIBRARY_FILTER_GENRE) {
		return (artist_lists & MEDIALIST_ARTIST_GENRES) != 0;
	}
	if (p->kind == LIBRARY_LIST_FAVOURITES) {
		return (artist_lists & MEDIALIST_ARTIST_FAVOURITES) != 0;
	}
	if (p->kind == LIBRARY_LIST_PLAYLIST) {
		return (artist_lists & MEDIALIST_ARTIST_PLAYLISTS) != 0;
	}
	return false;
}

// The line under the title: the artist, the badge and the length on a track;
// the artist on an album; the count on an artist, a genre or a year (once it
// is known, see counts_update).
static void row_update_detail(panel_t *p, row_t *row, int index, const char *path) {
	row_kind_t kind = panel_row_kind(p);
	if (kind == RK_PERSON || kind == RK_GENRE || kind == RK_YEAR) {
		// A space while the count is still to come: the line keeps its place,
		// so the title does not jump up when the count lands (an empty line
		// is taken away, sx_row_set_subtitle).
		char text[64] = " ";
		if (row->count >= 0) {
			snprintf(text, sizeof(text), tr(kind == RK_GENRE ? "lists_tracks_n" : "lists_albums_n"), row->count);
		}
		sx_row_set_subtitle(row->row, text, NULL, NULL);
		return;
	}

	const char *artist = panel_shows_artist(p) ? row_artist_at(p, index) : NULL;
	// Inside an album, only a track by someone other than the album's artist.
	if (artist && p->kind == LIBRARY_LIST_TRACKS && p->filter == LIBRARY_FILTER_ALBUM &&
		(!artist[0] || strcasecmp(artist, p->album_artist) == 0)) {
		artist = NULL;
	}
	if (kind == RK_ALBUM) {
		sx_row_set_subtitle(row->row, artist ? artist : "", NULL, NULL);
		return;
	}
	char badge[24];
	char length[16];
	listacts_track_badge(path, badge, sizeof(badge), length, sizeof(length));
	char badge_text[32] = "";
	if (badge[0]) {
		snprintf(badge_text, sizeof(badge_text), artist ? "\xC2\xB7 %s" : "%s", badge);
	}
	sx_row_set_subtitle(row->row, artist ? artist : "", badge_text, length);
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

static void play_menu_hide(void);
static library_filter_t filter_for(library_list_t kind);

// The lists rows can be chosen on: tracks, albums, artists, album artists, a
// playlist, the favourites and the most played, read from the index. Not
// genres, and not a list handed in as paths.
static bool panel_selectable(const panel_t *p) {
	if (p->from_paths) {
		return false;
	}
	return p->kind == LIBRARY_LIST_TRACKS || p->kind == LIBRARY_LIST_ALBUMS || p->kind == LIBRARY_LIST_ARTISTS ||
		   p->kind == LIBRARY_LIST_ALBUM_ARTISTS || p->kind == LIBRARY_LIST_PLAYLIST ||
		   p->kind == LIBRARY_LIST_FAVOURITES || p->kind == LIBRARY_LIST_MOST_PLAYED ||
		   p->kind == LIBRARY_LIST_RECENTLY_PLAYED;
}

// What a row stands for in the selection: its position on a playlist, which
// can hold the same track twice; the track on the other track lists; the name
// on the rest. `buf` holds the position.
static const char *row_key(const panel_t *p, int index, const char *name, const char *path, char *buf,
						   size_t size) {
	if (p->kind == LIBRARY_LIST_PLAYLIST) {
		snprintf(buf, size, "#%d", index);
		return buf;
	}
	return p->is_tracks ? path : name;
}

static int sel_find(const panel_t *p, const char *key) {
	if (!key) {
		return -1;
	}
	for (int i = 0; i < p->sel_count; i++) {
		if (strcmp(p->sel[i].key, key) == 0) {
			return i;
		}
	}
	return -1;
}

static void row_update_trail(panel_t *p, row_t *row, const char *name, const char *path);

static void row_update_selection(panel_t *p, row_t *row, const char *name, const char *path) {
	row_update_trail(p, row, name, path);
}

static void rows_update_selection(panel_t *p) {
	for (int i = 0; i < ROW_POOL; i++) {
		row_t *row = &p->rows[i];
		const char *name = NULL, *path = NULL;
		if (row->index >= 0 && row->index < p->count && row_at(p, row->index, &name, &path)) {
			row_update_selection(p, row, name, path);
		}
	}
}

static void sel_clear(panel_t *p) {
	for (int i = 0; i < p->sel_count; i++) {
		free(p->sel[i].key);
	}
	free(p->sel);
	p->sel = NULL;
	p->sel_count = 0;
	p->sel_cap = 0;
}

// The corner's usual buttons, in the order medialist_open() shows them, for
// saving and bringing back around the mode.
static lv_obj_t **corner_usual(panel_t *p, int *count) {
	static lv_obj_t *buttons[5];
	buttons[0] = p->album_btn;
	buttons[1] = p->reorder_btn;
	buttons[2] = p->play_btn;
	buttons[3] = p->reverse_btn;
	buttons[4] = p->sort_btn;
	*count = 5;
	return buttons;
}

static void show_if(lv_obj_t *obj, bool show) {
	if (show) {
		lv_obj_set_hidden(obj, false);
	} else {
		lv_obj_set_hidden(obj, true);
	}
}

// Selection mode's buttons, four on every list: the queue first, close last,
// and in between what fits the list. A playlist trades "add to a playlist" for
// "take out of this one", the favourites trade the star for its removal.
static void sel_buttons_show(panel_t *p, bool on) {
	bool playlist = p->kind == LIBRARY_LIST_PLAYLIST;
	bool favourites = p->kind == LIBRARY_LIST_FAVOURITES;
	show_if(p->sel_queue_btn, on);
	show_if(p->sel_fav_btn, on && !favourites);
	show_if(p->sel_add_btn, on && !playlist);
	show_if(p->sel_unfav_btn, on && favourites);
	show_if(p->sel_unlist_btn, on && playlist);
	show_if(p->sel_close_btn, on);
	if (p->sel_delete_btn) {
		show_if(p->sel_delete_btn, on && allow_delete && !p->from_paths);
		lv_obj_move_foreground(p->sel_delete_btn);
	}
}

static void head_bar_update(panel_t *p);

static void selection_stop(panel_t *p) {
	if (!p->selecting) {
		return;
	}
	p->selecting = false;
	sel_clear(p);

	int n = 0;
	lv_obj_t **buttons = corner_usual(p, &n);
	for (int i = 0; i < n; i++) {
		if (buttons[i] && (p->corner_shown & (1u << i))) {
			lv_obj_set_hidden(buttons[i], false);
		}
	}
	sel_buttons_show(p, false);
	settingsrow_title_corner_slots(p->title_label, p->cfg, p->corner_slots);
	rows_update_selection(p);
	head_bar_update(p);
}

static void selection_start(panel_t *p) {
	if (p->selecting) {
		return;
	}
	play_menu_hide();
	p->selecting = true;

	p->corner_shown = 0;
	int n = 0;
	lv_obj_t **buttons = corner_usual(p, &n);
	for (int i = 0; i < n; i++) {
		if (buttons[i] && !lv_obj_is_hidden(buttons[i])) {
			p->corner_shown |= 1u << i;
			lv_obj_set_hidden(buttons[i], true);
		}
	}
	sel_buttons_show(p, true);
	settingsrow_title_corner_slots(p->title_label, p->cfg, 4);
	lv_obj_move_foreground(p->corner);
	rows_update_selection(p);
	head_bar_update(p);
}

// Chooses the row at `index`, or lets it go. Letting go of the last one leaves
// the mode.
static void selection_toggle(panel_t *p, row_t *row, int index) {
	const char *name = NULL, *path = NULL;
	if (!row_at(p, index, &name, &path)) {
		return;
	}
	char buf[16];
	const char *key = row_key(p, index, name, path, buf, sizeof(buf));
	if (!key || !key[0]) {
		return;
	}

	int at = sel_find(p, key);
	if (at >= 0) {
		free(p->sel[at].key);
		p->sel[at] = p->sel[--p->sel_count];
		if (p->sel_count == 0) {
			selection_stop(p);
			return;
		}
	} else {
		if (p->sel_count == p->sel_cap) {
			int cap = p->sel_cap ? p->sel_cap * 2 : 16;
			struct sel_item *grown = realloc(p->sel, (size_t)cap * sizeof(*grown));
			if (!grown) {
				return;
			}
			p->sel = grown;
			p->sel_cap = cap;
		}
		char *copy = strdup(key);
		if (!copy) {
			return;
		}
		p->sel[p->sel_count].index = index;
		p->sel[p->sel_count].key = copy;
		p->sel_count++;
	}
	if (row) {
		row_update_selection(p, row, name, path);
	}
}

static void sel_close_cb(lv_event_t *e) { selection_stop(lv_event_get_user_data(e)); }

static int sel_by_index(const void *a, const void *b) {
	const struct sel_item *x = a, *y = b;
	return (x->index > y->index) - (x->index < y->index);
}

// The paths gathered for "add to playlist", in the order they go in.
typedef struct {
	char **paths;
	int count;
	int cap;
} path_list_t;

#define SELECTION_MAX_TRACKS 20000

static bool path_list_add(path_list_t *l, const char *path) {
	if (!path || !path[0] || l->count >= SELECTION_MAX_TRACKS) {
		return l->count < SELECTION_MAX_TRACKS;
	}
	if (l->count == l->cap) {
		int cap = l->cap ? l->cap * 2 : 64;
		char **grown = realloc(l->paths, (size_t)cap * sizeof(*grown));
		if (!grown) {
			return false;
		}
		l->paths = grown;
		l->cap = cap;
	}
	char *copy = strdup(path);
	if (!copy) {
		return false;
	}
	l->paths[l->count++] = copy;
	return true;
}

static bool collect_path_cb(const char *name, const char *path, const char *artist, void *user) {
	(void)name;
	(void)artist;
	return path_list_add(user, path);
}

static void path_list_free(path_list_t *l) {
	for (int i = 0; i < l->count; i++) {
		free(l->paths[i]);
	}
	free(l->paths);
	l->paths = NULL;
	l->count = l->cap = 0;
}

// Positions into a path list, sorted by path, for finding the repeats.
static char **dedupe_base;
static int dedupe_cmp(const void *a, const void *b) {
	int x = *(const int *)a, y = *(const int *)b;
	int c = strcmp(dedupe_base[x], dedupe_base[y]);
	return c ? c : (x > y) - (x < y);
}

// Drops every path already seen earlier in the list, keeping the order.
static void path_list_dedupe(path_list_t *l) {
	if (l->count < 2) {
		return;
	}
	int *order = malloc((size_t)l->count * sizeof(*order));
	bool *drop = calloc((size_t)l->count, sizeof(*drop));
	if (!order || !drop) {
		free(order);
		free(drop);
		return;
	}
	for (int i = 0; i < l->count; i++) {
		order[i] = i;
	}
	dedupe_base = l->paths;
	qsort(order, (size_t)l->count, sizeof(*order), dedupe_cmp);
	for (int i = 1; i < l->count; i++) {
		if (strcmp(l->paths[order[i]], l->paths[order[i - 1]]) == 0) {
			drop[order[i]] = true; // the later of the two, since ties sort by position
		}
	}
	int kept = 0;
	for (int i = 0; i < l->count; i++) {
		if (drop[i]) {
			free(l->paths[i]);
		} else {
			l->paths[kept++] = l->paths[i];
		}
	}
	l->count = kept;
	free(order);
	free(drop);
}

// The tracks behind the chosen rows, in list order, without repeats: the rows
// themselves on a track list; on the others, each album's tracks in its running
// order, each artist's gathered by record. Leaves the mode.
static void sel_gather(panel_t *p, path_list_t *list) {
	qsort(p->sel, (size_t)p->sel_count, sizeof(*p->sel), sel_by_index);

	for (int i = 0; i < p->sel_count; i++) {
		if (p->kind == LIBRARY_LIST_PLAYLIST) {
			char path[512];
			if (row_path_copy(p, p->sel[i].index, path, sizeof(path)) && !path_list_add(list, path)) {
				break;
			}
			continue;
		}
		if (p->is_tracks) {
			if (!path_list_add(list, p->sel[i].key)) {
				break;
			}
			continue;
		}
		library_order_t order = p->kind == LIBRARY_LIST_ALBUMS ? LIBRARY_ORDER_DEFAULT : LIBRARY_ORDER_ALBUM;
		library_index_t *ix = library_index_open(LIBRARY_LIST_TRACKS, filter_for(p->kind), p->sel[i].key, order, false);
		int total = library_index_count(ix);
		bool more = true;
		for (int first = 0; first < total && more; first += 256) {
			int want = total - first < 256 ? total - first : 256;
			int got = library_index_window(ix, first, want, collect_path_cb, list);
			more = got == want && list->count < SELECTION_MAX_TRACKS;
		}
		library_index_close(ix);
	}
	path_list_dedupe(list);
	selection_stop(p);
}

static void sel_add_cb(lv_event_t *e) {
	panel_t *p = lv_event_get_user_data(e);
	if (!p || !p->selecting || p->sel_count == 0) {
		return;
	}
	path_list_t list = {NULL, 0, 0};
	sel_gather(p, &list);
	if (list.count == 0) {
		gui_notify_popup("playlist_cannot_add_the_tracks");
	} else {
		playlistpage_add_tracks((const char *const *)list.paths, list.count);
	}
	path_list_free(&list);
}

// The chosen tracks into the queue, the first chosen first: after the one
// playing ("Play next", see playlist_insert_next()) or after everything ("Add to
// end of queue"), and the queue on disk written once for the set.
static void sel_queue_chosen(playlist_queue_where_t where, void *user) {
	panel_t *p = user;
	if (!p || !p->selecting || p->sel_count == 0) {
		return;
	}
	path_list_t list = {NULL, 0, 0};
	sel_gather(p, &list);
	trackmenu_queue_paths((const char *const *)list.paths, list.count, where);
	path_list_free(&list);
}

// The button asks which of the two first: a selection bar has one button for
// the queue, and both answers are common enough to be one tap away.
static void sel_queue_cb(lv_event_t *e) {
	panel_t *p = lv_event_get_user_data(e);
	if (!p || !p->selecting || p->sel_count == 0) {
		return;
	}
	trackmenu_queue_choose(lv_event_get_current_target(e), sel_queue_chosen, p);
}

static void sel_fav_cb(lv_event_t *e) {
	panel_t *p = lv_event_get_user_data(e);
	if (!p || !p->selecting || p->sel_count == 0) {
		return;
	}
	path_list_t list = {NULL, 0, 0};
	sel_gather(p, &list);
	if (list.count == 0) {
		gui_notify_popup("favourites_cannot_add_the_tracks");
	} else {
		playlistpage_add_favourites((const char *const *)list.paths, list.count);
	}
	path_list_free(&list);
}

static void window_update(panel_t *p);

// Taking rows out of the list on screen. Asked first: a slip of the finger on a
// selection of fifty is fifty tracks to find again.
static panel_t *remove_panel;
static int *remove_positions; // a playlist: positions in the list
static char **remove_paths;	  // the favourites: the tracks
static int remove_count;

static void remove_pending_free(void) {
	for (int i = 0; remove_paths && i < remove_count; i++) {
		free(remove_paths[i]);
	}
	free(remove_paths);
	free(remove_positions);
	remove_paths = NULL;
	remove_positions = NULL;
	remove_count = 0;
	remove_panel = NULL;
}

static void remove_confirmed(void *user) {
	(void)user;
	panel_t *p = remove_panel;
	if (!p || remove_count == 0) {
		remove_pending_free();
		return;
	}
	int removed = 0;
	if (remove_positions) {
		removed = playlists_remove_positions(p->playlist, remove_positions, remove_count);
	} else {
		removed = library_fav_remove_many((const char *const *)remove_paths, remove_count);
	}
	int wanted = remove_count;
	remove_pending_free();

	// The index behind the list has moved on; the list follows it.
	panel_refresh_stale(p);
	window_update(p);

	if (removed == wanted) {
		toast_success(removed == 1 ? "medialist_track_removed" : "medialist_tracks_removed");
	} else if (removed > 0) {
		toast_error("medialist_some_tracks_not_removed");
	} else {
		gui_notify_popup("medialist_remove_failed");
	}
}

static void sel_remove_ask(panel_t *p) {
	if (!p || !p->selecting || p->sel_count == 0) {
		return;
	}
	remove_pending_free();
	qsort(p->sel, (size_t)p->sel_count, sizeof(*p->sel), sel_by_index);

	int n = p->sel_count;
	if (p->kind == LIBRARY_LIST_PLAYLIST) {
		remove_positions = malloc((size_t)n * sizeof(*remove_positions));
		for (int i = 0; remove_positions && i < n; i++) {
			remove_positions[remove_count++] = p->sel[i].index;
		}
	} else {
		remove_paths = calloc((size_t)n, sizeof(*remove_paths));
		for (int i = 0; remove_paths && i < n; i++) {
			remove_paths[remove_count] = strdup(p->sel[i].key);
			if (remove_paths[remove_count]) {
				remove_count++;
			}
		}
	}
	bool playlist = p->kind == LIBRARY_LIST_PLAYLIST;
	selection_stop(p);
	if (remove_count == 0) {
		remove_pending_free();
		return;
	}
	remove_panel = p;

	char message[96];
	if (remove_count == 1) {
		snprintf(message, sizeof(message), "%s", tr("selection_one_track"));
	} else {
		snprintf(message, sizeof(message), tr("selection_tracks_count"), remove_count);
	}
	confirm_show(playlist ? "medialist_remove_from_playlist_2" : "remove_from_favourites_2", message, "remove",
				 remove_confirmed, NULL);
}

static void sel_unlist_cb(lv_event_t *e) { sel_remove_ask(lv_event_get_user_data(e)); }

static void sel_unfav_cb(lv_event_t *e) { sel_remove_ask(lv_event_get_user_data(e)); }

// Deleting the chosen tracks: off the card, and out of the library with them
// (library_forget_deleted), after asking. Kept between the question and the
// answer, the selection being gone by then.
static path_list_t delete_pending;
static panel_t *delete_panel;

static void delete_confirmed(void *user) {
	(void)user;
	panel_t *p = delete_panel;
	delete_panel = NULL;
	int wanted = delete_pending.count;
	if (!p || wanted == 0) {
		path_list_free(&delete_pending);
		return;
	}

	const char **gone = calloc((size_t)wanted, sizeof(*gone));
	int deleted = 0;
	for (int i = 0; gone && i < wanted; i++) {
		const char *path = delete_pending.paths[i];
		// A track of a cue sheet is a stretch of a file that holds the others
		// too: there is nothing of its own to delete.
		if (strstr(path, "?track=")) {
			continue;
		}
		if (unlink(path) == 0 || errno == ENOENT) {
			gone[deleted++] = path;
		} else {
			fprintf(stderr, "medialist: cannot delete %s: %s\n", path, strerror(errno));
		}
	}
	if (deleted > 0) {
		library_forget_deleted(gone, deleted);
	}
	free(gone);
	path_list_free(&delete_pending);

	// The index behind the list has moved on; the list follows it.
	panel_refresh_stale(p);
	window_update(p);

	if (deleted == wanted) {
		toast_success(deleted == 1 ? "medialist_track_deleted" : "medialist_tracks_deleted");
	} else if (deleted > 0) {
		toast_error("medialist_some_tracks_not_deleted");
	} else {
		gui_notify_popup("medialist_delete_failed");
	}
}

static void sel_delete_cb(lv_event_t *e) {
	panel_t *p = lv_event_get_user_data(e);
	if (!p || !p->selecting || p->sel_count == 0 || !allow_delete) {
		return;
	}
	path_list_free(&delete_pending);
	sel_gather(p, &delete_pending); // leaves the mode, as the other buttons do
	if (delete_pending.count == 0) {
		return;
	}
	delete_panel = p;

	char message[96];
	if (delete_pending.count == 1) {
		snprintf(message, sizeof(message), "%s", tr("selection_one_track"));
	} else {
		snprintf(message, sizeof(message), tr("selection_tracks_count"), delete_pending.count);
	}
	confirm_show("medialist_delete_tracks_question", message, "delete", delete_confirmed, NULL);
}

static bool panel_swipes(const panel_t *p);

// The trailing slot for what the list is doing: the tick of a chosen row in
// selection mode, the grip while a playlist is put in order, otherwise the
// star of a track and the chevron of everything else.
static void row_update_trail(panel_t *p, row_t *row, const char *name, const char *path) {
	row_kind_t kind = panel_row_kind(p);
	if (p->selecting) {
		char buf[16];
		bool chosen = sel_find(p, row_key(p, row->index, name, path, buf, sizeof(buf))) >= 0;
		sx_row_set_trail(row->row, chosen ? SX_ROW_TRAIL_CHECK : SX_ROW_TRAIL_BLANK);
	} else if (p->reordering && kind == RK_TRACK) {
		sx_row_set_trail(row->row, SX_ROW_TRAIL_GRIP);
	} else if (kind == RK_TRACK) {
		sx_row_set_starred(row->row, path && library_fav_contains(path));
		sx_row_set_trail(row->row, path ? SX_ROW_TRAIL_STAR : SX_ROW_TRAIL_NONE);
	} else {
		sx_row_set_trail(row->row, SX_ROW_TRAIL_CHEVRON);
	}
	bool swipes = panel_swipes(p) && !p->selecting && !p->reordering;
	sx_row_set_swipes(row->row, swipes, swipes);
}

// The lead: the cover's plaque (and its picture, when the row already has
// it), the track number on the album page, a letter on a plaque for a name.
static void row_update_lead(panel_t *p, row_t *row, int index, const char *name, const char *path) {
	row_kind_t kind = panel_row_kind(p);
	if (kind == RK_TRACK && p->album_page) {
		sx_row_set_number(row->row, p->track_no ? (int)p->track_no[index] : index + 1);
		return;
	}
	if (kind == RK_TRACK || kind == RK_ALBUM) {
		if (kind == RK_ALBUM) {
			rowart_bind(&row->art, name);
		} else {
			char album[300];
			rowart_album_of_track(path, album, sizeof(album));
			rowart_bind(&row->art, album);
		}
		row->picture_shown = rowart_picture(&row->art) != NULL;
		sx_row_set_thumb(row->row, rowart_picture(&row->art), rowart_tone(&row->art));
		return;
	}
	char letter[8];
	if (kind == RK_YEAR) {
		size_t n = name ? strlen(name) : 0;
		snprintf(letter, sizeof(letter), "%s", n >= 2 ? name + n - 2 : (name ? name : ""));
	} else {
		listacts_first_letter(name, letter, sizeof(letter));
	}
	sx_row_set_letter(row->row, letter, listacts_name_tone(name), kind == RK_PERSON);
}

static void row_bind(panel_t *p, row_t *row, int index) {
	if (row->index == index) {
		return;
	}

	const char *name = NULL;
	const char *path = NULL;
	bool found = index >= 0 && index < p->count && row_at(p, index, &name, &path);
	if (row->index != index) {
		row->count = -1;
	}
	row->index = index;

	if (!found) {
		// Past the end, or the handle went stale under the list, or the row is
		// gone. Hiding the row is what a reload will fix; drawing a neighbour's
		// name would not look like a fault at all.
		lv_obj_set_hidden(row->row, true);
		return;
	}

	lv_obj_set_hidden(row->row, false);
	if (lv_obj_get_style_height(row->row, LV_PART_MAIN) != p->row_h) {
		sx_row_set_compact(row->row, false); // "Large rows" changed since it was last bound
	}
	lv_obj_set_y(row->row, row_y(p, index));
	// An album row is named by its value -- name and key, see library.h -- and
	// only the name is for reading.
	char shown[sizeof(((winrow_t *)0)->name)];
	library_album_title(name, shown, sizeof(shown));
	sx_row_set_title(row->row, shown);
	row_update_lead(p, row, index, name, path);
	row_update_detail(p, row, index, path);
	row_update_trail(p, row, name, path);
	row_update_playmark(p, row);
}

// ---------------------------------------------------------------------------
// What a row shows a moment after it is drawn
//
// The cover's thumbnail and an artist's, a genre's or a year's count each
// cost a read -- of the card, of the index -- so they are not done as a row is
// bound, which happens on every step of a scroll, but on a timer, for the rows
// actually on screen, a few at a time, and not while the list moves. Until
// then the row shows the plaque in the cover's tone and no count.
// ---------------------------------------------------------------------------

// The counts already worked out, by list and name: scrolling back up a list
// does not ask again. Forgotten when the library changes.
#define COUNT_CACHE 96
static struct {
	uint64_t key;
	int count;
} count_cache[COUNT_CACHE];
static int count_cache_next;
static unsigned count_cache_revision;

static uint64_t count_key(library_list_t kind, const char *name) {
	uint64_t h = 1469598103934665603ULL ^ (uint64_t)kind;
	for (const unsigned char *c = (const unsigned char *)name; *c; c++) {
		h ^= *c;
		h *= 1099511628211ULL;
	}
	return h ? h : 1;
}

// How many records an artist (or a year) has, or how many tracks a genre:
// what the row says under the name.
static int count_for(const panel_t *p, const char *name) {
	unsigned rev = library_revision(LIBRARY_LIST_TRACKS);
	if (rev != count_cache_revision) {
		memset(count_cache, 0, sizeof(count_cache));
		count_cache_revision = rev;
	}
	uint64_t key = count_key(p->kind, name);
	for (int i = 0; i < COUNT_CACHE; i++) {
		if (count_cache[i].key == key) {
			return count_cache[i].count;
		}
	}
	library_list_t kind = p->kind == LIBRARY_LIST_GENRES ? LIBRARY_LIST_TRACKS : LIBRARY_LIST_ALBUMS;
	library_filter_t filter = filter_for(p->kind);
	library_index_t *ix = library_index_open(kind, filter, name, LIBRARY_ORDER_DEFAULT, false);
	int count = library_index_count(ix);
	library_index_close(ix);
	count_cache[count_cache_next].key = key;
	count_cache[count_cache_next].count = count;
	count_cache_next = (count_cache_next + 1) % COUNT_CACHE;
	return count;
}

// Whether `row` is inside the viewport now.
static bool row_on_screen(const panel_t *p, const row_t *row) {
	if (row->index < 0 || row->index >= p->count || lv_obj_is_hidden(row->row)) {
		return false;
	}
	int scroll = lv_obj_get_scroll_y(p->list);
	int top = row_y(p, row->index);
	return top + p->row_h > scroll && top < scroll + lv_obj_get_height(p->list);
}

static void thumbs_update(panel_t *p) {
	if (!p->list || p->count <= 0) {
		return;
	}
	// Nothing touches the card while the list moves under a finger or flies
	// on: the timer comes back for it once it has settled.
	if (rowart_list_busy(p->list) || switcher_nav_anim_running()) {
		lv_timer_resume(thumb_timer);
		return;
	}
	row_kind_t kind = panel_row_kind(p);
	bool pending = false;
	int loads = 0;
	int counts = 0;
	for (int i = 0; i < ROW_POOL; i++) {
		row_t *row = &p->rows[i];
		if (!row_on_screen(p, row)) {
			continue;
		}
		if (kind == RK_PERSON || kind == RK_GENRE || kind == RK_YEAR) {
			if (row->count >= 0) {
				continue;
			}
			if (counts >= COUNTS_PER_PASS) {
				pending = true;
				continue;
			}
			const char *name = NULL;
			if (!row_at(p, row->index, &name, NULL) || !name) {
				continue;
			}
			char copy[256];
			snprintf(copy, sizeof(copy), "%s", name);
			row->count = count_for(p, copy);
			counts++;
			row_update_detail(p, row, row->index, NULL);
			continue;
		}
		if (kind == RK_TRACK && p->album_page) {
			continue; // numbers, not covers
		}
		if (!rowart_wanted(&row->art)) {
			if (!row->picture_shown && rowart_picture(&row->art)) {
				sx_row_set_thumb(row->row, rowart_picture(&row->art), rowart_tone(&row->art));
				row->picture_shown = true;
			}
			continue;
		}
		if (loads >= THUMB_LOADS_PER_PASS) {
			pending = true;
			continue;
		}
		loads++;
		if (rowart_load(&row->art)) {
			sx_row_set_thumb(row->row, rowart_picture(&row->art), rowart_tone(&row->art));
			row->picture_shown = rowart_picture(&row->art) != NULL;
		}
	}
	if (pending) {
		lv_timer_resume(thumb_timer);
	}
}

static void thumb_timer_cb(lv_timer_t *timer) {
	lv_timer_pause(timer); // thumbs_update resumes it while work is pending

	// Every panel that draws covers, the artist-records screen included.
	//
	// window_update() rather than thumbs_update() alone, so a list that went
	// stale while its covers were loading is read again and finishes them.
	panel_t *const panels[] = {&panel_names, &panel_tracks, &panel_artist_albums};
	lv_obj_t *active = lv_screen_active();
	for (size_t i = 0; i < sizeof(panels) / sizeof(panels[0]); i++) {
		if (active == panels[i]->screen) {
			window_update(panels[i]);
			return;
		}
	}
}

// covertone has new tones or thumbnails: the rows on screen look again.
static void covers_changed(void) {
	if (thumb_timer) {
		lv_timer_resume(thumb_timer);
	}
}

static void window_update(panel_t *p) {
	panel_refresh_stale(p);

	if (p->count == 0) {
		for (int i = 0; i < ROW_POOL; i++) {
			row_bind(p, &p->rows[i], -1);
		}
		return;
	}

	int scroll = lv_obj_get_scroll_y(p->list);
	if (scroll < 0) {
		scroll = 0;
	}

	int first = row_at_y(p, scroll) - 1;
	if (first + ROW_POOL > p->count) {
		first = p->count - ROW_POOL;
	}
	if (first < 0) {
		first = 0;
	}

	// index -> widget index % ROW_POOL, so a one-step slide of the band
	// rebinds exactly one row instead of renaming all of them.
	for (int i = 0; i < ROW_POOL; i++) {
		int index = first + i;
		row_bind(p, &p->rows[index % ROW_POOL], index < p->count ? index : -1);
	}

	// row_bind() is a no-op when a row's index has not moved, so a row whose
	// *content* changed underneath it (the list reloaded to the same length,
	// a reorder) would keep the mark it was given last time. Re-deciding it
	// for the whole pool here is twelve string compares and makes the mark
	// self-correcting whatever route the list took to get here.
	panel_refresh_playmarks(p);

	thumbs_update(p);
}

void medialist_refresh_visible(void) {
	panel_t *const panels[] = {&panel_names, &panel_tracks, &panel_artist_albums};
	lv_obj_t *active = lv_screen_active();
	for (size_t i = 0; i < sizeof(panels) / sizeof(panels[0]); i++) {
		if (panels[i]->rows[0].row && active == panels[i]->screen) {
			window_update(panels[i]);
			return;
		}
	}
}

// Every row of every panel bound again, for a setting that changes what a row
// shows. Only panels that have been built and hold a list.
static void rebind_all_panels(void) {
	panel_t *panels[] = {&panel_names, &panel_tracks, &panel_artist_albums};
	for (size_t i = 0; i < sizeof(panels) / sizeof(panels[0]); i++) {
		panel_t *p = panels[i];
		if (!p->rows[0].row || !p->list) {
			continue;
		}
		for (int r = 0; r < ROW_POOL; r++) {
			p->rows[r].index = -1;
		}
		window_update(p);
	}
}

void medialist_set_quality_badges(bool on) {
	load_view_settings();
	if (quality_badges == on) {
		return;
	}
	quality_badges = on;
	config_set_int("library", "quality_badges", on ? 1 : 0);
	config_save();

	// The rows already bound were bound with the old answer, and row_bind()
	// leaves a row whose index has not moved alone. Forgetting their indices is
	// what makes the switch show on the list behind the settings page instead
	// of at the next list opened.
	if (!panel_tracks.rows[0].row) {
		return;
	}
	for (int i = 0; i < ROW_POOL; i++) {
		panel_tracks.rows[i].index = -1;
	}
	window_update(&panel_tracks);
}

bool medialist_show_artist(void) {
	load_view_settings();
	return show_artist;
}

int medialist_artist_lists(void) {
	load_view_settings();
	return artist_lists;
}

static void rebind_all_panels(void);

void medialist_set_show_artist(bool on, int lists) {
	load_view_settings();
	if (show_artist == on && artist_lists == lists) {
		return;
	}
	show_artist = on;
	artist_lists = lists;
	config_set_int("library", "show_artist", on ? 1 : 0);
	config_set_int("library", "artist_lists", lists);
	config_save();
	// As with the badges: the list behind the settings page shows the change,
	// not only the next one opened.
	rebind_all_panels();
}

// Takes one entry out of the model and lays the list out again. Used when the
// user removes the thing a row stands for -- a favourite, a playlist entry --
// so the row goes at once instead of at the next visit to the page. Reloading
// the whole list would work too, but it loses the scroll position and throws
// away every thumbnail already decoded.
static void model_remove(panel_t *p, int index) {
	if (index < 0 || index >= p->count) {
		return;
	}

	// Only a list of paths gets here. A query-backed one is rebuilt instead:
	// everything that can take a row out of one (unstarring, for now) moves row
	// ids as it does so, which the handle notices and panel_refresh_stale acts
	// on -- and the caller runs that first.
	if (!p->from_paths) {
		return;
	}

	free(p->entries[index].name);
	free(p->entries[index].path);
	memmove(&p->entries[index], &p->entries[index + 1], (size_t)(p->count - index - 1) * sizeof(entry_t));
	p->count--;
	p->entries_count = p->count;

	// Every pooled row now stands for a different entry, so none of them may
	// keep the artwork or the index it had.
	for (int i = 0; i < ROW_POOL; i++) {
		row_drop_thumb(p, &p->rows[i]);
		p->rows[i].index = -2;
	}

	lv_obj_set_height(p->body, body_height(p));
	if (p->count == 0) {
		lv_obj_set_hidden(p->empty, false);
	}
	window_update(p);
}

// ---------------------------------------------------------------------------
// the A-Z strip
// ---------------------------------------------------------------------------

static const char *const INDEX_TEXT[INDEX_BUCKETS] = {"#", "A", "B", "C", "D", "E", "F", "G", "H", "I",
													  "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S",
													  "T", "U", "V", "W", "X", "Y", "Z", "\xE2\x80\xA6"};


static void index_show_bar(panel_t *p, bool shown) {
	if (!p->index_bar) {
		return;
	}
	if (shown) {
		lv_obj_set_hidden(p->index_bar, false);
		lv_obj_move_foreground(p->index_bar);
	} else {
		lv_obj_set_hidden(p->index_bar, true);
	}
}

// The strip is a lane that stays (the handoff's rail), so a scroll asks
// nothing of it any more; kept as the one place a future fade would go.
static void index_flash(panel_t *p) { (void)p; }

static void index_hint_show(panel_t *p, int shown) {
	if (!p->index_hint || shown < 0 || shown >= p->index_shown) {
		return;
	}
	lv_label_set_text(p->index_hint_label, INDEX_TEXT[p->index_bucket[shown]]);
	lv_obj_set_hidden(p->index_hint, false);
	lv_obj_move_foreground(p->index_hint);
	p->index_hint_at = lv_tick_get();
}

// The rows' width: the body's right margin is the lane's when there is one.
static void index_make_room(panel_t *p) {
	int right = p->index_enabled ? AZ_RIGHT_PAD : (int)p->cfg->padding;
	lv_obj_set_style_pad_right(p->list, right, 0);
	lv_obj_set_width(p->body, (int)p->cfg->screen_width - (int)p->cfg->padding - right);
	// The lane is where the scrollbar would be, and says more.
	lv_obj_set_scrollbar_mode(p->list, p->index_enabled ? LV_SCROLLBAR_MODE_OFF : LV_SCROLLBAR_MODE_AUTO);
}

// Rebuilds the letter -> row map from the model, and decides whether this list
// gets a strip at all: the handoff's rail, on an alphabetical list of
// AZ_MIN_ROWS rows or more.
static void index_rebuild(panel_t *p, bool enabled, bool descending) {
	p->index_enabled = enabled && p->count >= AZ_MIN_ROWS && p->index_bar;
	p->index_desc = descending;
	p->index_shown = 0;

	if (!p->index_bar) {
		return;
	}
	if (p->index_hint) {
		lv_obj_set_hidden(p->index_hint, true);
	}

	// Where each letter's rows begin.
	//
	// Taken from the counts the handle keeps as it goes past (see
	// library_index_open): the first row of a bucket is the sum of the ones
	// before it. That holds only because bucket order and collation order are
	// the same thing: '#' covers the groups the collation files above A, A-Z is
	// Latin folded to its first letter, and the last bucket is everything filed
	// below Z.
	//
	// A list of paths has no handle and no counts, so it walks its own names --
	// those lists are a playlist long, not a library long.
	int first[INDEX_BUCKETS];
	for (int i = 0; i < INDEX_BUCKETS; i++) {
		first[i] = -1;
	}

	int counts[LIBRARY_INDEX_BUCKETS];
	if (!p->index_enabled) {
		// nothing to count
	} else if (!p->from_paths && library_index_buckets(p->ix, counts)) {
		int seen = 0;
		for (int bucket = 0; bucket < INDEX_BUCKETS; bucket++) {
			if (counts[bucket] > 0) {
				// Ascending, the bucket starts after everything before it.
				// Descending, the list is the same rows read from the other
				// end, so it starts that far from the bottom.
				first[bucket] = descending ? p->count - seen - counts[bucket] : seen;
				seen += counts[bucket];
			}
		}
	} else if (p->from_paths) {
		for (int i = 0; i < p->entries_count; i++) {
			int bucket = library_index_slot(library_index_letter(p->entries[i].name));
			if (first[bucket] < 0) {
				first[bucket] = i;
			}
		}
	} else {
		// No trustworthy counts: better no strip than one that lands on the
		// wrong rows.
		p->index_enabled = false;
	}

	// Top to bottom on screen, which is the alphabet turned round on a Z-A
	// list, and only the letters the list has: the strip always reads the way
	// the list under it runs, and every letter on it leads somewhere.
	if (p->index_enabled) {
		for (int slot = 0; slot < INDEX_BUCKETS; slot++) {
			int bucket = descending ? INDEX_BUCKETS - 1 - slot : slot;
			if (first[bucket] < 0) {
				continue;
			}
			p->index_bucket[p->index_shown] = bucket;
			p->index_row[p->index_shown] = first[bucket];
			p->index_shown++;
		}
		// One letter is no index at all.
		if (p->index_shown < 2) {
			p->index_enabled = false;
			p->index_shown = 0;
		}
	}
	for (int i = 0; i < INDEX_BUCKETS; i++) {
		bool on = i < p->index_shown;
		lv_obj_set_hidden(p->index_letters[i], !on);
		if (on) {
			lv_label_set_text(p->index_letters[i], INDEX_TEXT[p->index_bucket[i]]);
		}
	}
	index_make_room(p);
	index_show_bar(p, p->index_enabled);
}

static void index_go(panel_t *p, int shown) {
	if (p->index_shown <= 0) {
		return;
	}
	if (shown < 0) {
		shown = 0;
	}
	if (shown >= p->index_shown) {
		shown = p->index_shown - 1;
	}

	index_hint_show(p, shown);

	int row = p->index_row[shown];
	if (row < 0) {
		return;
	}
	int max = body_height(p) - lv_obj_get_height(p->list) + lv_obj_get_style_pad_bottom(p->list, LV_PART_MAIN);
	int y = row_y(p, row);
	lv_obj_scroll_to_y(p->list, y > max && max > 0 ? max : y, LV_ANIM_OFF);
	p->saved_scroll = lv_obj_get_scroll_y(p->list);
	window_update(p);
}

// The letter the finger is over. Asked of the letters themselves rather than
// worked out from the strip's height: the labels are laid out with even gaps
// around them, so they do not quite fill the box, and arithmetic on the box
// answers a press near either end with the neighbouring letter.
static void index_go_at(panel_t *p, lv_point_t point) {
	for (int i = 0; i < p->index_shown; i++) {
		lv_area_t area;
		lv_obj_get_coords(p->index_letters[i], &area);
		if (point.y <= area.y2) {
			index_go(p, i);
			return;
		}
	}
	index_go(p, p->index_shown - 1); // past the last letter: the last letter
}

// The strip carries the page's own gestures as well -- the swipe back and the
// pull that brings the player in -- so a press on it does not commit to the
// alphabet until it is clear that is what it is. Sideways belongs to the page;
// up and down, or a tap that does not move at all, belongs to the strip. Which
// is why nothing jumps on the press itself: a swipe that begins over the strip
// would have moved the list first and slid the page away over the result.
static void index_bar_cb(lv_event_t *e) {
	panel_t *p = lv_event_get_user_data(e);
	lv_event_code_t code = lv_event_get_code(e);

	// The page took the gesture: the strip is out of it until the next press.
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		p->index_engaged = false;
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		lv_indev_t *indev = lv_indev_active();
		if (code == LV_EVENT_RELEASED && !p->index_engaged && indev) {
			// It never moved: a tap on a letter, which is the other way the
			// strip is meant to be used. Acted on at the lift.
			lv_point_t point;
			lv_indev_get_point(indev, &point);
			index_go_at(p, point);
		}
		p->index_hint_at = lv_tick_get(); // from here the big letter fades out
		p->index_engaged = false;
		return;
	}

	lv_indev_t *indev = lv_indev_active();
	if (!indev) {
		return;
	}
	lv_point_t point;
	lv_indev_get_point(indev, &point);

	index_flash(p); // the strip stays up for as long as a finger is on it

	if (code == LV_EVENT_PRESSED) {
		p->index_press = point;
		p->index_engaged = false;
		return;
	}
	if (code != LV_EVENT_PRESSING) {
		return;
	}

	if (!p->index_engaged) {
		int dx = LV_ABS(point.x - p->index_press.x);
		int dy = LV_ABS(point.y - p->index_press.y);
		if (dx > dy) {
			return; // going sideways: this is the page's, not the strip's
		}
		if (dy < INDEX_ENGAGE_PX) {
			return; // not enough to tell yet
		}
		p->index_engaged = true;
	}

	index_go_at(p, point);
}

// ---------------------------------------------------------------------------
// The sentinel
//
// The strip's two objects are built once and never deleted, so a bad pointer in
// either can only mean something wrote over the panel -- and in panel_t they sit
// immediately after a 600-byte char buffer. They are therefore written down when
// they are built and checked before they are used: if the two disagree the log
// says so and the strip switches itself off.
//
// The copy is kept outside panel_t on purpose. One stored next to what it guards
// is overwritten by the same run of bytes that overwrote the original, and would
// agree with it all the way into the fault.
// ---------------------------------------------------------------------------

static struct {
	const panel_t *panel;
	lv_obj_t *bar;
	lv_obj_t *hint;
	bool reported;
} index_built[3];
static int index_built_count;

static void index_guard_note(panel_t *p) {
	for (int i = 0; i < index_built_count; i++) {
		if (index_built[i].panel == p) {
			index_built[i].bar = p->index_bar;
			index_built[i].hint = p->index_hint;
			return;
		}
	}
	if (index_built_count >= (int)(sizeof(index_built) / sizeof(index_built[0]))) {
		return;
	}
	index_built[index_built_count].panel = p;
	index_built[index_built_count].bar = p->index_bar;
	index_built[index_built_count].hint = p->index_hint;
	index_built_count++;
}

// False when the panel no longer holds the objects it was built with. Says so
// once, then takes the strip out of service: a pointer that has changed by
// itself is not one to hand to LVGL.
static bool index_guard_ok(panel_t *p) {
	for (int i = 0; i < index_built_count; i++) {
		if (index_built[i].panel != p) {
			continue;
		}
		if (p->index_bar == index_built[i].bar && p->index_hint == index_built[i].hint) {
			return true;
		}
		if (!index_built[i].reported) {
			index_built[i].reported = true;
			fprintf(stderr,
					"medialist: the A-Z strip was overwritten (bar %p, was %p; hint %p, was %p) -- "
					"switching it off\n",
					(void *)p->index_bar, (void *)index_built[i].bar, (void *)p->index_hint,
					(void *)index_built[i].hint);
			fflush(stderr);
		}
		p->index_bar = NULL;
		p->index_hint = NULL;
		p->index_enabled = false;
		return false;
	}
	return true; // never recorded: there is nothing to compare against
}

// Takes the strip and the big letter away once nothing has asked for them for
// a moment. One timer for all three panels; only the one on screen can be
// showing anything.
static void index_tick(panel_t *p) {
	if (!index_guard_ok(p)) {
		return;
	}
	if (p->index_hint && !lv_obj_is_hidden(p->index_hint) && lv_tick_elaps(p->index_hint_at) > INDEX_HINT_MS) {
		lv_obj_set_hidden(p->index_hint, true);
	}
}

static void index_timer_cb(lv_timer_t *timer) {
	(void)timer;
	// All three: the artist's albums has a strip of its own, and a panel left
	// out here has nothing to hide its strip again once it has shown.
	index_tick(&panel_names);
	index_tick(&panel_tracks);
	index_tick(&panel_artist_albums);
}

// The reorder button's glyph: the accent while the list is being reordered,
// ordinary ink the rest of the time.
//
// It is worth a function of its own because it is the only place that decides
// this colour, and because a hand-set colour is a snapshot: the glyph is built
// wearing theme_style_icon and follows the palette by itself only until reorder
// mode is first entered or left, at which point a local property is pinned on it
// and the shared style can no longer reach it. The theme refresh therefore calls
// this too.
static void reorder_icon_paint(panel_t *p) {
	if (!p->reorder_btn) {
		return;
	}
	lv_obj_t *glyph = lv_obj_get_child(p->reorder_btn, 0);
	if (glyph) {
		lv_obj_set_style_image_recolor(glyph, p->reordering ? theme()->accent : theme()->text_primary, 0);
	}
}

// Whether this panel's list runs by date rather than by name.
static bool sort_by_date(const panel_t *p) { return sort_can_date(p->kind, p->filter) && sort_is_added(p->kind); }

// Whether it runs by release year.
static bool sort_by_year(const panel_t *p) { return sort_can_year(p->kind, p->filter) && sort_is_year(p->kind); }

// Whether it runs by artist. Checked before the other two: it is the one a
// list can be in while a year or date bit is still set from an older build.
static bool sort_by_artist(const panel_t *p) {
	return sort_can_artist(p->kind, p->filter) && sort_is_artist(p->kind);
}

// The sort button's glyph says how the list runs now: by name, by date or by
// year, and which way. By name and by date the arrow points the way the list
// reads; the year glyphs have theirs the other way round, down for the years
// going up.
// The big letter follows the accent, which a palette change moves; the
// letters follow their shared style on their own.
static void index_theme_refresh(void) {
	panel_t *panels[] = {&panel_names, &panel_tracks, &panel_artist_albums};
	for (size_t i = 0; i < sizeof(panels) / sizeof(panels[0]); i++) {
		reorder_icon_paint(panels[i]);
	}
}

// The strip and the big letter, built once with the panel and hidden.
static void index_build(panel_t *p, gui_config_t *cfg) {
	int content_top = settingsrow_content_top(cfg);

	// Down the right edge from the body's top to the screen's bottom; the
	// frame keeps its end clear of the mini player (sx_frame_reserve_bottom
	// does not apply to a box, so its height follows the slot by hand).
	p->index_bar = lv_obj_create(p->screen);
	lv_obj_remove_style_all(p->index_bar);
	lv_obj_set_size(p->index_bar, INDEX_BAR_WIDTH, cfg->screen_height - content_top - 8);
	lv_obj_align(p->index_bar, LV_ALIGN_TOP_RIGHT, -2, content_top + 4);
	lv_obj_set_scrollable(p->index_bar, false);
	lv_obj_set_hidden(p->index_bar, true);
	lv_obj_set_flex_flow(p->index_bar, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(p->index_bar, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	sx_frame_exempt(p->index_bar);

	// The strip takes its own presses -- it must not scroll the list underneath
	// -- but it is still part of the page, so the pull that brings the player
	// in starts on it as well. Which a press is gets decided in index_bar_cb,
	// from the direction it moves.
	lv_obj_set_clickable(p->index_bar, true);
	lv_obj_add_event_cb(p->index_bar, index_bar_cb, LV_EVENT_PRESSED, p);
	lv_obj_add_event_cb(p->index_bar, index_bar_cb, LV_EVENT_PRESSING, p);
	lv_obj_add_event_cb(p->index_bar, index_bar_cb, LV_EVENT_RELEASED, p);
	lv_obj_add_event_cb(p->index_bar, index_bar_cb, LV_EVENT_PRESS_LOST, p);
	switcher_attach_back_gesture(p->index_bar);
	player_sheet_attach_drag(p->index_bar, true);

	for (int i = 0; i < INDEX_BUCKETS; i++) {
		p->index_letters[i] = lv_label_create(p->index_bar);
		lv_label_set_text(p->index_letters[i], INDEX_TEXT[i]);
		lv_obj_add_style(p->index_letters[i], &sx_style_text2, 0);
		lv_obj_set_style_text_font(p->index_letters[i], &font_ui_17, 0);
		// The presses belong to the strip as a whole: the letter under the
		// finger comes from where it is, not from which label it landed on.
		lv_obj_set_clickable(p->index_letters[i], false);
	}

	// The letter under the finger, in the middle of the screen where the hand
	// is not: the handoff's bubble, SX_AZ_BUBBLE across, in the accent.
	p->index_hint = lv_obj_create(p->screen);
	lv_obj_remove_style_all(p->index_hint);
	lv_obj_set_size(p->index_hint, SX_AZ_BUBBLE, SX_AZ_BUBBLE);
	lv_obj_align(p->index_hint, LV_ALIGN_CENTER, 0, 0);
	lv_obj_add_style(p->index_hint, &sx_style_accent, 0);
	lv_obj_set_style_bg_opa(p->index_hint, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(p->index_hint, 26, 0);
	lv_obj_set_scrollable(p->index_hint, false);
	lv_obj_set_clickable(p->index_hint, false);
	lv_obj_set_hidden(p->index_hint, true);
	sx_frame_exempt(p->index_hint);

	p->index_hint_label = lv_label_create(p->index_hint);
	lv_label_set_text(p->index_hint_label, "");
	lv_obj_set_style_text_font(p->index_hint_label, &font_ui_64_bold, 0);
	lv_obj_center(p->index_hint_label);

	index_guard_note(p);
}

// The lane ends where the bottom slot starts while the mini player is up.
static void index_fit_slot(void) {
	panel_t *panels[] = {&panel_names, &panel_tracks, &panel_artist_albums};
	for (size_t i = 0; i < sizeof(panels) / sizeof(panels[0]); i++) {
		panel_t *p = panels[i];
		if (p->index_bar && p->cfg) {
			int top = settingsrow_content_top(p->cfg);
			lv_obj_set_height(p->index_bar, (int)p->cfg->screen_height - top - 8 - sx_frame_body_bottom());
		}
	}
}

static void head_follow(panel_t *p);

static void scroll_cb(lv_event_t *e) {
	panel_t *p = lv_event_get_user_data(e);
	p->saved_scroll = lv_obj_get_scroll_y(p->list);
	index_flash(p);
	head_follow(p);
	window_update(p);
}

// ---------------------------------------------------------------------------
// clicks
// ---------------------------------------------------------------------------

static library_filter_t filter_for(library_list_t kind) {
	switch (kind) {
	case LIBRARY_LIST_ALBUMS:
		return LIBRARY_FILTER_ALBUM;
	case LIBRARY_LIST_ARTISTS:
		return LIBRARY_FILTER_ARTIST;
	case LIBRARY_LIST_ALBUM_ARTISTS:
		return LIBRARY_FILTER_ALBUM_ARTIST;
	case LIBRARY_LIST_GENRES:
		return LIBRARY_FILTER_GENRE;
	case LIBRARY_LIST_YEARS:
		return LIBRARY_FILTER_YEAR;
	default:
		return LIBRARY_FILTER_NONE;
	}
}

static void row_sheet(panel_t *p, row_t *row);

// A tap on a track or a name: the track plays, with this list as the queue in
// the order it is on screen; a name opens its records or its tracks.
static void row_activate(panel_t *p, int index) {
	if (index < 0 || index >= p->count) {
		return;
	}

	char path[512];
	char name[256];
	{
		const char *n = NULL;
		const char *pth = NULL;
		if (!row_at(p, index, &n, &pth)) {
			return;
		}
		snprintf(name, sizeof(name), "%s", n ? n : "");
		snprintf(path, sizeof(path), "%s", pth ? pth : "");
		if (p->is_tracks && !pth) {
			return;
		}
	}

	if (p->is_tracks) {
		// The queue is this list, so next/prev walk it -- not the folder the
		// file happens to live in.
		//
		// A query-backed list hands the queue a copy of its row ids, not its
		// rows: the queue is then the same four bytes a track the list is,
		// instead of a second full set of paths beside it.
		bool queued = false;
		if (!p->from_paths) {
			// The same refresh menu_tracks_open() does, and for the same reason:
			// a clone of a stale handle is stale too, and the queue cannot read
			// the path of the row it was told to start on. The rows are bound
			// again with it.
			//
			// It also keeps the reopen off the audio thread's back: done here,
			// the index is current before playback is asked for, instead of
			// being rebuilt inside playlist_current_path() while the playback
			// thread is trying to take the Bluetooth transport again.
			window_update(p);
			if (index >= p->count) {
				return; // the list got shorter under the finger
			}
			queued = device_state_play_index(library_index_clone(p->ix), index);
		} else {
			const char **paths = malloc((size_t)p->entries_count * sizeof(char *));
			if (paths) {
				for (int i = 0; i < p->entries_count; i++) {
					paths[i] = p->entries[i].path;
				}
				device_state_play_list(paths, p->entries_count, index);
				free(paths);
				queued = true;
			}
		}

		if (queued) {
			// If this list is an album, say so: "play consecutive albums"
			// needs to know which record is ending to pick the next one.
			if (p->filter == LIBRARY_FILTER_ALBUM) {
				playlist_set_album(p->filter_value);
			}
			listacts_played();
		} else {
			player_play_file(path); // out of memory: at least play it
		}
		return;
	}

	// An artist, a genre or a year: its records, or its tracks when the album
	// view is off. An album: the tracks.
	bool grouped = p->kind == LIBRARY_LIST_ARTISTS || p->kind == LIBRARY_LIST_ALBUM_ARTISTS ||
				   p->kind == LIBRARY_LIST_GENRES || p->kind == LIBRARY_LIST_YEARS;
	bool artist_row = p->kind == LIBRARY_LIST_ARTISTS || p->kind == LIBRARY_LIST_ALBUM_ARTISTS;
	if (grouped && (medialist_album_view() || (artist_row && artist_records))) {
		medialist_open(name, LIBRARY_LIST_ALBUMS, filter_for(p->kind), name);
		return;
	}
	medialist_open(name, LIBRARY_LIST_TRACKS, filter_for(p->kind), name);
}

// The gestures (sx_row.h): which row, and what it stands for.
static row_t *row_live(void *user) {
	row_t *row = user;
	if (!row || !row->panel || row->index < 0 || row->index >= row->panel->count) {
		return NULL;
	}
	return row;
}

static void row_tap_cb(lv_obj_t *obj, void *user) {
	(void)obj;
	row_t *row = row_live(user);
	if (!row) {
		return;
	}
	panel_t *p = row->panel;
	if (p->reordering) {
		// The list is being arranged, not listened to. A tap that started a
		// track here would replace what is playing with the entry the user was
		// only trying to take hold of.
		return;
	}
	if (p->selecting) {
		selection_toggle(p, row, row->index);
		return;
	}
	row_activate(p, row->index);
}

static void row_hold_cb(lv_obj_t *obj, void *user) {
	(void)obj;
	row_t *row = row_live(user);
	if (!row || row->panel->reordering) {
		return;
	}
	panel_t *p = row->panel;
	if (p->selecting) {
		// Held in selection mode, as before: the row is chosen (never let go
		// of -- a hold is not a slip).
		const char *name = NULL, *path = NULL;
		char buf[16];
		if (row_at(p, row->index, &name, &path) &&
			sel_find(p, row_key(p, row->index, name, path, buf, sizeof(buf))) < 0) {
			selection_toggle(p, row, row->index);
		}
		return;
	}
	row_sheet(p, row);
}

// A track, or a record's tracks, into the queue.
static void row_queue(row_t *row, playlist_queue_where_t where) {
	panel_t *p = row->panel;
	const char *name = NULL, *path = NULL;
	if (!row_at(p, row->index, &name, &path)) {
		return;
	}
	char shown[256];
	library_album_title(name, shown, sizeof(shown));
	if (p->is_tracks) {
		if (!path) {
			return;
		}
		char copy[512];
		snprintf(copy, sizeof(copy), "%s", path);
		const char *one = copy;
		listacts_queue(&one, 1, where, shown);
	} else if (p->kind == LIBRARY_LIST_ALBUMS) {
		char value[256];
		snprintf(value, sizeof(value), "%s", name ? name : "");
		listacts_queue_index(listacts_album_tracks(value), where, shown);
	}
}

static void row_next_cb(lv_obj_t *obj, void *user) {
	(void)obj;
	row_t *row = row_live(user);
	if (row) {
		row_queue(row, PLAYLIST_QUEUE_NEXT);
	}
}

static void row_end_cb(lv_obj_t *obj, void *user) {
	(void)obj;
	row_t *row = row_live(user);
	if (row) {
		row_queue(row, PLAYLIST_QUEUE_END);
	}
}

static void row_star_cb(lv_obj_t *obj, void *user) {
	row_t *row = row_live(user);
	if (!row || !row->panel->is_tracks) {
		return;
	}
	panel_t *p = row->panel;
	const char *name = NULL, *path = NULL;
	if (!row_at(p, row->index, &name, &path) || !path) {
		return;
	}
	char copy_path[512], copy_name[256], copy_artist[128];
	snprintf(copy_path, sizeof(copy_path), "%s", path);
	snprintf(copy_name, sizeof(copy_name), "%s", name ? name : "");
	const char *artist = row_artist_at(p, row->index);
	snprintf(copy_artist, sizeof(copy_artist), "%s", artist ? artist : "");
	bool on = listacts_star(copy_path, copy_name, copy_artist);
	sx_row_set_starred(obj, on);
}

// Which lists take the swipes: tracks and records (the handoff's matrix);
// artists, genres and years only open.
static bool panel_swipes(const panel_t *p) { return p->is_tracks || p->kind == LIBRARY_LIST_ALBUMS; }

static void search_clicked_cb(lv_event_t *e) {
	(void)e;
	search_open();
}

// One of the small transparent glyph buttons that sit on the title row.
static lv_obj_t *corner_button(lv_obj_t *parent, const lv_image_dsc_t *glyph, lv_event_cb_t cb, panel_t *p) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, CORNER_BUTTON_SIZE, CORNER_BUTTON_SIZE);
	lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_pad_all(btn, 0, 0);
	lv_obj_set_hidden(btn, true);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, p);
	// Meeting its neighbours half way and reaching the margins above and below
	// the row (touch.h); the shelf is built tall enough to hold that.
	touch_pad(btn, CORNER_GAP / 2, CORNER_GAP / 2, CORNER_REACH, CORNER_REACH);

	lv_obj_t *image = lv_image_create(btn);
	lv_image_set_src(image, glyph);
	lv_obj_add_style(image, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
	lv_obj_center(image);
	return btn;
}

// ---------------------------------------------------------------------------
// Reordering a playlist by hand
//
// A playlist is the one list whose order is the user's rather than the
// database's: it comes back ORDER BY idx, and idx is what a drag rewrites. So
// the corner button is a mode, not an action -- lit in the accent while it is
// on, like the album grouping and the Favourites reverse.
//
// While it is on, the ellipsis at the right of each row becomes the grip, and
// that grip is the handle: dragging it moves the entry, dragging anywhere else
// still scrolls the list. Handles are why the grip glyph exists -- without one,
// a list that reorders on a drag is a list that cannot be scrolled.
//
// What moves is written down as it happens, one entry per drop, inside a
// transaction, so leaving the mode has nothing to commit and nothing is ever
// held half-reordered in memory.
// ---------------------------------------------------------------------------

// How close to the top or bottom edge a finger has to be before the list starts
// moving under it, and how far it moves each time the event comes round.
#define REORDER_EDGE_PX 70
#define REORDER_EDGE_STEP 18

// Only one drag can be in flight, so this is one set of variables rather than a
// set per panel.
static panel_t *drag_panel;
static int drag_from = -1; // the entry being carried, by position in the list
static int drag_to = -1;   // where it would land if the finger lifted now
static lv_obj_t *drop_line;

static void row_bind(panel_t *p, row_t *row, int index);
static void window_update(panel_t *p);

// The ellipsis or the grip, and whether a drag on it scrolls the list.
//
// The scroll chain is the reason this is done per row rather than once: a press
// that lands on a child walks up looking for something scrollable, and while
// the grip is a handle it must not find the list. Outside the mode the flag
// goes back, because there the ellipsis is an ordinary button and a swipe that
// happens to start on it should still scroll the page.
static void row_apply_reorder_look(panel_t *p, row_t *row) {
	if (!row->row || row->index < 0 || row->index >= p->count) {
		return;
	}
	const char *name = NULL, *path = NULL;
	if (row_at(p, row->index, &name, &path)) {
		row_update_trail(p, row, name, path);
	}
}

// The accent rule that says where the entry would land. Drawn on the body, in
// the list's own coordinates, so it scrolls with the rows.
static void drop_line_show(panel_t *p, int before_index) {
	if (!drop_line) {
		drop_line = lv_obj_create(p->body);
		lv_obj_set_ignore_layout(drop_line, true);
		lv_obj_set_scrollable(drop_line, false);
		lv_obj_set_clickable(drop_line, false);
		lv_obj_set_style_border_width(drop_line, 0, 0);
		lv_obj_set_style_shadow_width(drop_line, 0, 0);
		lv_obj_set_style_pad_all(drop_line, 0, 0);
		lv_obj_add_style(drop_line, &theme_style_accent_bg, 0);
		lv_obj_set_style_radius(drop_line, 2, 0);
	} else if (lv_obj_get_parent(drop_line) != p->body) {
		lv_obj_set_parent(drop_line, p->body);
	}
	lv_obj_set_size(drop_line, lv_obj_get_width(p->body) - 2 * SX_ROW_PAD_L, 4);
	lv_obj_set_pos(drop_line, SX_ROW_PAD_L, row_y(p, before_index) - SX_ROW_GAP / 2 - 2);
	lv_obj_set_hidden(drop_line, false);
	lv_obj_move_foreground(drop_line);
}

static void drop_line_hide(void) {
	if (drop_line) {
		lv_obj_set_hidden(drop_line, true);
	}
}

// The entry the row being carried would take, from where the finger is.
static int index_under(const panel_t *p, lv_coord_t y) {
	lv_area_t body;
	lv_obj_get_coords(p->body, &body);
	return row_at_y(p, (int)(y - body.y1));
}

// Marks the row being carried, so the user can see which one they have. Runs
// over the pool because the row for an index moves as the list scrolls.
static void drag_mark_rows(panel_t *p) {
	for (int i = 0; i < ROW_POOL; i++) {
		bool carried = drag_panel == p && p->rows[i].index >= 0 && p->rows[i].index == drag_from;
		lv_obj_set_style_border_width(p->rows[i].row, carried ? 2 : 0, 0);
		if (carried) {
			lv_obj_set_style_border_color(p->rows[i].row, sx_pal()->accent, 0);
			lv_obj_set_style_border_opa(p->rows[i].row, LV_OPA_COVER, 0);
		}
	}
}

static void drag_end(void) {
	panel_t *p = drag_panel;
	drop_line_hide();
	drag_panel = NULL;
	int from = drag_from;
	int to = drag_to;
	drag_from = -1;
	drag_to = -1;
	if (p) {
		drag_mark_rows(p);
	}

	if (!p || from < 0 || to < 0 || from == to || !p->playlist[0]) {
		return;
	}

	if (!library_playlist_move(p->playlist, from, to)) {
		toast_error("medialist_reorder_failed");
		return;
	}
	p->reorder_moved = true;

	// The move bumped the playlist generation, so the handle this page holds is
	// stale by definition. Forgetting the pool's indices as well is what makes
	// every row rebind: the ids under them have moved, and a row whose index
	// has not changed would otherwise keep the name it had.
	for (int i = 0; i < ROW_POOL; i++) {
		p->rows[i].index = -1;
	}
	window_update(p);
	drag_mark_rows(p);
}

static void grip_cb(lv_event_t *e) {
	panel_t *p = lv_event_get_user_data(e);
	lv_event_code_t code = lv_event_get_code(e);

	if (!p->reordering) {
		return; // out of the mode this is the star, handled by the row
	}
	lv_event_stop_bubbling(e);

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		drag_end();
		return;
	}

	lv_indev_t *indev = lv_indev_active();
	if (!indev) {
		return;
	}
	lv_point_t point;
	lv_indev_get_point(indev, &point);

	if (code == LV_EVENT_PRESSED) {
		lv_obj_t *button = lv_event_get_current_target(e);
		drag_panel = p;
		drag_from = -1;
		for (int i = 0; i < ROW_POOL; i++) {
			if (sx_row_trail(p->rows[i].row) == button) {
				drag_from = p->rows[i].index;
				break;
			}
		}
		if (drag_from < 0 || drag_from >= p->count) {
			drag_panel = NULL;
			drag_from = -1;
			return;
		}
		drag_to = drag_from;
		drag_mark_rows(p);
		return;
	}
	if (code != LV_EVENT_PRESSING || drag_panel != p || drag_from < 0) {
		return;
	}

	// The list follows the finger when it reaches either end, so an entry can
	// be carried past the screenful it started on.
	lv_area_t view;
	lv_obj_get_coords(p->list, &view);
	int step = 0;
	if (point.y < view.y1 + REORDER_EDGE_PX) {
		step = -REORDER_EDGE_STEP;
	} else if (point.y > view.y2 - REORDER_EDGE_PX) {
		step = REORDER_EDGE_STEP;
	}
	if (step != 0) {
		int scroll = lv_obj_get_scroll_y(p->list);
		int limit = body_height(p) - lv_obj_get_height(p->list);
		if (limit < 0) {
			limit = 0;
		}
		int wanted = scroll + step;
		wanted = wanted < 0 ? 0 : (wanted > limit ? limit : wanted);
		if (wanted != scroll) {
			lv_obj_scroll_to_y(p->list, wanted, LV_ANIM_OFF);
			window_update(p);
			drag_mark_rows(p);
		}
	}

	drag_to = index_under(p, point.y);
	drop_line_show(p, drag_to > drag_from ? drag_to + 1 : drag_to);
}

// The corner button: on turns the mode on, on again turns it off. There is
// nothing to save at the second press -- every drop was written when it
// happened -- so this only says what changed.
static void reorder_clicked_cb(lv_event_t *e) {
	panel_t *p = lv_event_get_user_data(e);

	drag_end();
	p->reordering = !p->reordering;

	if (p->reordering) {
		p->reorder_moved = false;
	} else if (p->reorder_moved) {
		p->reorder_moved = false;
		toast_success("medialist_playlist_order_saved");
	}

	reorder_icon_paint(p);
	for (int i = 0; i < ROW_POOL; i++) {
		row_apply_reorder_look(p, &p->rows[i]);
	}
}

// Leaves the mode without asking, for the routes that are not the button:
// walking off the page, the list being reloaded under it, a card going away.
static void reorder_stop(panel_t *p) {
	drag_end();
	if (!p->reordering) {
		return;
	}
	p->reordering = false;
	p->reorder_moved = false;
	reorder_icon_paint(p);
	for (int i = 0; i < ROW_POOL; i++) {
		row_apply_reorder_look(p, &p->rows[i]);
	}
}

static void screen_unloaded_cb(lv_event_t *e) {
	reorder_stop(lv_event_get_user_data(e));
	selection_stop(lv_event_get_user_data(e));
}

// Set while a list is reopened under a new ordering: it comes back at the top,
// not on what is playing. A new order is something to read from the start of;
// the next time the list is entered, it opens on the current track again.
static bool reopening_resorted;

// Reopens whatever this panel is showing, under whatever the ordering now is.
// The signature is cleared first so the list comes back at the top: after a
// re-sort the old scroll position points at nothing in particular.
static void reload_current(panel_t *p) {
	if (p->from_paths) {
		return;
	}
	char title[sizeof(p->title)];
	char value[sizeof(p->filter_value)];
	snprintf(title, sizeof(title), "%s", p->title);
	snprintf(value, sizeof(value), "%s", p->filter_value);

	p->signature[0] = '\0';
	reopening_resorted = true;
	medialist_open(title, p->kind, p->filter, value[0] ? value : NULL);
	reopening_resorted = false;
}

// The ways a datable list can run, as the menu offers them -- the last two only
// on the lists that have a year. Each is a set of bits, and the menu's tick is
// whichever set is on now.
typedef enum {
	SORT_NAME_AZ,
	SORT_NAME_ZA,
	SORT_ADDED_NEW,
	SORT_ADDED_OLD,
	SORT_YEAR_UP,
	SORT_YEAR_DOWN,
	SORT_ARTIST, // all the records only
	SORT_CHOICES,
} sort_choice_t;

static panel_t *sort_menu_panel;

static void sort_picked(void *user) {
	panel_t *p = sort_menu_panel;
	if (!p) {
		return;
	}
	sort_choice_t choice = (sort_choice_t)(intptr_t)user;
	sort_set_added(p->kind, choice == SORT_ADDED_NEW || choice == SORT_ADDED_OLD);
	sort_set_year(p->kind, choice == SORT_YEAR_UP || choice == SORT_YEAR_DOWN);
	sort_set_artist(p->kind, choice == SORT_ARTIST);
	sort_set_desc(p->kind, choice == SORT_NAME_ZA || choice == SORT_ADDED_NEW || choice == SORT_YEAR_DOWN);
	reload_current(p);
}

// The sort sheet (⇅): every way this list can run, the one in force ticked.
// A name list has two (A-Z and Z-A, and by date where it has one); all the
// tracks and all the records have the years as well, and all the records the
// artist.
static void sort_clicked_cb(lv_event_t *e) {
	panel_t *p = lv_event_get_user_data(e);
	if (!p) {
		return;
	}
	bool desc = sort_is_desc(p->kind);
	sort_choice_t current = desc ? SORT_NAME_ZA : SORT_NAME_AZ;
	if (sort_by_artist(p)) {
		current = SORT_ARTIST;
	} else if (sort_by_year(p)) {
		current = desc ? SORT_YEAR_DOWN : SORT_YEAR_UP;
	} else if (sort_by_date(p)) {
		current = desc ? SORT_ADDED_NEW : SORT_ADDED_OLD;
	}

	static const char *const labels[SORT_CHOICES] = {
		[SORT_NAME_AZ] = "medialist_sort_name_az",	   [SORT_NAME_ZA] = "medialist_sort_name_za",
		[SORT_ADDED_NEW] = "medialist_sort_added_new", [SORT_ADDED_OLD] = "medialist_sort_added_old",
		[SORT_YEAR_UP] = "medialist_sort_year_up",	   [SORT_YEAR_DOWN] = "medialist_sort_year_down",
		[SORT_ARTIST] = "medialist_sort_artist",
	};
	sx_sheet_item_t items[SORT_CHOICES];
	int n = 0;
	for (int i = 0; i < SORT_CHOICES; i++) {
		if ((i == SORT_ADDED_NEW || i == SORT_ADDED_OLD) && !sort_can_date(p->kind, p->filter)) {
			continue;
		}
		if ((i == SORT_YEAR_UP || i == SORT_YEAR_DOWN) && !sort_can_year(p->kind, p->filter)) {
			continue;
		}
		if (i == SORT_ARTIST && !sort_can_artist(p->kind, p->filter)) {
			continue;
		}
		items[n++] = (sx_sheet_item_t){tr(labels[i]), NULL, i == (int)current, false, sort_picked, (void *)(intptr_t)i};
	}
	sort_menu_panel = p;
	sx_sheet_menu(tr("lists_sort"), p->title, items, n);
}

// Favourites reversed: the most recently starred track on top. The database
// returns the list in its own order, oldest first; the button flips it, and
// while on it carries the accent colour, because it is a state, not an action.
static bool fav_reversed;

static void reverse_clicked_cb(lv_event_t *e) {
	panel_t *p = lv_event_get_user_data(e);
	if (!p) {
		return;
	}
	fav_reversed = !fav_reversed;
	config_set_int("library", "fav_reversed", fav_reversed ? 1 : 0);
	config_save();
	reload_current(p);
}

// Set while the disc button swaps an artist's tracks for their records or back:
// the new page takes the old one's place instead of going on top of it, so the
// way back leads to wherever the artist was opened from.
static bool open_replacing;

static void album_order_clicked_cb(lv_event_t *e) {
	panel_t *p = lv_event_get_user_data(e);
	if (!p || p->from_paths) {
		return;
	}
	bool to_records = p->kind == LIBRARY_LIST_TRACKS;
	artist_records = to_records;
	config_set_int("library", "artist_records", artist_records ? 1 : 0);
	config_save();

	char title[sizeof(p->title)];
	char value[sizeof(p->filter_value)];
	snprintf(title, sizeof(title), "%s", p->title);
	snprintf(value, sizeof(value), "%s", p->filter_value);
	open_replacing = true;
	medialist_open(title, to_records ? LIBRARY_LIST_ALBUMS : LIBRARY_LIST_TRACKS, p->filter, value);
	open_replacing = false;
}

void medialist_open_artist(const char *name) {
	if (!name || !name[0]) {
		return;
	}
	medialist_open(name, artist_records ? LIBRARY_LIST_ALBUMS : LIBRARY_LIST_TRACKS, LIBRARY_FILTER_ARTIST, name);
}

// What "Show artist" opens an artist as: what a tap on them in the Artists list
// opens -- their records with the album view on or the disc button left lit,
// their tracks otherwise.
static library_list_t track_artist_kind(void) {
	return medialist_album_view() || artist_records ? LIBRARY_LIST_ALBUMS : LIBRARY_LIST_TRACKS;
}

bool medialist_open_track_artist(const char *path) {
	char name[256];
	bool by_album_artist = false;
	if (!library_track_artist_page(path, name, sizeof(name), &by_album_artist)) {
		return false;
	}
	medialist_open(name, track_artist_kind(), by_album_artist ? LIBRARY_FILTER_ALBUM_ARTIST : LIBRARY_FILTER_ARTIST,
				   name);
	return true;
}

// Copies the path of the one row a window was asked for.
static bool take_one_path_cb(const char *name, const char *path, const char *artist, void *user) {
	(void)name;
	(void)artist;
	char *out = user;
	snprintf(out, 512, "%s", path ? path : "");
	return false; // one row is all that was wanted
}

// ---------------------------------------------------------------------------
// Starting a whole artist
//
// The corner button on an artist's records opens a small card in the middle of
// the screen -- the shape the Gearboy pause menu uses -- rather than doing one
// thing. There are three sensible ways to start an artist and no reason to
// choose one of them for the user.
//
// All three build the same queue: that artist's tracks, gathered by record and
// each record in its running order. What differs is where playback starts and
// in what order it goes on.
//
// The album chaining option keeps working through this. What it needs is a
// record to carry on from when the queue runs out, so the queue is stamped with
// the artist's last one: play the artist through and the next record in the
// library follows, which is what the option promises.
// ---------------------------------------------------------------------------

static panel_t *play_menu_panel;

static void play_menu_hide(void) { play_menu_panel = NULL; }

// The tracks the menu plays, as a handle. What that is depends on what the list
// underneath is showing:
//
//   a list of tracks  -- those tracks, in the order they are on screen. Whether
//                        that is all of them, one artist's, one artist's
//                        gathered by record, the favourites or a playlist, the
//                        list is the answer;
//   a list of records -- the tracks of those records, gathered by record and
//                        each record in its running order, so "in sequence"
//                        starts at the top of the list and works down.
//
// NULL when there is nothing to play, which includes a list handed in as an
// array of paths: there is no query behind one, and menu_paths_collect() below
// is what that case uses instead.
//
// The caller owns it: device_state_play_index() takes it, and the callers that
// only read it close it themselves.
static library_index_t *menu_tracks_open(panel_t *p) {
	if (!p || p->from_paths) {
		return NULL;
	}

	// The handle the queue is about to be built from has to be the current one.
	// Row ids are only a promise, and a stale handle hands out a clone that is
	// stale too: library_index_window() refuses it and the queue cannot read the
	// path of the track it was told to start on. The rows are bound again with
	// it.
	window_update(p);

	library_index_t *ix = NULL;
	if (p->kind == LIBRARY_LIST_TRACKS || p->kind == LIBRARY_LIST_FAVOURITES || p->kind == LIBRARY_LIST_PLAYLIST ||
		p->kind == LIBRARY_LIST_MOST_PLAYED || p->kind == LIBRARY_LIST_RECENTLY_PLAYED) {
		ix = library_index_clone(p->ix);
	} else if (p->kind == LIBRARY_LIST_ALBUMS) {
		ix = library_index_open(LIBRARY_LIST_TRACKS, p->filter, p->filter_value[0] ? p->filter_value : NULL,
								LIBRARY_ORDER_ALBUM, false);
	}

	if (ix && library_index_count(ix) > 0) {
		return ix;
	}
	library_index_close(ix);
	return NULL;
}

// The record the queue ends on, for album chaining to carry on from: the last
// row of the album list the user is looking at, which is the same list the
// queue was built from.
//
// Cleared when the list is not one of records. A queue of tracks has no record
// to be the end of, and a leftover name from an earlier list would send the
// chaining off after the wrong one.
static void stamp_last_album(const panel_t *p) {
	// One album's tracks end on that album, as they do when a row of it is
	// tapped (row_clicked_cb).
	if (p->kind == LIBRARY_LIST_TRACKS && p->filter == LIBRARY_FILTER_ALBUM && !p->from_paths) {
		playlist_set_album(p->filter_value);
		return;
	}
	const char *name = NULL;
	if (p->kind == LIBRARY_LIST_ALBUMS && p->count > 0 && row_at((panel_t *)p, p->count - 1, &name, NULL) && name &&
		name[0]) {
		playlist_set_album(name);
		return;
	}
	playlist_set_album("");
}

// The playable paths of a list handed in as an array, for the lists that have
// no query behind them. The caller frees the array; the strings belong to the
// panel and outlive the call.
static int menu_paths_collect(const panel_t *p, const char ***out) {
	*out = NULL;
	if (!p || !p->from_paths || p->entries_count <= 0) {
		return 0;
	}
	const char **paths = malloc((size_t)p->entries_count * sizeof(*paths));
	if (!paths) {
		return 0;
	}
	int usable = 0;
	for (int i = 0; i < p->entries_count; i++) {
		if (p->entries[i].path) {
			paths[usable++] = p->entries[i].path;
		}
	}
	if (usable == 0) {
		free(paths);
		return 0;
	}
	*out = paths;
	return usable;
}

static void play_artist(panel_t *p, bool shuffled) {
	if (!p) {
		return;
	}

	// Mode first: with shuffle already on, the queue deals its random order as
	// it is loaded, so it comes out shuffled in one step. Which of the two
	// shuffles is the "Continuous shuffle" option -- the plain one stops at the
	// end of the deal, the other comes round again in the same order.
	playback_mode_t shuffle_mode =
		musicsettings_endless_shuffle() ? PLAYBACK_MODE_SHUFFLE_REPEAT : PLAYBACK_MODE_SHUFFLE;
	playlist_set_mode(shuffled ? shuffle_mode : PLAYBACK_MODE_NORMAL);

	if (p->from_paths) {
		const char **paths = NULL;
		int count = menu_paths_collect(p, &paths);
		if (count == 0) {
			return;
		}
		int start = shuffled && count > 1 ? (int)lv_rand(0, (uint32_t)(count - 1)) : 0;
		device_state_play_list(paths, count, start);
		free(paths);
	} else {
		library_index_t *ix = menu_tracks_open(p);
		if (!ix) {
			return;
		}
		int count = library_index_count(ix);
		int start = shuffled && count > 1 ? (int)lv_rand(0, (uint32_t)(count - 1)) : 0;
		if (!device_state_play_index(ix, start)) {
			return; // the handle was taken either way
		}
	}
	stamp_last_album(p);
	listacts_played();
	if (shuffled) {
		char text[256];
		snprintf(text, sizeof(text), tr("lists_shuffled"), p->title);
		sx_toast(text);
	}
}

static void play_menu_shuffle_cb(void *user) {
	(void)user;
	panel_t *p = play_menu_panel;
	play_menu_hide();
	play_artist(p, true);
}

static void play_menu_sequence_cb(void *user) {
	(void)user;
	panel_t *p = play_menu_panel;
	play_menu_hide();
	play_artist(p, false);
}

// One of the list's tracks, chosen at random, put after what is playing.
// Reads the one row it needs out of the handle rather than the whole list.
static void queue_random_track(panel_t *p) {
	if (!p) {
		return;
	}

	char path[512] = "";
	if (p->from_paths) {
		const char **paths = NULL;
		int count = menu_paths_collect(p, &paths);
		if (count == 0) {
			return;
		}
		int pick = count > 1 ? (int)lv_rand(0, (uint32_t)(count - 1)) : 0;
		snprintf(path, sizeof(path), "%s", paths[pick]);
		free(paths);
	} else {
		library_index_t *ix = menu_tracks_open(p);
		if (!ix) {
			return;
		}
		int count = library_index_count(ix);
		int pick = count > 1 ? (int)lv_rand(0, (uint32_t)(count - 1)) : 0;
		library_index_window(ix, pick, 1, take_one_path_cb, path);
		library_index_close(ix);
	}

	if (path[0]) {
		// Said by name: "added to the queue" alone, after a button that
		// stands beside Play under an album's cover, reads as the whole album
		// added. With the undo, as every change to the queue.
		char title[256] = "";
		const char *one = path;
		if (!library_track_title(path, title, sizeof(title))) {
			const char *slash = strrchr(path, '/');
			snprintf(title, sizeof(title), "%s", slash ? slash + 1 : path);
		}
		listacts_queue(&one, 1, PLAYLIST_QUEUE_NEXT, title);
	}
}

static void play_menu_random_track_cb(void *user) {
	(void)user;
	panel_t *p = play_menu_panel;
	play_menu_hide();
	queue_random_track(p);
}

// The whole list into the queue rather than in place of it: an album, an
// artist's records, a playlist, the favourites -- whatever the page is. In the
// order "Play in sequence" would play it, read in windows like the selection
// is, and with the selection's ceiling: "All tracks" queued whole is a library
// copied into memory.
static void play_menu_queue(playlist_queue_where_t where) {
	panel_t *p = play_menu_panel;
	play_menu_hide();
	if (!p) {
		return;
	}
	if (p->from_paths) {
		const char **paths = NULL;
		int count = menu_paths_collect(p, &paths);
		if (count > 0) {
			listacts_queue(paths, count, where, p->title);
		}
		free(paths);
		return;
	}
	library_index_t *ix = menu_tracks_open(p);
	if (!ix) {
		return;
	}
	path_list_t list = {NULL, 0, 0};
	int total = library_index_count(ix);
	bool more = true;
	for (int first = 0; first < total && more; first += 256) {
		int want = total - first < 256 ? total - first : 256;
		int got = library_index_window(ix, first, want, collect_path_cb, &list);
		more = got == want && list.count < SELECTION_MAX_TRACKS;
	}
	library_index_close(ix);
	listacts_queue((const char *const *)list.paths, list.count, where, p->title);
	path_list_free(&list);
}

static void play_menu_queue_cb(void *user) { play_menu_queue((playlist_queue_where_t)(intptr_t)user); }

// One of the records on screen at random, its tracks in order, and the records
// after it at random too (device_state_play_random_album). In order, whatever
// the mode was: a mode changed while it plays then carries on to the records
// after it, as any mode does.
static void play_menu_random_album_cb(void *user) {
	(void)user;
	panel_t *p = play_menu_panel;
	play_menu_hide();
	if (!p || p->kind != LIBRARY_LIST_ALBUMS) {
		return;
	}
	playlist_set_mode(PLAYBACK_MODE_NORMAL);
	if (!device_state_play_random_album(p->filter, p->filter_value[0] ? p->filter_value : NULL)) {
		return;
	}
	listacts_played();
}

// Every way of starting the whole list, as a sheet: what the circle-play
// card offered before 2.0, now the ⤮ button held (a tap on it shuffles at
// once, the handoff's header action) and the album page's ⋮.
static int play_menu_items(panel_t *p, sx_sheet_item_t *items) {
	int n = 0;
	items[n++] = (sx_sheet_item_t){tr("medialist_play_in_random_order"), &icon_shuffle, false, false,
								   play_menu_shuffle_cb, NULL};
	items[n++] = (sx_sheet_item_t){tr("medialist_play_in_sequence"), &icon_play, false, false, play_menu_sequence_cb,
								   NULL};
	// Records in random order only where there are records to choose between,
	// and only with the option that plays one record after another: without it
	// the choice would be one record at random and then silence.
	if (p->kind == LIBRARY_LIST_ALBUMS && !p->from_paths && p->count >= 2 && device_state_album_chaining()) {
		items[n++] = (sx_sheet_item_t){tr("medialist_play_albums_in_random_order"), &icon_album, false, false,
									   play_menu_random_album_cb, NULL};
	}
	items[n++] = (sx_sheet_item_t){tr("medialist_queue_random_track"), &icon_music2, false, false,
								   play_menu_random_track_cb, NULL};
	items[n++] = (sx_sheet_item_t){tr("queue_play_next"), &icon_list_start, false, false, play_menu_queue_cb,
								   (void *)(intptr_t)PLAYLIST_QUEUE_NEXT};
	items[n++] = (sx_sheet_item_t){tr("queue_add_to_end"), &icon_list_plus_small, false, false, play_menu_queue_cb,
								   (void *)(intptr_t)PLAYLIST_QUEUE_END};
	return n;
}

static void play_menu_open(panel_t *p) {
	if (!p || p->count <= 0) {
		return;
	}
	sx_sheet_item_t items[8];
	int n = play_menu_items(p, items);
	play_menu_panel = p;
	sx_sheet_menu(p->title, NULL, items, n);
}

// ⤮ tapped: the list at random, now. Held: the sheet above.
static void play_menu_clicked_cb(lv_event_t *e) {
	panel_t *p = lv_event_get_user_data(e);
	if (!p || p->count <= 0 || switcher_back_drag_active()) {
		return;
	}
	if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
		lv_indev_t *indev = lv_indev_active();
		if (indev) {
			lv_indev_wait_release(indev); // the lift is not also a tap
		}
		play_menu_open(p);
		return;
	}
	play_artist(p, true);
}

// A row's sheet (held 450 ms): the track's or the record's full menu
// (listacts.c), with the page's own items after it -- take a track out of the
// playlist on screen, and "Select" for selection mode. An artist's sheet is
// the play menu of their records.
static char menu_path[512];
static char menu_name[256];
// The row the sheet was opened on, so an action that removes the thing the row
// stands for can take the row away with it.
static panel_t *menu_panel;
static int menu_index = -1;

// The row goes now, not at the next visit. Only when the list is one whose
// membership the action just changed: taking a track out of a playlist has to
// empty its row there and has to leave every other list alone.
static void menu_drop_row_if_listed(void) {
	// The index was taken when the sheet opened, and a list can reload
	// underneath one -- a scan finishing, a sort flipped. Checking that the row
	// still stands for the same track before dropping it costs one compare and
	// stops the wrong row disappearing; the file itself is always operated on by
	// path, so it is only ever the view that could go out of step.
	if (menu_panel) {
		panel_refresh_stale(menu_panel);
	}
	if (menu_panel && menu_index >= 0 && menu_index < menu_panel->count) {
		char path[512];
		if (row_path_copy(menu_panel, menu_index, path, sizeof(path)) && strcmp(path, menu_path) == 0) {
			model_remove(menu_panel, menu_index);
		}
	}
	if (menu_panel) {
		window_update(menu_panel);
	}
	menu_panel = NULL;
	menu_index = -1;
}

static void menu_playlist_remove_action(void *user) {
	(void)user;
	if (!menu_panel || !menu_panel->playlist[0] || !menu_path[0]) {
		return;
	}
	if (playlists_remove_track(menu_panel->playlist, menu_path)) {
		toast_success("medialist_track_removed");
		menu_drop_row_if_listed();
	} else {
		gui_notify_popup("medialist_remove_failed");
	}
}

// "Select": selection mode, with this row chosen.
static void menu_select_action(void *user) {
	(void)user;
	panel_t *p = menu_panel;
	if (!p || menu_index < 0 || menu_index >= p->count || !panel_selectable(p) || p->reordering) {
		return;
	}
	row_t *row = NULL;
	for (int i = 0; i < ROW_POOL; i++) {
		if (p->rows[i].index == menu_index) {
			row = &p->rows[i];
		}
	}
	if (!p->selecting) {
		selection_start(p);
	}
	selection_toggle(p, row, menu_index);
}

static void row_sheet(panel_t *p, row_t *row) {
	if (!p || !row || row->index < 0 || row->index >= p->count) {
		return;
	}
	const char *row_name = NULL;
	const char *row_path = NULL;
	if (!row_at(p, row->index, &row_name, &row_path)) {
		return;
	}
	snprintf(menu_path, sizeof(menu_path), "%s", row_path ? row_path : "");
	snprintf(menu_name, sizeof(menu_name), "%s", row_name ? row_name : "");
	menu_panel = p;
	menu_index = row->index;

	sx_sheet_item_t extra[3];
	int n = 0;
	// Inside a playlist the useful action is the opposite one: this list is
	// where the track already is.
	if (p->is_tracks && p->playlist[0]) {
		extra[n++] = (sx_sheet_item_t){tr("medialist_remove_from_playlist"), &icon_list_x, false, true,
									   menu_playlist_remove_action, NULL};
	}
	if (panel_selectable(p)) {
		extra[n++] = (sx_sheet_item_t){tr("lists_select"), &icon_check, false, false, menu_select_action, NULL};
	}

	row_kind_t kind = panel_row_kind(p);
	if (kind == RK_TRACK) {
		if (!menu_path[0]) {
			return;
		}
		const char *artist = row_artist_at(p, row->index);
		char artist_copy[128];
		snprintf(artist_copy, sizeof(artist_copy), "%s", artist ? artist : "");
		bool artist_page = p->filter == LIBRARY_FILTER_ARTIST || p->filter == LIBRARY_FILTER_ALBUM_ARTIST;
		listacts_track_sheet(menu_path, menu_name, artist_copy, NULL,
							 p->filter == LIBRARY_FILTER_ALBUM ? p->filter_value : NULL,
							 artist_page ? p->filter_value : NULL, extra, n);
		return;
	}
	if (kind == RK_ALBUM) {
		const char *artist = row_artist_at(p, row->index);
		char artist_copy[128];
		snprintf(artist_copy, sizeof(artist_copy), "%s", artist ? artist : "");
		listacts_album_sheet(menu_name, artist_copy, NULL, extra, n);
		return;
	}
	if (kind == RK_PERSON && panel_selectable(p)) {
		// An artist's sheet: "Select" for picking several, which before 2.0
		// was the long press itself.
		sx_sheet_menu(menu_name, NULL, extra, n);
	}
}

// ---------------------------------------------------------------------------
// construction
// ---------------------------------------------------------------------------

static void album_page_enter(panel_t *p);
static void album_page_leave(panel_t *p);

// Coming back to a list through the back chevron. The library can have been
// rescanned, or the card taken out and put back, while the page sat on the
// stack: without this the rows keep the labels they were bound with and every
// tap is dead until the user scrolls. The album page takes its hue back.
static void screen_loaded_cb(lv_event_t *e) {
	panel_t *p = lv_event_get_user_data(e);
	if (p) {
		album_page_enter(p);
		window_update(p);
	}
}

// Leaving: the album page gives the interface back the playing track's hue as
// it starts to slide away, so the page underneath comes in in its own colours.
static void screen_unload_start_cb(lv_event_t *e) {
	panel_t *p = lv_event_get_user_data(e);
	if (p) {
		album_page_leave(p);
	}
}

static void build_panel(panel_t *p, gui_config_t *cfg, bool is_tracks) {
	p->cfg = cfg;
	p->row_h = sx_row_height(false);
	p->is_tracks = is_tracks;
	p->window_first = -1;

	lv_obj_add_style(p->screen, &theme_style_screen, 0);
	lv_obj_add_event_cb(p->screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, p);
	lv_obj_add_event_cb(p->screen, screen_unload_start_cb, LV_EVENT_SCREEN_UNLOAD_START, p);
	p->title_label = settingsrow_title(p->screen, cfg, "");

	int content_top = settingsrow_content_top(cfg);
	int width = cfg->screen_width - 2 * cfg->padding;

	// Full-width viewport with the rows inset by the padding: that puts the
	// scrollbar out in the margin, beside the rows, instead of drawing it on
	// top of them.
	p->list = lv_obj_create(p->screen);
	lv_obj_set_size(p->list, cfg->screen_width, cfg->screen_height - content_top);
	lv_obj_align(p->list, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(p->list, 0, 0);
	lv_obj_set_style_border_width(p->list, 0, 0);
	lv_obj_set_style_radius(p->list, 0, 0);
	lv_obj_set_style_pad_hor(p->list, cfg->padding, 0);
	lv_obj_set_style_pad_ver(p->list, 0, 0);
	lv_obj_set_scroll_dir(p->list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(p->list, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_add_event_cb(p->list, scroll_cb, LV_EVENT_SCROLL, p);
	// The last row clear of the mini player, whatever the slot does.
	sx_frame_reserve_bottom(p->list);

	// Behind the header row on the album page, while rows are being chosen:
	// the selection's buttons stand there over the rows scrolling by.
	p->head_bar = NULL;
	if (is_tracks) {
		p->head_bar = lv_obj_create(p->screen);
		lv_obj_remove_style_all(p->head_bar);
		lv_obj_set_style_bg_opa(p->head_bar, LV_OPA_COVER, 0);
		lv_obj_set_size(p->head_bar, cfg->screen_width, content_top - cfg->top_bar_height);
		lv_obj_align(p->head_bar, LV_ALIGN_TOP_LEFT, 0, cfg->top_bar_height);
		lv_obj_set_scrollable(p->head_bar, false);
		// It takes the presses on it, so a row passing under the heading is not
		// tapped through it; the page's own gestures still start there.
		lv_obj_set_clickable(p->head_bar, true);
		player_sheet_attach_drag(p->head_bar, true);
		switcher_attach_back_gesture(p->head_bar);
		lv_obj_set_hidden(p->head_bar, true);
	}

	p->body = lv_obj_create(p->list);
	lv_obj_set_width(p->body, width);
	lv_obj_set_height(p->body, row_pitch(p));
	lv_obj_set_pos(p->body, 0, 0);
	lv_obj_set_style_bg_opa(p->body, 0, 0);
	lv_obj_set_style_border_width(p->body, 0, 0);
	lv_obj_set_style_pad_all(p->body, 0, 0);
	lv_obj_set_scrollable(p->body, false);
	// Row presses bubble row -> body -> list; without this flag they stop here
	// and the player sheet's pull never sees a press that began on a row.
	lv_obj_set_event_bubble(p->body, true);

	// The corner strip, on the title row's right: whichever of the buttons
	// this particular list has a use for. A row rather than aligned objects,
	// so two of them showing at once lay themselves out.
	p->corner = lv_obj_create(p->screen);
	lv_obj_remove_style_all(p->corner);

	// A fixed width, not LV_SIZE_CONTENT.
	//
	// Hiding a flex child takes it out of the layout, so a content-sized box
	// is one button wide and the second button of a pair lands outside its own
	// parent, at x = -60, and is never drawn. Sizing for the most buttons that
	// can coexist costs a strip of empty space on the pages that show fewer.
	//
	// Taller than its buttons by CORNER_REACH above and below, with the buttons
	// centred in it where they always were: a press is only looked for among a
	// shelf's buttons when it lands on the shelf, so their larger areas
	// (corner_button()) need the room. The shelf takes no presses itself, so
	// the margin it adds is not a dead strip.
	lv_obj_set_size(p->corner, CORNER_MAX_BUTTONS * CORNER_BUTTON_SIZE + (CORNER_MAX_BUTTONS - 1) * CORNER_GAP,
					CORNER_BUTTON_SIZE + 2 * CORNER_REACH);
	lv_obj_set_clickable(p->corner, false); // it is a shelf, not a control
	lv_obj_align(p->corner, LV_ALIGN_TOP_RIGHT, -cfg->padding, cfg->padding + cfg->top_bar_height - CORNER_REACH);
	lv_obj_set_scrollable(p->corner, false);
	lv_obj_set_flex_flow(p->corner, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(p->corner, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(p->corner, CORNER_GAP, 0);

	// Tracks of one artist, gathered by record instead of strung out
	// alphabetically. Only ever shown on an artist's own track list.
	p->album_btn = NULL;
	if (is_tracks) {
		p->album_btn = corner_button(p->corner, &icon_album_corner, album_order_clicked_cb, p);
	}

	// Hand-ordering, on the playlists only, left of shuffle.
	p->reorder_btn = NULL;
	if (is_tracks) {
		p->reorder_btn = corner_button(p->corner, &icon_reorder, reorder_clicked_cb, p);
	}

	// ⌕ on the artists' lists (the handoff's header there).
	p->search_btn = NULL;
	if (!is_tracks) {
		p->search_btn = corner_button(p->corner, &icon_search, search_clicked_cb, p);
	}

	// ⤮: the list at random at a tap, every way of starting it when held.
	// Built on every panel: which lists actually wear it is medialist_open()'s
	// to decide.
	p->play_btn = corner_button(p->corner, &icon_shuffle, play_menu_clicked_cb, p);
	lv_obj_add_event_cb(p->play_btn, play_menu_clicked_cb, LV_EVENT_LONG_PRESSED, p);

	// The Favourites reverse button: the newest starred on top, lit while on.
	p->reverse_btn = NULL;
	if (is_tracks) {
		p->reverse_btn = corner_button(p->corner, &icon_sort_date_new, reverse_clicked_cb, p);
	}

	// ⇅: the sort sheet.
	p->sort_btn = corner_button(p->corner, &icon_arrow_down_up, sort_clicked_cb, p);

	// Selection mode's buttons, last so that they sit at the right edge: add
	// the chosen rows to the queue, to the favourites, to a playlist, take them
	// out of the favourites or of this playlist, and leave the mode.
	// sel_buttons_show() picks four of them.
	p->sel_queue_btn = corner_button(p->corner, &icon_list_queue, sel_queue_cb, p);
	p->sel_fav_btn = corner_button(p->corner, &icon_star_plus, sel_fav_cb, p);
	p->sel_add_btn = corner_button(p->corner, &icon_list_plus, sel_add_cb, p);
	p->sel_unfav_btn = corner_button(p->corner, &icon_star_x, sel_unfav_cb, p);
	p->sel_unlist_btn = corner_button(p->corner, &icon_list_x, sel_unlist_cb, p);
	p->sel_close_btn = corner_button(p->corner, &icon_close, sel_close_cb, p);

	// The bin, a red disc in the bottom right corner rather than one more
	// button in the corner above: it is the one choice here that cannot be
	// taken back, and it stands apart from the others for it. Tracks only.
	if (is_tracks) {
		p->sel_delete_btn = lv_btn_create(p->screen);
		lv_obj_set_size(p->sel_delete_btn, DELETE_FAB_SIZE, DELETE_FAB_SIZE);
		lv_obj_set_style_radius(p->sel_delete_btn, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_color(p->sel_delete_btn, lv_color_hex(SX_DANGER_BG), 0);
		lv_obj_set_style_bg_color(p->sel_delete_btn, lv_color_make(192, 28, 40), LV_STATE_PRESSED);
		lv_obj_set_style_bg_opa(p->sel_delete_btn, LV_OPA_COVER, 0);
		lv_obj_set_style_border_width(p->sel_delete_btn, 0, 0);
		lv_obj_set_style_shadow_width(p->sel_delete_btn, 0, 0);
		lv_obj_set_style_pad_all(p->sel_delete_btn, 0, 0);
		lv_obj_align(p->sel_delete_btn, LV_ALIGN_BOTTOM_RIGHT, -(int)cfg->padding - 6, -(int)cfg->padding - 6);
		lv_obj_add_event_cb(p->sel_delete_btn, sel_delete_cb, LV_EVENT_CLICKED, p);
		lv_obj_t *bin = lv_image_create(p->sel_delete_btn);
		lv_image_set_src(bin, &icon_trash);
		lv_obj_set_style_image_recolor(bin, lv_color_white(), 0);
		lv_obj_set_style_image_recolor_opa(bin, LV_OPA_COVER, 0);
		lv_obj_center(bin);
		lv_obj_set_hidden(p->sel_delete_btn, true);
		sx_frame_lift(p->sel_delete_btn);
	}
	lv_obj_set_hidden(p->sel_queue_btn, true);
	lv_obj_set_hidden(p->sel_fav_btn, true);
	lv_obj_set_hidden(p->sel_add_btn, true);
	lv_obj_set_hidden(p->sel_close_btn, true);

	p->empty = lv_label_create(p->list);
	lv_label_set_text(p->empty, tr("library_empty_note"));
	lv_obj_set_style_text_align(p->empty, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(p->empty, &sx_style_text2, 0);
	lv_obj_set_style_text_font(p->empty, &font_ui_24, 0);
	lv_obj_align(p->empty, LV_ALIGN_TOP_MID, 0, 120);
	lv_obj_set_hidden(p->empty, true);

	static const sx_row_handlers_t HANDLERS = {
		.on_tap = row_tap_cb,
		.on_long_press = row_hold_cb,
		.on_swipe_next = row_next_cb,
		.on_swipe_end = row_end_cb,
		.on_star = row_star_cb,
	};
	for (int i = 0; i < ROW_POOL; i++) {
		row_t *row = &p->rows[i];
		row->panel = p;
		row->row = sx_row_create(p->body, &HANDLERS, false);
		sx_row_set_user(row->row, row);
		lv_obj_set_x(row->row, 0);
		lv_obj_set_hidden(row->row, true);
		// The grip of a playlist being put in order, on the trailing slot.
		lv_obj_t *trail = sx_row_trail(row->row);
		lv_obj_add_event_cb(trail, grip_cb, LV_EVENT_PRESSED, p);
		lv_obj_add_event_cb(trail, grip_cb, LV_EVENT_PRESSING, p);
		lv_obj_add_event_cb(trail, grip_cb, LV_EVENT_RELEASED, p);
		lv_obj_add_event_cb(trail, grip_cb, LV_EVENT_PRESS_LOST, p);
		row->index = -1;
		row->count = -1;
	}

	// Walking off the page leaves the mode. Otherwise the panel comes back to
	// another list still in it -- with a playlist name in p->playlist that is no
	// longer the list on screen, which is the one way a drop could be written
	// to the wrong playlist.
	lv_obj_add_event_cb(p->screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, p);

	// The player sheet can be pulled in from the list, like everywhere else,
	// and the swipe-back drag can start on it too.
	player_sheet_attach_drag(p->list, true); // a page surface: swiping left pulls the player in
	switcher_attach_back_gesture(p->list);

	// Last, so both sit above the rows they cover.
	index_build(p, cfg);
}

// ---------------------------------------------------------------------------
// One album: the disc headings and the album head
// ---------------------------------------------------------------------------

static void disc_heads_clear(panel_t *p) {
	for (int i = 0; i < p->disc_count; i++) {
		if (p->disc_heads && p->disc_heads[i]) {
			lv_obj_delete(p->disc_heads[i]);
		}
	}
	free(p->disc_heads);
	free(p->disc_first);
	free(p->disc_no);
	p->disc_heads = NULL;
	p->disc_first = NULL;
	p->disc_no = NULL;
	p->disc_count = 0;
	free(p->track_no);
	p->track_no = NULL;
}

// Every heading put back above the first row of its disc. Their places move
// with the album head, so this runs whenever the head is built or taken away.
static void disc_heads_place(panel_t *p) {
	for (int i = 0; i < p->disc_count; i++) {
		lv_obj_t *head = p->disc_heads[i];
		int line = lv_font_get_line_height(lv_obj_get_style_text_font(head, 0));
		lv_obj_set_pos(head, SX_ROW_PAD_L, row_y(p, p->disc_first[i]) - DISC_HEAD_BELOW - line);
	}
}

// Reads where every track of the album sits on its record and, on a record of
// more than one disc, puts a heading above the first track of each. On the
// album page the track numbers are kept for the rows as well.
//
// Nothing here changes which rows the list has or their order: the rows are
// the handle's, and a heading is only space above one of them. When the index
// and the list disagree about how many tracks the album has -- a scan between
// the two reads -- the list stays plain rather than putting a heading over the
// wrong row.
static void album_places_load(panel_t *p) {
	disc_heads_clear(p);
	if (p->count <= 0 || p->count > ALBUM_PLACES_MAX) {
		return;
	}

	uint16_t *disc = malloc((size_t)p->count * sizeof(*disc));
	uint16_t *track = malloc((size_t)p->count * sizeof(*track));
	if (!disc || !track || library_album_places(p->filter_value, disc, track, p->count) != p->count) {
		free(disc);
		free(track);
		return;
	}

	int runs = 1;
	for (int i = 1; i < p->count; i++) {
		if (disc[i] != disc[i - 1]) {
			runs++;
		}
	}

	if (runs >= 2 && runs <= DISC_HEAD_MAX) {
		p->disc_first = malloc((size_t)runs * sizeof(*p->disc_first));
		p->disc_no = malloc((size_t)runs * sizeof(*p->disc_no));
		p->disc_heads = calloc((size_t)runs, sizeof(*p->disc_heads));
		if (p->disc_first && p->disc_no && p->disc_heads) {
			int n = 0;
			for (int i = 0; i < p->count; i++) {
				if (i == 0 || disc[i] != disc[i - 1]) {
					p->disc_first[n] = i;
					p->disc_no[n] = disc[i];
					n++;
				}
			}
			for (int i = 0; i < runs; i++) {
				char text[64];
				snprintf(text, sizeof(text), "%s %u", tr("medialist_disc"), (unsigned)p->disc_no[i]);
				lv_obj_t *head = lv_label_create(p->body);
				lv_label_set_text(head, text);
				lv_obj_set_style_text_font(head, &font_ui_20_bold, 0);
				if (p->album_page) {
					// On the gradient: white, quieter.
					lv_obj_set_style_text_color(head, lv_color_white(), 0);
					lv_obj_set_style_text_opa(head, LV_OPA_80, 0);
				} else {
					lv_obj_add_style(head, &sx_style_text2, 0);
				}
				lv_obj_set_clickable(head, false);
				p->disc_heads[i] = head;
				p->disc_count = i + 1;
			}
			disc_heads_place(p);
		} else {
			free(p->disc_first);
			free(p->disc_no);
			free(p->disc_heads);
			p->disc_first = NULL;
			p->disc_no = NULL;
			p->disc_heads = NULL;
		}
	}
	free(disc);

	if (p->album_page) {
		p->track_no = track;
	} else {
		free(track);
	}
}

// ---------------------------------------------------------------------------
// The first-visit hint
// ---------------------------------------------------------------------------

static bool hint_seen(void) { return config_get_int("ui", "list_hint_seen", 0) != 0; }

static void hint_clear(panel_t *p) {
	if (p->hint) {
		lv_obj_delete(p->hint);
		p->hint = NULL;
	}
}

static void relayout_rows(panel_t *p);

static void hint_done_cb(lv_event_t *e) {
	(void)e;
	config_set_int("ui", "list_hint_seen", 1);
	config_save();
	// Off both pages that may show it, at once.
	panel_t *panels[] = {&panel_names, &panel_tracks, &panel_artist_albums};
	for (size_t i = 0; i < sizeof(panels) / sizeof(panels[0]); i++) {
		panel_t *p = panels[i];
		if (p->hint) {
			hint_clear(p);
			p->head_height = 0;
			relayout_rows(p);
		}
	}
}

// The card above the rows of All tracks and Albums until it is dismissed:
// what the swipes and the long press do (the handoff's "Жесты в списках").
static void hint_build(panel_t *p) {
	hint_clear(p);
	p->hint = sx_card_create(p->body);
	lv_obj_set_pos(p->hint, 0, 0);
	sx_card_title(p->hint, tr("lists_hint_title"));
	lv_obj_t *sub = sx_card_subtitle(p->hint, tr("lists_hint_text"), false);
	if (sub) {
		lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
	}
	sx_card_action(p->hint, tr("lists_hint_ok"), hint_done_cb, p);
	lv_obj_update_layout(p->hint);
	p->head_height = lv_obj_get_height(p->hint) + HINT_GAP;
}

// ---------------------------------------------------------------------------
// The album page
// ---------------------------------------------------------------------------

// Where the list starts: under the header row as on every other page, or, on
// the album page, right under the status bar, the head scrolling up to it.
static void list_place(panel_t *p, bool under_status_bar_only) {
	int top = under_status_bar_only ? (int)p->cfg->top_bar_height : settingsrow_content_top(p->cfg);
	lv_obj_set_size(p->list, p->cfg->screen_width, p->cfg->screen_height - top);
	lv_obj_align(p->list, LV_ALIGN_TOP_LEFT, 0, top);
}

static void head_cover_show(panel_t *p, const lv_image_dsc_t *dsc) {
	if (!p->head_cover) {
		return;
	}
	lv_image_set_src(p->head_cover, dsc);
	int32_t w = dsc ? (int32_t)dsc->header.w : 0;
	lv_image_set_scale(p->head_cover, w > 0 && w != HEAD_COVER_SIZE ? (uint32_t)(HEAD_COVER_SIZE * LV_SCALE_NONE / w)
																	 : LV_SCALE_NONE);
}

static void head_large_cb(int64_t album_id, int size, const lv_image_dsc_t *dsc, void *user) {
	(void)size;
	panel_t *p = user;
	p->head_ticket = 0;
	if (!dsc) {
		return; // no picture: the stretched thumbnail or the plaque stays
	}
	if (!p->head || album_id != p->album_id) {
		lv_image_dsc_t copy = *dsc;
		covertone_thumb_free(&copy);
		return;
	}
	if (p->head_has_large) {
		lv_image_set_src(p->head_cover, NULL);
		covertone_thumb_free(&p->head_large_dsc);
	}
	p->head_large_dsc = *dsc;
	p->head_has_large = true;
	head_cover_show(p, &p->head_large_dsc);
}

// The page's colours: the gradient behind everything (made once per album),
// the status bar in the colour of its top, the rows on it in their "lift"
// look. Only while the album page is the page on screen and its hue pushed.
static lv_color_t grad_drawn_top;
static void show_corner(lv_obj_t *btn, bool shown);

static void album_paint(panel_t *p) {
	const sx_palette_t *pal = sx_pal();
	if (!p->grad) {
		int h = (int)p->cfg->screen_height;
		sx_grad_stop_t stops[3] = {
			{pal->album_grad[0], 0},
			{pal->album_grad[1], GRAD_Y1},
			{pal->album_grad[2], GRAD_Y2},
		};
		p->grad = sx_gradient_create(GRAD_STRIP_W, h, stops, 3);
		grad_drawn_top = pal->album_grad[0];
	}
	if (p->grad) {
		lv_obj_set_style_bg_image_src(p->screen, p->grad, 0);
		lv_obj_set_style_bg_image_tiled(p->screen, true, 0);
	}
	lv_obj_set_style_bg_color(p->screen, pal->album_grad[1], 0);
	topbar_tint_on_screen(p->screen, true, pal->album_grad[0], lv_color_white());
	if (p->head_bar) {
		lv_obj_set_style_bg_color(p->head_bar, pal->album_grad[0], 0);
	}
	// The mini player's band stands on the page's colour where it is (the
	// gradient between its 380 and 900 px stops), not on the theme's page.
	lv_obj_t *mini = sx_mini_obj();
	if (mini) {
		int y = (int)p->cfg->screen_height - SX_MINI_SLOT_H / 2;
		int pct = y <= GRAD_Y1 ? 0 : y >= GRAD_Y2 ? 100 : (y - GRAD_Y1) * 100 / (GRAD_Y2 - GRAD_Y1);
		lv_obj_set_style_bg_color(mini, sx_mix(pal->album_grad[2], pal->album_grad[1], (uint8_t)pct), 0);
	}
	lv_color_t around = pal->album_grad[1];
	for (int i = 0; i < ROW_POOL; i++) {
		sx_row_set_lift(p->rows[i].row, true, around);
	}
	if (p->head_play_label) {
		lv_obj_set_style_text_color(p->head_play_label, sx_oklch(0.42f, 0.08f, p->album_hue), 0);
	}
}

static void album_unpaint(panel_t *p) {
	lv_obj_remove_local_style_prop(p->screen, LV_STYLE_BG_IMAGE_SRC, 0);
	lv_obj_remove_local_style_prop(p->screen, LV_STYLE_BG_IMAGE_TILED, 0);
	lv_obj_remove_local_style_prop(p->screen, LV_STYLE_BG_COLOR, 0);
	topbar_tint_on_screen(p->screen, false, lv_color_black(), lv_color_white());
	for (int i = 0; i < ROW_POOL; i++) {
		sx_row_set_lift(p->rows[i].row, false, lv_color_black());
	}
}

static void album_page_enter(panel_t *p) {
	if (!p->album_page || p->hue_pushed) {
		return;
	}
	p->hue_pushed = true;
	sx_palette_push_hue(p->album_hue, p->album_chroma);
	back_btn_hide_on_screen(p->screen, true);
	album_paint(p);
}

// The palette moved (a theme, a tint, the playing track's hue): the album
// page's gradient follows when its colours are no longer the ones it was
// drawn in.

static void head_theme_refresh(void) {
	panel_t *p = &panel_tracks;
	if (!p->album_page || !p->hue_pushed) {
		return;
	}
	if (p->grad && !lv_color_eq(grad_drawn_top, sx_pal()->album_grad[0])) {
		lv_obj_remove_local_style_prop(p->screen, LV_STYLE_BG_IMAGE_SRC, 0);
		sx_gradient_destroy(p->grad);
		p->grad = NULL;
	}
	album_paint(p);
}

static void album_page_leave(panel_t *p) {
	if (!p->hue_pushed) {
		return;
	}
	p->hue_pushed = false;
	sx_palette_pop_hue();
	if (sx_mini_obj()) {
		lv_obj_remove_local_style_prop(sx_mini_obj(), LV_STYLE_BG_COLOR, 0);
	}
	back_btn_hide_on_screen(p->screen, false);
	topbar_tint_on_screen(p->screen, false, lv_color_black(), lv_color_white());
}

// Takes the album page away: the head, its picture, its gradient and hue, and
// gives the page back its header.
static void head_clear(panel_t *p) {
	if (p->head_ticket) {
		covertone_thumb_cancel(p->head_ticket);
		p->head_ticket = 0;
	}
	if (p->head) {
		lv_obj_delete(p->head); // the picture goes before its pixels do
		p->head = NULL;
		p->head_cover = NULL;
		p->head_play_label = NULL;
	}
	if (p->head_has_large) {
		covertone_thumb_free(&p->head_large_dsc);
		p->head_has_large = false;
	}
	if (p->album_page) {
		album_page_leave(p);
		album_unpaint(p);
		p->album_page = false;
		lv_label_set_text(p->title_label, p->title);
		lv_obj_set_hidden(p->title_label, false);
		list_place(p, false);
		if (p->back_float) {
			lv_obj_set_hidden(p->back_float, true);
		}
	}
	if (p->grad) {
		lv_obj_remove_local_style_prop(p->screen, LV_STYLE_BG_IMAGE_SRC, 0);
		sx_gradient_destroy(p->grad);
		p->grad = NULL;
	}
	if (p->head_bar) {
		lv_obj_set_hidden(p->head_bar, true);
	}
	p->head_height = 0;
}

static void head_shuffle_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	play_artist(lv_event_get_user_data(e), true);
}

static void head_play_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	play_artist(lv_event_get_user_data(e), false);
}

// ⋮: the album's sheet, with the play menu's other choices after it.
static void head_menu_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	panel_t *p = lv_event_get_user_data(e);
	if (!p || p->count <= 0) {
		return;
	}
	play_menu_panel = p;
	sx_sheet_item_t extra[3];
	int n = 0;
	extra[n++] = (sx_sheet_item_t){tr("medialist_queue_random_track"), &icon_music2, false, false,
								   play_menu_random_track_cb, NULL};
	if (panel_selectable(p)) {
		menu_panel = p;
		menu_index = 0;
		extra[n++] = (sx_sheet_item_t){tr("lists_select"), &icon_check, false, false, menu_select_action, NULL};
	}
	listacts_album_sheet(p->filter_value, p->album_artist[0] ? p->album_artist : NULL, NULL, extra, n);
}

static void back_float_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	back_btn_cb(e);
}

// A bare white glyph on the gradient: shuffle, the sheet.
static lv_obj_t *head_glyph_button(lv_obj_t *parent, const lv_image_dsc_t *glyph, lv_event_cb_t cb, panel_t *p) {
	lv_obj_t *button = lv_btn_create(parent);
	lv_obj_remove_style_all(button);
	lv_obj_set_size(button, HEAD_BTN_W, HEAD_BTN_H);
	lv_obj_set_style_radius(button, HEAD_BTN_H / 2, 0);
	lv_obj_set_style_bg_color(button, lv_color_white(), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(button, 56, LV_STATE_PRESSED);
	lv_obj_set_event_bubble(button, true); // a swipe across it still scrolls or goes back
	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, p);

	lv_obj_t *image = lv_image_create(button);
	lv_image_set_src(image, glyph);
	lv_obj_set_style_image_recolor(image, lv_color_white(), 0);
	lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
	lv_obj_set_clickable(image, false);
	lv_obj_center(image);
	return button;
}

static lv_obj_t *head_line(lv_obj_t *parent, const char *text, const lv_font_t *font, int lines, int width) {
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text(label, text);
	lv_obj_set_width(label, LV_PCT(100));
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_color(label, lv_color_white(), 0);
	lv_obj_set_clickable(label, false);
	// As many lines as the text needs, up to `lines`.
	lv_point_t size;
	lv_text_get_size(&size, text, font, 0, 0, width, LV_TEXT_FLAG_NONE);
	int line = lv_font_get_line_height(font);
	int needed = line > 0 ? (size.y + line - 1) / line : 1;
	needed = needed < 1 ? 1 : (needed > lines ? lines : needed);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_height(label, needed * line);
	return label;
}

// The album page's head: cover, title, artist, the figures, the buttons. Built
// for the album being opened and thrown away with it.
static void head_build(panel_t *p) {
	library_album_info_t info;
	memset(&info, 0, sizeof(info));
	library_album_info(p->filter_value, &info);

	p->album_page = true;
	p->album_id = covertone_album_id(p->filter_value);
	float h = 0.0f, c = 0.0f;
	if (covertone_for_album(p->album_id, &h, &c, NULL)) {
		p->album_hue = h;
		p->album_chroma = c;
	} else {
		char title[256];
		library_album_title(p->filter_value, title, sizeof(title));
		p->album_hue = (float)covertone_hash_hue(title);
		p->album_chroma = -1.0f;
	}

	int width = (int)p->cfg->screen_width - 2 * (int)p->cfg->padding;

	// The list up to the status bar, no header row: the floating back button
	// stands over the head instead.
	list_place(p, true);
	lv_label_set_text(p->title_label, "");
	lv_obj_set_hidden(p->title_label, true);
	if (!p->back_float) {
		p->back_float = lv_btn_create(p->screen);
		lv_obj_remove_style_all(p->back_float);
		lv_obj_set_size(p->back_float, HEAD_BACK_SIZE, HEAD_BACK_SIZE);
		lv_obj_set_style_radius(p->back_float, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_color(p->back_float, lv_color_black(), 0);
		lv_obj_set_style_bg_opa(p->back_float, 56, 0);
		lv_obj_set_style_bg_opa(p->back_float, 110, LV_STATE_PRESSED);
		lv_obj_add_event_cb(p->back_float, back_float_cb, LV_EVENT_CLICKED, NULL);
		lv_obj_t *chevron = lv_image_create(p->back_float);
		lv_image_set_src(chevron, &icon_chevron_left);
		lv_obj_set_style_image_recolor(chevron, lv_color_white(), 0);
		lv_obj_set_style_image_recolor_opa(chevron, LV_OPA_COVER, 0);
		lv_obj_center(chevron);
		sx_frame_exempt(p->back_float);
	}
	lv_obj_set_pos(p->back_float, 10, (int)p->cfg->top_bar_height + 8);
	lv_obj_set_hidden(p->back_float, false);
	lv_obj_move_foreground(p->back_float);
	lv_obj_move_foreground(p->corner);

	p->head = lv_obj_create(p->body);
	lv_obj_remove_style_all(p->head);
	lv_obj_set_size(p->head, width, LV_SIZE_CONTENT);
	lv_obj_set_pos(p->head, 0, 0);
	lv_obj_set_flex_flow(p->head, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(p->head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_top(p->head, 8, 0);
	lv_obj_set_style_pad_bottom(p->head, HEAD_GAP, 0);
	lv_obj_set_scrollable(p->head, false);
	// A part of the page, not a control: a swipe that starts on the cover
	// scrolls the list, pulls the player in or goes back like one on a row.
	lv_obj_set_clickable(p->head, false);
	lv_obj_set_event_bubble(p->head, true);

	// The plaque in the album's tone at once, the 56 px thumbnail stretched
	// over it when there is one (a 6 KB read), the 240 px one read on the
	// worker in its place.
	p->head_cover = lv_image_create(p->head);
	lv_obj_set_size(p->head_cover, HEAD_COVER_SIZE, HEAD_COVER_SIZE);
	lv_obj_set_style_radius(p->head_cover, SX_ROW_THUMB_R, 0);
	lv_obj_set_style_clip_corner(p->head_cover, true, 0);
	lv_obj_set_style_bg_color(p->head_cover, sx_oklch(0.55f, p->album_chroma < 0 ? 0.10f : LV_MIN(p->album_chroma, 0.10f),
													  p->album_hue),
							  0);
	lv_obj_set_style_bg_opa(p->head_cover, LV_OPA_COVER, 0);
	lv_image_set_inner_align(p->head_cover, LV_IMAGE_ALIGN_CENTER);
	lv_obj_set_clickable(p->head_cover, false);
	lv_obj_set_style_margin_bottom(p->head_cover, 10, 0);
	if (!p->head_small) {
		p->head_small = malloc(COVERTONE_THUMB_BYTES(COVERTONE_THUMB_SMALL));
	}
	if (p->head_small && covertone_thumb(p->album_id, COVERTONE_THUMB_SMALL, &p->head_small_dsc, p->head_small,
										 COVERTONE_THUMB_BYTES(COVERTONE_THUMB_SMALL))) {
		head_cover_show(p, &p->head_small_dsc);
	} else {
		covertone_request(p->album_id);
	}
	p->head_ticket = covertone_thumb_async(p->album_id, COVERTONE_THUMB_LARGE, head_large_cb, p);

	lv_obj_t *title = head_line(p->head, p->title, &font_ui_28_bold, HEAD_TITLE_LINES, width);
	(void)title;
	const char *artist = info.artist[0] ? info.artist : p->album_artist;
	if (artist && artist[0]) {
		lv_obj_t *line = head_line(p->head, artist, &font_ui_24, 1, width);
		lv_obj_set_style_margin_top(line, 2, 0);
	}

	// The figures: genre, year, how many tracks and how long. The minutes only
	// when every track has a length on record: an index from before the scan
	// timed tracks has none, and a sum over some of them would be a wrong
	// number rather than a missing one.
	char meta[256] = "";
	size_t at = 0;
	if (info.genre[0]) {
		at += (size_t)snprintf(meta + at, sizeof(meta) - at, "%s", info.genre);
	}
	if (info.year > 0 && at < sizeof(meta)) {
		at += (size_t)snprintf(meta + at, sizeof(meta) - at, "%s%d", at ? " \xC2\xB7 " : "", info.year);
	}
	if (info.tracks > 0 && at < sizeof(meta)) {
		char tracks[64];
		snprintf(tracks, sizeof(tracks), tr("lists_tracks_n"), info.tracks);
		at += (size_t)snprintf(meta + at, sizeof(meta) - at, "%s%s", at ? " \xC2\xB7 " : "", tracks);
	}
	if (!info.length_partial && info.length_ms > 0 && at < sizeof(meta)) {
		char minutes[48];
		snprintf(minutes, sizeof(minutes), tr("medialist_album_minutes"), (int)((info.length_ms + 30000) / 60000));
		snprintf(meta + at, sizeof(meta) - at, "%s%s", at ? " \xC2\xB7 " : "", minutes);
	}
	if (meta[0]) {
		lv_obj_t *line = head_line(p->head, meta, &font_ui_20, 1, width);
		lv_obj_set_style_text_opa(line, 199, 0); // .78
		lv_obj_set_style_margin_top(line, 2, 0);
	}

	lv_obj_t *buttons = lv_obj_create(p->head);
	lv_obj_remove_style_all(buttons);
	lv_obj_set_size(buttons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(buttons, HEAD_BTN_GAP, 0);
	lv_obj_set_style_margin_top(buttons, 12, 0);
	lv_obj_set_scrollable(buttons, false);
	lv_obj_set_clickable(buttons, false);
	lv_obj_set_event_bubble(buttons, true);
	head_glyph_button(buttons, &icon_shuffle, head_shuffle_cb, p);

	// "Play": the white pill, its word in the album's ink.
	lv_obj_t *play = lv_btn_create(buttons);
	lv_obj_remove_style_all(play);
	lv_obj_set_size(play, LV_SIZE_CONTENT, HEAD_PLAY_HEIGHT);
	lv_obj_set_style_bg_color(play, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(play, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(play, lv_color_hex(0xDCDCDC), LV_STATE_PRESSED);
	lv_obj_set_style_radius(play, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_pad_hor(play, HEAD_PLAY_PAD, 0);
	lv_obj_set_event_bubble(play, true);
	lv_obj_add_event_cb(play, head_play_cb, LV_EVENT_CLICKED, p);
	p->head_play_label = lv_label_create(play);
	lv_label_set_text(p->head_play_label, tr("medialist_play"));
	lv_obj_set_style_text_font(p->head_play_label, &font_ui_24_bold, 0);
	lv_obj_set_style_text_color(p->head_play_label, sx_oklch(0.42f, 0.08f, p->album_hue), 0);
	lv_obj_center(p->head_play_label);

	head_glyph_button(buttons, &icon_ellipsis_vertical, head_menu_cb, p);

	// The real height of all that, for the rows to start under. The screen may
	// never have been laid out yet -- it is built hidden -- so it is laid out
	// here rather than read as whatever LVGL started the objects at.
	lv_obj_update_layout(p->head);
	p->head_height = lv_obj_get_height(p->head);
}

// The heading row on the album page stays empty (the head says it all); in
// selection mode its bar stands behind the buttons.
static void head_follow(panel_t *p) { (void)p; }

static void head_bar_update(panel_t *p) {
	if (!p->head_bar) {
		return;
	}
	bool bar = p->album_page && p->selecting;
	lv_obj_set_hidden(p->head_bar, !bar);
	if (bar) {
		lv_obj_move_foreground(p->head_bar);
		lv_obj_move_foreground(p->corner);
	}
}

// Takes away whatever the last list put above and between its rows -- and
// takes the rows' height for the list about to be shown, which every way of
// opening one passes through here first.
static void album_extras_clear(panel_t *p) {
	head_clear(p);
	hint_clear(p);
	disc_heads_clear(p);
	p->head_height = 0;
	p->row_h = sx_row_height(false);
}

// Builds them for the list now loaded: on one album the album page (with the
// alternative album view, the default) and the disc headings; on All tracks
// and on Albums the hint, until it has been read.
static void album_extras_build(panel_t *p) {
	if (panel_is_album(p)) {
		if (album_head) {
			head_build(p);
		}
		album_places_load(p);
	} else if (!p->from_paths && !hint_seen() && p->filter == LIBRARY_FILTER_NONE &&
			   (p->kind == LIBRARY_LIST_TRACKS || p->kind == LIBRARY_LIST_ALBUMS)) {
		hint_build(p);
	}

	// Building the head moves the list up to the status bar. On a list that
	// was scrolled to its end, LVGL pulls the scroll back to fit, and the
	// scroll event binds rows before the head has a height or the rows their
	// track numbers: they land at the top, over the cover. row_bind() leaves a
	// row whose index has not changed where it is, so every row is bound again
	// now that the head and the discs are known.
	for (int i = 0; i < ROW_POOL; i++) {
		p->rows[i].index = -2;
	}
}

// The rows placed again under a head that changed height (the hint gone).
static void relayout_rows(panel_t *p) {
	lv_obj_set_height(p->body, body_height(p));
	disc_heads_place(p);
	for (int i = 0; i < ROW_POOL; i++) {
		p->rows[i].index = -2;
	}
	window_update(p);
}

bool medialist_album_head(void) {
	load_view_settings();
	return album_head;
}

void medialist_set_album_head(bool on) {
	load_view_settings();
	if (album_head == on) {
		return;
	}
	album_head = on;
	config_set_int("library", "album_head", on ? 1 : 0);
	config_save();

	// An album left open behind the settings is laid out again, so going back
	// to it shows the change rather than the layout it was opened with.
	panel_t *p = &panel_tracks;
	if (!p->rows[0].row || !panel_is_album(p) || !p->ix) {
		return;
	}
	for (int i = 0; i < ROW_POOL; i++) {
		row_drop_thumb(p, &p->rows[i]);
		p->rows[i].index = -2;
	}
	// ⤮ only without the page's own buttons.
	show_corner(p->play_btn, !on);
	p->corner_slots = on ? 0 : 1;
	settingsrow_title_corner_slots(p->title_label, p->cfg, p->corner_slots);
	album_extras_clear(p);
	album_extras_build(p);
	lv_obj_set_height(p->body, body_height(p));
	lv_obj_scroll_to_y(p->list, 0, LV_ANIM_OFF);
	p->saved_scroll = 0;
	window_update(p);
}

// ---------------------------------------------------------------------------
// opening
// ---------------------------------------------------------------------------

static void show_corner(lv_obj_t *btn, bool shown) {
	if (!btn) {
		return;
	}
	if (shown) {
		lv_obj_set_hidden(btn, false);
	} else {
		lv_obj_set_hidden(btn, true);
	}
}

// The row of the list `p` has just loaded that "Go to the current track" opens
// on: the playing track in All tracks, and in the four name lists whatever it
// belongs to -- its record, its artist, its album artist, its genre. -1 when
// the option is off, the list is not one of those five, or nothing in it is
// playing.
static int current_row(panel_t *p) {
	if (!medialist_go_to_current() || !p->ix || reopening_resorted) {
		return -1;
	}
	np_cache_refresh();
	if (!np_path[0]) {
		return -1;
	}
	switch (p->kind) {
	case LIBRARY_LIST_TRACKS:
		return p->filter == LIBRARY_FILTER_NONE ? library_index_find_path(p->ix, np_path) : -1;
	case LIBRARY_LIST_ALBUMS:
		if (p->filter != LIBRARY_FILTER_NONE) {
			return -1;
		}
		return library_index_find_name(p->ix, np_album_value[0] ? np_album_value : np_album);
	case LIBRARY_LIST_ARTISTS:
		return library_index_find_name(p->ix, np_artist);
	case LIBRARY_LIST_ALBUM_ARTISTS:
		return library_index_find_name(p->ix, np_album_artist);
	case LIBRARY_LIST_GENRES:
		return library_index_find_name(p->ix, np_genre);
	default:
		return -1;
	}
}

// ---------------------------------------------------------------------------
// Stepping back on one screen
// ---------------------------------------------------------------------------

// Set while a list is reopened by the step back: it is not a step forward, and
// it comes back where it was scrolled to (step_back_scroll).
static bool opening_step_back;
static int step_back_scroll = -1;

// What the last medialist_open() did to the screen it opened on, for
// medialist_step_back_to_player(): the panel whose list it stacked, or the
// panel it left showing the same list it already showed.
static panel_t *last_stacked;
static panel_t *last_unchanged;

// Called by medialist_open() before the panel takes the new list. A different
// list opened on the screen that is already showing stacks the one it
// replaces; one opened from another screen starts the panel's steps afresh,
// since the switcher's history is what leads back there. Re-reading the same
// list, a re-sort and the disc button's swap are not steps.
static void panel_push_step(panel_t *p) {
	if (p->back_count == PANEL_BACK_DEPTH) {
		// The oldest goes: past this many the way back is the page before.
		memmove(&p->back[0], &p->back[1], (size_t)(PANEL_BACK_DEPTH - 1) * sizeof(p->back[0]));
		p->back_count--;
	}
	panel_step_t *step = &p->back[p->back_count++];
	step->kind = p->kind;
	step->filter = p->filter;
	snprintf(step->value, sizeof(step->value), "%s", p->filter_value);
	snprintf(step->title, sizeof(step->title), "%s", p->title);
	step->scroll = lv_obj_get_scroll_y(p->list);
	step->from_player = false;
}

static void panel_note_step(panel_t *p, const char *signature) {
	last_stacked = NULL;
	last_unchanged = NULL;
	if (opening_step_back || open_replacing || reopening_resorted) {
		return;
	}
	if (lv_screen_active() != p->screen) {
		p->back_count = 0;
		return;
	}
	if (p->from_paths || !p->signature[0]) {
		return;
	}
	if (strcmp(p->signature, signature) == 0) {
		last_unchanged = p;
		return;
	}
	panel_push_step(p);
	last_stacked = p;
}

static panel_t *panel_on_screen(void) {
	lv_obj_t *active = lv_screen_active();
	panel_t *const panels[] = {&panel_names, &panel_tracks, &panel_artist_albums};
	for (size_t i = 0; i < sizeof(panels) / sizeof(panels[0]); i++) {
		if (panels[i]->screen == active) {
			return panels[i];
		}
	}
	return NULL;
}

// Whether the step back stays on this screen; the swipe-back asks before it
// draws what is underneath.
static bool panel_back_peek(void) {
	panel_t *p = panel_on_screen();
	return p && p->back_count > 0 && library_is_open();
}

// The chevron on a list screen: the list this one replaced comes back first,
// where it was scrolled to; only with none left does the page go.
static bool panel_back_guard(void) {
	panel_t *p = panel_on_screen();
	if (!p || p->back_count == 0) {
		return false;
	}
	if (!library_is_open()) {
		p->back_count = 0; // nothing can be reopened without the card
		return false;
	}
	panel_step_t step = p->back[--p->back_count];
	opening_step_back = true;
	step_back_scroll = step.scroll;
	medialist_open(step.title, step.kind, step.filter, step.value[0] ? step.value : NULL);
	opening_step_back = false;
	step_back_scroll = -1;
	if (step.from_player) {
		player_sheet_open(false);
	}
	return true;
}

bool medialist_step_back_to_player(void) {
	panel_t *stacked = last_stacked;
	panel_t *unchanged = last_unchanged;
	last_stacked = NULL;
	last_unchanged = NULL;
	// The list asked for was already the one on the page under the player:
	// the step back is to that same list, with the player over it.
	if (unchanged && lv_screen_active() == unchanged->screen) {
		panel_push_step(unchanged);
		stacked = unchanged;
	}
	if (!stacked || stacked->back_count == 0 || lv_screen_active() != stacked->screen) {
		return false;
	}
	stacked->back[stacked->back_count - 1].from_player = true;
	return true;
}

void medialist_open(const char *title, library_list_t kind, library_filter_t filter, const char *filter_value) {
	last_stacked = NULL;
	last_unchanged = NULL;

	// Nothing to open without the card. The index lives on it, so every list
	// here would come up empty -- and an empty list under a title, with "scan
	// the library" written across it, tells the user to do the one thing that
	// cannot work right now.
	if (!library_is_open()) {
		gui_notify_no_card();
		return;
	}

	// Which of the three screens this list belongs on. Tracks are always the
	// last level; an album list narrowed to one artist or genre is the middle
	// one; everything else is where a walk starts.
	bool tracks_like = (kind == LIBRARY_LIST_TRACKS || kind == LIBRARY_LIST_FAVOURITES ||
						kind == LIBRARY_LIST_PLAYLIST || kind == LIBRARY_LIST_MOST_PLAYED ||
						kind == LIBRARY_LIST_RECENTLY_PLAYED);
	// The records added last count as one of these too: a short list in an
	// order of its own, with no letters to jump by and nothing to re-sort.
	bool artist_albums =
		kind == LIBRARY_LIST_ALBUMS && (filter == LIBRARY_FILTER_ARTIST || filter == LIBRARY_FILTER_ALBUM_ARTIST ||
										filter == LIBRARY_FILTER_GENRE || filter == LIBRARY_FILTER_YEAR ||
										filter == LIBRARY_FILTER_RECENT);
	panel_t *p = tracks_like ? &panel_tracks : (artist_albums ? &panel_artist_albums : &panel_names);

	char signature[600];
	snprintf(signature, sizeof(signature), "%d|%d|%s", (int)kind, (int)filter, filter_value ? filter_value : "");
	panel_note_step(p, signature);

	p->kind = kind;
	// A playlist is a query like any other, and the query's value is its
	// name -- which is also what the ellipsis needs to know to offer taking a
	// track out of it.
	if (kind == LIBRARY_LIST_PLAYLIST) {
		snprintf(p->playlist, sizeof(p->playlist), "%s", filter_value ? filter_value : "");
	} else {
		p->playlist[0] = '\0';
	}
	p->filter = filter;
	p->from_paths = false;
	snprintf(p->filter_value, sizeof(p->filter_value), "%s", filter_value ? filter_value : "");
	// Callers hand an album's value in as its title as well; the heading shows
	// the name.
	library_album_title(title, p->title, sizeof(p->title));
	if (kind == LIBRARY_LIST_TRACKS && filter == LIBRARY_FILTER_ALBUM) {
		library_album_artist(p->filter_value, p->album_artist, sizeof(p->album_artist));
	} else {
		p->album_artist[0] = '\0';
	}

	// Coming back to the very list that was open before? Then put the user
	// back where they left it instead of at the top.
	bool same_list = strcmp(signature, p->signature) == 0;
	snprintf(p->signature, sizeof(p->signature), "%s", signature);

	lv_label_set_text(p->title_label, p->title);

	// Which of the corner buttons this list has a use for.
	//
	// An artist's own tracks and records get the disc button that swaps one for
	// the other; the four flat alphabetical lists get the direction switch;
	// Favourites gets shuffle.
	bool artist_tracks =
		kind == LIBRARY_LIST_TRACKS && (filter == LIBRARY_FILTER_ARTIST || filter == LIBRARY_FILTER_ALBUM_ARTIST);
	bool artist_records_page =
		artist_albums && (filter == LIBRARY_FILTER_ARTIST || filter == LIBRARY_FILTER_ALBUM_ARTIST);
	bool sortable = (kind == LIBRARY_LIST_TRACKS && filter == LIBRARY_FILTER_NONE) ||
					(kind == LIBRARY_LIST_ALBUMS && !artist_albums) || kind == LIBRARY_LIST_ARTISTS ||
					kind == LIBRARY_LIST_ALBUM_ARTISTS;

	bool want_album = artist_tracks || artist_records_page;
	// ⌕ on the artists' lists, as the handoff draws them.
	bool want_search = kind == LIBRARY_LIST_ARTISTS || kind == LIBRARY_LIST_ALBUM_ARTISTS;
	// ⤮ (held: every way of starting the list), wherever "play all of this"
	// is a question worth asking: an artist's records, an artist's tracks, all
	// the tracks, the favourites, the most played, a playlist, inside one
	// album. Not on the name lists, where a row is not a set of tracks -- nor on
	// all the records, whose header the handoff keeps to the sort.
	bool want_play = artist_albums ||
					 kind == LIBRARY_LIST_FAVOURITES || kind == LIBRARY_LIST_PLAYLIST ||
					 kind == LIBRARY_LIST_MOST_PLAYED || kind == LIBRARY_LIST_RECENTLY_PLAYED ||
					 (kind == LIBRARY_LIST_TRACKS &&
					  (filter == LIBRARY_FILTER_NONE || filter == LIBRARY_FILTER_ARTIST ||
					   filter == LIBRARY_FILTER_ALBUM_ARTIST || filter == LIBRARY_FILTER_ALBUM));

	// The album page: its own buttons start it, so the corner does not offer
	// them a second time.
	if (album_head && kind == LIBRARY_LIST_TRACKS && filter == LIBRARY_FILTER_ALBUM) {
		want_play = false;
	}

	bool want_sort = sortable;
	bool want_reverse = kind == LIBRARY_LIST_FAVOURITES;
	// A playlist is the only list whose order belongs to the user. Every other
	// one comes back from the database in an order the database decides, and
	// dragging a row in it would have nowhere to be written down.
	bool want_reorder = kind == LIBRARY_LIST_PLAYLIST;

	// Whatever this panel was showing before, it is not showing it now.
	reorder_stop(p);
	selection_stop(p);

	show_corner(p->reorder_btn, want_reorder);
	show_corner(p->search_btn, want_search);
	show_corner(p->album_btn, want_album);
	show_corner(p->play_btn, want_play);
	show_corner(p->sort_btn, want_sort);
	show_corner(p->reverse_btn, want_reverse);

	// The heading stops where the corner shelf starts, and the shelf is a
	// different width on every one of these lists, so the heading is told how
	// many buttons to clear rather than the one settingsrow_title() assumes. A
	// long title -- a translated list name, or an album's own name -- otherwise
	// runs under them.
	int corner_buttons = (want_album ? 1 : 0) + (want_play ? 1 : 0) + (want_sort ? 1 : 0) + (want_reverse ? 1 : 0) +
						 (want_reorder ? 1 : 0) + (want_search ? 1 : 0);
	p->corner_slots = corner_buttons;
	settingsrow_title_corner_slots(p->title_label, p->cfg, corner_buttons);

	if (p->album_btn) {
		// Lit in the accent on the records: the button is a state, not an
		// action that happens once.
		lv_obj_set_style_image_recolor(lv_obj_get_child(p->album_btn, 0),
									   artist_albums ? sx_pal()->accent : sx_pal()->text, 0);
	}
	if (p->reverse_btn) {
		// Lit in the accent colour for as long as the list is reversed.
		lv_obj_set_style_image_recolor(lv_obj_get_child(p->reverse_btn, 0),
									   fav_reversed ? sx_pal()->accent : sx_pal()->text, 0);
	}
	bool by_date = sort_by_date(p) || sort_by_year(p) || sort_by_artist(p);
	lv_obj_move_foreground(p->corner);

	// Every pool row lets go of its artwork *before* the entries under it are
	// freed and reloaded.
	for (int i = 0; i < ROW_POOL; i++) {
		row_drop_thumb(p, &p->rows[i]);
		p->rows[i].index = -2; // force a rebind (indices may look unchanged)
	}

	model_clear(p);
	library_order_t order = LIBRARY_ORDER_DEFAULT;
	bool sort_desc = false;
	sort_order_for(kind, filter, &order, &sort_desc);

	// Z-A is the same list read backwards. The database has already done the
	// hard part -- the collation groups by script, folds case and accents and
	// skips leading articles -- and reading that backwards is exactly "the
	// other way round", where a second DESC query would only be the same sort
	// again. Reversed favourites are the same list read backwards as well: the
	// database returns it oldest-starred first, and with the button on, the
	// newest belongs on top.
	bool desc = (sortable && sort_desc) || (kind == LIBRARY_LIST_FAVOURITES && fav_reversed);

	// Most played starts out empty for a reason of its own: nothing has been
	// listened to long enough yet, not a library still to scan. Its note is
	// wrapped inside the page, since a translation can run wider than the
	// screen; the usual one keeps the width of its own lines.
	bool most_played = kind == LIBRARY_LIST_MOST_PLAYED;
	bool recently_played = kind == LIBRARY_LIST_RECENTLY_PLAYED;
	lv_label_set_text(p->empty, tr(most_played		 ? "medialist_most_played_empty"
								   : recently_played ? "medialist_recently_played_empty"
													 : "library_empty_note"));
	lv_obj_set_width(p->empty, most_played || recently_played ? lv_pct(90) : LV_SIZE_CONTENT);

	p->from_paths = false;
	p->list_order = order;
	p->list_desc = desc;
	p->ix = library_index_open(kind, filter, filter_value, order, desc);
	p->count = library_index_count(p->ix);

	// One album: the head, when it is on, and the disc headings.
	album_extras_clear(p);
	album_extras_build(p);

	lv_obj_set_height(p->body, body_height(p));
	if (p->count == 0) {
		lv_obj_set_hidden(p->empty, false);
	} else {
		lv_obj_set_hidden(p->empty, true);
	}

	// The A-Z strip, on the two long flat lists: all the tracks and all the
	// records. The others are either short (an artist's albums) or not filed by
	// letter at all (an artist's tracks gathered by record, Favourites in the
	// order they were starred), and a strip of letters on those would point at
	// rows that are not where it says.
	p->index_wanted = !by_date && ((kind == LIBRARY_LIST_TRACKS && filter == LIBRARY_FILTER_NONE) ||
								   (kind == LIBRARY_LIST_ALBUMS && !artist_albums) || kind == LIBRARY_LIST_ARTISTS ||
								   kind == LIBRARY_LIST_ALBUM_ARTISTS);
	index_rebuild(p, p->index_wanted, sortable && sort_is_desc(kind));

	// The list's real height before anything is worked out from it. Until its
	// screen has been shown once nothing has laid it out, and the height LVGL
	// reports is the few dozen pixels every object starts at: a row centred on
	// that would sit at the top of the page, under the heading.
	lv_obj_update_layout(p->list);
	int view_height = lv_obj_get_height(p->list);

	// The same list resumes at its old scroll position; a different one
	// starts at the top -- unless it opens on what is playing.
	int target_scroll = same_list ? p->saved_scroll : 0;
	int current = current_row(p);
	if (current >= 0) {
		// In the middle of the screen rather than at the top, so the rows
		// around it show where in the list it is.
		target_scroll = row_y(p, current) - (view_height - p->row_h) / 2;
		if (target_scroll < 0) {
			target_scroll = 0;
		}
	}
	// The step back: where the list was left, playing track or not.
	if (opening_step_back && step_back_scroll >= 0) {
		target_scroll = step_back_scroll;
	}
	int max_scroll = body_height(p) - view_height;
	if (target_scroll > max_scroll) {
		target_scroll = max_scroll > 0 ? max_scroll : 0;
	}
	lv_obj_scroll_to_y(p->list, target_scroll, LV_ANIM_OFF);
	p->saved_scroll = target_scroll;
	head_follow(p);
	window_update(p);

	// The album page takes its hue now, so it slides in in its own colours;
	// one opened on the screen already showing takes it at once.
	album_page_enter(p);

	if (open_replacing) {
		switch_screen_no_history(p->screen);
	} else {
		switch_screen(p->screen);
	}
}

// A track list built from paths handed in rather than queried: a playlist's
// contents. It reuses the tracks panel wholesale -- covers, the ellipsis
// menu, the tap that starts the whole list as the queue -- by filling the
// model through the very callback library_for_each would have used.
void medialist_open_paths(const char *title, const char *const *paths, const char *const *names,
						  const char *const *artists, int count, const char *playlist) {
	panel_t *p = &panel_tracks;
	selection_stop(p);
	p->kind = LIBRARY_LIST_TRACKS;
	p->filter = LIBRARY_FILTER_NONE;
	p->from_paths = true;
	snprintf(p->playlist, sizeof(p->playlist), "%s", playlist ? playlist : "");

	// Never "the same list" as a database one: the contents can change under
	// a playlist between visits, so always start at the top.
	snprintf(p->signature, sizeof(p->signature), "paths|%s", title ? title : "");

	// No album head and no disc headings on a list handed in.
	album_extras_clear(p);
	lv_label_set_text(p->title_label, title ? title : "");

	// A list handed in as paths has an order of its own -- the one whoever
	// built it put it in -- so the buttons that reorder the list stay away.
	// circle-play is not one of them: it orders the queue, not the list.
	show_corner(p->play_btn, true);
	show_corner(p->sort_btn, false);
	show_corner(p->album_btn, false);
	show_corner(p->reverse_btn, false);
	show_corner(p->reorder_btn, false);
	show_corner(p->search_btn, false);
	p->corner_slots = 1;
	settingsrow_title_corner_slots(p->title_label, p->cfg, 1);

	for (int i = 0; i < ROW_POOL; i++) {
		row_drop_thumb(p, &p->rows[i]);
		p->rows[i].index = -2;
	}

	model_clear(p);
	load_ctx_t ctx = {.panel = p, .capacity = 0};
	for (int i = 0; i < count; i++) {
		if (!paths || !paths[i] || !paths[i][0]) {
			continue;
		}
		const char *name = (names && names[i] && names[i][0]) ? names[i] : NULL;
		if (!name) {
			const char *slash = strrchr(paths[i], '/');
			name = slash ? slash + 1 : paths[i];
		}
		const char *artist = (artists && artists[i] && artists[i][0]) ? artists[i] : NULL;
		if (!load_row_cb(name, paths[i], artist, &ctx)) {
			break;
		}
	}

	lv_obj_set_height(p->body, body_height(p));
	lv_label_set_text(p->empty, tr("library_empty_note"));
	lv_obj_set_width(p->empty, LV_SIZE_CONTENT);
	if (p->count == 0) {
		lv_obj_set_hidden(p->empty, false);
	} else {
		lv_obj_set_hidden(p->empty, true);
	}

	// No A-Z strip on a playlist: its order is the one the user put it in.
	p->index_wanted = false;
	index_rebuild(p, false, false);

	lv_obj_scroll_to_y(p->list, 0, LV_ANIM_OFF);
	p->saved_scroll = 0;
	window_update(p);

	switch_screen(p->screen);
}

void medialist_playlist_renamed(const char *old_name, const char *new_name) {
	panel_t *p = &panel_tracks;
	if (!old_name || !new_name || !new_name[0] || strcmp(p->playlist, old_name) != 0) {
		return;
	}
	snprintf(p->playlist, sizeof(p->playlist), "%s", new_name);
	// The name is also the query: the list is a handle over the table called
	// after it, so a rename that stopped at the heading would leave the panel
	// asking for a table that is no longer there the next time it reloads.
	if (p->kind == LIBRARY_LIST_PLAYLIST) {
		snprintf(p->filter_value, sizeof(p->filter_value), "%s", new_name);
		snprintf(p->title, sizeof(p->title), "%s", new_name);
		snprintf(p->signature, sizeof(p->signature), "%d|%d|%s", (int)p->kind, (int)p->filter, new_name);
	}
	// The heading is the playlist's name here, and the list is still on screen
	// behind the page the rename was typed on.
	lv_label_set_text(p->title_label, new_name);
}

// A star, an undo or a queue change elsewhere: the rows' stars are read again,
// and a list of the favourites follows its table.
static void lists_changed(void) {
	panel_t *panels[] = {&panel_names, &panel_tracks, &panel_artist_albums};
	for (size_t i = 0; i < sizeof(panels) / sizeof(panels[0]); i++) {
		panel_t *p = panels[i];
		if (!p->rows[0].row || !p->list || !p->is_tracks) {
			continue;
		}
		panel_refresh_stale(p);
		for (int r = 0; r < ROW_POOL; r++) {
			row_t *row = &p->rows[r];
			const char *name = NULL, *path = NULL;
			if (row->index >= 0 && row->index < p->count && row_at(p, row->index, &name, &path)) {
				row_update_trail(p, row, name, path);
			}
		}
		if (lv_screen_active() == p->screen) {
			window_update(p);
		}
	}
}

void medialist_init(gui_config_t *cfg) {
	coverloader_start();

	sort_desc_mask = (unsigned)config_get_int("library", "sort_desc", 0);
	sort_added_mask = (unsigned)config_get_int("library", "sort_added", 0);
	sort_year_mask = (unsigned)config_get_int("library", "sort_year", 0);
	sort_artist_mask = (unsigned)config_get_int("library", "sort_artist", 0);
	artist_records = config_get_int("library", "artist_records", 0) != 0;
	load_view_settings();
	fav_reversed = config_get_int("library", "fav_reversed", 0) != 0;

	panel_names.screen = medialist_screen;
	panel_tracks.screen = medialist_tracks_screen;
	panel_artist_albums.screen = medialist_albums_screen;

	build_panel(&panel_names, cfg, false);
	build_panel(&panel_tracks, cfg, true);
	build_panel(&panel_artist_albums, cfg, false);

	panel_t *const panels[] = {&panel_names, &panel_tracks, &panel_artist_albums};
	for (size_t i = 0; i < sizeof(panels) / sizeof(panels[0]); i++) {
		switcher_set_back_guard(panels[i]->screen, panel_back_guard);
		switcher_set_back_guard_peek(panels[i]->screen, panel_back_peek);
	}
	// The records of one artist carry the disc button too, lit, to go back to
	// the tracks; leftmost, as on the track list.
	panel_artist_albums.album_btn =
		corner_button(panel_artist_albums.corner, &icon_album_corner, album_order_clicked_cb, &panel_artist_albums);
	lv_obj_move_to_index(panel_artist_albums.album_btn, 0);

	thumb_timer = lv_timer_create(thumb_timer_cb, THUMB_POLL_MS, NULL);
	lv_timer_pause(thumb_timer);

	// The A-Z strip fades itself out; one timer serves all three panels.
	//
	// Registered for standby: with the screen dark there is no strip to take
	// away and nobody to see it go, and five wake-ups a second all night is
	// exactly the sort of thing power_screen_off() exists to stop.
	lv_timer_t *index_timer = lv_timer_create(index_timer_cb, 200, NULL);
	power_pause_in_standby(index_timer);
	theme_register_refresh(index_theme_refresh);
	theme_register_refresh(head_theme_refresh);
	covertone_watch_add(covers_changed);
	listacts_on_change(lists_changed);
	sx_frame_on_body_change(index_fit_slot);
	index_fit_slot();
}

// ---------------------------------------------------------------------------
// The order, for SonixLink
// ---------------------------------------------------------------------------

// The same decisions the corner buttons make in the list itself: which lists
// can be reversed or put by date or by year.
void medialist_list_order(library_list_t kind, library_filter_t filter, library_order_t *order, bool *desc) {
	bool sortable = (kind == LIBRARY_LIST_TRACKS && filter == LIBRARY_FILTER_NONE) ||
					(kind == LIBRARY_LIST_ALBUMS && filter == LIBRARY_FILTER_NONE) || kind == LIBRARY_LIST_ARTISTS ||
					kind == LIBRARY_LIST_ALBUM_ARTISTS;
	library_order_t sort_order = LIBRARY_ORDER_DEFAULT;
	bool sort_desc = false;
	sort_order_for(kind, filter, &sort_order, &sort_desc);
	if (order) {
		*order = sort_order;
	}
	if (desc) {
		*desc = (sortable && sort_desc) || (kind == LIBRARY_LIST_FAVOURITES && fav_reversed);
	}
}

void medialist_sort_prefs(unsigned *desc_mask, unsigned *added_mask, bool *artist_by_album, bool *favourites_reversed) {
	// The app knows names and dates, not years: a list the player runs by year
	// is described to it as A-Z rather than as a direction it would apply to
	// the names.
	if (desc_mask) {
		*desc_mask = sort_desc_mask & ~sort_year_mask & ~sort_artist_mask;
	}
	if (added_mask) {
		*added_mask = sort_added_mask;
	}
	// An artist's tracks always run by title now; their records are a page of
	// their own.
	if (artist_by_album) {
		*artist_by_album = false;
	}
	if (favourites_reversed) {
		*favourites_reversed = fav_reversed;
	}
}

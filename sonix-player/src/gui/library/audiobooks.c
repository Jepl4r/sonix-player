#include "audiobooks.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/shell/confirm.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/playback/sleeptimer.h"
#include "src/system/playback/audiobook.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/playback/device_state.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"

lv_obj_t *audiobooks_screen;
lv_obj_t *audiobooksettings_screen;
lv_obj_t *audiobookscan_screen;
lv_obj_t *audiobookcontrols_screen;

// ---------------------------------------------------------------------------
// the index page: All / Recent / Finished
//
// The rows are the ones the all-tracks list uses -- same height, same 72 px
// thumbnail, same now-playing mark, same windowed pool of twelve widgets
// standing in for however many entries the database holds. A book carries a
// cover far more reliably than a loose track does (it is the jacket), which is
// most of the reason for the list looking like this at all.
//
// The data is windowed with the widgets: the page holds a handle -- four bytes
// a book -- and a band of forty-eight rows read back as the viewport moves, so
// there is no ceiling on how many books the card may hold.
// ---------------------------------------------------------------------------

#define ROW_HEIGHT 100
#define ROW_GAP 8
#define ROW_PITCH (ROW_HEIGHT + ROW_GAP)
#define ROW_RADIUS 12
#define ROW_PAD 14
#define THUMB_SIZE 72
#define ROW_POOL 12

// How many rows are read from the database at a time. Four times the pool, so
// scrolling a screenful does not go back to the database, and small enough that
// the band is a few kilobytes.
#define WINDOW_ROWS 48

#define PLAYMARK_WIDTH 6
#define PLAYMARK_HEIGHT 52
#define PLAYMARK_INSET 4 // from the row's left edge

#define THUMB_POLL_MS 150

// A row of the band, at fixed width. WINDOW_ROWS of these, bought once, beats
// an allocation per row bound -- and a book's name and path are the two things
// a row needs to draw itself.
typedef struct {
	char name[256];
	char path[512];
} winrow_t;

typedef struct {
	lv_obj_t *button;
	lv_obj_t *icon;
	lv_obj_t *label;
	lv_obj_t *playmark;
	int index;

	bool has_thumb;
	bool thumb_requested;
	bool thumb_settled;
	cover_image_t thumb;
} row_t;

static lv_obj_t *btn_all;
static lv_obj_t *btn_recent;
static lv_obj_t *btn_finished;
static lv_obj_t *book_list; // the scrollable viewport
static lv_obj_t *book_body; // the fixed-height canvas the rows are placed on
static lv_obj_t *empty_label;
static lv_timer_t *thumb_timer;

static row_t rows[ROW_POOL];

// The list, as a handle: four bytes a book, with the rows themselves read back
// a bandful at a time, as in medialist.
static audiobookdb_index_t *book_index;
static int entry_count;

// The band: which rows have been read, and from where.
static winrow_t *window_rows; // WINDOW_ROWS of them, allocated once
static int window_first = -1;
static int window_count;

static int row_width;

static audiobook_list_t current_view = AUDIOBOOK_LIST_ALL;
static char np_path[512]; // the book playing now, for the mark

static int row_slot(const row_t *row) { return COVERLOADER_AUDIOBOOKS_BASE + (int)(row - rows); }

// Reads a book out of the band, bringing the band over it first. Defined with
// the rest of the windowing further down.
static bool row_at(int index, const char **name_out, const char **path_out);

static void refresh_view_buttons(void) {
	lv_obj_t *const buttons[] = {btn_all, btn_recent, btn_finished};
	const audiobook_list_t views[] = {AUDIOBOOK_LIST_ALL, AUDIOBOOK_LIST_RECENT, AUDIOBOOK_LIST_FINISHED};

	for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
		if (!buttons[i]) {
			continue;
		}
		bool on = views[i] == current_view;
		lv_obj_set_style_bg_color(buttons[i], on ? theme()->accent : theme()->surface_pressed, 0);
		lv_obj_set_style_text_color(lv_obj_get_child(buttons[i], 0), on ? lv_color_white() : theme()->text_primary, 0);
	}
}

// ---------------------------------------------------------------------------
// rows
// ---------------------------------------------------------------------------

static void row_show_glyph(row_t *row) {
	lv_image_set_src(row->icon, &icon_book_headphones_row);
	lv_obj_add_style(row->icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(row->icon, LV_OPA_COVER, 0);
}

// A jacket must not be tinted, so the recolour the glyph relies on is switched
// off for as long as a picture is on the row.
static void row_show_cover(row_t *row, const lv_image_dsc_t *dsc) {
	lv_image_set_src(row->icon, dsc);
	lv_obj_set_style_image_recolor_opa(row->icon, LV_OPA_TRANSP, 0);
}

static void row_drop_thumb(row_t *row) {
	coverloader_release(row_slot(row));

	if (row->has_thumb) {
		row_show_glyph(row); // stop pointing at the pixels before they go
		cover_free(&row->thumb);
		row->has_thumb = false;
	}
	row->thumb_requested = false;
	row->thumb_settled = false;
}

static void row_update_playmark(row_t *row) {
	if (!row->playmark) {
		return;
	}
	const char *path = NULL;
	bool playing = np_path[0] && row_at(row->index, NULL, &path) && path[0] && strcmp(path, np_path) == 0;
	if (playing) {
		lv_obj_remove_flag(row->playmark, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(row->playmark, LV_OBJ_FLAG_HIDDEN);
	}
}

static void refresh_playmarks(void) {
	device_state_t state;
	device_state_get(&state);
	snprintf(np_path, sizeof(np_path), "%s", state.live ? "" : state.current_file);

	for (int i = 0; i < ROW_POOL; i++) {
		row_update_playmark(&rows[i]);
	}
}

static void row_bind(row_t *row, int index) {
	if (row->index == index) {
		return;
	}

	row_drop_thumb(row);
	row->index = index;

	if (index < 0 || index >= entry_count) {
		lv_obj_add_flag(row->button, LV_OBJ_FLAG_HIDDEN);
		return;
	}

	const char *name = NULL;
	if (!row_at(index, &name, NULL)) {
		// The handle went stale under the list, or the row is gone. Hiding it
		// is what the next window_update() undoes, once the list is rebuilt.
		lv_obj_add_flag(row->button, LV_OBJ_FLAG_HIDDEN);
		return;
	}

	lv_obj_remove_flag(row->button, LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_y(row->button, index * ROW_PITCH);
	lv_label_set_text(row->label, name);
	row_show_glyph(row);
	row_update_playmark(row);
}

// Asks for the artwork of the visible rows and collects what the worker has
// finished. The same loop the browser and the library lists run; here the
// source is the .m4b itself, whose `covr` atom is where a book keeps its
// jacket.
static void thumbs_update(void) {
	bool anything_pending = false;

	for (int i = 0; i < ROW_POOL; i++) {
		row_t *row = &rows[i];
		if (row->index < 0 || row->index >= entry_count || row->thumb_settled) {
			continue;
		}

		const char *source = NULL;
		if (!row_at(row->index, NULL, &source) || !source[0]) {
			row->thumb_settled = true;
			continue;
		}

		if (!row->thumb_requested) {
			coverloader_request(row_slot(row), source, THUMB_SIZE);
			row->thumb_requested = true;
		}

		bool finished = false;
		cover_image_t image;
		if (coverloader_take(row_slot(row), &image, &finished)) {
			row->thumb = image;
			row->has_thumb = true;
			row->thumb_settled = true;
			row_show_cover(row, &row->thumb.dsc);
		} else if (finished) {
			row->thumb_settled = true; // no jacket in this one; the book glyph stays
		} else {
			anything_pending = true;
		}
	}

	if (anything_pending) {
		lv_timer_resume(thumb_timer);
	}
}

static void thumb_timer_cb(lv_timer_t *timer) {
	lv_timer_pause(timer); // thumbs_update resumes it while work is pending
	if (lv_screen_active() == audiobooks_screen) {
		thumbs_update();
	}
}

// A rescan, or a card change, or a book marked finished: the rows the handle
// names have moved, so the list is rebuilt where it stands rather than left
// showing other people\'s books. The same thing medialist does.
static void refresh_stale(void) {
	if (!book_index || !audiobookdb_index_stale(book_index)) {
		return;
	}
	audiobookdb_index_t *fresh = audiobookdb_index_open(current_view);
	audiobookdb_index_close(book_index);
	book_index = fresh;
	entry_count = audiobookdb_index_count(fresh);
	window_first = -1;
	window_count = 0;

	for (int i = 0; i < ROW_POOL; i++) {
		row_drop_thumb(&rows[i]);
		rows[i].index = -2; // every row now stands for a different book
	}
	lv_obj_set_height(book_body, entry_count > 0 ? entry_count * ROW_PITCH : 1);
	if (entry_count == 0) {
		lv_obj_remove_flag(empty_label, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(empty_label, LV_OBJ_FLAG_HIDDEN);
	}
}

static void window_update(void) {
	refresh_stale();
	if (entry_count == 0) {
		for (int i = 0; i < ROW_POOL; i++) {
			row_bind(&rows[i], -1);
		}
		return;
	}

	int scroll = lv_obj_get_scroll_y(book_list);
	if (scroll < 0) {
		scroll = 0;
	}

	int first = (scroll / ROW_PITCH) - 1;
	if (first + ROW_POOL > entry_count) {
		first = entry_count - ROW_POOL;
	}
	if (first < 0) {
		first = 0;
	}

	// index -> widget index % ROW_POOL, so a one-step slide of the band
	// rebinds exactly one row instead of renaming all twelve.
	for (int i = 0; i < ROW_POOL; i++) {
		int index = first + i;
		row_bind(&rows[index % ROW_POOL], index < entry_count ? index : -1);
	}

	thumbs_update();
}

static void scroll_cb(lv_event_t *e) {
	(void)e;
	window_update();
}

// ---------------------------------------------------------------------------
// loading the list
// ---------------------------------------------------------------------------

static bool window_fill_cb(const char *name, const char *path, void *user) {
	int *filled = user;
	if (*filled >= WINDOW_ROWS) {
		return false;
	}
	winrow_t *row = &window_rows[*filled];
	snprintf(row->name, sizeof(row->name), "%s", name ? name : "");
	snprintf(row->path, sizeof(row->path), "%s", path ? path : "");
	(*filled)++;
	return true;
}

// Brings `index` into the band, reading a windowful around it. The band starts
// a pool's worth before the wanted row, so scrolling back up a little does not
// send it to the database again.
static bool window_cover(int index) {
	if (!book_index || index < 0 || index >= entry_count) {
		return false;
	}
	if (!window_rows) {
		window_rows = calloc(WINDOW_ROWS, sizeof(*window_rows));
		if (!window_rows) {
			return false;
		}
	}
	if (window_first >= 0 && index >= window_first && index < window_first + window_count) {
		return true;
	}

	int first = index - ROW_POOL;
	if (first < 0) {
		first = 0;
	}
	if (first + WINDOW_ROWS > entry_count) {
		first = entry_count - WINDOW_ROWS;
	}
	if (first < 0) {
		first = 0;
	}

	int filled = 0;
	audiobookdb_index_window(book_index, first, WINDOW_ROWS, window_fill_cb, &filled);
	window_first = first;
	window_count = filled;
	return index >= first && index < first + filled;
}

// The name of a book and its file. False when the row is not there -- a stale
// handle, or an index past the end.
static bool row_at(int index, const char **name_out, const char **path_out) {
	if (index < 0 || index >= entry_count || !window_cover(index)) {
		return false;
	}
	const winrow_t *row = &window_rows[index - window_first];
	if (name_out) {
		*name_out = row->name;
	}
	if (path_out) {
		*path_out = row->path;
	}
	return true;
}

static const char *empty_text(void) {
	switch (current_view) {
	case AUDIOBOOK_LIST_RECENT:
		return tr("audiobook_recent_empty");
	case AUDIOBOOK_LIST_FINISHED:
		return tr("audiobook_finished_empty");
	default:
		return tr("audiobook_empty_note");
	}
}

static void reload_list(void) {
	// Unbind first, and set the index by hand afterwards: row_bind() is a no-op
	// when the index has not moved, so switching between two lists of the same
	// length would otherwise leave every row showing the old titles.
	for (int i = 0; i < ROW_POOL; i++) {
		row_bind(&rows[i], -1);
		rows[i].index = -1;
	}

	audiobookdb_index_close(book_index);
	book_index = audiobookdb_index_open(current_view);
	entry_count = audiobookdb_index_count(book_index);
	window_first = -1;
	window_count = 0;

	lv_obj_set_height(book_body, entry_count > 0 ? entry_count * ROW_PITCH : 1);
	lv_obj_scroll_to_y(book_list, 0, LV_ANIM_OFF);

	lv_label_set_text(empty_label, empty_text());
	if (entry_count == 0) {
		lv_obj_remove_flag(empty_label, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(empty_label, LV_OBJ_FLAG_HIDDEN);
	}

	refresh_playmarks();
	window_update();
}

// ---------------------------------------------------------------------------
// clicks
// ---------------------------------------------------------------------------

static void book_clicked_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return; // a swipe across the list, not a tap
	}

	lv_obj_t *button = lv_event_get_current_target(e);
	int index = -1;
	for (int i = 0; i < ROW_POOL; i++) {
		if (rows[i].button == button) {
			index = rows[i].index;
			break;
		}
	}
	const char *found = NULL;
	if (!row_at(index, NULL, &found) || !found[0]) {
		return;
	}

	const char *path = found;

	// Where this book was left. Zero for one never opened, and zero again for
	// one heard to the end -- finishing a book puts it on the Finished shelf and
	// winds it back, so it starts over instead of resuming into the credits.
	double resume = audiobook_saved_position(path);
	if (resume > 1.0) {
		device_state_play_file_at(path, resume);
	} else {
		device_state_play_file(path);
	}

	audiobookdb_touch(path); // it belongs in Recent from the moment it starts

	player_refresh_now_playing();
	player_sheet_open(true);
}

static void pick_view_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	current_view = (audiobook_list_t)(uintptr_t)lv_event_get_user_data(e);
	refresh_view_buttons();
	reload_list();
}

static lv_obj_t *make_view_choice(lv_obj_t *parent, const char *text, audiobook_list_t view) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, LV_SIZE_CONTENT, 56);
	// Three pills across the page rather than two, so they are tighter: enough
	// that Finished does not run off the right edge.
	lv_obj_set_style_pad_hor(btn, 20, 0);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0); // Adwaita pill button
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_add_event_cb(btn, pick_view_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)view);

	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, tr(text));
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	lv_obj_center(label);

	return btn;
}

static void books_screen_loaded_cb(lv_event_t *e) {
	(void)e;
	refresh_view_buttons();
	reload_list(); // a scan, or a book reaching its end, may have happened since
}

// ---------------------------------------------------------------------------

static void build_books_page(gui_config_t *cfg) {
	lv_obj_add_style(audiobooks_screen, &theme_style_screen, 0);
	settingsrow_title(audiobooks_screen, cfg, "audiobooks");

	// Options, top right, like the Music page's.
	lv_obj_t *options_btn = lv_btn_create(audiobooks_screen);
	lv_obj_set_size(options_btn, 56, 56);
	lv_obj_set_style_bg_opa(options_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(options_btn, 0, 0);
	lv_obj_set_style_shadow_width(options_btn, 0, 0);
	lv_obj_set_style_pad_all(options_btn, 0, 0);
	lv_obj_align(options_btn, LV_ALIGN_TOP_RIGHT, -cfg->padding, cfg->padding + cfg->top_bar_height);
	lv_obj_add_event_cb(options_btn, switch_screen_cb, LV_EVENT_CLICKED, audiobooksettings_screen);

	lv_obj_t *gear = lv_image_create(options_btn);
	lv_image_set_src(gear, &icon_music_settings);
	lv_obj_add_style(gear, &theme_style_icon, 0);
	lv_obj_center(gear);

	int content_top = settingsrow_content_top(cfg);
	row_width = cfg->screen_width - 2 * cfg->padding;

	lv_obj_t *container = lv_obj_create(audiobooks_screen);
	lv_obj_set_size(container, lv_pct(100), cfg->screen_height - content_top);
	lv_obj_align(container, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(container, 0, 0);
	lv_obj_set_style_border_width(container, 0, 0);
	lv_obj_set_style_radius(container, 0, 0);
	lv_obj_set_style_pad_hor(container, cfg->padding, 0);
	lv_obj_set_style_pad_ver(container, 0, 0);
	lv_obj_set_style_pad_gap(container, 12, 0);
	lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	// All | Recent | Finished.
	lv_obj_t *chooser = lv_obj_create(container);
	lv_obj_set_size(chooser, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(chooser, 0, 0);
	lv_obj_set_style_border_width(chooser, 0, 0);
	lv_obj_set_style_pad_all(chooser, 0, 0);
	lv_obj_set_style_pad_gap(chooser, 10, 0);
	lv_obj_remove_flag(chooser, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(chooser, LV_FLEX_FLOW_ROW);

	btn_all = make_view_choice(chooser, "audiobook_all", AUDIOBOOK_LIST_ALL);
	btn_recent = make_view_choice(chooser, "audiobook_recent", AUDIOBOOK_LIST_RECENT);
	btn_finished = make_view_choice(chooser, "audiobook_finished", AUDIOBOOK_LIST_FINISHED);

	// The list: a viewport with a fixed-height canvas inside it, so twelve row
	// widgets can stand in for however many books the card holds.
	book_list = lv_obj_create(container);
	lv_obj_set_width(book_list, lv_pct(100));
	lv_obj_set_flex_grow(book_list, 1);
	lv_obj_set_style_bg_opa(book_list, 0, 0);
	lv_obj_set_style_border_width(book_list, 0, 0);
	lv_obj_set_style_radius(book_list, 0, 0);
	lv_obj_set_style_pad_all(book_list, 0, 0);
	lv_obj_set_scroll_dir(book_list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(book_list, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_add_event_cb(book_list, scroll_cb, LV_EVENT_SCROLL, NULL);
	// Presses on the list (its rows bubble into it) and on the empty-message
	// area must reach the gesture handlers; set at creation, because with no
	// audiobooks on the card no row is ever bound to set it later.
	lv_obj_add_flag(book_list, LV_OBJ_FLAG_EVENT_BUBBLE);

	book_body = lv_obj_create(book_list);
	lv_obj_set_width(book_body, row_width);
	lv_obj_set_height(book_body, 1);
	lv_obj_set_pos(book_body, 0, 0);
	lv_obj_set_style_bg_opa(book_body, 0, 0);
	lv_obj_set_style_border_width(book_body, 0, 0);
	lv_obj_set_style_pad_all(book_body, 0, 0);
	lv_obj_remove_flag(book_body, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(book_body, LV_OBJ_FLAG_EVENT_BUBBLE);

	empty_label = lv_label_create(book_list);
	lv_label_set_text(empty_label, empty_text());
	lv_obj_set_style_text_align(empty_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(empty_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(empty_label, &font_ui_24, 0);
	lv_obj_align(empty_label, LV_ALIGN_TOP_MID, 0, 90);
	lv_obj_add_flag(empty_label, LV_OBJ_FLAG_HIDDEN);

	for (int i = 0; i < ROW_POOL; i++) {
		row_t *row = &rows[i];

		row->button = lv_btn_create(book_body);
		lv_obj_set_size(row->button, row_width, ROW_HEIGHT);
		lv_obj_set_x(row->button, 0);
		lv_obj_add_style(row->button, &theme_style_card, 0);
		lv_obj_add_style(row->button, &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_set_style_radius(row->button, ROW_RADIUS, 0);
		lv_obj_set_style_border_width(row->button, 0, 0);
		lv_obj_set_style_shadow_width(row->button, 0, 0);
		lv_obj_set_style_pad_all(row->button, ROW_PAD, 0);
		lv_obj_set_style_pad_column(row->button, ROW_PAD, 0);
		lv_obj_add_flag(row->button, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(row->button, LV_OBJ_FLAG_EVENT_BUBBLE); // so the player sheet can be dragged in
		lv_obj_add_event_cb(row->button, book_clicked_cb, LV_EVENT_CLICKED, NULL);
		lv_obj_set_flex_flow(row->button, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row->button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

		row->icon = lv_image_create(row->button);
		lv_obj_set_size(row->icon, THUMB_SIZE, THUMB_SIZE);
		lv_image_set_inner_align(row->icon, LV_IMAGE_ALIGN_CENTER);

		row->label = lv_label_create(row->button);
		lv_label_set_long_mode(row->label, LV_LABEL_LONG_DOT);
		lv_obj_set_flex_grow(row->label, 1);
		lv_obj_add_style(row->label, &theme_style_text, 0);
		lv_obj_set_style_text_font(row->label, &font_ui_24, 0);
		// Two lines at most: the cap on the height is what makes LV_LABEL_LONG_DOT
		// cut the name there, with the ellipsis, instead of wrapping on past the
		// row. A cap and not a height, so a one-line name stays centred.
		lv_obj_set_style_max_height(row->label,
									2 * lv_font_get_line_height(&font_ui_24) +
										lv_obj_get_style_text_line_space(row->label, LV_PART_MAIN),
									0);

		// Out of the flex layout on purpose: it sits in the row's own left
		// padding, so adding it moves nothing.
		row->playmark = lv_obj_create(row->button);
		lv_obj_add_flag(row->playmark, LV_OBJ_FLAG_IGNORE_LAYOUT);
		lv_obj_set_size(row->playmark, PLAYMARK_WIDTH, PLAYMARK_HEIGHT);
		lv_obj_align(row->playmark, LV_ALIGN_LEFT_MID, PLAYMARK_INSET - ROW_PAD, 0);
		lv_obj_add_style(row->playmark, &theme_style_accent_bg, 0);
		lv_obj_set_style_radius(row->playmark, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_border_width(row->playmark, 0, 0);
		lv_obj_set_style_shadow_width(row->playmark, 0, 0);
		lv_obj_set_style_pad_all(row->playmark, 0, 0);
		lv_obj_remove_flag(row->playmark, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_remove_flag(row->playmark, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_flag(row->playmark, LV_OBJ_FLAG_HIDDEN);

		row->index = -1;
		row->has_thumb = false;
		row->thumb_requested = false;
		row->thumb_settled = false;
	}

	thumb_timer = lv_timer_create(thumb_timer_cb, THUMB_POLL_MS, NULL);
	lv_timer_pause(thumb_timer);

	player_sheet_attach_drag(container, true); // a page surface: swiping left pulls the player in
	player_sheet_attach_drag(book_list, true);
	switcher_attach_back_gesture(container);
	switcher_attach_back_gesture(book_list);

	refresh_view_buttons();
	theme_register_refresh(refresh_view_buttons);
	lv_obj_add_event_cb(audiobooks_screen, books_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}

// ---------------------------------------------------------------------------
// the options page
// ---------------------------------------------------------------------------

static void scan_row_cb(lv_event_t *e);
// A dim line under a row, explaining what the row does not say by itself.
static void option_note(lv_obj_t *parent, const char *text) {
	lv_obj_t *note = lv_label_create(parent);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_22, 0);
	lv_label_set_text(note, tr(text));
}

static void paint_choice(lv_obj_t *btn, bool on);

// ---------------------------------------------------------------------------
// A toggle whose choices live on pills underneath it, and disappear with it.
//
// The same idea as automatic screen-off on the Display page -- options that
// only mean something while the switch is on should not be sitting there while
// it is off -- but with pills instead of a slider, because "end of chapter" is
// not a point on a scale of minutes.
//
// The card is LV_SIZE_CONTENT rather than two fixed heights, so hiding the row
// collapses it by exactly the right amount and five pills may wrap onto a
// second line without anything having to be measured.
// ---------------------------------------------------------------------------

typedef struct {
	lv_obj_t *card;
	lv_obj_t *toggle;
	lv_obj_t *pills;
} toggle_pills_t;

static void toggle_pills_expanded(const toggle_pills_t *tp, bool expanded) {
	if (!tp || !tp->pills) {
		return;
	}
	if (expanded) {
		lv_obj_remove_flag(tp->pills, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(tp->pills, LV_OBJ_FLAG_HIDDEN);
	}
}

static void build_toggle_pills(lv_obj_t *parent, const char *title, lv_event_cb_t toggle_cb, toggle_pills_t *out) {
	lv_obj_t *card = lv_obj_create(parent);
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

	// The name and the switch share the top line.
	lv_obj_t *head = lv_obj_create(card);
	lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(head, 0, 0);
	lv_obj_set_style_border_width(head, 0, 0);
	lv_obj_set_style_pad_all(head, 0, 0);
	lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(head, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *name = lv_label_create(head);
	lv_label_set_text(name, tr(title));
	lv_obj_add_style(name, &theme_style_text, 0);
	lv_obj_set_style_text_font(name, &font_ui_24, 0);

	lv_obj_t *toggle = lv_switch_create(head);
	lv_obj_set_size(toggle, 68, 36);
	lv_obj_add_style(toggle, &theme_style_switch, LV_PART_MAIN);
	lv_obj_add_style(toggle, &theme_style_switch_checked, LV_PART_INDICATOR | LV_STATE_CHECKED);
	lv_obj_add_event_cb(toggle, toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);

	lv_obj_t *pills = lv_obj_create(card);
	lv_obj_set_size(pills, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(pills, 0, 0);
	lv_obj_set_style_border_width(pills, 0, 0);
	lv_obj_set_style_pad_all(pills, 0, 0);
	lv_obj_set_style_pad_gap(pills, 10, 0);
	lv_obj_remove_flag(pills, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(pills, LV_OBJ_FLAG_EVENT_BUBBLE);
	// Wrapping, because the four minute pills plus "end of chapter" are more
	// than one line of this screen.
	lv_obj_set_flex_flow(pills, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(pills, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

	out->card = card;
	out->toggle = toggle;
	out->pills = pills;
}

static lv_obj_t *make_pill(lv_obj_t *parent, const char *text, int value, lv_event_cb_t cb) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, LV_SIZE_CONTENT, 56);
	lv_obj_set_style_pad_hor(btn, 18, 0);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0); // Adwaita pill button
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)(intptr_t)value);

	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, tr(text));
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_obj_center(label);

	return btn;
}

// --- the three options ---

#define REWIND_CHOICES 4

static lv_obj_t *stop_chapter_switch;
static lv_obj_t *duration_total_pill;
static lv_obj_t *duration_chapter_pill;
static settingsrow_duration_t sleep_row;
static toggle_pills_t rewind_card;
static lv_obj_t *rewind_pill[REWIND_CHOICES];
static const int REWIND_VALUES[REWIND_CHOICES] = {3, 5, 7, 10};

static void refresh_settings_page(void) {
	if (!stop_chapter_switch) {
		return;
	}

	bool per_chapter = audiobook_duration_per_chapter();
	settingsrow_pill_active(duration_total_pill, !per_chapter);
	settingsrow_pill_active(duration_chapter_pill, per_chapter);

	if (audiobook_stop_at_chapter_end()) {
		lv_obj_add_state(stop_chapter_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(stop_chapter_switch, LV_STATE_CHECKED);
	}

	settingsrow_duration_expanded(&sleep_row, sleeptimer_enabled(SLEEPTIMER_AUDIOBOOK));
	settingsrow_duration_repaint(&sleep_row);

	bool rewind_on = audiobook_rewind_enabled();
	if (rewind_on) {
		lv_obj_add_state(rewind_card.toggle, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(rewind_card.toggle, LV_STATE_CHECKED);
	}
	toggle_pills_expanded(&rewind_card, rewind_on);

	int seconds = audiobook_rewind_seconds();
	for (int i = 0; i < REWIND_CHOICES; i++) {
		paint_choice(rewind_pill[i], REWIND_VALUES[i] == seconds);
	}
}

static void duration_pick_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	audiobook_set_duration_per_chapter(lv_event_get_user_data(e) != NULL);
	config_save();
	refresh_settings_page();
}

static void stop_chapter_cb(lv_event_t *e) {
	audiobook_set_stop_at_chapter_end(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void sleep_toggle_cb(lv_event_t *e) {
	(void)e;
	sleeptimer_set_enabled(SLEEPTIMER_AUDIOBOOK, lv_obj_has_state(sleep_row.toggle, LV_STATE_CHECKED));
	refresh_settings_page();
}

static void sleep_wheel_cb(lv_event_t *e) {
	(void)e;
	sleeptimer_set_minutes(SLEEPTIMER_AUDIOBOOK, settingsrow_duration_minutes(&sleep_row));
}

static void rewind_toggle_cb(lv_event_t *e) {
	audiobook_set_rewind_enabled(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
	refresh_settings_page();
}

static void rewind_pick_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	audiobook_set_rewind_seconds((int)(intptr_t)lv_event_get_user_data(e));
	refresh_settings_page();
}

static void settings_loaded_cb(lv_event_t *e) {
	(void)e;
	refresh_settings_page();
}

static void build_settings_page(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(audiobooksettings_screen, cfg, "audiobooks");
	settingsrow_add(container, "change_controls", NULL, switch_screen_cb, audiobookcontrols_screen);

	// What the progress bar on the player stands for: the whole book, or the
	// chapter being listened to. Only a book with chapter marks is affected.
	lv_obj_t *duration_pills;
	settingsrow_pills(container, "audiobook_show_duration", &duration_pills);
	duration_total_pill = settingsrow_pill(duration_pills, "audiobook_duration_total", 0, duration_pick_cb);
	duration_chapter_pill = settingsrow_pill(duration_pills, "audiobook_duration_chapter", 1, duration_pick_cb);

	settingsrow_toggle(container, "audiobook_stop_at_end_of_chapter", &stop_chapter_switch, stop_chapter_cb);

	// The same wheels as the music page. "End of chapter" is not among the
	// choices here -- it is not a length, and the row above is the switch for
	// it.
	settingsrow_toggle_duration(container, "sleep_timer", sleep_toggle_cb, sleep_wheel_cb, &sleep_row);
	settingsrow_duration_set_minutes(&sleep_row, sleeptimer_minutes(SLEEPTIMER_AUDIOBOOK));

	build_toggle_pills(container, "audiobook_rewind_after_pause", rewind_toggle_cb, &rewind_card);
	static const char *const REWIND_LABELS[REWIND_CHOICES] = {"audiobook_3_sec", "audiobook_5_sec", "audiobook_7_sec", "audiobook_10_sec"};
	for (int i = 0; i < REWIND_CHOICES; i++) {
		rewind_pill[i] = make_pill(rewind_card.pills, REWIND_LABELS[i], REWIND_VALUES[i], rewind_pick_cb);
	}

	// An action, not a page: it asks before it starts, so no chevron.
	settingsrow_action(container, "audiobook_scan_audiobooks_2", scan_row_cb, NULL);
	// Said on the row itself: a scan that finds nothing because the books are
	// somewhere else looks like a scan that is broken.
	option_note(container, "audiobook_folder_note");

	refresh_settings_page();
	theme_register_refresh(refresh_settings_page);
	lv_obj_add_event_cb(audiobooksettings_screen, settings_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}

// ---------------------------------------------------------------------------
// Change controls: how far each of the two transport buttons jumps
//
// Two settings, not one, and that is the point of the page: going back thirty
// seconds to catch a sentence again and going forward ten to step over a pause
// are different sized moves, and there is no reason the two buttons should
// have to agree. -10 with +30 is a perfectly sensible pair.
// ---------------------------------------------------------------------------

#define SKIP_CHOICES 3

static lv_obj_t *back_choice[SKIP_CHOICES];
static lv_obj_t *forward_choice[SKIP_CHOICES];
static const int SKIP_VALUES[SKIP_CHOICES] = {AUDIOBOOK_SKIP_SHORT, AUDIOBOOK_SKIP_LONG, AUDIOBOOK_SKIP_HUGE};

static void paint_choice(lv_obj_t *btn, bool on) {
	if (!btn) {
		return;
	}
	lv_obj_set_style_bg_color(btn, on ? theme()->accent : theme()->surface_pressed, 0);
	lv_obj_set_style_text_color(lv_obj_get_child(btn, 0), on ? lv_color_white() : theme()->text_primary, 0);
}

static void refresh_control_buttons(void) {
	int back = audiobook_skip_back();
	int forward = audiobook_skip_forward();

	for (int i = 0; i < SKIP_CHOICES; i++) {
		paint_choice(back_choice[i], SKIP_VALUES[i] == back);
		paint_choice(forward_choice[i], SKIP_VALUES[i] == forward);
	}
}

static void pick_back_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	audiobook_set_skip_back((int)(intptr_t)lv_event_get_user_data(e));
	refresh_control_buttons();
	player_refresh_now_playing(); // the button it names changes glyph
}

static void pick_forward_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	audiobook_set_skip_forward((int)(intptr_t)lv_event_get_user_data(e));
	refresh_control_buttons();
	player_refresh_now_playing();
}

static lv_obj_t *make_skip_choice(lv_obj_t *parent, const char *text, int seconds, lv_event_cb_t cb) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, LV_SIZE_CONTENT, 64);
	// Three across the card instead of two, so tighter than the Appearance
	// page's pair: enough that -60 does not fall off the right edge.
	lv_obj_set_style_pad_hor(btn, 22, 0);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0); // Adwaita pill button
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)(intptr_t)seconds);

	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, tr(text));
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	lv_obj_center(label);

	return btn;
}

// One card per button: its name, then its pills. The same shape as the
// Appearance page's theme card, because it is the same kind of choice.
static lv_obj_t *make_control_card(lv_obj_t *parent, const char *title) {
	lv_obj_t *card = lv_obj_create(parent);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 12, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, 20, 0);
	lv_obj_set_style_pad_gap(card, 18, 0);
	lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_obj_t *name = lv_label_create(card);
	lv_label_set_text(name, tr(title));
	lv_obj_add_style(name, &theme_style_text, 0);
	lv_obj_set_style_text_font(name, &font_ui_24, 0);

	lv_obj_t *row = lv_obj_create(card);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(row, 0, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_set_style_pad_gap(row, 14, 0);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	return row;
}

static void controls_loaded_cb(lv_event_t *e) {
	(void)e;
	refresh_control_buttons();
}

static void build_controls_page(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(audiobookcontrols_screen, cfg, "change_controls");

	lv_obj_t *back_row = make_control_card(container, "back_button");
	back_choice[0] = make_skip_choice(back_row, "-10", AUDIOBOOK_SKIP_SHORT, pick_back_cb);
	back_choice[1] = make_skip_choice(back_row, "-30", AUDIOBOOK_SKIP_LONG, pick_back_cb);
	back_choice[2] = make_skip_choice(back_row, "-60", AUDIOBOOK_SKIP_HUGE, pick_back_cb);

	lv_obj_t *forward_row = make_control_card(container, "forward_button");
	forward_choice[0] = make_skip_choice(forward_row, "+10", AUDIOBOOK_SKIP_SHORT, pick_forward_cb);
	forward_choice[1] = make_skip_choice(forward_row, "+30", AUDIOBOOK_SKIP_LONG, pick_forward_cb);
	forward_choice[2] = make_skip_choice(forward_row, "+60", AUDIOBOOK_SKIP_HUGE, pick_forward_cb);

	refresh_control_buttons();
	theme_register_refresh(refresh_control_buttons);
	lv_obj_add_event_cb(audiobookcontrols_screen, controls_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}

// ---------------------------------------------------------------------------
// the scan page: the music scan's layout with its own icon and words
// ---------------------------------------------------------------------------

#define SCAN_POLL_MS 200

static lv_obj_t *scan_count_label;
static lv_obj_t *scan_status_label;
static lv_obj_t *scan_ok_button;
static lv_obj_t *scan_cancel_button;
static lv_timer_t *scan_poll_timer;
// The one folder audiobooks are read from, at the root of the card.
#define AUDIOBOOK_FOLDER "Audiobooks"

static char scan_root[512];

static void scan_show_finished(int found) {
	lv_label_set_text_fmt(scan_count_label, "%d", found);
	lv_label_set_text(scan_status_label, found == 1 ? tr("audiobook_found") : tr("audiobook_found_count"));

	lv_obj_add_flag(scan_cancel_button, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag(scan_ok_button, LV_OBJ_FLAG_HIDDEN);
	power_hold_screen_on(false);
}

static void scan_poll_cb(lv_timer_t *timer) {
	int found = audiobookdb_scan_found();
	lv_label_set_text_fmt(scan_count_label, "%d", found);

	if (audiobookdb_scan_running()) {
		return;
	}

	lv_timer_pause(timer);
	scan_show_finished(found);
}

static void scan_ok_cb(lv_event_t *e) {
	(void)e;
	switch_screen(audiobooks_screen);
	// The way here was menu -> Audiobooks -> options -> scan, and walking back
	// through a finished scan would only confuse. From the list, back now means
	// the menu.
	screen_history_reset();
}

static void scan_cancel_cb(lv_event_t *e) {
	(void)e;
	audiobookdb_scan_stop();
	lv_timer_pause(scan_poll_timer);
	power_hold_screen_on(false);
	switch_screen(audiobooks_screen);
	screen_history_reset();
}

static void scan_begin(void) {
	lv_label_set_text(scan_count_label, "0");
	lv_label_set_text(scan_status_label, tr("audiobook_found_count"));
	lv_obj_add_flag(scan_ok_button, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag(scan_cancel_button, LV_OBJ_FLAG_HIDDEN);

	if (!audiobookdb_scan_start(scan_root)) {
		lv_label_set_text(scan_status_label, tr("no_card_to_scan"));
		lv_obj_add_flag(scan_cancel_button, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(scan_ok_button, LV_OBJ_FLAG_HIDDEN);
		return;
	}

	power_hold_screen_on(true);
	lv_timer_reset(scan_poll_timer);
	lv_timer_resume(scan_poll_timer);
}

// Same contract as the music scan: minutes of work, so it asks first.
static void start_scan(void *user) {
	(void)user;
	switch_screen(audiobookscan_screen);
	scan_begin();
}

static void scan_row_cb(lv_event_t *e) {
	(void)e;
	confirm_show("audiobook_scan_audiobooks",
				 "audiobook_rescan_confirm_note",
				 "scan", start_scan, NULL);
}

static lv_obj_t *scan_make_button(const char *text, lv_color_t colour, lv_event_cb_t cb, gui_config_t *cfg) {
	lv_obj_t *button = lv_btn_create(audiobookscan_screen);
	lv_obj_set_size(button, 240, 68);
	lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, -(cfg->padding * 2));
	lv_obj_add_style(button, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_bg_color(button, colour, 0);
	lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *label = lv_label_create(button);
	lv_label_set_text(label, tr(text));
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	lv_obj_set_style_text_color(label, lv_color_white(), 0);
	lv_obj_center(label);

	return button;
}

static void build_scan_page(gui_config_t *cfg) {
	// The folder and not the whole card: see the note in audiobookdb.c. Built
	// once here so the scan and the line under the button cannot disagree about
	// where books live.
	snprintf(scan_root, sizeof(scan_root), "%s/%s", cfg->sd_root_path ? cfg->sd_root_path : "", AUDIOBOOK_FOLDER);

	lv_obj_add_style(audiobookscan_screen, &theme_style_screen, 0);

	lv_obj_t *title = settingsrow_title(audiobookscan_screen, cfg, "scan_2");
	lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

	int content_top = settingsrow_content_top(cfg);

	lv_obj_t *container = lv_obj_create(audiobookscan_screen);
	lv_obj_set_size(container, lv_pct(100), cfg->screen_height - content_top);
	lv_obj_align(container, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(container, 0, 0);
	lv_obj_set_style_border_width(container, 0, 0);
	lv_obj_set_style_radius(container, 0, 0);
	lv_obj_set_style_pad_all(container, cfg->padding, 0);
	lv_obj_set_style_pad_gap(container, 10, 0);
	lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *book = lv_image_create(container);
	lv_image_set_src(book, &icon_book_headphones);
	lv_obj_add_style(book, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(book, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_bottom(book, 16, 0);

	scan_count_label = lv_label_create(container);
	lv_label_set_text(scan_count_label, "0");
	lv_obj_set_style_text_color(scan_count_label, theme()->accent, 0);
	lv_obj_set_style_text_font(scan_count_label, &font_ui_32, 0);

	scan_status_label = lv_label_create(container);
	lv_label_set_text(scan_status_label, tr("audiobook_found_count"));
	lv_obj_add_style(scan_status_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(scan_status_label, &font_ui_26, 0);

	scan_cancel_button = scan_make_button("cancel", lv_color_make(210, 66, 58), scan_cancel_cb, cfg);
	scan_ok_button = scan_make_button("ok", theme()->accent, scan_ok_cb, cfg);
	lv_obj_add_flag(scan_ok_button, LV_OBJ_FLAG_HIDDEN);

	scan_poll_timer = lv_timer_create(scan_poll_cb, SCAN_POLL_MS, NULL);
	lv_timer_pause(scan_poll_timer);
}

void audiobooks_init(gui_config_t *cfg) {
	build_books_page(cfg);
	build_settings_page(cfg);
	build_controls_page(cfg);
	build_scan_page(cfg);
}

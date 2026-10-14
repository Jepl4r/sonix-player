#include "audiobooks.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/library/audiobookextras.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/sx_card.h"
#include "src/gui/shell/sx_frame.h"
#include "src/gui/shell/sx_palette.h"
#include "src/gui/shell/sx_row.h"
#include "src/gui/shell/sx_sheet.h"
#include "src/gui/shell/sx_styles.h"
#include "src/gui/shell/sx_toast.h"
#include "src/gui/shell/sx_tokens.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"
#include "src/system/device/usb.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/library/library.h"
#include "src/system/playback/audiobook.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/playback/sleeptimer.h"

lv_obj_t *audiobooks_screen;
lv_obj_t *audiobooksettings_screen;
lv_obj_t *audiobookscan_screen;
lv_obj_t *audiobookcontrols_screen;

// The two list pages the tiles lead to: books, and authors or series.
static lv_obj_t *books_screen;
static lv_obj_t *names_screen;

static void start_scan(void *user);
static void scan_stop_cb(lv_event_t *e);

// ---------------------------------------------------------------------------
// Which list a page shows, and the order it is read in
//
// Each list keeps its own order, under [audiobook] in the config, and offers
// only the orders that mean something for it.
// ---------------------------------------------------------------------------

typedef enum {
	LIST_LIBRARY,
	LIST_CONTINUE,
	LIST_FINISHED,
	LIST_AUTHOR_BOOKS,
	LIST_SERIES_BOOKS,
	LIST_AUTHORS,
	LIST_SERIES,
	LIST_COUNT,
} list_id_t;

typedef enum {
	SORT_AZ,
	SORT_ZA,
	SORT_NEW,
	SORT_OLD,
	SORT_PLAYED,
	SORT_SERIES,
} sort_choice_t;

typedef struct {
	const char *config_key;
	sort_choice_t fallback;
	int choice_count;
	sort_choice_t choices[6];
} list_sorts_t;

static const list_sorts_t LIST_SORTS[LIST_COUNT] = {
	[LIST_LIBRARY] = {"sort_library", SORT_AZ, 5, {SORT_AZ, SORT_ZA, SORT_NEW, SORT_OLD, SORT_PLAYED}}, [LIST_CONTINUE] = {"sort_continue", SORT_PLAYED, 3, {SORT_PLAYED, SORT_AZ, SORT_ZA}}, [LIST_FINISHED] = {"sort_finished", SORT_NEW, 4, {SORT_AZ, SORT_ZA, SORT_NEW, SORT_OLD}}, [LIST_AUTHOR_BOOKS] = {"sort_author_books", SORT_AZ, 4, {SORT_AZ, SORT_ZA, SORT_NEW, SORT_OLD}}, [LIST_SERIES_BOOKS] = {"sort_series_books", SORT_SERIES, 5, {SORT_SERIES, SORT_AZ, SORT_ZA, SORT_NEW, SORT_OLD}}, [LIST_AUTHORS] = {"sort_authors", SORT_AZ, 4, {SORT_AZ, SORT_ZA, SORT_NEW, SORT_OLD}}, [LIST_SERIES] = {"sort_series", SORT_AZ, 4, {SORT_AZ, SORT_ZA, SORT_NEW, SORT_OLD}},
};

static const char *const SORT_LABELS[] = {
	[SORT_AZ] = "medialist_sort_name_az", [SORT_ZA] = "medialist_sort_name_za", [SORT_NEW] = "medialist_sort_added_new", [SORT_OLD] = "medialist_sort_added_old", [SORT_PLAYED] = "audiobook_sort_played", [SORT_SERIES] = "audiobook_sort_series",
};

static sort_choice_t list_sort(list_id_t list) {
	const list_sorts_t *sorts = &LIST_SORTS[list];
	long value = config_get_int("audiobook", sorts->config_key, sorts->fallback);
	for (int i = 0; i < sorts->choice_count; i++) {
		if (sorts->choices[i] == value) {
			return (sort_choice_t)value;
		}
	}
	return sorts->fallback;
}

static const lv_image_dsc_t *sort_icon(sort_choice_t choice) {
	switch (choice) {
	case SORT_ZA:
		return &icon_sort_za;
	case SORT_NEW:
		return &icon_sort_date_new;
	case SORT_OLD:
		return &icon_sort_date_old;
	case SORT_PLAYED:
		return &icon_sort_played;
	case SORT_SERIES:
		return &icon_sort_series;
	default:
		return &icon_sort_az;
	}
}

static void sort_to_order(sort_choice_t choice, audiobook_order_t *order, bool *desc) {
	*desc = choice == SORT_ZA || choice == SORT_NEW || choice == SORT_PLAYED;
	*order = choice == SORT_NEW || choice == SORT_OLD ? AUDIOBOOK_ORDER_ADDED : choice == SORT_PLAYED ? AUDIOBOOK_ORDER_PLAYED : choice == SORT_SERIES ? AUDIOBOOK_ORDER_SERIES : AUDIOBOOK_ORDER_NAME;
}

// The sort button's menu. `reload` puts the page back together in the new
// order.
static list_id_t sort_menu_list;
static void (*sort_menu_reload)(void);

static void sort_picked(void *user) {
	config_set_int("audiobook", LIST_SORTS[sort_menu_list].config_key, (long)(intptr_t)user);
	config_save();
	if (sort_menu_reload) {
		sort_menu_reload();
	}
}

static void sort_menu_show(lv_obj_t *anchor, list_id_t list, void (*reload)(void)) {
	const list_sorts_t *sorts = &LIST_SORTS[list];
	sort_choice_t current = list_sort(list);
	popover_item_t items[6];
	for (int i = 0; i < sorts->choice_count; i++) {
		sort_choice_t choice = sorts->choices[i];
		items[i] = (popover_item_t){SORT_LABELS[choice], sort_picked, (void *)(intptr_t)choice, choice == current};
	}
	sort_menu_list = list;
	sort_menu_reload = reload;
	popover_show(anchor, items, sorts->choice_count);
}

// A glyph button on the title row, counted from the right edge. 56 x 56 at the
// header's height: touch_header() makes it the 2.0 header's round 72 px target
// when the page is shown.
static lv_obj_t *corner_button(lv_obj_t *screen, gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph, lv_event_cb_t cb) {
	lv_obj_t *button = lv_btn_create(screen);
	lv_obj_set_size(button, 56, 56);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -cfg->padding - slot * (56 + 6), cfg->padding + cfg->top_bar_height);
	if (cb) {
		lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);
	}

	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, glyph);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_center(icon);
	return button;
}

// The viewport of a windowed list: a scrollable column under the header with,
// in it, an optional section heading and a fixed-height canvas the pool of
// rows is placed on, so twelve row widgets stand in for however many entries.
static lv_obj_t *make_viewport(lv_obj_t *screen, gui_config_t *cfg, lv_obj_t **body_out, lv_obj_t **empty_out, lv_event_cb_t scroll_cb, void *user) {
	int content_top = settingsrow_content_top(cfg);

	lv_obj_t *list = lv_obj_create(screen);
	lv_obj_remove_style_all(list);
	lv_obj_set_size(list, lv_pct(100), cfg->screen_height - content_top);
	lv_obj_align(list, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_pad_hor(list, cfg->padding, 0);
	lv_obj_set_style_pad_row(list, 6, 0);
	lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scroll_dir(list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_add_event_cb(list, scroll_cb, LV_EVENT_SCROLL, user);
	// Presses on the rows and on the empty area bubble into the gesture
	// handlers; set here because an empty list binds no row to set it later.
	lv_obj_set_event_bubble(list, true);

	lv_obj_t *body = lv_obj_create(list);
	lv_obj_remove_style_all(body);
	lv_obj_set_width(body, lv_pct(100));
	lv_obj_set_height(body, 1);
	lv_obj_set_scrollable(body, false);
	lv_obj_set_event_bubble(body, true);

	lv_obj_t *empty = lv_label_create(list);
	lv_obj_set_width(empty, lv_pct(100));
	lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(empty, &sx_style_text2, 0);
	lv_obj_set_style_text_font(empty, &font_ui_22, 0);
	lv_obj_set_style_pad_top(empty, 80, 0);
	lv_obj_set_hidden(empty, true);

	player_sheet_attach_drag(list, true);
	switcher_attach_back_gesture(list);

	*body_out = body;
	*empty_out = empty;
	return list;
}

// ---------------------------------------------------------------------------
// A list of books
//
// 2.0: the books are sx_rows -- the cover's thumbnail at the left (a plaque
// with the title's first letter in the title's tone until there is one, and
// for a book without a picture), the title, the author under it, the playing
// book on the surface with its title in the accent. A tap plays the book from
// where it was left (the handoff's matrix: a book plays, it has no swipes and
// no sheet).
//
// Two of these exist: the section page's own -- every book, the page the menu
// opens on -- and the page its sheet opens for the rest (continue listening,
// the finished books, an author's books, a series). They share the twelve
// cover slots the coverloader keeps for the books (60..71): only the one on
// screen asks for pictures, and the other lets go of what it was waiting for
// as it leaves (pictures it already holds it keeps).
//
// The data is windowed with the widgets: a list holds a handle -- four bytes a
// book -- and a band of forty-eight rows read back as the viewport moves. The
// author is read for a row the first time it is shown (one indexed query),
// not for the whole band.
// ---------------------------------------------------------------------------

#define ROW_POOL 12

// How many rows are read from the database at a time. Four times the pool, so
// scrolling a screenful does not go back to the database.
#define WINDOW_ROWS 48

#define THUMB_POLL_MS 150

typedef struct {
	char name[256];
	char path[512];
	char author[128];
	bool author_read;
} winrow_t;

typedef struct booklist booklist_t;

typedef struct {
	lv_obj_t *obj; // the sx_row
	booklist_t *owner;
	int index;

	bool has_thumb;
	bool thumb_requested;
	bool thumb_settled;
	cover_image_t thumb;
} row_t;

struct booklist {
	lv_obj_t *screen;
	lv_obj_t *title;
	lv_obj_t *sort_icon;
	lv_obj_t *viewport; // scrolls
	lv_obj_t *group;	// "BOOKS · 3"; the section page's only
	lv_obj_t *body;		// the fixed-height canvas the rows are placed on
	lv_obj_t *empty;	// what an empty list says
	row_t rows[ROW_POOL];

	audiobookdb_index_t *index;
	int count;
	winrow_t *window; // WINDOW_ROWS of them, allocated on first use
	int window_first;
	int window_count;

	int row_height; // sx_row_height() when the rows were last sized
	int lead;		// the thumbnail's side at that height

	// What it shows: the list, and the author or series for the two lists
	// that are one of those.
	list_id_t list;
	char value[256];
};

static booklist_t shelf; // the section page: every book
static booklist_t other; // continue, finished, an author's, a series'
static lv_timer_t *thumb_timer;
static char np_book[512]; // the book playing now, for the mark

static int row_slot(const row_t *row) { return COVERLOADER_AUDIOBOOKS_BASE + (int)(row - row->owner->rows); }
static int list_pitch(const booklist_t *bl) { return bl->row_height + SX_ROW_GAP; }

static bool row_at(booklist_t *bl, int index, winrow_t **out);

static audiobook_list_t list_kind(list_id_t list) {
	switch (list) {
	case LIST_FINISHED:
		return AUDIOBOOK_LIST_FINISHED;
	case LIST_CONTINUE:
		return AUDIOBOOK_LIST_CONTINUE;
	case LIST_AUTHOR_BOOKS:
		return AUDIOBOOK_LIST_AUTHOR;
	case LIST_SERIES_BOOKS:
		return AUDIOBOOK_LIST_SERIES;
	default:
		return AUDIOBOOK_LIST_ALL;
	}
}

static audiobookdb_index_t *list_index_open(const booklist_t *bl) {
	audiobook_order_t order;
	bool desc;
	sort_to_order(list_sort(bl->list), &order, &desc);
	return audiobookdb_index_open(list_kind(bl->list), bl->value, order, desc);
}

// The plaque a row stands on until its picture comes, and for good when the
// book has none: the title's first letter on the title's own tone, the way the
// genre and artist lists show theirs.
static void row_show_plaque_as(row_t *row, const char *name, bool with_letter) {
	char letter[8] = "";
	if (with_letter && name && name[0]) {
		// One character, which in UTF-8 is up to four bytes.
		int n = 1;
		unsigned char c = (unsigned char)name[0];
		if (c >= 0xF0) {
			n = 4;
		} else if (c >= 0xE0) {
			n = 3;
		} else if (c >= 0xC0) {
			n = 2;
		}
		for (int i = 0; i < n && name[i]; i++) {
			letter[i] = name[i];
		}
	}
	sx_row_set_letter(row->obj, letter, sx_tone(SX_T_COVER_TONE, sx_palette_hue_of_name(name ? name : "")), false);
}

static void row_show_plaque(row_t *row, const char *name) { row_show_plaque_as(row, name, true); }

static void row_drop_thumb(row_t *row) {
	coverloader_release(row_slot(row));

	if (row->has_thumb) {
		// Stop pointing at the pixels before they go.
		winrow_t *w = NULL;
		row_show_plaque(row, row_at(row->owner, row->index, &w) ? w->name : "");
		cover_free(&row->thumb);
		row->has_thumb = false;
	}
	row->thumb_requested = false;
	row->thumb_settled = false;
}

static void row_update_playing(row_t *row) {
	winrow_t *w = NULL;
	bool there = row_at(row->owner, row->index, &w);
	bool playing = np_book[0] && there && w->path[0] && strcmp(w->path, np_book) == 0;
	// On a plaque the bars stand alone: the letter under them would read as
	// a smudge.
	if (there && !row->has_thumb) {
		row_show_plaque_as(row, w->name, !playing);
	}
	sx_row_set_playing(row->obj, playing);
}

// The mark follows the book, so every file of a folder book lights its row.
static void refresh_playmarks(booklist_t *bl) {
	device_state_t state;
	device_state_get(&state);
	np_book[0] = '\0';
	if (!state.live && state.current_file[0]) {
		audiobookdb_book_for_file(state.current_file, np_book, sizeof(np_book));
	}

	for (int i = 0; i < ROW_POOL; i++) {
		row_update_playing(&bl->rows[i]);
	}
}

static void row_bind(row_t *row, int index) {
	if (row->index == index) {
		return;
	}

	row_drop_thumb(row);
	row->index = index;

	booklist_t *bl = row->owner;
	winrow_t *w = NULL;
	if (index < 0 || index >= bl->count || !row_at(bl, index, &w)) {
		// Past the end, or the handle went stale under the list: hiding it is
		// what the next window_update() undoes, once the list is rebuilt.
		lv_obj_set_hidden(row->obj, true);
		return;
	}

	if (!w->author_read) {
		audiobookdb_book_info(w->path, NULL, 0, w->author, sizeof(w->author), NULL);
		w->author_read = true;
	}

	lv_obj_set_hidden(row->obj, false);
	lv_obj_set_y(row->obj, index * list_pitch(bl));
	sx_row_set_title(row->obj, w->name);
	sx_row_set_subtitle(row->obj, w->author[0] ? w->author : NULL, NULL, NULL);
	row_show_plaque(row, w->name);
	row_update_playing(row);
}

static bool first_part_cb(const char *path, const char *title, void *user) {
	(void)title;
	snprintf(user, 512, "%s", path);
	return false;
}

// Where a book's cover is looked for: its own file, or the first file of a
// folder book -- the picture inside it, else the cover image beside it.
static const char *cover_source(const char *book, char *first, size_t size) {
	if (size < 512 || !audiobookdb_is_folder_book(book)) {
		return book;
	}
	first[0] = '\0';
	audiobookdb_parts_for_each(book, first_part_cb, first);
	return first[0] ? first : book;
}

// Asks for the artwork of the visible rows and collects what the worker has
// finished. Only for the list on screen: the two share the slots.
static void thumbs_update(booklist_t *bl) {
	if (lv_screen_active() != bl->screen || lv_obj_is_hidden(bl->viewport)) {
		return;
	}
	bool anything_pending = false;

	for (int i = 0; i < ROW_POOL; i++) {
		row_t *row = &bl->rows[i];
		if (row->index < 0 || row->index >= bl->count || row->thumb_settled) {
			continue;
		}

		winrow_t *w = NULL;
		if (!row_at(bl, row->index, &w) || !w->path[0]) {
			row->thumb_settled = true;
			continue;
		}

		if (!row->thumb_requested) {
			char first[512];
			coverloader_request(row_slot(row), cover_source(w->path, first, sizeof(first)), bl->lead);
			row->thumb_requested = true;
		}

		bool finished = false;
		cover_image_t image;
		if (coverloader_take(row_slot(row), &image, &finished)) {
			row->thumb = image;
			row->has_thumb = true;
			row->thumb_settled = true;
			sx_row_set_thumb(row->obj, &row->thumb.dsc, sx_tone(SX_T_COVER_TONE, sx_palette_hue_of_name(w->name)));
		} else if (finished) {
			row->thumb_settled = true; // no jacket in this one; the plaque stays
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
	if (lv_screen_active() == shelf.screen) {
		thumbs_update(&shelf);
	} else if (lv_screen_active() == other.screen) {
		thumbs_update(&other);
	}
}

// Leaving the page: what the coverloader was still decoding for it is let go,
// so the other list can have the slots. Pictures already on rows stay.
static void list_let_go(booklist_t *bl) {
	for (int i = 0; i < ROW_POOL; i++) {
		row_t *row = &bl->rows[i];
		if (row->thumb_requested && !row->has_thumb) {
			coverloader_release(row_slot(row));
			row->thumb_requested = false;
			row->thumb_settled = false;
		}
	}
}

static void list_unload_cb(lv_event_t *e) { list_let_go(lv_event_get_user_data(e)); }

// The rows' height follows Appearance > Large rows, read as the list is
// (re)loaded; pictures of the other size are dropped with it.
static void list_resize(booklist_t *bl) {
	int height = sx_row_height(false);
	if (height == bl->row_height) {
		return;
	}
	bl->row_height = height;
	bl->lead = height >= SX_ROW_H_BIG ? SX_ROW_THUMB_BIG : SX_ROW_THUMB;
	for (int i = 0; i < ROW_POOL; i++) {
		row_drop_thumb(&bl->rows[i]);
		sx_row_set_compact(bl->rows[i].obj, false);
		bl->rows[i].index = -2; // placed again at the new pitch
	}
}

static void list_set_count(booklist_t *bl, int count) {
	bl->count = count;
	lv_obj_set_height(bl->body, count > 0 ? count * list_pitch(bl) : 1);
	lv_obj_set_hidden(bl->empty, count != 0);
	if (bl->group) {
		lv_obj_set_hidden(bl->group, count == 0);
		settingsrow_group_set(bl->group, "audiobook_books_group", count);
	}
}

// A rescan, or a card change, or a book marked finished: the rows the handle
// names have moved, so the list is rebuilt where it stands.
static void refresh_stale(booklist_t *bl) {
	if (!bl->index || !audiobookdb_index_stale(bl->index)) {
		return;
	}
	audiobookdb_index_t *fresh = list_index_open(bl);
	audiobookdb_index_close(bl->index);
	bl->index = fresh;
	bl->window_first = -1;
	bl->window_count = 0;

	for (int i = 0; i < ROW_POOL; i++) {
		row_drop_thumb(&bl->rows[i]);
		bl->rows[i].index = -2; // every row now stands for a different book
	}
	list_set_count(bl, audiobookdb_index_count(fresh));
}

static void window_update(booklist_t *bl) {
	refresh_stale(bl);
	if (bl->count == 0) {
		for (int i = 0; i < ROW_POOL; i++) {
			row_bind(&bl->rows[i], -1);
		}
		return;
	}

	// The canvas starts under the heading, so the scroll is measured from it.
	int scroll = lv_obj_get_scroll_y(bl->viewport) - lv_obj_get_y(bl->body);
	if (scroll < 0) {
		scroll = 0;
	}

	int first = (scroll / list_pitch(bl)) - 1;
	if (first + ROW_POOL > bl->count) {
		first = bl->count - ROW_POOL;
	}
	if (first < 0) {
		first = 0;
	}

	// index -> widget index % ROW_POOL, so a one-step slide of the band
	// rebinds exactly one row instead of renaming all twelve.
	for (int i = 0; i < ROW_POOL; i++) {
		int index = first + i;
		row_bind(&bl->rows[index % ROW_POOL], index < bl->count ? index : -1);
	}

	thumbs_update(bl);
}

static void list_scroll_cb(lv_event_t *e) { window_update(lv_event_get_user_data(e)); }

static bool window_fill_cb(const char *name, const char *path, void *user) {
	booklist_t *bl = user;
	if (bl->window_count >= WINDOW_ROWS) {
		return false;
	}
	winrow_t *row = &bl->window[bl->window_count];
	snprintf(row->name, sizeof(row->name), "%s", name ? name : "");
	snprintf(row->path, sizeof(row->path), "%s", path ? path : "");
	row->author[0] = '\0';
	row->author_read = false;
	bl->window_count++;
	return true;
}

// Brings `index` into the band, reading a windowful around it. The band starts
// a pool's worth before the wanted row, so scrolling back up a little does not
// send it to the database again.
static bool window_cover(booklist_t *bl, int index) {
	if (!bl->index || index < 0 || index >= bl->count) {
		return false;
	}
	if (!bl->window) {
		bl->window = calloc(WINDOW_ROWS, sizeof(*bl->window));
		if (!bl->window) {
			return false;
		}
	}
	if (bl->window_first >= 0 && index >= bl->window_first && index < bl->window_first + bl->window_count) {
		return true;
	}

	int first = index - ROW_POOL;
	if (first + WINDOW_ROWS > bl->count) {
		first = bl->count - WINDOW_ROWS;
	}
	if (first < 0) {
		first = 0;
	}

	bl->window_count = 0;
	audiobookdb_index_window(bl->index, first, WINDOW_ROWS, window_fill_cb, bl);
	bl->window_first = first;
	return index >= first && index < first + bl->window_count;
}

// A book's row of the band. False when the row is not there -- a stale
// handle, or an index past the end.
static bool row_at(booklist_t *bl, int index, winrow_t **out) {
	if (index < 0 || index >= bl->count || !window_cover(bl, index)) {
		return false;
	}
	*out = &bl->window[index - bl->window_first];
	return true;
}

static const char *list_empty_text(list_id_t list) {
	switch (list) {
	case LIST_FINISHED:
		return tr("audiobook_finished_empty");
	case LIST_CONTINUE:
		return tr("audiobook_continue_empty");
	default:
		return tr("audiobook_list_empty");
	}
}

static void list_reload(booklist_t *bl) {
	list_resize(bl);
	// Unbind first, and set the index by hand afterwards: row_bind() is a no-op
	// when the index has not moved, so switching between two lists of the same
	// length would otherwise leave every row showing the old titles.
	for (int i = 0; i < ROW_POOL; i++) {
		row_bind(&bl->rows[i], -1);
		bl->rows[i].index = -1;
	}

	audiobookdb_index_close(bl->index);
	bl->index = list_index_open(bl);
	bl->window_first = -1;
	bl->window_count = 0;

	list_set_count(bl, audiobookdb_index_count(bl->index));
	lv_obj_scroll_to_y(bl->viewport, 0, LV_ANIM_OFF);
	lv_label_set_text(bl->empty, list_empty_text(bl->list));
	lv_image_set_src(bl->sort_icon, sort_icon(list_sort(bl->list)));
	lv_obj_update_layout(bl->viewport); // where the canvas starts, for the band

	refresh_playmarks(bl);
	window_update(bl);
}

// Plays a book from where it was left: a book in one file from its saved
// second, a folder book as a queue of its parts from the part and second it
// was left at.
static bool collect_part(const char *path, const char *title, void *user) {
	(void)title;
	char ***list = user;
	size_t n = 0;
	while ((*list) && (*list)[n]) {
		n++;
	}
	char **grown = realloc(*list, (n + 2) * sizeof(char *));
	if (!grown) {
		return false;
	}
	*list = grown;
	grown[n] = strdup(path);
	grown[n + 1] = NULL;
	return grown[n] != NULL;
}

// Starts `book` in `file` at `seconds`: a single-file book directly, a folder
// book as a queue of its parts. A file that is no longer one of the book's
// parts starts the book from its first, from the beginning.
static void play_from(const char *book, const char *file, double seconds) {
	if (!audiobookdb_is_folder_book(book)) {
		if (seconds > 1.0) {
			device_state_play_file_at(file, seconds);
		} else {
			device_state_play_file(file);
		}
		return;
	}

	char **list = NULL;
	audiobookdb_parts_for_each(book, collect_part, &list);
	int count = 0;
	int start = -1;
	while (list && list[count]) {
		if (start < 0 && strcmp(list[count], file) == 0) {
			start = count;
		}
		count++;
	}
	if (count > 0) {
		double at = start >= 0 && seconds > 1.0 ? seconds : -1.0;
		device_state_play_list_ordered_at((const char *const *)list, count, start >= 0 ? start : 0, at);
	}
	for (int i = 0; i < count; i++) {
		free(list[i]);
	}
	free(list);
}

static void play_book(const char *book) {
	char file[512];
	double resume = 0;
	if (!audiobook_resume_point(book, file, sizeof(file), &resume)) {
		return;
	}
	play_from(book, file, resume);

	audiobookdb_touch(book); // the order by last listened moves it to the top
	// 2.0: the page stays, as for a track, a station or an episode (the
	// handoff: a tap on a book plays it in the mini player); the player is a
	// tap on the mini player away.
	player_refresh_now_playing();
}

void audiobooks_play_at(const char *book, const char *file, double seconds) {
	if (!book || !book[0] || !file || !file[0]) {
		return;
	}
	play_from(book, file, seconds);
	// Written through: this is where the book now stands, whatever comes next.
	audiobookdb_save_position(book, file, seconds);
	audiobookdb_touch(book);
	player_refresh_now_playing();
}

static void book_tap(lv_obj_t *obj, void *user) {
	(void)obj;
	row_t *row = user;
	winrow_t *w = NULL;
	if (!row || !row_at(row->owner, row->index, &w) || !w->path[0]) {
		return;
	}
	char book[512];
	snprintf(book, sizeof(book), "%s", w->path);
	play_book(book);
	refresh_playmarks(row->owner); // the page stays: the book now playing is marked
}

// The list coming on screen: read for the first time, or brought up to date
// -- a scan, or a book reaching its end, may have happened since; Large rows
// may have been switched.
static void list_refresh(booklist_t *bl) {
	if (!bl->index || bl->row_height != sx_row_height(false)) {
		list_reload(bl);
		return;
	}
	refresh_stale(bl);
	refresh_playmarks(bl);
	window_update(bl);
}

static void list_loaded_cb(lv_event_t *e) { list_refresh(lv_event_get_user_data(e)); }

static void sort_shelf_cb(lv_event_t *e);
static void sort_other_cb(lv_event_t *e);

static void list_reload_shelf(void) { list_reload(&shelf); }
static void list_reload_other(void) { list_reload(&other); }

static void sort_shelf_cb(lv_event_t *e) { sort_menu_show(lv_event_get_current_target(e), shelf.list, list_reload_shelf); }
static void sort_other_cb(lv_event_t *e) { sort_menu_show(lv_event_get_current_target(e), other.list, list_reload_other); }

// The list's widgets on `screen`, its header buttons from `slot` leftwards.
static void list_build(booklist_t *bl, lv_obj_t *screen, gui_config_t *cfg, bool with_group, int sort_slot, lv_event_cb_t sort_cb) {
	bl->screen = screen;
	bl->window_first = -1;
	lv_obj_t *sort_btn = corner_button(screen, cfg, sort_slot, &icon_sort_az, sort_cb);
	bl->sort_icon = lv_obj_get_child(sort_btn, 0);

	bl->viewport = make_viewport(screen, cfg, &bl->body, &bl->empty, list_scroll_cb, bl);
	if (with_group) {
		bl->group = settingsrow_group(bl->viewport, "audiobook_books_group", 0);
		lv_obj_move_to_index(bl->group, 0);
		lv_obj_set_hidden(bl->group, true);
	}

	static const sx_row_handlers_t handlers = {.on_tap = book_tap};
	bl->row_height = sx_row_height(false);
	bl->lead = bl->row_height >= SX_ROW_H_BIG ? SX_ROW_THUMB_BIG : SX_ROW_THUMB;
	for (int i = 0; i < ROW_POOL; i++) {
		row_t *row = &bl->rows[i];
		row->owner = bl;
		row->obj = sx_row_create(bl->body, &handlers, false);
		sx_row_set_user(row->obj, row);
		sx_row_set_trail(row->obj, SX_ROW_TRAIL_NONE);
		lv_obj_set_hidden(row->obj, true);
		row->index = -1;
	}

	player_sheet_attach_drag(screen, true);
	switcher_attach_back_gesture(screen);
	// The section page refreshes its list itself, after deciding whether it
	// shows (section_loaded_cb).
	if (bl != &shelf) {
		lv_obj_add_event_cb(screen, list_loaded_cb, LV_EVENT_SCREEN_LOADED, bl);
	}
	lv_obj_add_event_cb(screen, list_unload_cb, LV_EVENT_SCREEN_UNLOAD_START, bl);
}

// Opens the other books page on one of its lists. `value` is the author or the
// series, `title` what the page is headed with.
static void books_open(list_id_t list, const char *value, const char *title) {
	other.list = list;
	snprintf(other.value, sizeof(other.value), "%s", value ? value : "");
	lv_label_set_text(other.title, title);
	list_reload(&other);
	switch_screen(other.screen);
}

static void build_books_page(gui_config_t *cfg) {
	lv_obj_add_style(books_screen, &theme_style_screen, 0);
	other.title = settingsrow_title(books_screen, cfg, "audiobooks");
	settingsrow_title_corner_slots(other.title, cfg, 1);
	other.list = LIST_CONTINUE;
	list_build(&other, books_screen, cfg, false, 0, sort_other_cb);
}

// ---------------------------------------------------------------------------
// the authors and series page
//
// The same windowed pool, with a name and a count of books on each row: the
// 2.0 row with the name's first letter on a plaque, "Books: 3" under it and a
// chevron, as the artists are listed. The names are few enough to hold -- one
// per author or series, not per book.
// ---------------------------------------------------------------------------

typedef struct {
	lv_obj_t *obj;
	int index;
} name_row_t;

typedef struct {
	char *name;
	int books;
} name_entry_t;

static lv_obj_t *names_title;
static lv_obj_t *names_sort_icon;
static lv_obj_t *names_list;
static lv_obj_t *names_body;
static lv_obj_t *names_empty;
static name_row_t name_rows[ROW_POOL];
static name_entry_t *names;
static int name_count;
static int name_capacity;
static int name_pitch;
static list_id_t names_kind = LIST_AUTHORS;

static void names_clear(void) {
	for (int i = 0; i < name_count; i++) {
		free(names[i].name);
	}
	name_count = 0;
}

static bool names_add_cb(const char *name, int books, void *user) {
	(void)user;
	if (name_count == name_capacity) {
		int grown = name_capacity ? name_capacity * 2 : 64;
		name_entry_t *bigger = realloc(names, (size_t)grown * sizeof(*bigger));
		if (!bigger) {
			return false;
		}
		names = bigger;
		name_capacity = grown;
	}
	names[name_count].name = strdup(name);
	if (!names[name_count].name) {
		return false;
	}
	names[name_count].books = books;
	name_count++;
	return true;
}

// The name a row shows: the books with no author are one row of their own.
static const char *shown_name(const char *name) { return name[0] ? name : tr("audiobook_unknown_author"); }

static void name_row_bind(name_row_t *row, int index) {
	if (row->index == index) {
		return;
	}
	row->index = index;
	if (index < 0 || index >= name_count) {
		lv_obj_set_hidden(row->obj, true);
		return;
	}
	lv_obj_set_hidden(row->obj, false);
	lv_obj_set_y(row->obj, index * name_pitch);
	const char *name = shown_name(names[index].name);
	sx_row_set_title(row->obj, name);
	char books[64];
	lv_snprintf(books, sizeof(books), tr("audiobook_books_count"), names[index].books);
	sx_row_set_subtitle(row->obj, books, NULL, NULL);
	char letter[8] = "";
	int n = (unsigned char)name[0] >= 0xF0 ? 4 : (unsigned char)name[0] >= 0xE0 ? 3 : (unsigned char)name[0] >= 0xC0 ? 2 : 1;
	for (int i = 0; i < n && name[i]; i++) {
		letter[i] = name[i];
	}
	sx_row_set_letter(row->obj, letter, sx_tone(SX_T_COVER_TONE, sx_palette_hue_of_name(name)), names_kind == LIST_AUTHORS);
}

static void names_window_update(void) {
	int scroll = lv_obj_get_scroll_y(names_list);
	if (scroll < 0) {
		scroll = 0;
	}
	int first = (scroll / name_pitch) - 1;
	if (first + ROW_POOL > name_count) {
		first = name_count - ROW_POOL;
	}
	if (first < 0) {
		first = 0;
	}
	for (int i = 0; i < ROW_POOL; i++) {
		int index = first + i;
		name_row_bind(&name_rows[index % ROW_POOL], index < name_count ? index : -1);
	}
}

static void names_scroll_cb(lv_event_t *e) {
	(void)e;
	names_window_update();
}

static void reload_names(void) {
	// Large rows as it is now.
	int height = sx_row_height(false);
	if (height + SX_ROW_GAP != name_pitch) {
		name_pitch = height + SX_ROW_GAP;
		for (int i = 0; i < ROW_POOL; i++) {
			sx_row_set_compact(name_rows[i].obj, false);
		}
	}
	for (int i = 0; i < ROW_POOL; i++) {
		name_row_bind(&name_rows[i], -1);
		name_rows[i].index = -1;
	}
	names_clear();

	sort_choice_t choice = list_sort(names_kind);
	bool by_added = choice == SORT_NEW || choice == SORT_OLD;
	bool desc = choice == SORT_ZA || choice == SORT_NEW;
	audiobookdb_names_for_each(names_kind == LIST_SERIES ? AUDIOBOOK_NAMES_SERIES : AUDIOBOOK_NAMES_AUTHORS, by_added, desc, names_add_cb, NULL);

	lv_obj_set_height(names_body, name_count > 0 ? name_count * name_pitch : 1);
	lv_obj_scroll_to_y(names_list, 0, LV_ANIM_OFF);
	lv_label_set_text(names_empty, tr(names_kind == LIST_SERIES ? "audiobook_series_empty" : "audiobook_list_empty"));
	lv_obj_set_hidden(names_empty, name_count != 0);
	lv_image_set_src(names_sort_icon, sort_icon(choice));
	names_window_update();
}

static void names_open(list_id_t kind) {
	names_kind = kind;
	lv_label_set_text(names_title, tr(kind == LIST_SERIES ? "audiobook_series" : "audiobook_authors"));
	reload_names();
	switch_screen(names_screen);
}

static void name_tap(lv_obj_t *obj, void *user) {
	(void)obj;
	name_row_t *row = user;
	int index = row ? row->index : -1;
	if (index < 0 || index >= name_count) {
		return;
	}
	char value[256];
	snprintf(value, sizeof(value), "%s", names[index].name);
	books_open(names_kind == LIST_SERIES ? LIST_SERIES_BOOKS : LIST_AUTHOR_BOOKS, value, shown_name(value));
}

static void names_sort_cb(lv_event_t *e) { sort_menu_show(lv_event_get_current_target(e), names_kind, reload_names); }

static void names_loaded_cb(lv_event_t *e) {
	(void)e;
	if (name_pitch != sx_row_height(false) + SX_ROW_GAP) {
		reload_names();
		return;
	}
	names_window_update();
}

static void build_names_page(gui_config_t *cfg) {
	lv_obj_add_style(names_screen, &theme_style_screen, 0);
	names_title = settingsrow_title(names_screen, cfg, "audiobook_authors");
	settingsrow_title_corner_slots(names_title, cfg, 1);
	lv_obj_t *sort_btn = corner_button(names_screen, cfg, 0, &icon_sort_az, names_sort_cb);
	names_sort_icon = lv_obj_get_child(sort_btn, 0);

	names_list = make_viewport(names_screen, cfg, &names_body, &names_empty, names_scroll_cb, NULL);

	static const sx_row_handlers_t handlers = {.on_tap = name_tap};
	name_pitch = sx_row_height(false) + SX_ROW_GAP;
	for (int i = 0; i < ROW_POOL; i++) {
		name_row_t *row = &name_rows[i];
		row->obj = sx_row_create(names_body, &handlers, false);
		sx_row_set_user(row->obj, row);
		sx_row_set_trail(row->obj, SX_ROW_TRAIL_CHEVRON);
		lv_obj_set_hidden(row->obj, true);
		row->index = -1;
	}

	player_sheet_attach_drag(names_screen, true);
	switcher_attach_back_gesture(names_screen);
	lv_obj_add_event_cb(names_screen, names_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}

// ---------------------------------------------------------------------------
// the section page
//
// 2.0 (the handoff's "state -> progress -> list"): with nothing indexed, an
// empty state that says where the books go -- the folder in force, by name --
// and a Scan button that starts at once; while a scan runs, a card with what
// it has found so far; then the books themselves, a tap plays one.
//
// Everything else the section had -- the lists to continue, the finished
// books, authors, series, bookmarks, the options -- is one tap on the ⋮ in
// the header away, in its sheet; the sort is the ⇅ beside it. Without a card
// the page says so and looks for it once a second while it shows.
// ---------------------------------------------------------------------------

static void open_continue(void *user) {
	(void)user;
	books_open(LIST_CONTINUE, NULL, tr("audiobook_continue"));
}
static void open_finished(void *user) {
	(void)user;
	books_open(LIST_FINISHED, NULL, tr("audiobook_menu_finished"));
}
static void open_authors(void *user) {
	(void)user;
	names_open(LIST_AUTHORS);
}
static void open_series(void *user) {
	(void)user;
	names_open(LIST_SERIES);
}
static void open_bookmarks(void *user) {
	(void)user;
	audiobookextras_open_bookmarks();
}
static void open_options(void *user) {
	(void)user;
	switch_screen(audiobooksettings_screen);
}

static void section_menu_cb(lv_event_t *e) {
	(void)e;
	bool any = audiobookdb_count() > 0 && !audiobookdb_scan_running();
	sx_sheet_item_t items[7];
	int n = 0;
	if (any) {
		items[n++] = (sx_sheet_item_t){tr("audiobook_continue"), &icon_sort_played, false, false, open_continue, NULL};
		items[n++] = (sx_sheet_item_t){tr("audiobook_authors"), &icon_artist, false, false, open_authors, NULL};
		items[n++] = (sx_sheet_item_t){tr("audiobook_series"), &icon_sort_series, false, false, open_series, NULL};
		items[n++] = (sx_sheet_item_t){tr("audiobook_menu_finished"), &icon_book_finished, false, false, open_finished, NULL};
		items[n++] = (sx_sheet_item_t){tr("bookmarks"), &icon_bookmark, false, false, open_bookmarks, NULL};
	}
	if (!audiobookdb_scan_running() && gui_card_available()) {
		items[n++] = (sx_sheet_item_t){tr("audiobook_rescan"), &icon_refresh, false, false, start_scan, NULL};
	}
	items[n++] = (sx_sheet_item_t){tr("settings"), &icon_music_settings, false, false, open_options, NULL};
	sx_sheet_menu(tr("audiobooks"), NULL, items, n);
}

// An index written by an older scan has no authors, series or folder books.
// It is read again once, the first time the section is opened.
static bool upgrade_offered;

// The empty state and the scan card.
static lv_obj_t *section_sort_btn;
static lv_obj_t *empty_panel;
static lv_obj_t *empty_icon;
static lv_obj_t *empty_title;
static lv_obj_t *empty_text;
static lv_obj_t *empty_button;
static lv_obj_t *scan_box;	 // holds the scan card, under the header
static lv_obj_t *scan_card;
static lv_obj_t *scan_note;	 // "Found: 2"
static lv_obj_t *scan_bar;
static lv_timer_t *card_timer; // looks for the card while the page says it is missing

static void section_show(void);

static void card_timer_cb(lv_timer_t *timer) {
	// Only while the page is up; a page left behind looks again when it is
	// opened.
	if (lv_screen_active() != audiobooks_screen) {
		lv_timer_pause(timer);
		return;
	}
	if (gui_card_available()) {
		lv_timer_pause(timer);
		section_show();
	}
}

// The empty state's words: the folder in force by its name (the last part of
// its path), so "put the books in the Audiobooks folder" says the folder this
// player reads, not the one it reads by default.
static void empty_say_folder(void) {
	char folder[AUDIOBOOKDB_FOLDER_MAX];
	audiobookdb_folder(folder, sizeof(folder));
	const char *name = strrchr(folder, '/');
	name = name ? name + 1 : folder;
	char text[AUDIOBOOKDB_FOLDER_MAX + 256];
	lv_snprintf(text, sizeof(text), tr("audiobook_empty_text"), name);
	lv_label_set_text(empty_text, text);
}

// The empty state stands centred in what the mini player leaves of the page.
static int section_top;
static int section_height;

static void section_layout(void) {
	if (empty_panel) {
		lv_obj_set_height(empty_panel, section_height - section_top - sx_frame_body_bottom());
	}
}

static void section_show(void) {
	section_layout();
	bool card = gui_card_available();
	bool scanning = audiobookdb_scan_running();
	bool empty = card && !scanning && audiobookdb_count() == 0;

	lv_obj_set_hidden(scan_box, !scanning);
	lv_obj_set_hidden(empty_panel, card && !empty);
	lv_obj_set_hidden(shelf.viewport, !card || scanning || empty);
	lv_obj_set_hidden(section_sort_btn, !card || scanning || empty);

	if (!card) {
		lv_image_set_src(empty_icon, &icon_sd_card);
		lv_image_set_scale(empty_icon, 256 * 40 / 128);
		lv_label_set_text(empty_title, tr(usb_storage_active() ? "usb_storage_shared" : "sd_card_missing"));
		lv_obj_set_hidden(empty_text, true);
		lv_obj_set_hidden(empty_button, true);
		if (!card_timer) {
			card_timer = lv_timer_create(card_timer_cb, 1000, NULL);
		}
		lv_timer_resume(card_timer);
		return;
	}
	if (card_timer) {
		lv_timer_pause(card_timer);
	}
	if (empty) {
		lv_image_set_src(empty_icon, &icon_files_audiobook);
		lv_image_set_scale(empty_icon, 256 * 40 / 32);
		lv_label_set_text(empty_title, tr("audiobook_empty_title"));
		empty_say_folder();
		lv_obj_set_hidden(empty_text, false);
		lv_obj_set_hidden(empty_button, false);
	}
}

// Before the slide in: which state the page is in, and the list read or
// brought up to date, so the page arrives finished.
static void section_load_start_cb(lv_event_t *e) {
	(void)e;
	section_show();
	if (gui_card_available() && !lv_obj_is_hidden(shelf.viewport)) {
		list_refresh(&shelf);
	}
}

// On screen: the pictures (only the list on screen asks for them).
static void section_loaded_cb(lv_event_t *e) {
	(void)e;
	if (!gui_card_available()) {
		return;
	}
	thumbs_update(&shelf);
	if (!upgrade_offered && audiobookdb_needs_rescan()) {
		upgrade_offered = true;
		// After this page's own load has finished, not inside it.
		lv_async_call(start_scan, NULL);
	}
}

static void section_unloaded_cb(lv_event_t *e) {
	(void)e;
	if (card_timer) {
		lv_timer_pause(card_timer);
	}
}

static void empty_scan_cb(lv_event_t *e) {
	(void)e;
	start_scan(NULL);
}

static void build_empty_panel(gui_config_t *cfg) {
	int top = settingsrow_content_top(cfg);
	section_top = top;
	section_height = cfg->screen_height;
	empty_panel = lv_obj_create(audiobooks_screen);
	lv_obj_remove_style_all(empty_panel);
	sx_frame_exempt(empty_panel);
	lv_obj_set_size(empty_panel, lv_pct(100), cfg->screen_height - top);
	lv_obj_align(empty_panel, LV_ALIGN_TOP_LEFT, 0, top);
	lv_obj_set_style_pad_hor(empty_panel, 36, 0);
	lv_obj_set_scrollable(empty_panel, false);
	lv_obj_set_flex_flow(empty_panel, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(empty_panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_hidden(empty_panel, true);
	// A swipe back may start anywhere on it.
	lv_obj_set_event_bubble(empty_panel, true);

	// The glyph on a round surface, 84 across.
	lv_obj_t *disc = lv_obj_create(empty_panel);
	lv_obj_remove_style_all(disc);
	lv_obj_set_size(disc, 84, 84);
	lv_obj_add_style(disc, &sx_style_surface, 0);
	lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_margin_bottom(disc, 22, 0);
	lv_obj_set_scrollable(disc, false);
	empty_icon = lv_image_create(disc);
	lv_obj_add_style(empty_icon, &sx_style_icon, 0);
	lv_obj_center(empty_icon);

	empty_title = lv_label_create(empty_panel);
	lv_obj_set_width(empty_title, lv_pct(100));
	lv_label_set_long_mode(empty_title, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_align(empty_title, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(empty_title, &sx_style_text, 0);
	lv_obj_set_style_text_font(empty_title, &font_ui_26_bold, 0);
	lv_obj_set_style_margin_bottom(empty_title, 8, 0);

	empty_text = lv_label_create(empty_panel);
	lv_obj_set_width(empty_text, lv_pct(100));
	lv_label_set_long_mode(empty_text, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_align(empty_text, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(empty_text, &sx_style_text2, 0);
	lv_obj_set_style_text_font(empty_text, &font_ui_21, 0);
	lv_obj_set_style_text_line_space(empty_text, 4, 0);
	lv_obj_set_style_margin_bottom(empty_text, 34, 0);

	// The accent pill, 68 tall: the one thing to do here.
	empty_button = lv_btn_create(empty_panel);
	lv_obj_remove_style_all(empty_button);
	lv_obj_set_size(empty_button, LV_SIZE_CONTENT, SX_SHEET_BTN_H);
	lv_obj_set_style_min_width(empty_button, 272, 0);
	lv_obj_set_style_pad_hor(empty_button, 36, 0);
	lv_obj_set_style_radius(empty_button, LV_RADIUS_CIRCLE, 0);
	lv_obj_add_style(empty_button, &sx_style_accent, 0);
	lv_obj_add_style(empty_button, &sx_style_press, LV_STATE_PRESSED);
	lv_obj_add_event_cb(empty_button, empty_scan_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *label = lv_label_create(empty_button);
	lv_label_set_text(label, tr("scan"));
	lv_obj_set_style_text_font(label, &font_ui_26_bold, 0);
	lv_obj_center(label);
}

static void build_section_page(gui_config_t *cfg) {
	lv_obj_add_style(audiobooks_screen, &theme_style_screen, 0);
	shelf.title = settingsrow_title(audiobooks_screen, cfg, "audiobooks");
	settingsrow_title_corner_slots(shelf.title, cfg, 2);
	corner_button(audiobooks_screen, cfg, 0, &icon_ellipsis_vertical, section_menu_cb);
	shelf.list = LIST_LIBRARY;
	list_build(&shelf, audiobooks_screen, cfg, true, 1, sort_shelf_cb);
	section_sort_btn = lv_obj_get_parent(shelf.sort_icon);

	build_empty_panel(cfg);

	// The scan card, where the list starts.
	int top = settingsrow_content_top(cfg);
	scan_box = lv_obj_create(audiobooks_screen);
	lv_obj_remove_style_all(scan_box);
	lv_obj_set_size(scan_box, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_align(scan_box, LV_ALIGN_TOP_LEFT, 0, top);
	lv_obj_set_style_pad_hor(scan_box, cfg->padding, 0);
	lv_obj_set_scrollable(scan_box, false);
	lv_obj_set_event_bubble(scan_box, true);
	lv_obj_set_hidden(scan_box, true);
	scan_card = sx_card_create(scan_box);
	sx_card_title(scan_card, tr("audiobook_scanning"));
	scan_note = sx_card_subtitle(scan_card, "", false);
	scan_bar = sx_card_bar(scan_card, 0);
	sx_card_action(scan_card, tr("audiobook_scan_stop"), scan_stop_cb, NULL);

	lv_obj_add_event_cb(audiobooks_screen, section_load_start_cb, LV_EVENT_SCREEN_LOAD_START, NULL);
	lv_obj_add_event_cb(audiobooks_screen, section_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(audiobooks_screen, section_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
}

// ---------------------------------------------------------------------------
// the options page
// ---------------------------------------------------------------------------

// A line under a card in the secondary colour, saying what the card does not
// say by itself.
static void option_note(lv_obj_t *parent, const char *text) {
	lv_obj_t *note = lv_label_create(parent);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &sx_style_text2, 0);
	lv_obj_set_style_text_font(note, &font_ui_21, 0);
	lv_obj_set_style_pad_hor(note, 6, 0);
	lv_label_set_text(note, tr(text));
}

// ---------------------------------------------------------------------------
// The audiobook folder
//
// Where the books are read from: "Audiobooks" at the root of the card unless
// the user picks another here. A row on the settings page shows the folder in
// force and opens a small folder browser: the folders of the card, a tap goes
// into one, the back chevron or swipe comes out of it, and "Use this folder"
// at the top of every level below the card's root takes the one on screen.
// The card's root itself cannot be taken: the music scan leaves the audiobook
// folder out, and leaving out the whole card would leave no music.
//
// Taking a new folder costs two scans: the audiobooks are read again from the
// new folder, and the music library -- which passed over the old folder and
// read the new one -- is scanned again without it. 2.0 starts them at once
// and says so on the "Use this folder" card itself.
//
// The browser's rows are made here rather than with settingsrow_add(), whose
// rows keep a small record for the life of the program: right for a settings
// page built once, a leak for a list built again at every folder entered.
// ---------------------------------------------------------------------------

// Folders listed in one level at most. A card's root holds a few dozen, an
// author's folder a few hundred books at the very most; past this the rest
// are left off rather than built into rows the device has no memory for.
#define FOLDER_ROWS_MAX 200
#define FOLDER_ROW_HEIGHT 100

static const char *card_root;
static lv_obj_t *folder_value;	// the settings row's value: the folder in force
static lv_obj_t *folder_screen; // the browser
static lv_obj_t *folder_list;

// Where the browser is, relative to the root of the card ("" is the root), and
// the folders in it, sorted.
static char folder_at[AUDIOBOOKDB_FOLDER_MAX];
static char **folder_names;
static int folder_count;
static bool folder_shown; // the rows on the page are those of folder_at

static void folder_absolute(const char *rel, char *out, size_t size) {
	snprintf(out, size, "%s%s%s", card_root ? card_root : "", rel[0] ? "/" : "", rel);
}

static bool folder_exists(const char *rel) {
	char path[768];
	folder_absolute(rel, path, sizeof(path));
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// The settings row: the folder's path, or that it is not on the card -- a
// folder renamed on a computer would otherwise look like a scan that finds
// nothing for no reason.
static void folder_value_refresh(void) {
	if (!folder_value) {
		return;
	}
	char current[AUDIOBOOKDB_FOLDER_MAX];
	audiobookdb_folder(current, sizeof(current));
	if (folder_exists(current)) {
		lv_label_set_text(folder_value, current);
	} else {
		lv_label_set_text_fmt(folder_value, tr("audiobook_folder_missing"), current);
	}
}

static void folder_names_free(void) {
	for (int i = 0; i < folder_count; i++) {
		free(folder_names[i]);
	}
	free(folder_names);
	folder_names = NULL;
	folder_count = 0;
}

static int folder_name_cmp(const void *a, const void *b) {
	return strcasecmp(*(const char *const *)a, *(const char *const *)b);
}

// The subfolders of `folder_at`: not the hidden ones (the player's own .local
// among them) and not the ones a desktop leaves behind, as the scans skip
// them too.
static void folder_read(void) {
	folder_names_free();
	char path[768];
	folder_absolute(folder_at, path, sizeof(path));
	DIR *dir = opendir(path);
	if (!dir) {
		return;
	}
	int capacity = 0;
	struct dirent *de;
	while ((de = readdir(dir)) != NULL && folder_count < FOLDER_ROWS_MAX) {
		if (de->d_name[0] == '.' || playlist_is_junk_name(de->d_name)) {
			continue;
		}
		char child[1024];
		if (snprintf(child, sizeof(child), "%s/%s", path, de->d_name) >= (int)sizeof(child)) {
			continue;
		}
		struct stat st;
		if (stat(child, &st) != 0 || !S_ISDIR(st.st_mode)) {
			continue;
		}
		if (folder_count == capacity) {
			int grown = capacity ? capacity * 2 : 32;
			char **names = realloc(folder_names, (size_t)grown * sizeof(*names));
			if (!names) {
				break;
			}
			folder_names = names;
			capacity = grown;
		}
		folder_names[folder_count] = strdup(de->d_name);
		if (!folder_names[folder_count]) {
			break;
		}
		folder_count++;
	}
	closedir(dir);
	if (folder_count > 1) {
		qsort(folder_names, (size_t)folder_count, sizeof(folder_names[0]), folder_name_cmp);
	}
}

static void folder_show(void);

static void folder_enter_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return; // a swipe across the row, not a tap on it
	}
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	if (i < 0 || i >= folder_count) {
		return;
	}
	char deeper[AUDIOBOOKDB_FOLDER_MAX];
	int wrote = snprintf(deeper, sizeof(deeper), "%s%s%s", folder_at, folder_at[0] ? "/" : "", folder_names[i]);
	if (wrote < 0 || wrote >= (int)sizeof(deeper)) {
		toast_error("audiobook_folder_too_deep");
		return;
	}
	snprintf(folder_at, sizeof(folder_at), "%s", deeper);
	folder_read();
	folder_show();
}

// Back out of a folder rather than out of the browser, while there is one to
// back out of.
static bool folder_back_has_step(void) { return folder_at[0] != '\0'; }

static bool folder_back_guard(void) {
	if (!folder_back_has_step()) {
		return false;
	}
	char *slash = strrchr(folder_at, '/');
	if (slash) {
		*slash = '\0';
	} else {
		folder_at[0] = '\0';
	}
	folder_read();
	folder_show();
	return true;
}

// A new folder costs two scans: the audiobooks are read again from it, and
// the music library -- which passed over the old folder and read this one --
// is scanned again without it. 2.0 starts them at once (scans need no "are
// you sure?"); the row that does it says so under its name.
static void folder_use_cb(lv_event_t *e) {
	(void)e;
	char current[AUDIOBOOKDB_FOLDER_MAX];
	audiobookdb_folder(current, sizeof(current));
	if (audiobookdb_folder_matches(current, folder_at)) {
		// Nothing changes: no scans.
		switch_screen_return_to(audiobooksettings_screen);
		return;
	}
	if (!audiobookdb_set_folder(folder_at)) {
		return;
	}
	folder_value_refresh();

	// The music first, in the background. Not a library that was never
	// scanned -- building it is the Scan button's job -- and not under a scan
	// already running, which goes on with the folder it started with.
	if (library_track_count() > 0 && card_root && card_root[0]) {
		if (!library_scan_start(card_root)) {
			printf("audiobooks: the music library is busy; it keeps the old folder until its next scan\n");
		}
	}
	// Then the books, on the section page.
	start_scan(NULL);
}

// A 2.0 card with a glyph before its name: a folder (a chevron after it), or
// the accent "Use this folder" with what taking it does under the name.
static lv_obj_t *folder_row(const lv_image_dsc_t *glyph, const char *text, const char *note, bool accent, lv_event_cb_t cb, int index) {
	lv_obj_t *row = lv_btn_create(folder_list);
	lv_obj_remove_style_all(row);
	sx_card_style(row);
	// A fixed height for a plain folder: LVGL places a content-sized row's
	// single track at its top when a minimum height makes it taller.
	lv_obj_set_size(row, lv_pct(100), note ? LV_SIZE_CONTENT : SX_CARD_MIN_H + 2 * SX_CARD_PAD_V);
	lv_obj_set_style_pad_column(row, 16, 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(row, false);
	// Presses bubble up to the list, so the swipe-back can start on a row.
	lv_obj_set_event_bubble(row, true);
	lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

	lv_obj_t *icon = lv_image_create(row);
	lv_image_set_src(icon, glyph);
	lv_obj_add_style(icon, accent ? &sx_style_icon_accent : &sx_style_icon2, 0);

	lv_obj_t *column = lv_obj_create(row);
	lv_obj_remove_style_all(column);
	lv_obj_set_height(column, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(column, 1);
	lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(column, 4, 0);
	lv_obj_set_event_bubble(column, true);
	lv_obj_set_clickable(column, false);

	lv_obj_t *label = lv_label_create(column);
	lv_obj_set_width(label, lv_pct(100));
	lv_label_set_text(label, text);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_add_style(label, accent ? &sx_style_text_accent : &sx_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_24, 0);

	if (note) {
		lv_obj_t *sub = lv_label_create(column);
		lv_obj_set_width(sub, lv_pct(100));
		lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
		lv_label_set_text(sub, note);
		lv_obj_add_style(sub, &sx_style_text2, 0);
		lv_obj_set_style_text_font(sub, &font_ui_21, 0);
	}
	return row;
}

static void folder_show(void) {
	folder_shown = true;
	lv_obj_clean(folder_list);
	lv_label_set_text(settingsrow_page_title(folder_screen), folder_at[0] ? folder_at : tr("audiobook_folder_card"));

	char current[AUDIOBOOKDB_FOLDER_MAX];
	audiobookdb_folder(current, sizeof(current));

	if (folder_at[0]) {
		folder_row(&icon_check, tr("audiobook_folder_use"), tr("audiobook_folder_change_note"), true, folder_use_cb, -1);
	}
	for (int i = 0; i < folder_count; i++) {
		char rel[AUDIOBOOKDB_FOLDER_MAX + 256];
		snprintf(rel, sizeof(rel), "%s%s%s", folder_at, folder_at[0] ? "/" : "", folder_names[i]);
		// The folder in force stands out, so the way back to it is plain.
		lv_obj_t *row = folder_row(&icon_folder, folder_names[i], NULL, audiobookdb_folder_matches(current, rel), folder_enter_cb, i);
		lv_obj_t *chev = lv_image_create(row);
		lv_image_set_src(chev, &icon_chevron_right);
		lv_obj_add_style(chev, &sx_style_icon2, 0);
	}
	if (folder_count == 0) {
		option_note(folder_list, "audiobook_folder_none");
	}
	lv_obj_scroll_to_y(folder_list, 0, LV_ANIM_OFF);
}

static void folder_open_cb(lv_event_t *e) {
	(void)e;
	// Opened on the folder that holds the one in force, so a change is one
	// tap away from where the user already is; at the root when that folder
	// is gone.
	char current[AUDIOBOOKDB_FOLDER_MAX];
	audiobookdb_folder(current, sizeof(current));
	folder_at[0] = '\0';
	char *slash = strrchr(current, '/');
	if (slash) {
		*slash = '\0';
		if (folder_exists(current)) {
			snprintf(folder_at, sizeof(folder_at), "%s", current);
		}
	}
	folder_read();
	folder_show();
	switch_screen(folder_screen);
}

static void folder_screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	// Nothing of the browser is kept while it is not on screen.
	lv_obj_clean(folder_list);
	folder_names_free();
	folder_shown = false;
}

// Back on the browser from a page opened over it (the player, say): the
// level it was on is read and drawn again.
static void folder_screen_loaded_cb(lv_event_t *e) {
	(void)e;
	if (!folder_shown) {
		folder_read();
		folder_show();
	}
}

static void build_folder_page(gui_config_t *cfg) {
	card_root = cfg->sd_root_path;
	folder_screen = lv_obj_create(NULL);
	folder_list = settingsrow_page(folder_screen, cfg, "audiobook_folder");
	settingsrow_title_corner_slots(settingsrow_page_title(folder_screen), cfg, 0);
	lv_label_set_long_mode(settingsrow_page_title(folder_screen), LV_LABEL_LONG_DOT);
	switcher_attach_back_gesture(folder_screen);
	switcher_set_back_guard(folder_screen, folder_back_guard);
	switcher_set_back_guard_peek(folder_screen, folder_back_has_step);
	lv_obj_add_event_cb(folder_screen, folder_screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	lv_obj_add_event_cb(folder_screen, folder_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}

// --- the three options ---

#define REWIND_CHOICES 4

static lv_obj_t *stop_chapter_switch;
static lv_obj_t *duration_total_pill;
static lv_obj_t *duration_chapter_pill;
static settingsrow_duration_t sleep_row;
static lv_obj_t *rewind_switch;
static lv_obj_t *rewind_pills;
static lv_obj_t *rewind_pill[REWIND_CHOICES];
static const int REWIND_VALUES[REWIND_CHOICES] = {3, 5, 7, 10};

static void refresh_settings_page(void) {
	if (!stop_chapter_switch) {
		return;
	}
	folder_value_refresh();

	bool per_chapter = audiobook_duration_per_chapter();
	settingsrow_pill_active(duration_total_pill, !per_chapter);
	settingsrow_pill_active(duration_chapter_pill, per_chapter);

	if (audiobook_stop_at_chapter_end()) {
		lv_obj_add_state(stop_chapter_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(stop_chapter_switch, LV_STATE_CHECKED);
	}

	// The control centre switches this timer too, so the switch and the
	// wheels are read again rather than trusted from the last visit.
	settingsrow_duration_set_minutes(&sleep_row, sleeptimer_minutes(SLEEPTIMER_AUDIOBOOK));
	settingsrow_duration_expanded(&sleep_row, sleeptimer_enabled(SLEEPTIMER_AUDIOBOOK));
	settingsrow_duration_repaint(&sleep_row);

	bool rewind_on = audiobook_rewind_enabled();
	if (rewind_on) {
		lv_obj_add_state(rewind_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(rewind_switch, LV_STATE_CHECKED);
	}
	// The seconds only mean something while it is on.
	lv_obj_set_hidden(rewind_pills, !rewind_on);

	int seconds = audiobook_rewind_seconds();
	for (int i = 0; i < REWIND_CHOICES; i++) {
		settingsrow_pill_active(rewind_pill[i], REWIND_VALUES[i] == seconds);
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

static void scan_row_cb(lv_event_t *e) {
	(void)e;
	start_scan(NULL);
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

	settingsrow_toggle_pills(container, "audiobook_rewind_after_pause", rewind_toggle_cb, &rewind_switch, &rewind_pills);
	static const char *const REWIND_LABELS[REWIND_CHOICES] = {"audiobook_3_sec", "audiobook_5_sec", "audiobook_7_sec", "audiobook_10_sec"};
	for (int i = 0; i < REWIND_CHOICES; i++) {
		rewind_pill[i] = settingsrow_pill(rewind_pills, REWIND_LABELS[i], REWIND_VALUES[i], rewind_pick_cb);
	}

	// An action, not a page: it starts the scan and shows it on the section
	// page, so no chevron.
	settingsrow_action(container, "audiobook_scan_audiobooks_2", scan_row_cb, NULL);

	// Where the books are read from, and the way to change it; the note under
	// it is about this row.
	settingsrow_add(container, "audiobook_folder", &folder_value, folder_open_cb, NULL);
	lv_label_set_long_mode(folder_value, LV_LABEL_LONG_DOT);
	lv_obj_set_style_max_width(folder_value, lv_pct(55), 0);
	// Said on the row itself: a scan that finds nothing because the books are
	// somewhere else looks like a scan that is broken.
	option_note(container, "audiobook_folder_note");

	refresh_settings_page();
	theme_register_refresh(refresh_settings_page);
	lv_obj_add_event_cb(audiobooksettings_screen, settings_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	// And when the control centre switches the timer with the page under it.
	lv_obj_add_event_cb(audiobooksettings_screen, settings_loaded_cb, LV_EVENT_REFRESH, NULL);
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

static void refresh_control_buttons(void) {
	int back = audiobook_skip_back();
	int forward = audiobook_skip_forward();

	for (int i = 0; i < SKIP_CHOICES; i++) {
		settingsrow_pill_active(back_choice[i], SKIP_VALUES[i] == back);
		settingsrow_pill_active(forward_choice[i], SKIP_VALUES[i] == forward);
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

static void controls_loaded_cb(lv_event_t *e) {
	(void)e;
	refresh_control_buttons();
}

static void build_controls_page(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(audiobookcontrols_screen, cfg, "change_controls");

	// One pill card per button: its name, then the three jumps.
	lv_obj_t *back_row = NULL;
	settingsrow_pills(container, "back_button", &back_row);
	back_choice[0] = settingsrow_pill_text(back_row, "-10", AUDIOBOOK_SKIP_SHORT, pick_back_cb);
	back_choice[1] = settingsrow_pill_text(back_row, "-30", AUDIOBOOK_SKIP_LONG, pick_back_cb);
	back_choice[2] = settingsrow_pill_text(back_row, "-60", AUDIOBOOK_SKIP_HUGE, pick_back_cb);

	lv_obj_t *forward_row = NULL;
	settingsrow_pills(container, "forward_button", &forward_row);
	forward_choice[0] = settingsrow_pill_text(forward_row, "+10", AUDIOBOOK_SKIP_SHORT, pick_forward_cb);
	forward_choice[1] = settingsrow_pill_text(forward_row, "+30", AUDIOBOOK_SKIP_LONG, pick_forward_cb);
	forward_choice[2] = settingsrow_pill_text(forward_row, "+60", AUDIOBOOK_SKIP_HUGE, pick_forward_cb);

	refresh_control_buttons();
	theme_register_refresh(refresh_control_buttons);
	lv_obj_add_event_cb(audiobookcontrols_screen, controls_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}

// ---------------------------------------------------------------------------
// The scan
//
// 2.0 (principle 6): a scan starts at once, from the empty state's button, the
// sheet or the options, with no question first -- it changes nothing that
// cannot be had back by scanning again. It shows on the section page as a
// card where the list starts: what has been found so far, a bar measured
// against what the last scan found (none on a first scan: there is nothing to
// measure against, and a bar that guesses would lie), and Stop. When it ends
// the books come in where the card was, and a toast says how many.
//
// The progress is looked at twice a second while a scan runs, wherever the
// user has gone meanwhile -- the scan holds the screen on, and only this
// notices it end to let go -- and the card is written only while it shows.
// Nothing runs once it is over.
// ---------------------------------------------------------------------------

#define SCAN_POLL_MS 500

static lv_timer_t *scan_poll_timer;
static int scan_expected; // books the last scan found: the bar's measure; 0 unknown
static char scan_root[512];
static const char *scan_card_root = "";

static void scan_card_update(int found) {
	char text[96];
	lv_snprintf(text, sizeof(text), tr("audiobook_scan_found"), found);
	sx_card_subtitle(scan_card, text, false);
	if (scan_expected > 0) {
		// Short of the end until it is the end: a card may hold more books
		// than last time.
		int percent = found * 100 / scan_expected;
		sx_card_bar_set(scan_bar, percent > 95 ? 95 : percent);
	}
	lv_obj_set_hidden(scan_bar, scan_expected <= 0);
}

static void scan_poll_cb(lv_timer_t *timer) {
	int found = audiobookdb_scan_found();
	if (audiobookdb_scan_running()) {
		if (lv_screen_active() == audiobooks_screen) {
			scan_card_update(found);
		}
		return;
	}

	lv_timer_pause(timer);
	power_hold_screen_on(false);
	section_show(); // first: the list asks for its pictures only once it shows
	list_reload(&shelf);
	char text[96];
	lv_snprintf(text, sizeof(text), tr("audiobook_scan_found"), found);
	sx_toast(text);
}

static void scan_stop_cb(lv_event_t *e) {
	(void)e;
	// The poll sees it end and shows what was found until then.
	audiobookdb_scan_stop();
}

static void start_scan(void *user) {
	(void)user;
	if (!audiobookdb_scan_running()) {
		// The folder in force now, which the options may have just changed.
		char rel[AUDIOBOOKDB_FOLDER_MAX];
		audiobookdb_folder(rel, sizeof(rel));
		snprintf(scan_root, sizeof(scan_root), "%s/%s", scan_card_root, rel);

		scan_expected = audiobookdb_count();
		if (!audiobookdb_scan_start(scan_root)) {
			toast_error("no_card_to_scan");
			return;
		}
		power_hold_screen_on(true);
		lv_timer_reset(scan_poll_timer);
		lv_timer_resume(scan_poll_timer);
	}
	scan_card_update(audiobookdb_scan_found());
	section_show();
	// To the section page, where the scan shows: from the options or the
	// folder browser, back to the page they were opened over.
	if (lv_screen_active() != audiobooks_screen) {
		switch_screen_return_to(audiobooks_screen);
	}
}

static void build_scan(gui_config_t *cfg) {
	// The folder and not the whole card: see the note in audiobookdb.c. The
	// folder itself is looked up at each scan (start_scan), since the options
	// can change it.
	scan_card_root = cfg->sd_root_path ? cfg->sd_root_path : "";
	scan_poll_timer = lv_timer_create(scan_poll_cb, SCAN_POLL_MS, NULL);
	lv_timer_pause(scan_poll_timer);
}

void audiobooks_init(gui_config_t *cfg) {
	books_screen = lv_obj_create(NULL);
	names_screen = lv_obj_create(NULL);
	thumb_timer = lv_timer_create(thumb_timer_cb, THUMB_POLL_MS, NULL);
	lv_timer_pause(thumb_timer);
	build_section_page(cfg);
	build_books_page(cfg);
	build_names_page(cfg);
	build_folder_page(cfg);
	build_settings_page(cfg);
	build_controls_page(cfg);
	build_scan(cfg);
	sx_frame_on_body_change(section_layout);
}

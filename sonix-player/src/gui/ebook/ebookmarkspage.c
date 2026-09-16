#include "ebookmarkspage.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "src/system/ebook/bookmarks.h"
#include "src/system/ebook/ebook.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/ebook/ebookcovers.h"
#include "src/gui/ebook/ebookreader.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"

lv_obj_t *ebookmarkspage_screen;
lv_obj_t *ebookmarkbookpage_screen;

// ---------------------------------------------------------------------------
// The bookmarks
//
// Two pages: the books that have any, and one book's marks.
//
// Which books those are is worked out by walking the Ebook folder and asking
// bookmarks.c about each one, rather than by keeping a list. Nothing then has to
// store a path, a book deleted from the card stops appearing on its own, and the
// list can never name a book that is not there.
//
// The covers come in behind, through ebookcovers.c, for the same reason the
// shelf's do: opening thirty books for thirty pictures is not something to do
// while a page is being built.
// ---------------------------------------------------------------------------

#define BOOKS_MAX 300
// The cover beside a row. Two-to-three like a paperback, and tall enough to be
// recognisable next to two lines of text.
#define ROW_COVER_W 56
#define ROW_COVER_H 84

typedef struct {
	char file[256];
	char name[256];
	int count; // how many bookmarks
	lv_obj_t *image;
	lv_obj_t *label;
	cover_image_t cover;
	bool has_cover;
} book_entry_t;

static gui_config_t *g_cfg;
static lv_obj_t *books_list;
static lv_obj_t *books_container; // the scroller, for working out what is on screen
static lv_obj_t *books_empty;
static lv_obj_t *marks_list;
static lv_obj_t *marks_title;

static book_entry_t *books;
static int book_count;
static char books_dir[512];
static uint32_t covers_token;

// Which book the second page is showing.
static char open_book_path[800];
static char open_book_name[256];

static void entry_path(const book_entry_t *entry, char *out, size_t size) {
	snprintf(out, size, "%s/%s", books_dir, entry->file);
}

// ---------------------------------------------------------------------------
// one book's marks
// ---------------------------------------------------------------------------

// True from a long press until the release it belongs to has gone by. LVGL
// sends LV_EVENT_CLICKED on release whether or not a long press was sent first
// (see indev_proc_release), so without this a hold would open the pop-over and
// then open the book behind it.
static bool swallow_next_click;

static void mark_pick_cb(lv_event_t *e) {
	if (swallow_next_click) {
		swallow_next_click = false;
		return;
	}
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	bookmark_t bm;
	if (bookmark_get(open_book_path, index, &bm)) {
		ebookreader_open_at(open_book_path, bm.spine, bm.block, bm.offset);
	}
}

static void build_marks(void);

// Which one the pop-over is about. The pop-over closes before it runs the
// action, so the index cannot be read back off the row at that point.
static int menu_mark;

static void delete_mark_action(void *user) {
	(void)user;
	if (bookmark_remove(open_book_path, menu_mark)) {
		toast_plain("ebookmarks_bookmark_deleted");
		build_marks();
	}
}

static void mark_long_pressed_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	menu_mark = (int)(intptr_t)lv_event_get_user_data(e);
	swallow_next_click = true;
	static const popover_item_t items[] = {{"ebookmarks_delete_bookmark", delete_mark_action, NULL, false}};
	popover_show(lv_event_get_current_target(e), items, 1);
}

static void build_marks(void) {
	lv_obj_clean(marks_list);
	if (marks_title) {
		lv_label_set_text(marks_title, open_book_name);
	}

	int count = bookmark_count(open_book_path);
	for (int i = 0; i < count; i++) {
		bookmark_t bm;
		if (!bookmark_get(open_book_path, i, &bm)) {
			continue;
		}

		lv_obj_t *row = lv_btn_create(marks_list);
		lv_obj_set_width(row, lv_pct(100));
		lv_obj_set_height(row, LV_SIZE_CONTENT);
		lv_obj_add_style(row, &theme_style_card, 0);
		lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_set_style_radius(row, 12, 0);
		lv_obj_set_style_border_width(row, 0, 0);
		lv_obj_set_style_shadow_width(row, 0, 0);
		lv_obj_set_style_pad_all(row, 18, 0);
		lv_obj_set_style_min_height(row, 88, 0);
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_style_pad_row(row, 6, 0);
		lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
		lv_obj_add_event_cb(row, mark_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
		lv_obj_add_event_cb(row, mark_long_pressed_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);

		// The line of the book that was on screen. It is what makes the list
		// readable as places rather than as numbers, so it goes first and gets
		// the room; a mark with no text falls back to the chapter alone.
		if (bm.text[0]) {
			lv_obj_t *text = lv_label_create(row);
			lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
			lv_obj_set_width(text, lv_pct(100));
			lv_label_set_text(text, bm.text);
			lv_obj_add_style(text, &theme_style_text, 0);
			lv_obj_set_style_text_font(text, &font_ui_20, 0);
			lv_obj_add_flag(text, LV_OBJ_FLAG_EVENT_BUBBLE);
		}

		lv_obj_t *where = lv_label_create(row);
		lv_label_set_text_fmt(where, "%s %u", tr("chapter"), bm.spine + 1u);
		lv_obj_add_style(where, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(where, &font_ui_18, 0);
		lv_obj_add_flag(where, LV_OBJ_FLAG_EVENT_BUBBLE);
	}
}

// ---------------------------------------------------------------------------
// the books that have any
// ---------------------------------------------------------------------------

static void marks_loaded_cb(lv_event_t *e) {
	(void)e;
	build_marks();
}

static void open_book_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= book_count) {
		return;
	}
	entry_path(&books[index], open_book_path, sizeof(open_book_path));
	snprintf(open_book_name, sizeof(open_book_name), "%s", books[index].name);
	switch_screen(ebookmarkbookpage_screen);
}

static bool cover_ready(int index, const char *title, const cover_image_t *cover) {
	if (!books || index < 0 || index >= book_count) {
		return false;
	}
	book_entry_t *entry = &books[index];
	if (title && title[0]) {
		snprintf(entry->name, sizeof(entry->name), "%s", title);
		if (entry->label) {
			lv_label_set_text(entry->label, entry->name);
		}
	}
	if (!cover || !entry->image) {
		return false;
	}
	entry->cover = *cover;
	entry->has_cover = true;
	lv_image_set_src(entry->image, &entry->cover.dsc);
	lv_obj_remove_flag(entry->image, LV_OBJ_FLAG_HIDDEN);
	return true;
}

static void books_free(void) {
	ebookcovers_stop(covers_token);
	if (books_list) {
		lv_obj_clean(books_list);
	}
	for (int i = 0; i < book_count; i++) {
		if (books[i].has_cover) {
			cover_free(&books[i].cover);
		}
	}
	book_count = 0;
	free(books);
	books = NULL;
}

static int compare_entries(const void *a, const void *b) {
	return strcasecmp(((const book_entry_t *)a)->name, ((const book_entry_t *)b)->name);
}

static void scan_folder(void) {
	snprintf(books_dir, sizeof(books_dir), "%s/Ebook", g_cfg->sd_root_path ? g_cfg->sd_root_path : "");

	DIR *d = opendir(books_dir);
	if (!d) {
		return;
	}
	if (!books) {
		books = calloc(BOOKS_MAX, sizeof(book_entry_t));
	}
	if (!books) {
		closedir(d);
		return;
	}

	struct dirent *de;
	while ((de = readdir(d)) != NULL && book_count < BOOKS_MAX) {
		size_t len = strlen(de->d_name);
		if (len < 6 || len >= sizeof(books[0].file) || strcasecmp(de->d_name + len - 5, ".epub") != 0) {
			continue;
		}
		char path[800];
		snprintf(path, sizeof(path), "%s/%s", books_dir, de->d_name);
		int marks = bookmark_count(path);
		if (marks <= 0) {
			continue; // a book nobody has marked is not on this page
		}

		book_entry_t *entry = &books[book_count];
		memset(entry, 0, sizeof(*entry));
		memcpy(entry->file, de->d_name, len + 1);
		snprintf(entry->name, sizeof(entry->name), "%.*s", (int)(len - 5), de->d_name);
		entry->count = marks;
		book_count++;
	}
	closedir(d);

	qsort(books, (size_t)book_count, sizeof(book_entry_t), compare_entries);
}

static void build_rows(void) {
	for (int i = 0; i < book_count; i++) {
		book_entry_t *entry = &books[i];

		lv_obj_t *row = lv_btn_create(books_list);
		lv_obj_set_width(row, lv_pct(100));
		lv_obj_set_height(row, LV_SIZE_CONTENT);
		lv_obj_add_style(row, &theme_style_card, 0);
		lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_set_style_radius(row, 12, 0);
		lv_obj_set_style_border_width(row, 0, 0);
		lv_obj_set_style_shadow_width(row, 0, 0);
		lv_obj_set_style_pad_all(row, 14, 0);
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_column(row, 14, 0);
		lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
		lv_obj_add_event_cb(row, open_book_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

		// The cover's place, held open whether or not one arrives: rows that
		// jump sideways as the pictures land are worse than rows that start
		// with a grey rectangle.
		lv_obj_t *plate = lv_obj_create(row);
		lv_obj_remove_style_all(plate);
		lv_obj_set_size(plate, ROW_COVER_W, ROW_COVER_H);
		lv_obj_set_style_bg_color(plate, theme()->screen_bg, 0);
		lv_obj_set_style_bg_opa(plate, LV_OPA_COVER, 0);
		lv_obj_set_style_radius(plate, 6, 0);
		lv_obj_remove_flag(plate, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_add_flag(plate, LV_OBJ_FLAG_EVENT_BUBBLE);

		lv_obj_t *glyph = lv_image_create(plate);
		lv_image_set_src(glyph, &icon_ebook_cover_small);
		lv_obj_center(glyph);
		lv_obj_set_style_image_recolor(glyph, theme()->text_secondary, 0);
		lv_obj_set_style_image_recolor_opa(glyph, LV_OPA_COVER, 0);
		lv_obj_set_style_image_opa(glyph, LV_OPA_50, 0);

		entry->image = lv_image_create(plate);
		lv_obj_center(entry->image);
		lv_obj_add_flag(entry->image, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(entry->image, LV_OBJ_FLAG_EVENT_BUBBLE);

		lv_obj_t *column = lv_obj_create(row);
		lv_obj_remove_style_all(column);
		lv_obj_set_height(column, LV_SIZE_CONTENT);
		lv_obj_set_flex_grow(column, 1);
		lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_style_pad_row(column, 6, 0);
		lv_obj_remove_flag(column, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_add_flag(column, LV_OBJ_FLAG_EVENT_BUBBLE);

		entry->label = lv_label_create(column);
		lv_label_set_long_mode(entry->label, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(entry->label, lv_pct(100));
		lv_label_set_text(entry->label, entry->name);
		lv_obj_add_style(entry->label, &theme_style_text, 0);
		lv_obj_set_style_text_font(entry->label, &font_ui_22, 0);
		lv_obj_add_flag(entry->label, LV_OBJ_FLAG_EVENT_BUBBLE);

		lv_obj_t *count = lv_label_create(column);
		lv_label_set_text_fmt(count, "%d %s", entry->count,
							  tr(entry->count == 1 ? "ebookmarks_bookmark_one" : "ebookmarks_bookmark_many"));
		lv_obj_add_style(count, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(count, &font_ui_18, 0);
		lv_obj_add_flag(count, LV_OBJ_FLAG_EVENT_BUBBLE);
	}
}

#define COVER_LOOKAHEAD 720

static void want_visible_covers(void) {
	if (!books || !book_count || !books_container) {
		return;
	}
	lv_obj_t **rows = calloc((size_t)book_count, sizeof(*rows));
	if (!rows) {
		return;
	}
	for (int i = 0; i < book_count; i++) {
		// The row is the cover's parent's parent; kept here so this page asks
		// the same question the shelf does.
		rows[i] = books[i].image ? lv_obj_get_parent(lv_obj_get_parent(books[i].image)) : NULL;
	}
	ebookcovers_want_visible(covers_token, books_container, rows, book_count, COVER_LOOKAHEAD);
	free(rows);
}

static void books_scrolled_cb(lv_event_t *e) {
	(void)e;
	want_visible_covers();
}

static void covers_begin(void) {
	if (!book_count || !books) {
		return;
	}
	char(*files)[256] = calloc((size_t)book_count, sizeof(files[0]));
	if (!files) {
		return;
	}
	for (int i = 0; i < book_count; i++) {
		memcpy(files[i], books[i].file, sizeof(files[0]));
	}
	covers_token =
		ebookcovers_start(books_dir, (const char(*)[256])files, book_count, ROW_COVER_W, ROW_COVER_H, cover_ready);
	free(files);
	want_visible_covers();
}

void ebookmarkspage_open(void) { switch_screen(ebookmarkspage_screen); }

// Built on arrival rather than in ebookmarkspage_open(), because arriving is not
// only what the shelf's button does: coming back from one book's marks lands
// here too, and a bookmark deleted in between has to be gone from the count.
static void books_loaded_cb(lv_event_t *e) {
	(void)e;
	books_free();
	scan_folder();

	if (book_count) {
		lv_obj_add_flag(books_empty, LV_OBJ_FLAG_HIDDEN);
		build_rows();
		covers_begin();
	} else {
		lv_obj_remove_flag(books_empty, LV_OBJ_FLAG_HIDDEN);
	}
}

static void books_unloaded_cb(lv_event_t *e) {
	(void)e;
	books_free();
}

void ebookmarkspage_init(gui_config_t *cfg) {
	g_cfg = cfg;

	// --- the books
	ebookmarkspage_screen = lv_obj_create(NULL);
	lv_obj_add_style(ebookmarkspage_screen, &theme_style_screen, 0);

	lv_obj_t *container = settingsrow_page(ebookmarkspage_screen, cfg, "bookmarks");
	books_container = container;
	lv_obj_add_event_cb(container, books_scrolled_cb, LV_EVENT_SCROLL, NULL);
	lv_obj_add_event_cb(container, books_scrolled_cb, LV_EVENT_SCROLL_END, NULL);
	lv_obj_set_style_pad_row(container, 10, 0);

	books_list = lv_obj_create(container);
	lv_obj_remove_style_all(books_list);
	lv_obj_set_size(books_list, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(books_list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(books_list, 10, 0);
	lv_obj_remove_flag(books_list, LV_OBJ_FLAG_SCROLLABLE);
	// One step at a time: a row hands the press to this, and this to the
	// container, which is where the swipe that goes back is watched.
	lv_obj_add_flag(books_list, LV_OBJ_FLAG_EVENT_BUBBLE);

	books_empty = lv_label_create(container);
	lv_label_set_long_mode(books_empty, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(books_empty, lv_pct(100));
	lv_obj_add_style(books_empty, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(books_empty, &font_ui_20, 0);
	lv_obj_set_style_text_align(books_empty, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_text(books_empty, tr("ebookmarks_empty_note"));
	lv_obj_add_flag(books_empty, LV_OBJ_FLAG_HIDDEN);

	lv_obj_add_event_cb(ebookmarkspage_screen, books_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(ebookmarkspage_screen, books_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(ebookmarkspage_screen);
	switcher_attach_back_gesture(container);
	player_sheet_attach_drag(container, true);

	// --- one book's marks
	ebookmarkbookpage_screen = lv_obj_create(NULL);
	lv_obj_add_style(ebookmarkbookpage_screen, &theme_style_screen, 0);

	lv_obj_t *book_container = settingsrow_page(ebookmarkbookpage_screen, cfg, "bookmarks");
	lv_obj_set_style_pad_row(book_container, 10, 0);
	// The heading says which book, so it is rewritten every time one is opened.
	marks_title = settingsrow_page_title(ebookmarkbookpage_screen);

	marks_list = lv_obj_create(book_container);
	lv_obj_remove_style_all(marks_list);
	lv_obj_set_size(marks_list, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(marks_list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(marks_list, 10, 0);
	lv_obj_remove_flag(marks_list, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(marks_list, LV_OBJ_FLAG_EVENT_BUBBLE);

	// Built on arrival and not when a book is picked: coming back here from the
	// reader is an arrival too, and a bookmark saved in between has to be in
	// the list.
	lv_obj_add_event_cb(ebookmarkbookpage_screen, marks_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(ebookmarkbookpage_screen);
	switcher_attach_back_gesture(book_container);
	player_sheet_attach_drag(book_container, true);
}

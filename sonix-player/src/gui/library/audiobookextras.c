#include "audiobookextras.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/library/audiobooks.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/uiscale.h"
#include "src/system/core/lang.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/playback/audiobook.h"
#include "src/system/playback/device_state.h"

static lv_obj_t *books_screen;	 // the books that have bookmarks
static lv_obj_t *marks_screen;	 // one book's
static lv_obj_t *summary_screen; // one book's summary

static lv_obj_t *books_list;
static lv_obj_t *books_empty;
static lv_obj_t *marks_list;
static lv_obj_t *marks_title;
static lv_obj_t *marks_empty;
static lv_obj_t *summary_book;
static lv_obj_t *summary_author;
static lv_obj_t *summary_text;

// The book the marks page and the summary page are about.
static char open_book[512];
static char open_title[256];

// The marks page was opened from the player's menu: a mark picked there is
// heard in the player, which the page then goes back to.
static bool marks_from_player;

static void format_clock(double seconds, char *out, size_t size) {
	if (seconds < 0) {
		seconds = 0;
	}
	long total = (long)seconds;
	long hours = total / 3600;
	long minutes = (total % 3600) / 60;
	long secs = total % 60;
	if (hours > 0) {
		snprintf(out, size, "%ld:%02ld:%02ld", hours, minutes, secs);
	} else {
		snprintf(out, size, "%ld:%02ld", minutes, secs);
	}
}

// A card row in a column list, holding a column of labels.
static lv_obj_t *make_card(lv_obj_t *parent) {
	lv_obj_t *row = lv_btn_create(parent);
	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_height(row, LV_SIZE_CONTENT);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, ui_px(12), 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, ui_px(18), 0);
	lv_obj_set_style_min_height(row, ui_px(88), 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_row(row, ui_px(6), 0);
	lv_obj_set_event_bubble(row, true);
	return row;
}

static lv_obj_t *card_line(lv_obj_t *row, const char *text, bool dim) {
	lv_obj_t *label = lv_label_create(row);
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(label, lv_pct(100));
	lv_label_set_text(label, text);
	lv_obj_add_style(label, dim ? &theme_style_text_dim : &theme_style_text, 0);
	lv_obj_set_style_text_font(label, dim ? &font_ui_18 : &font_ui_22, 0);
	lv_obj_set_event_bubble(label, true);
	return label;
}

static lv_obj_t *empty_note(lv_obj_t *parent, const char *text) {
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_add_style(label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(label, &font_ui_20, 0);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_pad_top(label, ui_px(60), 0);
	lv_label_set_text(label, tr(text));
	lv_obj_set_hidden(label, true);
	return label;
}

static lv_obj_t *column_list(lv_obj_t *container) {
	lv_obj_t *list = lv_obj_create(container);
	lv_obj_remove_style_all(list);
	lv_obj_set_size(list, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(list, ui_px(10), 0);
	lv_obj_set_scrollable(list, false);
	// One step at a time: a row hands the press to this, and this to the
	// container, which is where the swipe that goes back is watched.
	lv_obj_set_event_bubble(list, true);
	return list;
}

// ---------------------------------------------------------------------------
// one book's bookmarks
// ---------------------------------------------------------------------------

typedef struct {
	long long id;
	char file[512];
	double seconds;
} mark_t;

#define MARKS_MAX 64

static mark_t *marks;
static int mark_count;

// True from a long press until the release it belongs to has gone by: LVGL
// sends LV_EVENT_CLICKED on release after a long press too, which would play
// the mark behind the menu that just opened.
static bool swallow_next_click;

// Which one the menu is about. The menu closes before its action runs, so
// the row cannot be asked any more by then.
static int menu_mark = -1;

static void build_marks(void);

// The mark is heard where it was picked: in the player when the page came
// from there, and the player comes up when it came from the library. A mark in
// the file already playing is a seek; anything else starts the book there.
static void mark_pick_cb(lv_event_t *e) {
	if (swallow_next_click) {
		swallow_next_click = false;
		return;
	}
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (!marks || index < 0 || index >= mark_count) {
		return;
	}
	const mark_t *mark = &marks[index];

	char book[512] = "";
	char file[512] = "";
	if (audiobook_current_book(book, sizeof(book), file, sizeof(file)) && strcmp(book, open_book) == 0 && strcmp(file, mark->file) == 0) {
		device_state_seek(mark->seconds);
		audiobook_note_position(mark->seconds, 0, true);
		player_refresh_now_playing();
	} else {
		audiobooks_play_at(open_book, mark->file, mark->seconds);
	}

	if (marks_from_player) {
		back_btn_cb(NULL); // back to the player, which is where the jump is heard
	} else {
		player_sheet_open(true);
	}
}

static void delete_mark_action(void *user) {
	(void)user;
	if (!marks || menu_mark < 0 || menu_mark >= mark_count) {
		return;
	}
	if (audiobookdb_bookmark_remove(marks[menu_mark].id)) {
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

static bool collect_mark(long long id, const char *file, double seconds, const char *label, void *user) {
	(void)user;
	if (mark_count >= MARKS_MAX) {
		return false;
	}
	mark_t *mark = &marks[mark_count];
	mark->id = id;
	snprintf(mark->file, sizeof(mark->file), "%s", file);
	mark->seconds = seconds;

	lv_obj_t *row = make_card(marks_list);
	lv_obj_add_event_cb(row, mark_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)mark_count);
	lv_obj_add_event_cb(row, mark_long_pressed_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)mark_count);

	// The chapter or the part the mark falls in is what makes the list read as
	// places; the time under it says where in there. A book with neither shows
	// the time alone, on the first line.
	char clock[24];
	format_clock(seconds, clock, sizeof(clock));
	if (label && label[0]) {
		card_line(row, label, false);
		card_line(row, clock, true);
	} else {
		card_line(row, clock, false);
	}

	mark_count++;
	return true;
}

static void build_marks(void) {
	lv_obj_clean(marks_list);
	mark_count = 0;
	menu_mark = -1;
	if (marks_title) {
		lv_label_set_text(marks_title, open_title[0] ? open_title : tr("bookmarks"));
	}
	if (!marks) {
		marks = calloc(MARKS_MAX, sizeof(*marks));
	}
	if (marks) {
		audiobookdb_bookmarks_for_each(open_book, collect_mark, NULL);
	}
	if (mark_count > 0) {
		lv_obj_set_hidden(marks_empty, true);
	} else {
		lv_obj_set_hidden(marks_empty, false);
	}
}

// Built on arrival, not when a book is picked: a mark deleted, or added from
// the player in between, has to show.
static void marks_loaded_cb(lv_event_t *e) {
	(void)e;
	build_marks();
}

static void marks_unloaded_cb(lv_event_t *e) {
	(void)e;
	lv_obj_clean(marks_list);
	free(marks);
	marks = NULL;
	mark_count = 0;
}

// The heading: the book's title, or the file's name for a book the index has
// no row for.
static void remember_book(const char *book) {
	snprintf(open_book, sizeof(open_book), "%s", book);
	open_title[0] = '\0';
	audiobookdb_book_info(book, open_title, sizeof(open_title), NULL, 0, NULL);
	if (!open_title[0]) {
		const char *slash = strrchr(book, '/');
		snprintf(open_title, sizeof(open_title), "%.*s", (int)sizeof(open_title) - 1, slash ? slash + 1 : book);
	}
}

// ---------------------------------------------------------------------------
// the books that have any
// ---------------------------------------------------------------------------

typedef struct {
	char book[512];
	char title[256];
} marked_book_t;

static marked_book_t *marked;
static int marked_count;
static int marked_capacity;

static void open_marked_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= marked_count) {
		return;
	}
	snprintf(open_book, sizeof(open_book), "%s", marked[index].book);
	snprintf(open_title, sizeof(open_title), "%s", marked[index].title);
	marks_from_player = false;
	switch_screen(marks_screen);
}

static bool collect_book(const char *title, const char *book, int count, void *user) {
	(void)user;
	if (marked_count == marked_capacity) {
		int grown = marked_capacity ? marked_capacity * 2 : 16;
		marked_book_t *bigger = realloc(marked, (size_t)grown * sizeof(*bigger));
		if (!bigger) {
			return false;
		}
		marked = bigger;
		marked_capacity = grown;
	}
	marked_book_t *entry = &marked[marked_count];
	snprintf(entry->book, sizeof(entry->book), "%s", book);
	snprintf(entry->title, sizeof(entry->title), "%s", title);

	lv_obj_t *row = make_card(books_list);
	lv_obj_add_event_cb(row, open_marked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)marked_count);
	card_line(row, title, false);
	char line[64];
	snprintf(line, sizeof(line), "%d %s", count, tr(count == 1 ? "ebookmarks_bookmark_one" : "ebookmarks_bookmark_many"));
	card_line(row, line, true);

	marked_count++;
	return true;
}

static void books_free(void) {
	if (books_list) {
		lv_obj_clean(books_list);
	}
	free(marked);
	marked = NULL;
	marked_count = 0;
	marked_capacity = 0;
}

// Built on arrival, for the same reason as the marks: coming back from a book
// whose last mark was deleted lands here, and that book has to be gone.
static void books_loaded_cb(lv_event_t *e) {
	(void)e;
	books_free();
	audiobookdb_bookmarked_books_for_each(collect_book, NULL);
	if (marked_count > 0) {
		lv_obj_set_hidden(books_empty, true);
	} else {
		lv_obj_set_hidden(books_empty, false);
	}
}

static void books_unloaded_cb(lv_event_t *e) {
	(void)e;
	books_free();
}

void audiobookextras_open_bookmarks(void) { switch_screen(books_screen); }

// ---------------------------------------------------------------------------
// the summary
// ---------------------------------------------------------------------------

static void summary_fill(void) {
	char author[256] = "";
	char *summary = NULL;
	char title[256] = "";
	audiobookdb_book_info(open_book, title, sizeof(title), author, sizeof(author), &summary);

	lv_label_set_text(summary_book, title[0] ? title : open_title);
	if (author[0]) {
		lv_label_set_text(summary_author, author);
		lv_obj_set_hidden(summary_author, false);
	} else {
		lv_obj_set_hidden(summary_author, true);
	}
	if (summary) {
		lv_label_set_text(summary_text, summary);
		lv_obj_remove_style(summary_text, &theme_style_text_dim, 0);
		lv_obj_add_style(summary_text, &theme_style_text, 0);
		free(summary);
	} else {
		// A book scanned before summaries were read has none yet either; the
		// rescan the section offers on its next opening fills it in.
		lv_label_set_text(summary_text, tr("audiobook_summary_empty"));
		lv_obj_remove_style(summary_text, &theme_style_text, 0);
		lv_obj_add_style(summary_text, &theme_style_text_dim, 0);
	}
}

static void summary_loaded_cb(lv_event_t *e) {
	(void)e;
	summary_fill();
	lv_obj_scroll_to_y(lv_obj_get_parent(summary_text), 0, LV_ANIM_OFF);
}

// Big summaries are kilobytes of text in a label; it is let go when the page
// is left.
static void summary_unloaded_cb(lv_event_t *e) {
	(void)e;
	lv_label_set_text(summary_text, "");
}

// ---------------------------------------------------------------------------
// from the player
// ---------------------------------------------------------------------------

void audiobookextras_add_bookmark(void) {
	char book[512];
	char file[512];
	if (!audiobook_current_book(book, sizeof(book), file, sizeof(file))) {
		return;
	}
	device_state_t state;
	device_state_get(&state);
	double seconds = state.progress_current_secs;

	char label[192];
	audiobook_place_label(seconds, label, sizeof(label));
	switch (audiobookdb_bookmark_add(book, file, seconds, label)) {
	case AUDIOBOOK_BOOKMARK_SAVED:
		toast_glyph(&icon_bookmark_check, "ebookreader_bookmark_saved");
		break;
	case AUDIOBOOK_BOOKMARK_FULL:
		toast_error("audiobook_bookmarks_full");
		break;
	default:
		toast_error("audiobook_bookmark_failed");
		break;
	}
}

void audiobookextras_open_current_bookmarks(void) {
	char book[512];
	if (!audiobook_current_book(book, sizeof(book), NULL, 0)) {
		return;
	}
	remember_book(book);
	marks_from_player = true;
	player_sheet_close(false);
	switch_screen(marks_screen);
	switcher_set_player_return(marks_screen);
}

void audiobookextras_open_current_summary(void) {
	char book[512];
	if (!audiobook_current_book(book, sizeof(book), NULL, 0)) {
		return;
	}
	remember_book(book);
	player_sheet_close(false);
	switch_screen(summary_screen);
	switcher_set_player_return(summary_screen);
}

// ---------------------------------------------------------------------------

void audiobookextras_init(gui_config_t *cfg) {
	// --- the books that have bookmarks
	books_screen = lv_obj_create(NULL);
	lv_obj_add_style(books_screen, &theme_style_screen, 0);
	lv_obj_t *container = settingsrow_page(books_screen, cfg, "bookmarks");
	lv_obj_set_style_pad_row(container, ui_px(10), 0);
	books_list = column_list(container);
	books_empty = empty_note(container, "audiobook_bookmarks_empty");
	lv_obj_add_event_cb(books_screen, books_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(books_screen, books_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(books_screen);
	switcher_attach_back_gesture(container);
	player_sheet_attach_drag(container, true);

	// --- one book's
	marks_screen = lv_obj_create(NULL);
	lv_obj_add_style(marks_screen, &theme_style_screen, 0);
	container = settingsrow_page(marks_screen, cfg, "bookmarks");
	lv_obj_set_style_pad_row(container, ui_px(10), 0);
	// The heading says which book, so it is rewritten every time one opens.
	marks_title = settingsrow_page_title(marks_screen);
	marks_list = column_list(container);
	marks_empty = empty_note(container, "audiobook_bookmarks_empty");
	lv_obj_add_event_cb(marks_screen, marks_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(marks_screen, marks_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	// No player-sheet swipe: the page is often reached from the player, and
	// the chapters page leaves it out for the same reason.
	switcher_attach_back_gesture(marks_screen);
	switcher_attach_back_gesture(container);

	// --- the summary
	summary_screen = lv_obj_create(NULL);
	lv_obj_add_style(summary_screen, &theme_style_screen, 0);
	container = settingsrow_page(summary_screen, cfg, "audiobook_summary");
	lv_obj_set_style_pad_row(container, ui_px(8), 0);

	summary_book = lv_label_create(container);
	lv_label_set_long_mode(summary_book, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(summary_book, lv_pct(100));
	lv_obj_add_style(summary_book, &theme_style_text, 0);
	lv_obj_set_style_text_font(summary_book, &font_ui_24, 0);

	summary_author = lv_label_create(container);
	lv_label_set_long_mode(summary_author, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(summary_author, lv_pct(100));
	lv_obj_add_style(summary_author, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(summary_author, &font_ui_20, 0);

	summary_text = lv_label_create(container);
	lv_label_set_long_mode(summary_text, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(summary_text, lv_pct(100));
	lv_obj_add_style(summary_text, &theme_style_text, 0);
	lv_obj_set_style_text_font(summary_text, &font_ui_20, 0);
	lv_obj_set_style_text_line_space(summary_text, ui_px(4), 0);
	lv_obj_set_style_pad_top(summary_text, ui_px(12), 0);
	lv_obj_set_event_bubble(summary_text, true);

	lv_obj_add_event_cb(summary_screen, summary_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(summary_screen, summary_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(summary_screen);
	switcher_attach_back_gesture(container);
}

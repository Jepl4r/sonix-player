#include "ebooksettings.h"

#include <stdio.h>

#include "src/gui/ebook/ebookbar.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"

lv_obj_t *ebooksettings_screen;

// ---------------------------------------------------------------------------
// What goes along the bottom of a page of a book
//
// The switches, and above them a piece of paper showing the strip as it will
// look. The preview is built by ebookbar.c, the same code that draws the real
// one -- a preview drawn separately is a preview that lies the day one of the
// two changes.
//
// The paper does not scroll with the switches. It is what the page is for:
// every switch changes it, and a switch whose effect has just scrolled off the
// top is a switch nobody can judge. So the paper is pinned under the heading
// and the switches scroll underneath it.
//
// The numbers in it are made up on purpose: chapter three of twelve, page
// seven, two thirds through the chapter and a bit over a third through the
// book. A preview of the book that is open would be a preview nobody can read
// while no book is open, which is when this page is reached.
// ---------------------------------------------------------------------------

#define PREVIEW_CHAPTER 2u
#define PREVIEW_CHAPTERS 12u
#define PREVIEW_BOOK_PERCENT 37
#define PREVIEW_CHAPTER_PERCENT 68
#define PREVIEW_PAGE 7

static ebookbar_t preview;
static lv_obj_t *preview_paper;
static lv_obj_t *option_list;
static lv_obj_t *switches[EBOOKBAR_COUNT];
static lv_obj_t *scope_pills[EBOOKBAR_SCOPE_COUNT];
static lv_obj_t *scope_row;
static int list_top;	 // where the paper starts, and so where the list starts from
static int32_t list_bottom;	 // the bottom of the screen, which is where the list ends

// The preview sits on paper, not on the page's own background: what the strip
// looks like depends on what is behind it, and behind it is a book.
#define PAPER_COLOUR 0xFBFBF8
#define PAPER_INK 0x1A1A18

// Keeps the scrolling part under the paper. The paper's height follows what is
// switched on -- the progress line and the line of text each add to it -- so
// the top of the list is not a constant and has to be read back after a change.
static void relayout(void) {
	if (!option_list || !preview_paper) {
		return;
	}
	lv_obj_update_layout(preview_paper);
	int32_t top = list_top + lv_obj_get_height(preview_paper);
	lv_obj_set_y(option_list, top);
	lv_obj_set_height(option_list, list_bottom - top);
}

static void refresh_preview(void) {
	ebookbar_state_t state = {
		.chapter = PREVIEW_CHAPTER,
		.chapters = PREVIEW_CHAPTERS,
		.book_percent = PREVIEW_BOOK_PERCENT,
		.chapter_percent = PREVIEW_CHAPTER_PERCENT,
		.page = PREVIEW_PAGE,
		.ink = lv_color_hex(PAPER_INK),
	};
	ebookbar_refresh(&preview, &state);
	relayout();
}

// The pills belong to the progress line and are shown only while it is on:
// asking what a line that is not drawn should measure is a question with no
// answer.
static void scope_refresh(void) {
	ebookbar_scope_t chosen = ebookbar_scope();
	for (int i = 0; i < EBOOKBAR_SCOPE_COUNT; i++) {
		if (scope_pills[i]) {
			settingsrow_pill_active(scope_pills[i], i == (int)chosen);
		}
	}
	if (scope_row) {
		if (ebookbar_option(EBOOKBAR_PROGRESS)) {
			lv_obj_remove_flag(scope_row, LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_add_flag(scope_row, LV_OBJ_FLAG_HIDDEN);
		}
	}
}

// Which switch moved, worked out from the object rather than from user data:
// settingsrow_toggle() attaches the callback itself and passes none, and adding
// a second registration to carry an index would run this twice per press.
static void option_cb(lv_event_t *e) {
	lv_obj_t *target = lv_event_get_target(e);
	for (int i = 0; i < EBOOKBAR_COUNT; i++) {
		if (switches[i] == target) {
			ebookbar_set_option((ebookbar_option_t)i, lv_obj_has_state(target, LV_STATE_CHECKED));
			scope_refresh();
			refresh_preview();
			return;
		}
	}
}

static void scope_cb(lv_event_t *e) {
	ebookbar_set_scope((ebookbar_scope_t)(intptr_t)lv_event_get_user_data(e));
	scope_refresh();
	refresh_preview();
}

// The switches are read from the config every time the page is shown, because
// nothing else on the device writes them but the page can be left and returned
// to without being rebuilt.
static void loaded_cb(lv_event_t *e) {
	(void)e;
	for (int i = 0; i < EBOOKBAR_COUNT; i++) {
		if (!switches[i]) {
			continue;
		}
		if (ebookbar_option((ebookbar_option_t)i)) {
			lv_obj_add_state(switches[i], LV_STATE_CHECKED);
		} else {
			lv_obj_remove_state(switches[i], LV_STATE_CHECKED);
		}
	}
	scope_refresh();
	refresh_preview();
}

// The pills are painted from the palette, so a theme change has to repaint the
// chosen one.
static void refresh_theme(void) { scope_refresh(); }

// Every page built by settingsrow_page() reopens at the top; this one builds
// its own body, so it has to do that itself. On unload and not on load, for the
// reason settingsrow.c gives: on load it would race the rebuild.
static void unloaded_cb(lv_event_t *e) {
	(void)e;
	if (option_list) {
		lv_obj_scroll_to_y(option_list, 0, LV_ANIM_OFF);
	}
}

void ebooksettings_init(gui_config_t *cfg) {
	ebooksettings_screen = lv_obj_create(NULL);
	lv_obj_add_style(ebooksettings_screen, &theme_style_screen, 0);
	settingsrow_title(ebooksettings_screen, cfg, "ebooksettings_reading_bar");

	list_top = settingsrow_content_top(cfg);
	list_bottom = (int32_t)cfg->screen_height;

	// --- the paper, with the strip on it: a child of the screen and not of the
	// list, which is what keeps it still while the list moves under it.
	preview_paper = lv_obj_create(ebooksettings_screen);
	lv_obj_remove_style_all(preview_paper);
	lv_obj_set_size(preview_paper, (int32_t)cfg->screen_width - 2 * cfg->padding, LV_SIZE_CONTENT);
	lv_obj_set_pos(preview_paper, cfg->padding, list_top);
	lv_obj_set_style_bg_color(preview_paper, lv_color_hex(PAPER_COLOUR), 0);
	lv_obj_set_style_bg_opa(preview_paper, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(preview_paper, 12, 0);
	lv_obj_set_style_pad_all(preview_paper, 14, 0);
	lv_obj_set_style_pad_row(preview_paper, 10, 0);
	lv_obj_remove_flag(preview_paper, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(preview_paper, LV_FLEX_FLOW_COLUMN);
	lv_obj_add_flag(preview_paper, LV_OBJ_FLAG_EVENT_BUBBLE);

	// Two lines of a book above it, so the strip is seen where it lives rather
	// than floating in a box of its own.
	for (int i = 0; i < 2; i++) {
		lv_obj_t *line = lv_obj_create(preview_paper);
		lv_obj_remove_style_all(line);
		lv_obj_set_size(line, lv_pct(i == 0 ? 100 : 72), 10);
		lv_obj_set_style_radius(line, 5, 0);
		lv_obj_set_style_bg_color(line, lv_color_hex(PAPER_INK), 0);
		lv_obj_set_style_bg_opa(line, LV_OPA_20, 0);
		lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
	}

	ebookbar_create(&preview, preview_paper, (int32_t)cfg->screen_width - 2 * cfg->padding - 28);

	// --- and the switches, in what scrolls
	option_list = lv_obj_create(ebooksettings_screen);
	lv_obj_set_width(option_list, lv_pct(100));
	lv_obj_set_x(option_list, 0);
	lv_obj_set_style_bg_opa(option_list, 0, 0);
	lv_obj_set_style_border_width(option_list, 0, 0);
	lv_obj_set_style_radius(option_list, 0, 0);
	lv_obj_set_style_pad_hor(option_list, cfg->padding, 0);
	lv_obj_set_style_pad_top(option_list, 12, 0);
	lv_obj_set_flex_flow(option_list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(option_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_gap(option_list, 12, 0);

	for (int i = 0; i < EBOOKBAR_COUNT; i++) {
		if (i == EBOOKBAR_PROGRESS) {
			// The one option that is not only on or off: a line has to measure
			// something, and the two things it can measure are not the same
			// question.
			settingsrow_toggle_pills(option_list, ebookbar_option_tag((ebookbar_option_t)i), option_cb, &switches[i],
									 &scope_row);
			for (int s = 0; s < EBOOKBAR_SCOPE_COUNT; s++) {
				scope_pills[s] = settingsrow_pill(scope_row, ebookbar_scope_tag((ebookbar_scope_t)s), s, scope_cb);
			}
			continue;
		}
		settingsrow_toggle(option_list, ebookbar_option_tag((ebookbar_option_t)i), &switches[i], option_cb);
	}

	relayout();

	lv_obj_add_event_cb(ebooksettings_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(ebooksettings_screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(ebooksettings_screen);
	switcher_attach_back_gesture(option_list);
	player_sheet_attach_drag(option_list, true);
	theme_register_refresh(refresh_theme);
}

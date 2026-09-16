#include "src/gui/ebook/ebookbar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/system/core/config.h"
#include "src/system/playback/device_state.h"
#include "src/system/core/lang.h"

// See ebookbar.h for why the preview and the real strip are the same code.

#define BAR_TEXT_HEIGHT 22
#define BAR_LINE_HEIGHT 3
#define BAR_GAP 5

// The battery bitmap is rendered from the same 24x24 SVG the status bar uses,
// at the size that suits 16-point text rather than 24-point.
#define BATTERY_ICON_SIZE 26

// The hole inside the shell, in the icon's own pixels. Read off the rendered
// bitmap rather than scaled from the SVG's coordinates: at 26 px the SVG
// coordinates do not divide evenly, and the truncation offsets the fill from
// the hole by a pixel.
//
// Re-measure these four if BATTERY_ICON_SIZE changes: decode icon_battery_small
// and find the fully transparent rectangle inside the outline.
#define BATTERY_CAVITY_X 4
#define BATTERY_CAVITY_Y 8
#define BATTERY_CAVITY_W 14
#define BATTERY_CAVITY_H 10

// What goes between two facts on the strip, the same as the player puts
// between two facts about a track.
#define BAR_SEPARATOR "  \xC2\xB7  "

// The names, in their own plain array. tools/extract_strings.py finds a tag
// that reaches tr() through an index only by being told the array's name (its
// TABLES list), and it can read an array of strings but not a field of an array
// of structs, so these cannot live inside OPTIONS below.
static const char *const BAR_TAGS[EBOOKBAR_COUNT] = {
	"ebookbar_bar_show_chapter", "ebookbar_bar_show_progress", "ebookbar_bar_show_page", "ebookbar_bar_show_battery", "ebookbar_bar_show_clock",
};

// And the two the progress line can measure, for the same reason.
static const char *const SCOPE_TAGS[EBOOKBAR_SCOPE_COUNT] = {"ebookbar_book", "chapter"};

// What each option is called on disk, and whether it starts on. One table, so
// adding a sixth thing to the strip is one line here and one above.
//
// The progress line starts on because it is what a reader who has changed
// nothing sees of where they are.
static const struct {
	const char *key;
	bool on_by_default;
} OPTIONS[EBOOKBAR_COUNT] = {
	{"bar_chapter", true}, {"bar_progress", true}, {"bar_page", false}, {"bar_battery", false},
	{"bar_clock", false},
};

const char *ebookbar_option_tag(ebookbar_option_t which) {
	return which < EBOOKBAR_COUNT ? BAR_TAGS[which] : "";
}

bool ebookbar_option(ebookbar_option_t which) {
	if (which >= EBOOKBAR_COUNT) {
		return false;
	}
	return config_store_get_int(config_ebook_store(), "ebook", OPTIONS[which].key,
							    OPTIONS[which].on_by_default ? 1 : 0) != 0;
}

void ebookbar_set_option(ebookbar_option_t which, bool on) {
	if (which >= EBOOKBAR_COUNT) {
		return;
	}
	config_store_set_int(config_ebook_store(), "ebook", OPTIONS[which].key, on ? 1 : 0);
	config_store_save(config_ebook_store());
}

const char *ebookbar_scope_tag(ebookbar_scope_t scope) {
	return scope < EBOOKBAR_SCOPE_COUNT ? SCOPE_TAGS[scope] : "";
}

ebookbar_scope_t ebookbar_scope(void) {
	int stored = config_store_get_int(config_ebook_store(), "ebook", "bar_progress_scope", EBOOKBAR_SCOPE_BOOK);
	return (stored == EBOOKBAR_SCOPE_CHAPTER) ? EBOOKBAR_SCOPE_CHAPTER : EBOOKBAR_SCOPE_BOOK;
}

void ebookbar_set_scope(ebookbar_scope_t scope) {
	if (scope >= EBOOKBAR_SCOPE_COUNT) {
		return;
	}
	config_store_set_int(config_ebook_store(), "ebook", "bar_progress_scope", (int)scope);
	config_store_save(config_ebook_store());
}

// Whether anything is drawn on the line of text, the battery included: the
// battery is taller than the words but sits on the same line, so it does not
// add a second one.
static bool any_text_option(void) {
	return ebookbar_option(EBOOKBAR_CHAPTER) || ebookbar_option(EBOOKBAR_PAGE) ||
		   ebookbar_option(EBOOKBAR_BATTERY) || ebookbar_option(EBOOKBAR_CLOCK);
}

int32_t ebookbar_height(void) {
	int32_t height = 0;
	if (any_text_option()) {
		// The battery is the tallest thing that can be on this line, so with it
		// switched on the line is as tall as it is.
		height += ebookbar_option(EBOOKBAR_BATTERY) ? BATTERY_ICON_SIZE : BAR_TEXT_HEIGHT;
	}
	if (ebookbar_option(EBOOKBAR_PROGRESS)) {
		height += BAR_LINE_HEIGHT + (height ? BAR_GAP : 0);
	}
	// A page that gave the strip no room at all would have its last line
	// touching the bottom edge of the glass.
	return height ? height + BAR_GAP : BAR_GAP;
}

void ebookbar_create(ebookbar_t *bar, lv_obj_t *parent, int32_t width) {
	memset(bar, 0, sizeof(*bar));

	bar->root = lv_obj_create(parent);
	lv_obj_remove_style_all(bar->root);
	lv_obj_set_size(bar->root, width, LV_SIZE_CONTENT);
	lv_obj_remove_flag(bar->root, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_remove_flag(bar->root, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_flex_flow(bar->root, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(bar->root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(bar->root, BAR_GAP, 0);

	// The line is the width of the page and its fill is the part read. Two
	// objects rather than a bar widget: a slider or a bar brings a knob, a
	// pressed state and a theme of its own, none of which belong on paper.
	bar->line = lv_obj_create(bar->root);
	lv_obj_remove_style_all(bar->line);
	lv_obj_set_size(bar->line, width - 40, BAR_LINE_HEIGHT);
	lv_obj_set_style_radius(bar->line, BAR_LINE_HEIGHT / 2, 0);
	lv_obj_remove_flag(bar->line, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *fill = lv_obj_create(bar->line);
	lv_obj_remove_style_all(fill);
	lv_obj_set_height(fill, BAR_LINE_HEIGHT);
	lv_obj_set_style_radius(fill, BAR_LINE_HEIGHT / 2, 0);
	lv_obj_align(fill, LV_ALIGN_LEFT_MID, 0, 0);

	// The line of facts. A row rather than one label because the battery is a
	// picture and it belongs beside its own number, not before the chapter or
	// after the clock -- so the text is cut in two where the picture goes.
	bar->facts = lv_obj_create(bar->root);
	lv_obj_remove_style_all(bar->facts);
	lv_obj_set_size(bar->facts, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_remove_flag(bar->facts, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(bar->facts, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(bar->facts, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(bar->facts, 3, 0);

	bar->text = lv_label_create(bar->facts);
	lv_obj_set_style_text_font(bar->text, &font_ui_16, 0);
	lv_label_set_text(bar->text, "");

	// Fill first and shell over it, so the level shows through the hole in the
	// middle of the outline. The same order the status bar stacks them in.
	bar->battery = lv_obj_create(bar->facts);
	lv_obj_remove_style_all(bar->battery);
	lv_obj_set_size(bar->battery, BATTERY_ICON_SIZE, BATTERY_ICON_SIZE);
	lv_obj_remove_flag(bar->battery, LV_OBJ_FLAG_SCROLLABLE);

	bar->charge = lv_obj_create(bar->battery);
	lv_obj_remove_style_all(bar->charge);
	lv_obj_set_style_radius(bar->charge, 1, 0);

	lv_obj_t *shell = lv_image_create(bar->battery);
	lv_image_set_src(shell, &icon_battery_small);
	lv_obj_set_style_image_recolor_opa(shell, LV_OPA_COVER, 0);
	lv_obj_set_pos(shell, 0, 0);

	bar->tail = lv_label_create(bar->facts);
	lv_obj_set_style_text_font(bar->tail, &font_ui_16, 0);
	lv_label_set_text(bar->tail, "");
}

// The battery level as a number, or -1 when there is nothing to read. Taken as
// a number rather than printed as it stands: device_state puts "!!" in there
// when sysfs gave it nothing, and "!!%" on the bottom of a page of a book is
// not a battery reading, it is a puzzle.
static int battery_level(void) {
	device_state_refresh_battery();
	device_state_t state;
	device_state_get(&state);
	const char *text = state.battery_percent;
	int level = (text[0] >= '0' && text[0] <= '9') ? atoi(text) : -1;
	return level > 100 ? 100 : level;
}

static void append(char *line, size_t size, size_t *used, const char *text) {
	*used += (size_t)snprintf(line + *used, size - *used, "%s", text);
}

void ebookbar_refresh(ebookbar_t *bar, const ebookbar_state_t *state) {
	if (!bar || !bar->root || !state) {
		return;
	}

	// The line, measuring whichever of the two the reader asked for.
	if (ebookbar_option(EBOOKBAR_PROGRESS)) {
		int percent = (ebookbar_scope() == EBOOKBAR_SCOPE_CHAPTER) ? state->chapter_percent : state->book_percent;
		if (percent < 0) {
			percent = 0;
		}
		if (percent > 100) {
			percent = 100;
		}
		lv_obj_remove_flag(bar->line, LV_OBJ_FLAG_HIDDEN);
		lv_obj_set_style_bg_color(bar->line, state->ink, 0);
		lv_obj_set_style_bg_opa(bar->line, LV_OPA_20, 0);
		lv_obj_t *fill = lv_obj_get_child(bar->line, 0);
		if (fill) {
			lv_obj_set_width(fill, lv_pct(percent));
			lv_obj_set_style_bg_color(fill, state->ink, 0);
			lv_obj_set_style_bg_opa(fill, LV_OPA_70, 0);
		}
	} else {
		lv_obj_add_flag(bar->line, LV_OBJ_FLAG_HIDDEN);
	}

	// And the line of facts, in two pieces with the battery between them: what
	// comes before it, ending with its own level, and the clock after it.
	char head[96];
	char tail[32];
	size_t head_used = 0, tail_used = 0;
	head[0] = tail[0] = '\0';

	if (ebookbar_option(EBOOKBAR_CHAPTER) && state->chapters) {
		head_used += (size_t)snprintf(head + head_used, sizeof(head) - head_used, "%u/%u", state->chapter + 1u,
									  state->chapters);
	}
	if (ebookbar_option(EBOOKBAR_PAGE) && state->page > 0) {
		if (head_used) {
			append(head, sizeof(head), &head_used, BAR_SEPARATOR);
		}
		head_used += (size_t)snprintf(head + head_used, sizeof(head) - head_used, "%s %d", tr("ebookbar_page_short"),
									  state->page);
	}

	bool battery_on = ebookbar_option(EBOOKBAR_BATTERY);
	int level = battery_on ? battery_level() : -1;
	if (battery_on) {
		if (head_used) {
			append(head, sizeof(head), &head_used, BAR_SEPARATOR);
		}
		if (level >= 0) {
			head_used += (size_t)snprintf(head + head_used, sizeof(head) - head_used, "%d%%", level);
		} else {
			append(head, sizeof(head), &head_used, "--%");
		}
	}

	if (ebookbar_option(EBOOKBAR_CLOCK)) {
		time_t now = time(NULL);
		struct tm local;
		localtime_r(&now, &local);
		// The separator goes in front of the clock only when something is in
		// front of it -- the battery counts, and it is not in either string.
		if (head_used || battery_on) {
			append(tail, sizeof(tail), &tail_used, BAR_SEPARATOR);
		}
		tail_used += (size_t)snprintf(tail + tail_used, sizeof(tail) - tail_used, "%02d:%02d", local.tm_hour,
									  local.tm_min);
	}

	if (head[0]) {
		lv_obj_remove_flag(bar->text, LV_OBJ_FLAG_HIDDEN);
		lv_label_set_text(bar->text, head);
		lv_obj_set_style_text_color(bar->text, state->ink, 0);
		lv_obj_set_style_text_opa(bar->text, LV_OPA_50, 0);
	} else {
		lv_obj_add_flag(bar->text, LV_OBJ_FLAG_HIDDEN);
	}

	if (tail[0]) {
		lv_obj_remove_flag(bar->tail, LV_OBJ_FLAG_HIDDEN);
		lv_label_set_text(bar->tail, tail);
		lv_obj_set_style_text_color(bar->tail, state->ink, 0);
		lv_obj_set_style_text_opa(bar->tail, LV_OPA_50, 0);
	} else {
		lv_obj_add_flag(bar->tail, LV_OBJ_FLAG_HIDDEN);
	}

	if (!battery_on) {
		lv_obj_add_flag(bar->battery, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_remove_flag(bar->battery, LV_OBJ_FLAG_HIDDEN);
		lv_obj_t *shell = lv_obj_get_child(bar->battery, 1);
		if (shell) {
			lv_obj_set_style_image_recolor(shell, state->ink, 0);
			lv_obj_set_style_image_opa(shell, LV_OPA_50, 0);
		}
		// The level in the page's own ink rather than green: the strip is one
		// colour on paper, and a green battery on a sepia page is the only
		// thing on it that did not come out of the book.
		if (level <= 0) {
			lv_obj_add_flag(bar->charge, LV_OBJ_FLAG_HIDDEN);
		} else {
			int32_t width = (BATTERY_CAVITY_W * level) / 100;
			if (width < 2) {
				width = 2; // a nearly flat battery still has to be visible
			}
			lv_obj_remove_flag(bar->charge, LV_OBJ_FLAG_HIDDEN);
			lv_obj_set_pos(bar->charge, BATTERY_CAVITY_X, BATTERY_CAVITY_Y);
			lv_obj_set_size(bar->charge, width, BATTERY_CAVITY_H);
			lv_obj_set_style_bg_color(bar->charge, state->ink, 0);
			lv_obj_set_style_bg_opa(bar->charge, LV_OPA_50, 0);
		}
	}
}

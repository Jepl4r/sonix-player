#include "ebookreader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/system/ebook/bookmarks.h"
#include "src/system/ebook/ebook.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/ebook/ebookbar.h"
#include "src/gui/ebook/ebookfonts.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/topbar.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"

lv_obj_t *ebookreader_screen;

// ---------------------------------------------------------------------------
// A page of a book
//
// The chapter is in memory as blocks of styled text (see ebook.h); a page is
// however many of those blocks fit on the screen, and the reader keeps only
// the one page. Turning a page lays out the next one and throws the old
// objects away, so a chapter of four hundred paragraphs costs one screenful of
// LVGL objects rather than four hundred.
//
// Where a page ends is found by measuring, not guessed: each block is built as
// a spangroup, asked how tall it would be at the page's width, and added while
// there is room. A block taller than what is left is cut at the last word that
// fits, which is a short binary search over the same measurement -- a handful
// of measurements per page, which is nothing next to drawing it.
//
// What is remembered between pages is a position and not a picture: which
// block, and how far into it. That is twelve bytes, it survives a font-size
// change, and it is what gets written to the config so the book opens where it
// was left.
// ---------------------------------------------------------------------------

#define READER_MARGIN_MIN 8
#define READER_MARGIN_MAX 40
#define READER_SIZE_MIN 14
#define READER_SIZE_MAX 34
#define READER_LINE_MIN 0	// extra pixels between lines
#define READER_LINE_MAX 16
// Word spacing, in thin spaces added after every space. LVGL styles a line's
// letters and its lines but not its words, so this is done in the text itself:
// see widen_spaces().
#define READER_WORD_MIN 0
#define READER_WORD_MAX 6

// How many page starts are remembered so that going back a page does not mean
// laying the chapter out again from its beginning. Five hundred pages is more
// than any chapter has; at eight bytes each the whole stack is four kilobytes.
#define PAGE_STACK_MAX 512

// The three ways a book can be lit. The publisher's own colours are ignored on
// purpose -- see the markdown: a book should look like the rest of the player,
// not like whichever stylesheet its converter emitted.
typedef enum {
	THEME_LIGHT = 0,
	THEME_SEPIA,
	THEME_DARK,
} reader_theme_t;

// How a page gives way to the next one.
//
//   INSTANT   the new page is simply there: one layout, one redraw.
//   SLIDE     the old page leaves and the new one arrives with it, the way the
//             player's pages do under a swipe -- except in both directions.
//             Two pages are laid out and drawn at once for the length of the
//             animation, which is what it costs.
//   VERTICAL  no pages at all: the whole chapter is one list and it scrolls.
typedef enum {
	TURN_INSTANT = 0,
	TURN_SLIDE,
	TURN_VERTICAL,
} turn_mode_t;

// How long the slide takes. Short enough not to be waited for, long enough to
// say which way the page went.
#define TURN_ANIM_MS 220

static gui_config_t *g_cfg;

static ebook_t *book;
// Two boxes, not one: the slide needs the page it is leaving and the page it is
// arriving at on screen together. `page_box` is always the one being read;
// `page_spare` is the other, empty and parked off-screen between turns. In the
// other two modes the spare is simply never used.
static lv_obj_t *page_box;
static lv_obj_t *page_spare;
static bool turning; // an animation is running: gestures wait for it
static ebookbar_t status_bar; // what the reader chose to see along the bottom

// Where the current page starts, and the trail of the pages before it.
static uint32_t page_block, page_offset;
static struct {
	uint32_t block, offset;
} page_stack[PAGE_STACK_MAX];
static int page_stack_depth;

// Where the page after this one starts. Worked out while laying this one out.
static uint32_t next_block, next_offset;
static bool at_chapter_end;

// Which page of the chapter is on screen, counted from one. Zero means nobody
// knows -- the reader arrived somewhere without walking there, which is what
// restoring a position, following a bookmark or changing the font all do.
//
// A page number cannot be remembered between one visit and the next, for the
// reason load_chapter_at_end() gives about the last page: how the chapter falls
// into pages depends on the font, the spacing and the margins. So it is either
// counted while reading, which is free, or worked out by laying the chapter out
// from its beginning, which is the same walk that function already does and is
// only done when the strip is actually showing the number.
static int page_number;

// And how many pages of the book lie before this chapter, so the strip can show
// the page of the BOOK. See anchor_pages_before(): guessed once on arrival, then
// moved by the pages really counted at every boundary crossed from there.
static int pages_before;

// The settings, read from the config once and written back when they change.
static int opt_size = 20;
static int opt_line = 4;
static int opt_margin = 20;
static int opt_word = 0;
static reader_theme_t opt_theme = THEME_LIGHT;
static turn_mode_t opt_turn = TURN_INSTANT;

// How much of the page the strip along the bottom takes, plus the gap that
// keeps the last line off it. Not a constant: the reader chooses what is on the
// strip, and a strip with nothing on it reserves no room.
static int32_t bar_space(void) { return ebookbar_height() + 6; }

static void show_page(void);
static void menu_open(void);
static void update_status(void);
static void position_save(void);
static bool load_chapter(uint32_t spine, uint32_t block, uint32_t offset);
static bool load_chapter_dir(uint32_t spine, uint32_t block, uint32_t offset, int direction);
static bool load_chapter_at_end(uint32_t spine);
static void present(int direction);

// ---------------------------------------------------------------------------
// the look
// ---------------------------------------------------------------------------

static lv_color_t paper_colour(void) {
	switch (opt_theme) {
	case THEME_SEPIA:
		return lv_color_hex(0xF3E9D2);
	case THEME_DARK:
		return lv_color_hex(0x14140F);
	case THEME_LIGHT:
	default:
		return lv_color_hex(0xFBFBF8);
	}
}

static lv_color_t ink_colour(void) {
	switch (opt_theme) {
	case THEME_SEPIA:
		return lv_color_hex(0x40352A);
	case THEME_DARK:
		return lv_color_hex(0xC8C6BE);
	case THEME_LIGHT:
	default:
		return lv_color_hex(0x1A1A18);
	}
}

static void apply_colours(void) {
	if (!ebookreader_screen) {
		return;
	}
	lv_obj_set_style_bg_color(ebookreader_screen, paper_colour(), 0);
	lv_obj_set_style_bg_opa(ebookreader_screen, LV_OPA_COVER, 0);
	update_status();
}

// ---------------------------------------------------------------------------
// where the reading was left
// ---------------------------------------------------------------------------

// A book is remembered by its path, and the path is turned into a key short
// enough to be a config key. Two books with the same hash would share a
// position, which costs a page of scrolling once in several billion books.
static void position_key(const char *path, char *out, size_t size) {
	uint64_t h = 1469598103934665603ull;
	for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
		h ^= *p;
		h *= 1099511628211ull;
	}
	snprintf(out, size, "pos_%08x%08x", (unsigned)(h >> 32), (unsigned)(h & 0xFFFFFFFFu));
}

static void position_save(void) {
	if (!book) {
		return;
	}
	char key[32], value[64];
	position_key(ebook_path(book), key, sizeof(key));
	snprintf(value, sizeof(value), "%u:%u:%u", ebook_chapter_index(book), page_block, page_offset);
	config_store_set(config_ebook_store(), "ebook", key, value);
	config_store_save(config_ebook_store());
}

static void position_load(const char *path, uint32_t *spine, uint32_t *block, uint32_t *offset) {
	*spine = *block = *offset = 0;
	char key[32];
	position_key(path, key, sizeof(key));
	const char *value = config_store_get(config_ebook_store(), "ebook", key, "");
	if (value && value[0]) {
		unsigned a = 0, b = 0, c = 0;
		if (sscanf(value, "%u:%u:%u", &a, &b, &c) == 3) {
			*spine = a;
			*block = b;
			*offset = c;
		}
	}
}

// ---------------------------------------------------------------------------
// laying a page out
// ---------------------------------------------------------------------------

// A heading is bold and centred, and the same size as the text around it.
//
// Not because that is prettier but because a FreeType face carries its size:
// six heading levels at their own sizes would be six more faces open at once,
// each with its own glyph cache, and the whole point of loading the faces from
// the card is that the reader holds as few of them as it can. Weight and
// centring carry the heading well enough at 480 px.

// The air a block asks for above and below itself. Two functions rather than
// two numbers written twice, because build_block() styles with them and
// render_page() measures with them: if the two drift apart, a page overflows by
// exactly the difference.
static int32_t block_margin_top(const ebook_block_t *b) {
	if (b->type == EBOOK_BLOCK_RULE || b->type == EBOOK_BLOCK_HEADING || b->type == EBOOK_BLOCK_IMAGE) {
		return opt_size / 2;
	}
	return 0;
}

static int32_t block_margin_bottom(const ebook_block_t *b) {
	if (b->type == EBOOK_BLOCK_RULE || b->type == EBOOK_BLOCK_IMAGE) {
		return opt_size / 2;
	}
	// A whole line under a heading, two thirds of one between paragraphs: with
	// only a third they read as one wall of text, with a whole one a page holds
	// noticeably less.
	return b->type == EBOOK_BLOCK_HEADING ? opt_size : opt_size * 2 / 3;
}

// ---------------------------------------------------------------------------
// pictures
//
// A picture is its own block, so pagination can measure it like any other and a
// page can be the picture alone when that is all that fits.
//
// Decoded straight to the size it is drawn at, which is the same thing the shelf
// does with covers: a 2000 px illustration never exists at 2000 px anywhere in
// this process. What is decoded belongs to the LVGL object that shows it and
// goes when that object does -- hooked to LV_EVENT_DELETE rather than tracked in
// a list here, because the objects of a page are deleted by lv_obj_clean() in
// four different places and a list would have to be right in all of them.
// ---------------------------------------------------------------------------

// How tall a picture is allowed to be, as a fraction of the page. A picture
// taller than the page cannot be paginated around: it would be a page that
// shows nothing but a clipped picture with no way to see the rest of it.
#define IMAGE_MAX_HEIGHT_NUM 3
#define IMAGE_MAX_HEIGHT_DEN 4

static void image_deleted_cb(lv_event_t *e) {
	cover_image_t *img = lv_event_get_user_data(e);
	if (img) {
		cover_free(img);
		free(img);
	}
}

// Builds the picture named by `b`, or NULL when the book does not have it or it
// is not a format the decoder knows.
static lv_obj_t *build_image(lv_obj_t *parent, const ebook_block_t *b, int width) {
	if (!b->span_count) {
		return NULL;
	}
	const ebook_span_t *sp = ebook_span(book, b->first_span);
	const char *pool = ebook_chapter_text(book);

	char path[512];
	uint32_t len = sp->length < sizeof(path) - 1 ? sp->length : (uint32_t)sizeof(path) - 1;
	memcpy(path, pool + sp->offset, len);
	path[len] = '\0';

	uint32_t size = 0;
	void *bytes = ebook_load_image(book, path, &size);
	if (!bytes || !size) {
		free(bytes);
		return NULL;
	}

	cover_image_t *img = calloc(1, sizeof(*img));
	if (!img) {
		free(bytes);
		return NULL;
	}
	int box_h = (int)g_cfg->screen_height * IMAGE_MAX_HEIGHT_NUM / IMAGE_MAX_HEIGHT_DEN;
	// CONTAIN and not COVER: an illustration cropped to fill a box is an
	// illustration with its edges cut off, which for a diagram or a map is the
	// part that mattered.
	bool ok = cover_load_image_memory(bytes, size, width, box_h, COVER_FIT_CONTAIN, img);
	free(bytes);
	if (!ok) {
		free(img);
		return NULL;
	}

	lv_obj_t *obj = lv_image_create(parent);
	lv_image_set_src(obj, &img->dsc);
	lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_margin_top(obj, block_margin_top(b), 0);
	lv_obj_set_style_margin_bottom(obj, block_margin_bottom(b), 0);
	lv_obj_add_event_cb(obj, image_deleted_cb, LV_EVENT_DELETE, img);
	return obj;
}

// Wider gaps between words.
//
// LVGL styles the space between a line's letters and between its lines, but not
// between its words, so this is done in the text handed to the span rather than
// in a style: every space is followed by `opt_word` copies of a space that is
// narrower than a real one.
//
// Which narrow space depends on the font, asked rather than assumed. U+2009 THIN
// SPACE is a fifth of an em, which makes a sensible step; a face without it
// would draw LVGL's missing-glyph box after every word, so a face that does not
// have it gets U+00A0 instead -- a full-width space every font has, coarse but
// never wrong. The answer is worked out once per book, when the faces change.
#define THIN_SPACE "\xE2\x80\x89"	  // U+2009
#define NBSP_SPACE "\xC2\xA0"		  // U+00A0

static const char *gap_text = THIN_SPACE;
static size_t gap_len = 3;

static void choose_word_gap(void) {
	const lv_font_t *face = ebookfonts_face(false, false);
	lv_font_glyph_dsc_t dsc;
	bool thin = face && lv_font_get_glyph_dsc(face, &dsc, 0x2009, 0);
	gap_text = thin ? THIN_SPACE : NBSP_SPACE;
	gap_len = thin ? 3u : 2u;
}

// Copies `len` bytes of `src` into a new string, putting `opt_word` gaps after
// every space. The caller frees it. Returns NULL only if there is no memory.
static char *widen_spaces(const char *src, size_t len) {
	if (opt_word <= 0) {
		char *plain = malloc(len + 1u);
		if (plain) {
			memcpy(plain, src, len);
			plain[len] = '\0';
		}
		return plain;
	}

	size_t spaces = 0;
	for (size_t i = 0; i < len; i++) {
		if (src[i] == ' ') {
			spaces++;
		}
	}
	char *out = malloc(len + spaces * gap_len * (size_t)opt_word + 1u);
	if (!out) {
		return NULL;
	}
	size_t w = 0;
	for (size_t i = 0; i < len; i++) {
		out[w++] = src[i];
		if (src[i] != ' ') {
			continue;
		}
		for (int k = 0; k < opt_word; k++) {
			memcpy(out + w, gap_text, gap_len);
			w += gap_len;
		}
	}
	out[w] = '\0';
	return out;
}

// Builds one block as a spangroup inside `parent`, with `chars` characters of
// its text starting at `from` (UINT32_MAX for all of it). Returns the object,
// or NULL when there was nothing to draw.
static lv_obj_t *build_block(lv_obj_t *parent, const ebook_block_t *b, uint32_t from, uint32_t chars, int width) {
	if (b->type == EBOOK_BLOCK_RULE) {
		lv_obj_t *rule = lv_obj_create(parent);
		lv_obj_remove_style_all(rule);
		lv_obj_set_size(rule, width / 3, 2);
		lv_obj_set_style_bg_color(rule, ink_colour(), 0);
		lv_obj_set_style_bg_opa(rule, LV_OPA_30, 0);
		lv_obj_remove_flag(rule, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_style_margin_top(rule, block_margin_top(b), 0);
		lv_obj_set_style_margin_bottom(rule, block_margin_bottom(b), 0);
		return rule;
	}

	if (b->type == EBOOK_BLOCK_IMAGE) {
		// The picture ignores `from` and `chars`: it is all there or it is on
		// the next page, and chars_that_fit() never asks it to be cut because
		// lay_out() gives an image block its own branch.
		return build_image(parent, b, width);
	}

	lv_obj_t *group = lv_spangroup_create(parent);
	// A spangroup is clickable like any other object, and a page is nothing but
	// spangroups: left that way they take every press themselves and the page
	// underneath -- which is what turns -- never hears a tap at all.
	lv_obj_remove_flag(group, LV_OBJ_FLAG_CLICKABLE);
	lv_spangroup_set_mode(group, LV_SPAN_MODE_BREAK);
	lv_spangroup_set_overflow(group, LV_SPAN_OVERFLOW_CLIP);
	lv_obj_set_width(group, width - b->indent * opt_size);
	lv_obj_set_style_margin_left(group, b->indent * opt_size, 0);
	lv_obj_set_style_text_line_space(group, opt_line, 0);
	lv_obj_set_style_margin_top(group, block_margin_top(b), 0);
	lv_obj_set_style_margin_bottom(group, block_margin_bottom(b), 0);
	// A heading is centred because that is what a heading looks like; anything
	// else is centred only if the book's stylesheet says so.
	if (b->type == EBOOK_BLOCK_HEADING || b->align == EBOOK_ALIGN_CENTRE) {
		lv_spangroup_set_align(group, LV_TEXT_ALIGN_CENTER);
	} else if (b->align == EBOOK_ALIGN_RIGHT) {
		lv_spangroup_set_align(group, LV_TEXT_ALIGN_RIGHT);
	}

	const char *pool = ebook_chapter_text(book);
	uint32_t seen = 0;	 // characters of this block already passed
	uint32_t taken = 0;	 // characters put into the group
	bool any = false;

	for (uint32_t s = 0; s < b->span_count && taken < chars; s++) {
		const ebook_span_t *sp = ebook_span(book, b->first_span + s);
		uint32_t start = 0;
		uint32_t len = sp->length;

		// Skip the part of the block that belongs to the previous page.
		if (seen + len <= from) {
			seen += len;
			continue;
		}
		if (seen < from) {
			start = from - seen;
			len -= start;
		}
		seen += sp->length;
		if (taken + len > chars) {
			len = chars - taken;
		}
		if (!len) {
			continue;
		}
		taken += len;

		lv_span_t *span = lv_spangroup_new_span(group);
		// A copy, because lv_span_set_text_static would point into the chapter
		// arena -- which is unmapped at the next chapter, while this object may
		// still be on screen for a frame.
		char *piece = widen_spaces(pool + sp->offset + start, len);
		if (!piece) {
			break;
		}
		lv_span_set_text(span, piece);
		free(piece);

		bool bold = (sp->style & EBOOK_STYLE_BOLD) || b->type == EBOOK_BLOCK_HEADING;
		bool italic = (sp->style & EBOOK_STYLE_ITALIC) || b->type == EBOOK_BLOCK_QUOTE;
		lv_style_t *style = lv_span_get_style(span);
		lv_style_set_text_font(style, ebookfonts_face(bold, italic));
		lv_style_set_text_color(style, ink_colour());
		any = true;
	}

	if (!any) {
		lv_obj_delete(group);
		return NULL;
	}
	lv_spangroup_refresh(group);
	return group;
}

static uint32_t block_chars(const ebook_block_t *b) {
	uint32_t total = 0;
	for (uint32_t s = 0; s < b->span_count; s++) {
		total += ebook_span(book, b->first_span + s)->length;
	}
	return total;
}

// How tall `group` wants to be. lv_spangroup answers this without drawing,
// which is the whole reason the page can be measured before it is shown.
static int32_t group_height(lv_obj_t *group, int width) {
	if (lv_obj_check_type(group, &lv_spangroup_class)) {
		return lv_spangroup_get_expand_height(group, width);
	}
	if (lv_obj_check_type(group, &lv_image_class)) {
		// Asked of the decoded picture rather than of the object: an image is
		// LV_SIZE_CONTENT and its height is not resolved until a layout pass,
		// which has not happened yet when a page is being measured.
		const lv_image_dsc_t *dsc = lv_image_get_src(group);
		return dsc ? (int32_t)dsc->header.h : 0;
	}
	return lv_obj_get_height(group);
}

// The last word of `b` that still fits in `room`, as a character count.
// Measured by halving, so a paragraph of two thousand characters costs eleven
// measurements rather than two thousand.
static uint32_t chars_that_fit(lv_obj_t *parent, const ebook_block_t *b, uint32_t from, uint32_t available,
							   int width, int32_t room) {
	uint32_t low = 0, high = available;
	while (low < high) {
		uint32_t mid = low + (high - low + 1u) / 2u;
		lv_obj_t *probe = build_block(parent, b, from, mid, width);
		int32_t h = probe ? group_height(probe, width - b->indent * opt_size) : 0;
		if (probe) {
			lv_obj_delete(probe);
		}
		if (h <= room) {
			low = mid;
		} else {
			high = mid - 1u;
		}
	}

	// Back up to a word boundary so a page never ends mid-word.
	if (low > 0 && low < available) {
		const char *pool = ebook_chapter_text(book);
		uint32_t seen = 0;
		uint32_t cut = low;
		for (uint32_t s = 0; s < b->span_count && cut > 0; s++) {
			const ebook_span_t *sp = ebook_span(book, b->first_span + s);
			if (seen + sp->length <= from + low) {
				seen += sp->length;
				continue;
			}
			// The character at the cut, inside this span.
			uint32_t within = from + low - seen;
			for (uint32_t k = within; k > 0; k--) {
				if (pool[sp->offset + k - 1] == ' ' || pool[sp->offset + k - 1] == '\n') {
					return low - (within - k);
				}
			}
			break;
		}
	}
	return low;
}

// Fills `box` with as much of the chapter as fits, starting at `block`/`offset`.
// Writes where the page after it begins into `end_block`/`end_offset`, and sets
// `*ended` when the chapter ran out.
//
// Parameterised rather than working on page_box directly because the slide lays
// the arriving page out in the other box while this one is still being read.
static void lay_out(lv_obj_t *box, uint32_t block, uint32_t offset, uint32_t *end_block, uint32_t *end_offset,
					bool *ended) {
	lv_obj_clean(box);

	int width = (int)g_cfg->screen_width - 2 * opt_margin;
	int32_t budget = (int32_t)g_cfg->screen_height - 2 * opt_margin - bar_space();
	int32_t used = 0;

	while (block < ebook_block_count(book)) {
		const ebook_block_t *b = ebook_block(book, block);
		uint32_t available = block_chars(b);
		if (offset > available) {
			offset = 0;
		}
		uint32_t remaining = available - offset;

		lv_obj_t *whole = build_block(box, b, offset, remaining ? remaining : 1u, width);
		int32_t h =
			whole ? group_height(whole, width - b->indent * opt_size) + block_margin_top(b) + block_margin_bottom(b)
				  : 0;

		if (b->type == EBOOK_BLOCK_IMAGE) {
			// A picture is all on the page or all on the next one: there is no
			// cutting it at a word, and chars_that_fit() below would be
			// measuring the length of a file path.
			if (!whole) {
				block++; // one the book names but does not have
				offset = 0;
				continue;
			}
			if (used + h <= budget) {
				used += h;
				block++;
				offset = 0;
				continue;
			}
			lv_obj_delete(whole);
			if (used > 0) {
				break; // whole, on the next page
			}
			// An empty page and it still does not fit, which means a picture
			// taller than three quarters of the screen on a page with the
			// margins of this one. Shown anyway and clipped: a page with
			// nothing on it cannot be turned past.
			build_block(box, b, 0, UINT32_MAX, width);
			block++;
			offset = 0;
			break;
		}

		if (whole && used + h <= budget) {
			used += h;
			block++;
			offset = 0;
			continue;
		}

		// It does not fit whole. Either cut it, or -- if this page has nothing
		// on it yet and not even one line fits -- put it on anyway, because a
		// page that shows nothing cannot be turned past.
		if (whole) {
			lv_obj_delete(whole);
		}
		// The bottom margin is not reserved for a block that is being cut: it
		// falls below the last line drawn, where clipping it costs nothing and
		// reserving it would throw away a line of text.
		int32_t room = budget - used - block_margin_top(b);
		uint32_t fits = remaining ? chars_that_fit(box, b, offset, remaining, width, room) : 0;
		if (fits == 0) {
			if (used > 0) {
				break; // it goes on the next page whole
			}
			fits = remaining; // nothing fits anywhere: show it and let it clip
		}
		lv_obj_t *part = build_block(box, b, offset, fits, width);
		(void)part;
		offset += fits;
		// The cut swallowed what was left of the block. Without this the next
		// page would start where this one ended, with nothing after it to draw,
		// and every further turn would show the same empty page.
		if (offset >= available) {
			block++;
			offset = 0;
		}
		break;
	}

	*end_block = block;
	*end_offset = offset;
	*ended = block >= ebook_block_count(book);
}

// Lays the chapter out from its beginning, page by page, in the spare box.
// Returns how many pages it has, and writes the 1-based number of the page that
// `block`/`offset` falls on into `*page_out`.
//
// The same walk as load_chapter_at_end(), and it costs the same: one layout of
// one chapter. It is called only when the reader has arrived somewhere without
// counting its way there AND the strip is showing the page number, so a reader
// who has not switched that on never pays for it.
//
// The position is answered by the page that CONTAINS it rather than by an exact
// match, because after a font change the saved offset can sit in the middle of a
// page rather than at its start.
static int paginate_chapter(uint32_t block, uint32_t offset, int *page_out) {
	uint32_t at_block = 0, at_offset = 0;
	int pages = 0;
	int found = 0;
	for (int guard = 0; guard < PAGE_STACK_MAX; guard++) {
		uint32_t end_block = 0, end_offset = 0;
		bool ended = false;
		lay_out(page_spare, at_block, at_offset, &end_block, &end_offset, &ended);
		pages++;
		if (!found && (end_block > block || (end_block == block && end_offset > offset))) {
			found = pages;
		}
		if (ended) {
			break;
		}
		at_block = end_block;
		at_offset = end_offset;
	}
	lv_obj_clean(page_spare);
	if (page_out) {
		*page_out = found ? found : pages;
	}
	return pages;
}

// How many pages of the book lie before the chapter on screen, guessed from how
// densely THIS chapter fills its pages and how many bytes the chapters before it
// hold (see ebook_chapter_weight).
//
// A guess, and the only one on the strip: counting for real would mean opening
// and laying out every earlier chapter, and the reader holds one at a time.
//
// It is made once, when the reader arrives somewhere without having read its
// way there. From then on every chapter boundary crossed moves it by the pages
// really counted, so a book read in order stays exact relative to where it was
// opened.
static void anchor_pages_before(int pages_in_chapter) {
	uint32_t chapter = ebook_chapter_index(book);
	uint64_t before = ebook_weight_before(book, chapter);
	uint32_t mine = ebook_chapter_weight(book, chapter);
	pages_before = (mine && pages_in_chapter > 0) ? (int)(before * (uint64_t)pages_in_chapter / mine) : 0;
}

// How far into the whole book this position is.
//
// The chapters are weighed by the size of their files rather than counted, so a
// two-page preface is worth two pages and not a whole chapter -- see
// ebook_chapter_weight(). A book whose chapters cannot be weighed falls back to
// counting them equal, which is the best that is left.
static int book_percent_now(uint32_t chapter, int chapter_percent) {
	uint64_t total = ebook_book_weight(book);
	if (total) {
		uint64_t before = ebook_weight_before(book, chapter);
		uint64_t into = (uint64_t)ebook_chapter_weight(book, chapter) * (uint64_t)chapter_percent / 100u;
		return (int)((before + into) * 100u / total);
	}
	uint32_t chapters = ebook_spine_count(book);
	return chapters ? (int)(((uint64_t)chapter * 100u + (uint64_t)chapter_percent) / chapters) : 0;
}

// The strip along the bottom. What is on it is the reader's choice; see
// ebookbar.h.
static void update_status(void) {
	if (!status_bar.root || !book) {
		return;
	}

	uint32_t blocks = ebook_block_count(book);
	uint32_t chapter = ebook_chapter_index(book);

	ebookbar_state_t state = {
		.chapter = chapter,
		.chapters = ebook_spine_count(book),
		.chapter_percent = blocks ? (int)((uint64_t)page_block * 100u / blocks) : 0,
		.page = 0,
		.ink = ink_colour(),
	};
	state.book_percent = book_percent_now(chapter, state.chapter_percent);
	// The page of the BOOK, which is what a page number means to a reader: the
	// pages before this chapter plus the page within it.
	state.page = page_number > 0 ? pages_before + page_number : 0;

	ebookbar_refresh(&status_bar, &state);
}

// Counts the page the reader is on, for the times it cannot be reached by
// counting: a position restored, a bookmark followed, a font size changed.
//
// Never called from update_status(), which would be the obvious place: a slide
// lays the arriving page out in the spare box and only then repaints the strip,
// and the walk above uses that same box. Counting from in there would throw
// away the page about to slide in. So it is done where the number goes unknown
// instead, which is always a moment with the spare box free.
static void page_number_recount(void) {
	page_number = 0;
	pages_before = 0;
	if (!book || !ebookbar_option(EBOOKBAR_PAGE) || opt_turn == TURN_VERTICAL) {
		return;
	}
	int page = 0;
	int pages = paginate_chapter(page_block, page_offset, &page);
	page_number = page;
	anchor_pages_before(pages);
}

static void render_page(void) {
	if (!book || !page_box) {
		return;
	}
	lay_out(page_box, page_block, page_offset, &next_block, &next_offset, &at_chapter_end);
	update_status();
	apply_colours();
}

// ---------------------------------------------------------------------------
// the chapter as one scrolling list
//
// Every block at once, and LVGL scrolls it -- the opposite of the paged modes,
// where only one screenful of objects exists. The cost is roughly a kilobyte of
// LVGL objects per block, held for as long as the chapter is open.
//
// A chapter is one file inside the EPUB, so what bounds this is how the book was
// split; even a single-file book of two thousand paragraphs stays within the
// device's memory, so there is no cap here.
//
// The chapter boundary is a button and not a gesture. Scrolling past the end to
// arrive somewhere else is the kind of thing that either fires when it should
// not or refuses to fire when it should, and in a list that already scrolls
// there is nowhere left for a gesture to mean something new.
// ---------------------------------------------------------------------------

#define NOT_A_BLOCK UINT32_MAX

static void chapter_step_cb(lv_event_t *e) {
	int step = (int)(intptr_t)lv_event_get_user_data(e);
	uint32_t index = ebook_chapter_index(book);
	if (step < 0 && index > 0) {
		load_chapter(index - 1u, 0, 0);
	} else if (step > 0 && index + 1u < ebook_spine_count(book)) {
		load_chapter(index + 1u, 0, 0);
	}
}

static void chapter_button(lv_obj_t *parent, const char *tag, int step) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_width(btn, lv_pct(100));
	lv_obj_set_height(btn, 72);
	lv_obj_set_user_data(btn, (void *)(uintptr_t)NOT_A_BLOCK);
	lv_obj_set_style_bg_color(btn, ink_colour(), 0);
	lv_obj_set_style_bg_opa(btn, LV_OPA_10, 0);
	lv_obj_set_style_radius(btn, 12, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_margin_ver(btn, 10, 0);
	lv_obj_add_event_cb(btn, chapter_step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)step);

	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, tr(tag));
	lv_obj_set_style_text_color(label, ink_colour(), 0);
	lv_obj_set_style_text_font(label, &font_ui_20, 0);
	lv_obj_center(label);
}

static void render_vertical(void) {
	if (!book || !page_box) {
		return;
	}
	lv_obj_clean(page_box);

	int width = (int)g_cfg->screen_width - 2 * opt_margin;
	uint32_t count = ebook_block_count(book);

	if (ebook_chapter_index(book) > 0) {
		chapter_button(page_box, "ebookreader_previous_chapter", -1);
	}
	lv_obj_t *first_wanted = NULL;
	for (uint32_t i = 0; i < count; i++) {
		lv_obj_t *obj = build_block(page_box, ebook_block(book, i), 0, UINT32_MAX, width);
		if (!obj) {
			continue;
		}
		// Which block each object is, so the scroll can be turned back into a
		// reading position and the position back into a scroll.
		lv_obj_set_user_data(obj, (void *)(uintptr_t)i);
		if (i == page_block) {
			first_wanted = obj;
		}
	}
	if (ebook_chapter_index(book) + 1u < ebook_spine_count(book)) {
		chapter_button(page_box, "ebookreader_next_chapter", 1);
	}

	apply_colours();
	// Where the reading was left, which in this mode is a scroll position.
	lv_obj_update_layout(page_box);
	if (first_wanted) {
		// To the TOP of that block, not merely into view. lv_obj_scroll_to_view
		// scrolls the least it can, which puts the block at the BOTTOM of the
		// screen; the topmost visible block would then be an earlier one and the
		// scroll handler would write that back as the reading position.
		int32_t top = lv_obj_get_y(first_wanted);
		lv_obj_scroll_to_y(page_box, top > 0 ? top : 0, LV_ANIM_OFF);
	} else {
		lv_obj_scroll_to_y(page_box, 0, LV_ANIM_OFF);
	}
	update_status();
}

// The topmost block still on screen, which is what the reading position means
// when the chapter is one long list.
static void vertical_scrolled_cb(lv_event_t *e) {
	(void)e;
	if (!book || opt_turn != TURN_VERTICAL) {
		return;
	}
	int32_t top = lv_obj_get_scroll_y(page_box);
	uint32_t count = lv_obj_get_child_count(page_box);
	for (uint32_t i = 0; i < count; i++) {
		lv_obj_t *child = lv_obj_get_child(page_box, i);
		uint32_t index = (uint32_t)(uintptr_t)lv_obj_get_user_data(child);
		if (index == NOT_A_BLOCK) {
			continue;
		}
		if (lv_obj_get_y(child) + lv_obj_get_height(child) > top) {
			page_block = index;
			page_offset = 0;
			break;
		}
	}
	update_status();
	position_save();
}

// ---------------------------------------------------------------------------
// turning the pages
// ---------------------------------------------------------------------------

static void show_page(void) {
	if (opt_turn == TURN_VERTICAL) {
		render_vertical();
	} else {
		render_page();
	}
}

// `direction` as in present(): a chapter reached by reading on slides like any
// other page, a chapter picked from the list simply appears.
static bool load_chapter_dir(uint32_t spine, uint32_t block, uint32_t offset, int direction) {
	// Read on into the next chapter: the page being left was the last one of its
	// chapter -- that is why the turn crossed at all -- so its number is how many
	// pages that chapter had, and the sum carries over exactly. Worked out before
	// the chapter is opened, because opening it throws the old one away.
	bool carry = direction > 0 && block == 0 && offset == 0 && page_number > 0;
	int carried = pages_before + page_number;

	if (!ebook_open_chapter(book, spine)) {
		return false;
	}
	page_block = block;
	page_offset = offset;
	page_stack_depth = 0;

	if (carry) {
		pages_before = carried;
		page_number = 1;
	} else {
		page_number_recount();
	}
	present(direction);
	position_save();
	return true;
}

static bool load_chapter(uint32_t spine, uint32_t block, uint32_t offset) {
	return load_chapter_dir(spine, block, offset, 0);
}

// Opens `spine` at its last page.
//
// Where a chapter's last page begins cannot be remembered from last time: it
// depends on the font size, the spacing and the margins, all of which the reader
// can change between one visit and the next. So the chapter is laid out page by
// page, in the spare box, until it runs out -- and the trail of page starts is
// kept on the way, which means backing up again after this walks page by page
// instead of jumping to the top.
//
// It costs one layout of one chapter, and it happens only when a reader crosses
// a chapter boundary backwards.
static bool load_chapter_at_end(uint32_t spine) {
	if (!ebook_open_chapter(book, spine)) {
		return false;
	}
	page_stack_depth = 0;

	uint32_t block = 0, offset = 0;
	for (;;) {
		uint32_t end_block = 0, end_offset = 0;
		bool ended = false;
		lay_out(page_spare, block, offset, &end_block, &end_offset, &ended);
		if (ended) {
			break; // this page reached the end of the chapter: it is the last one
		}
		if (page_stack_depth < PAGE_STACK_MAX) {
			page_stack[page_stack_depth].block = block;
			page_stack[page_stack_depth].offset = offset;
			page_stack_depth++;
		}
		block = end_block;
		offset = end_offset;
	}
	lv_obj_clean(page_spare);

	// The walk above counted the pages on the way, so the last one's number is
	// already known and costs nothing more. Going back over a boundary, what lies
	// before the chapter just entered is what lay before the one just left, less
	// this chapter -- again a number really counted and not guessed.
	if (ebookbar_option(EBOOKBAR_PAGE) && opt_turn != TURN_VERTICAL) {
		page_number = page_stack_depth + 1;
		pages_before -= page_number;
		if (pages_before < 0) {
			// Nothing to subtract from: the count had not been anchored in the
			// chapter being left, so it is anchored here instead.
			anchor_pages_before(page_number);
		}
	} else {
		page_number = 0;
		pages_before = 0;
	}

	page_block = block;
	page_offset = offset;
	present(-1);
	position_save();
	return true;
}

// The end of a slide: the page that left is emptied and parked, and the two
// boxes change roles. Emptied and not deleted, because the objects of a page of
// text are the expensive part and the next turn wants a box to build in.
static void slide_done(lv_anim_t *anim) {
	(void)anim;
	lv_obj_clean(page_spare);
	lv_obj_set_x(page_spare, (int32_t)g_cfg->screen_width);
	turning = false;
}

static void slide_in(int direction) {
	// The arriving page is already laid out in the spare; from here it is two
	// objects moving. `direction` is +1 when reading forward, so the new page
	// comes from the right.
	int32_t width = (int32_t)g_cfg->screen_width;
	lv_obj_set_x(page_spare, direction > 0 ? width : -width);

	lv_obj_t *leaving = page_box;
	lv_obj_t *arriving = page_spare;
	page_box = arriving;
	page_spare = leaving;
	turning = true;

	lv_anim_t out;
	lv_anim_init(&out);
	lv_anim_set_var(&out, leaving);
	lv_anim_set_exec_cb(&out, (lv_anim_exec_xcb_t)lv_obj_set_x);
	lv_anim_set_values(&out, 0, direction > 0 ? -width : width);
	lv_anim_set_duration(&out, TURN_ANIM_MS);
	lv_anim_set_path_cb(&out, lv_anim_path_ease_out);
	lv_anim_start(&out);

	lv_anim_t in;
	lv_anim_init(&in);
	lv_anim_set_var(&in, arriving);
	lv_anim_set_exec_cb(&in, (lv_anim_exec_xcb_t)lv_obj_set_x);
	lv_anim_set_values(&in, direction > 0 ? width : -width, 0);
	lv_anim_set_duration(&in, TURN_ANIM_MS);
	lv_anim_set_path_cb(&in, lv_anim_path_ease_out);
	// The tidying hangs off the arriving page, which is the one that finishes
	// at a known place; hanging it off both would run it twice.
	lv_anim_set_completed_cb(&in, slide_done);
	lv_anim_start(&in);
}

// Puts whatever is at page_block/page_offset on screen.
//
// `direction` is +1 or -1 when the reader is moving through the book and 0 when
// they have jumped to somewhere unrelated -- a chapter picked from the list, a
// bookmark, the book being opened. Only the slide cares, and only a move has a
// direction to slide in.
static void present(int direction) {
	if (opt_turn != TURN_SLIDE || direction == 0) {
		show_page();
		return;
	}
	lay_out(page_spare, page_block, page_offset, &next_block, &next_offset, &at_chapter_end);
	apply_colours();
	slide_in(direction);
	update_status();
}

// Moves the reading position to `block`/`offset` and shows it.
static void turn_to(uint32_t block, uint32_t offset, int direction) {
	page_block = block;
	page_offset = offset;
	present(direction);
	position_save();
}

static void page_next(void) {
	if (!book || turning) {
		return;
	}
	if (at_chapter_end) {
		uint32_t next = ebook_chapter_index(book) + 1u;
		if (next < ebook_spine_count(book)) {
			load_chapter_dir(next, 0, 0, +1);
		}
		return;
	}
	if (page_stack_depth < PAGE_STACK_MAX) {
		page_stack[page_stack_depth].block = page_block;
		page_stack[page_stack_depth].offset = page_offset;
		page_stack_depth++;
	}
	if (page_number > 0) {
		page_number++;
	}
	turn_to(next_block, next_offset, +1);
}

static void page_prev(void) {
	if (!book || turning) {
		return;
	}
	if (page_stack_depth > 0) {
		page_stack_depth--;
		if (page_number > 1) {
			page_number--;
		}
		turn_to(page_stack[page_stack_depth].block, page_stack[page_stack_depth].offset, -1);
		return;
	}
	// The start of the chapter, with nothing behind it: the chapter before, at
	// its LAST page -- which is the page that actually comes before this one.
	uint32_t index = ebook_chapter_index(book);
	if (index > 0) {
		load_chapter_at_end(index - 1u);
	}
}

// ---------------------------------------------------------------------------
// the gestures
// ---------------------------------------------------------------------------

// How long a press has to last to be a press and not a tap.
#define LONG_PRESS_MS 600

// Where a tap turns a page. Left of the first and right of the second, as
// percentages of the width; between them a tap does nothing at all, which is the
// room the long press needs -- a reader holding still in the middle to open the
// menu must not turn a page when the finger comes off.
#define TAP_BACK_UNTIL_PCT 36
#define TAP_NEXT_FROM_PCT 64

// And where a long press opens the menu: the middle half across and the middle
// two thirds down. Wider than the dead band above on purpose, because a hold is
// already told apart by how long it lasts, and the alternative is a reader
// pressing and pressing and nothing happening.
#define CENTRE_FRACTION_W 4
#define CENTRE_FRACTION_H 3

// Where a double tap saves a bookmark: the top of the page, across the same
// middle band a single tap does nothing in. It has to be inside that band --
// two taps on the right edge are two page turns, and they must stay two page
// turns.
#define BOOKMARK_ZONE_HEIGHT_PCT 25
// How long after the first tap the second still counts as the same gesture.
#define DOUBLE_TAP_MS 400

static bool in_the_bookmark_zone(const lv_point_t *p) {
	int32_t w = (int32_t)g_cfg->screen_width;
	int32_t h = (int32_t)g_cfg->screen_height;
	return p->y < h * BOOKMARK_ZONE_HEIGHT_PCT / 100 && p->x >= w * TAP_BACK_UNTIL_PCT / 100 &&
		   p->x <= w * TAP_NEXT_FROM_PCT / 100;
}

// The first line of what is on the page, so the list of bookmarks reads as
// places in a book and not as three numbers. Taken from the page's first block
// at the offset the page starts at, which is exactly the text under the
// reader's eye when the finger came down.
static void page_snippet(char *out, size_t out_size) {
	out[0] = '\0';
	if (!book || page_block >= ebook_block_count(book)) {
		return;
	}
	const ebook_block_t *b = ebook_block(book, page_block);
	if (!b->span_count) {
		return;
	}
	const char *pool = ebook_chapter_text(book);
	const ebook_span_t *sp = ebook_span(book, b->first_span);

	// page_offset counts from the start of the block, and the first span starts
	// there too, so a page that begins mid-paragraph begins mid-span.
	uint32_t skip = page_offset < sp->length ? page_offset : 0;
	uint32_t len = sp->length - skip;
	if (len > out_size - 1) {
		len = (uint32_t)out_size - 1;
	}
	memcpy(out, pool + sp->offset + skip, len);
	out[len] = '\0';

	// A snippet with a newline in it would be two lines in a one-line row.
	for (char *c = out; *c; c++) {
		if (*c == '\n' || *c == '\r') {
			*c = ' ';
		}
	}
}

static void save_bookmark(void) {
	if (!book) {
		return;
	}
	char text[BOOKMARK_TEXT_MAX];
	page_snippet(text, sizeof(text));
	if (bookmark_add(ebook_path(book), ebook_chapter_index(book), page_block, page_offset, text)) {
		toast_glyph(&icon_bookmark_check, "ebookreader_bookmark_saved");
	} else {
		// Already there, or the book is full. Saying so is better than a tap
		// that looks like it did nothing.
		toast_plain("ebookreader_bookmark_duplicate");
	}
}

static bool in_the_centre(const lv_point_t *p) {
	int32_t w = (int32_t)g_cfg->screen_width;
	int32_t h = (int32_t)g_cfg->screen_height;
	return LV_ABS(p->x - w / 2) < w / CENTRE_FRACTION_W && LV_ABS(p->y - h / 2) < h / CENTRE_FRACTION_H;
}

static void page_pressed_cb(lv_event_t *e) {
	static lv_point_t start;
	static uint32_t pressed_at;
	static bool tracking;
	// The tap before this one, for telling a double tap from two taps.
	static uint32_t last_tap_at;
	static lv_point_t last_tap_at_point;

	lv_indev_t *indev = lv_indev_active();
	if (!indev) {
		return;
	}
	lv_event_code_t code = lv_event_get_code(e);

	if (code == LV_EVENT_PRESSED) {
		lv_indev_get_point(indev, &start);
		pressed_at = lv_tick_get();
		tracking = true;
		return;
	}
	if (!tracking || code == LV_EVENT_PRESSING) {
		return;
	}
	if (code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) {
		return;
	}
	tracking = false;
	if (code == LV_EVENT_PRESS_LOST) {
		return;
	}

	lv_point_t end;
	lv_indev_get_point(indev, &end);
	int dx = end.x - start.x;
	int dy = end.y - start.y;
	bool moved = LV_ABS(dx) > 20 || LV_ABS(dy) > 20;

	// Two quick taps at the top, in the middle: a bookmark. Checked before
	// everything else because the band it lives in is one where a single tap
	// does nothing, so there is nothing for it to take away.
	if (!moved && in_the_bookmark_zone(&end)) {
		bool soon = lv_tick_elaps(last_tap_at) < DOUBLE_TAP_MS;
		bool near = LV_ABS(end.x - last_tap_at_point.x) < 60 && LV_ABS(end.y - last_tap_at_point.y) < 60;
		if (last_tap_at && soon && near) {
			last_tap_at = 0; // a third tap starts a new pair, not a second bookmark
			save_bookmark();
			return;
		}
		last_tap_at = lv_tick_get();
		last_tap_at_point = end;
		return;
	}
	last_tap_at = 0;

	// Held still: the menu, if it was held in the middle. Either way a hold
	// never turns a page -- someone who holds a finger down meant something by
	// it, and turning a page when they give up is the wrong answer to both.
	if (!moved && lv_tick_elaps(pressed_at) > LONG_PRESS_MS) {
		if (in_the_centre(&end)) {
			menu_open();
		}
		return;
	}

	// From here on it is about turning pages, which the vertical mode does not
	// do: there a finger that moved was scrolling and a finger that did not was
	// nothing at all.
	if (opt_turn == TURN_VERTICAL) {
		return;
	}

	// A sideways drag turns the page the way it is dragged.
	if (dx < -40 && LV_ABS(dx) > LV_ABS(dy)) {
		page_next();
		return;
	}
	if (dx > 40 && LV_ABS(dx) > LV_ABS(dy)) {
		page_prev();
		return;
	}
	if (moved) {
		return; // a drag that went nowhere in particular
	}

	// A tap: left edge back, right edge forward, and a band down the middle
	// that does nothing. The two edges are more than a third of the width each,
	// so neither has to be aimed at.
	int32_t w = (int32_t)g_cfg->screen_width;
	if (end.x < w * TAP_BACK_UNTIL_PCT / 100) {
		page_prev();
	} else if (end.x > w * TAP_NEXT_FROM_PCT / 100) {
		page_next();
	}
}

// ---------------------------------------------------------------------------
// the menu
//
// A long press in the middle brings up the veil the pop-overs use and a bar
// along the bottom with three things on it. Each of the three opens a card in
// the middle -- the shape Gearboy's in-game menu has -- and the card's chevron
// comes back to the bar. A tap on the veil puts the whole thing away.
// ---------------------------------------------------------------------------

// How far the menu stays off the edges of the screen. A bar that runs to the
// bottom and to both sides reads as a panel that has fallen off the page rather
// than one that has come up.
#define MENU_INSET 16

typedef enum {
	SECTION_CHAPTERS = 0,
	SECTION_FONT,
	SECTION_THEME,
} section_t;

static lv_obj_t *menu_veil;
static lv_obj_t *menu_bar;	 // the three icons and the way out of the book
static lv_obj_t *menu_card;	 // one section at a time, over the bar
static lv_obj_t *menu_heading;
static lv_obj_t *menu_body;
// Which section the card is showing. The panel is shared, so the chapter list's
// scroll handler has to know when the thing being scrolled is not its list.
static section_t menu_section = SECTION_CHAPTERS;

// How much of the chapter list is on the panel; see build_chapters().
static uint32_t chapters_built;	 // entries built so far
static uint32_t chapters_total;	 // entries the book has
static bool chapters_from_toc;	 // its own index, or the spine standing in for one

// The icons that have to be told about a theme change.
//
// An icon drawn from an SVG is stored white and recoloured at draw time, and
// the recolour is a style set when the object is built -- a snapshot of the
// theme at that moment. The menu is built once, at startup, so these have to be
// recoloured by hand when the theme changes.
#define MENU_GLYPH_MAX 4
static lv_obj_t *menu_glyphs[MENU_GLYPH_MAX];
static int menu_glyph_count;

static void remember_glyph(lv_obj_t *glyph) {
	if (menu_glyph_count < MENU_GLYPH_MAX) {
		menu_glyphs[menu_glyph_count++] = glyph;
	}
}

static void menu_theme_refresh(void) {
	for (int i = 0; i < menu_glyph_count; i++) {
		if (menu_glyphs[i]) {
			lv_obj_set_style_image_recolor(menu_glyphs[i], theme()->text_primary, 0);
		}
	}
}

static void build_chapters(void);
static void build_font(void);
static void build_theme(void);

static void menu_close(void) {
	if (menu_veil) {
		lv_obj_add_flag(menu_veil, LV_OBJ_FLAG_HIDDEN);
	}
	if (menu_body) {
		// The rows of a long chapter list are the one thing in here worth not
		// keeping around while a book is being read.
		lv_obj_clean(menu_body);
		chapters_built = 0;
	}
}

static void menu_open(void) {
	if (!menu_veil || !book) {
		return;
	}
	lv_obj_add_flag(menu_card, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag(menu_bar, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag(menu_veil, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(menu_veil);
}

static void menu_veil_cb(lv_event_t *e) {
	(void)e;
	menu_close();
}

static void section_open(lv_event_t *e) {
	section_t which = (section_t)(intptr_t)lv_event_get_user_data(e);
	static const char *const SECTION_TITLES[] = {"chapters", "ebookreader_font_settings", "ebookreader_book_theme"};

	lv_obj_clean(menu_body);
	chapters_built = 0;
	menu_section = which;
	lv_label_set_text(menu_heading, tr(SECTION_TITLES[which]));
	switch (which) {
	case SECTION_CHAPTERS:
		build_chapters();
		break;
	case SECTION_FONT:
		build_font();
		break;
	case SECTION_THEME:
		build_theme();
		break;
	}
	lv_obj_scroll_to_y(menu_body, 0, LV_ANIM_OFF);
	lv_obj_add_flag(menu_bar, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag(menu_card, LV_OBJ_FLAG_HIDDEN);
}

static void section_back_cb(lv_event_t *e) {
	(void)e;
	lv_obj_clean(menu_body);
	chapters_built = 0;
	lv_obj_add_flag(menu_card, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag(menu_bar, LV_OBJ_FLAG_HIDDEN);
}

// The way out of the book. It has to be in here and not on the screen edge: a
// sideways drag turns a page, so the swipe that leaves every other page of this
// player is the one gesture the reader cannot have.
static void close_book_cb(lv_event_t *e) {
	(void)e;
	menu_close();
	back_btn_cb(NULL); // what the chevron would do, which is not drawn here
}

// ---------------------------------------------------------------------------
// the chapter list
// ---------------------------------------------------------------------------

static void chapter_pick_cb(lv_event_t *e) {
	uint32_t spine = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
	menu_close();
	load_chapter(spine, 0, 0);
}

// One entry of the chapter list.
//
// Built here rather than with settingsrow_action() because a settings row is a
// fixed hundred pixels tall and steps its name down a font size when it does not
// fit, which makes a long chapter title unreadably small. Here the row grows and
// the title wraps instead.
static void chapter_entry(const char *title, uint32_t spine, uint8_t depth) {
	lv_obj_t *row = lv_btn_create(menu_body);
	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_height(row, LV_SIZE_CONTENT);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, 12, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, 20, 0);
	// Enough that a one-line entry still looks like a row and not like a label,
	// and a three-line one has the same air above and below it.
	lv_obj_set_style_pad_ver(row, 22, 0);
	lv_obj_set_style_min_height(row, 88, 0);
	lv_obj_add_event_cb(row, chapter_pick_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)spine);

	lv_obj_t *name = lv_label_create(row);
	lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
	lv_label_set_text(name, title);
	lv_obj_add_style(name, &theme_style_text, 0);
	lv_obj_set_style_text_font(name, &font_ui_22, 0);
	// A sub-entry is indented rather than given a smaller font: at this size a
	// smaller font is a font nobody can read.
	lv_obj_set_width(name, lv_pct(100) - depth * 18);
	lv_obj_align(name, LV_ALIGN_LEFT_MID, depth * 18, 0);
}

// ---------------------------------------------------------------------------
// The list is built a screenful at a time
//
// Every entry is a card with a title that wraps, and LVGL has to measure all of
// them before it can draw the first, at a cost that grows faster than the list
// does. All the reader waits on is the first few entries.
//
// So the first batch goes up and the rest follow as the list is scrolled. The
// entries that arrive are appended below the ones already there, which is the
// one place where adding something does not move what is on screen.
// ---------------------------------------------------------------------------

#define CHAPTERS_FIRST 12
#define CHAPTERS_MORE 12

// One entry by its place in the list, whichever of the two the book provides.
static void chapter_entry_at(uint32_t i) {
	if (chapters_from_toc) {
		const ebook_toc_entry_t *entry = ebook_toc(book, i);
		if (entry) {
			chapter_entry(entry->label, entry->spine, entry->depth);
		}
		return;
	}
	// A book with no table of contents still has chapters: the spine is the
	// list, and a numbered entry is better than an empty panel.
	char label[64];
	snprintf(label, sizeof(label), "%s %u", tr("chapter"), i + 1u);
	chapter_entry(label, i, 0);
}

static void chapters_add(uint32_t how_many) {
	uint32_t end = chapters_built + how_many;
	if (end > chapters_total) {
		end = chapters_total;
	}
	for (; chapters_built < end; chapters_built++) {
		chapter_entry_at(chapters_built);
	}
}

// Keeps going until there is something below the fold, so that a panel taller
// than the first batch still has a scroll to ask for the rest with. Bounded
// because a book of one-line chapters on a tall panel could otherwise walk the
// whole list here, which is the thing this is for avoiding.
static void chapters_fill_panel(void) {
	for (int guard = 0; guard < 4 && chapters_built < chapters_total; guard++) {
		lv_obj_update_layout(menu_body);
		if (lv_obj_get_scroll_bottom(menu_body) > 0) {
			return;
		}
		chapters_add(CHAPTERS_MORE);
	}
}

static void chapters_scrolled_cb(lv_event_t *e) {
	(void)e;
	if (menu_section != SECTION_CHAPTERS || chapters_built >= chapters_total) {
		return;
	}
	// Within one panel's worth of the bottom: far enough ahead that the next
	// batch is there before the finger reaches it.
	if (lv_obj_get_scroll_bottom(menu_body) < lv_obj_get_height(menu_body)) {
		chapters_add(CHAPTERS_MORE);
	}
}

static void build_chapters(void) {
	chapters_built = 0;
	chapters_total = ebook_toc_count(book);
	chapters_from_toc = chapters_total > 0;
	if (!chapters_from_toc) {
		chapters_total = ebook_spine_count(book);
	}

	chapters_add(CHAPTERS_FIRST);
	chapters_fill_panel();
}

// ---------------------------------------------------------------------------
// how the text is set
// ---------------------------------------------------------------------------

// The four things about how the text is set. One table rather than four copies
// of the same function: they differ only in their limits and in what has to be
// redone when they change.
typedef enum {
	OPT_SIZE = 0,
	OPT_LINE,
	OPT_MARGIN,
	OPT_WORD,
	OPT_COUNT,
} text_option_t;

typedef struct {
	const char *key; // in the reader's config file
	int *value;
	int min, max;
} text_option_def_t;

// The names, in their own plain array rather than as a field of the struct
// below. tools/extract_strings.py finds a tag that reaches tr() through an index
// only by being told the array's name (its TABLES list), and it can read an
// array of strings but not a field of an array of structs, so these cannot be
// folded in there.
static const char *const stepper_tags[OPT_COUNT] = {"ebookreader_text_size", "ebookreader_line_spacing", "ebookreader_page_margins", "ebookreader_word_spacing"};

static const text_option_def_t TEXT_OPTIONS[OPT_COUNT] = {
	{"font_size", &opt_size, READER_SIZE_MIN, READER_SIZE_MAX},
	{"line_space", &opt_line, READER_LINE_MIN, READER_LINE_MAX},
	{"margin", &opt_margin, READER_MARGIN_MIN, READER_MARGIN_MAX},
	{"word_space", &opt_word, READER_WORD_MIN, READER_WORD_MAX},
};

// The label between the two buttons: a value at the end of the row's name reads
// as part of the name, and the thing the buttons change should be the thing
// between them.
static lv_obj_t *stepper_values[OPT_COUNT];

static void steppers_refresh(void) {
	for (int i = 0; i < OPT_COUNT; i++) {
		if (stepper_values[i]) {
			lv_label_set_text_fmt(stepper_values[i], "%d", *TEXT_OPTIONS[i].value);
		}
	}
}

static void option_cb(lv_event_t *e) {
	// The option and the direction packed into one pointer: the row builds two
	// buttons and they differ only in the sign.
	int packed = (int)(intptr_t)lv_event_get_user_data(e);
	int which = packed >> 1;
	int step = (packed & 1) ? 1 : -1;
	if (which < 0 || which >= OPT_COUNT) {
		return;
	}

	const text_option_def_t *def = &TEXT_OPTIONS[which];
	int want = *def->value + step;
	if (want < def->min || want > def->max) {
		return;
	}
	*def->value = want;
	config_store_set_int(config_ebook_store(), "ebook", def->key, want);
	config_store_save(config_ebook_store());

	if (which == OPT_SIZE) {
		ebookfonts_set_size(opt_size);
	} else if (which == OPT_MARGIN) {
		lv_obj_set_style_pad_all(page_box, opt_margin, 0);
		lv_obj_set_style_pad_all(page_spare, opt_margin, 0);
	}

	// The reading position survives all four because it is a character offset
	// and not a pixel one: the same word stays at the top of the page. The trail
	// of page starts behind it does not, because every one of these changes
	// where a page ends -- and neither does the page number, for the same reason.
	page_stack_depth = 0;
	page_number_recount();
	show_page();
	steppers_refresh();
}

// A round button in the user's accent colour with a glyph in it. The plus and
// the minus of every stepper.
static void stepper_button(lv_obj_t *parent, const char *glyph, int which, bool up) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, 56, 56);
	lv_obj_add_style(btn, &theme_style_accent_bg, 0);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_pad_all(btn, 0, 0);
	lv_obj_add_event_cb(btn, option_cb, LV_EVENT_CLICKED, (void *)(intptr_t)((which << 1) | (up ? 1 : 0)));

	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, glyph);
	lv_obj_set_style_text_color(label, lv_color_white(), 0);
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	lv_obj_center(label);
}

// A row: the name, then minus, the value, plus.
static void stepper_row(lv_obj_t *parent, int which) {
	lv_obj_t *pills = NULL;
	settingsrow_pills(parent, stepper_tags[which], &pills);

	stepper_button(pills, "−", which, false);

	lv_obj_t *value = lv_label_create(pills);
	lv_label_set_text(value, "0");
	lv_obj_add_style(value, &theme_style_text, 0);
	lv_obj_set_style_text_font(value, &font_ui_24, 0);
	lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_CENTER, 0);
	// Wide enough for two digits at any of the four, so the plus does not walk
	// left and right as the number changes under it.
	lv_obj_set_width(value, 56);

	stepper_button(pills, "+", which, true);
	stepper_values[which] = value;
}

static void build_font(void) {
	for (int i = 0; i < OPT_COUNT; i++) {
		stepper_values[i] = NULL;
		stepper_row(menu_body, i);
	}
	steppers_refresh();

	if (!ebookfonts_present()) {
		// Said plainly: a reader quietly using the interface font looks like a
		// reader with a bug, and the fix is to put the files on the card.
		lv_obj_t *note = lv_label_create(menu_body);
		lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(note, lv_pct(100));
		lv_obj_add_style(note, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(note, &font_ui_20, 0);
		lv_label_set_text(note, tr("ebookreader_bookerly_missing"));
	}
}

// ---------------------------------------------------------------------------
// how the book looks, and how a page gives way to the next
// ---------------------------------------------------------------------------

static lv_obj_t *theme_pills[3];
static lv_obj_t *turn_rows[3];
static lv_obj_t *turn_ticks[3];

static void theme_refresh(void) {
	for (int i = 0; i < 3; i++) {
		if (theme_pills[i]) {
			settingsrow_pill_active(theme_pills[i], i == (int)opt_theme);
		}
	}
}

static void theme_cb(lv_event_t *e) {
	opt_theme = (reader_theme_t)(intptr_t)lv_event_get_user_data(e);
	config_store_set_int(config_ebook_store(), "ebook", "theme", (int)opt_theme);
	config_store_save(config_ebook_store());
	theme_refresh();
	show_page();
}

static void turn_refresh(void) {
	for (int i = 0; i < 3; i++) {
		if (turn_ticks[i]) {
			lv_obj_set_style_image_opa(turn_ticks[i], i == (int)opt_turn ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
		}
		if (turn_rows[i]) {
			lv_obj_set_style_bg_opa(turn_rows[i], i == (int)opt_turn ? LV_OPA_20 : LV_OPA_TRANSP, 0);
		}
	}
}

static void turn_cb(lv_event_t *e) {
	turn_mode_t want = (turn_mode_t)(intptr_t)lv_event_get_user_data(e);
	if (want == opt_turn) {
		return;
	}
	bool was_list = opt_turn == TURN_VERTICAL;
	bool now_list = want == TURN_VERTICAL;

	opt_turn = want;
	config_store_set_int(config_ebook_store(), "ebook", "turn", (int)opt_turn);
	config_store_save(config_ebook_store());
	turn_refresh();

	// Whatever the slide was doing, it is not doing it any more.
	lv_anim_delete(page_box, NULL);
	lv_anim_delete(page_spare, NULL);
	turning = false;
	lv_obj_clean(page_spare);
	lv_obj_set_x(page_box, 0);
	lv_obj_set_x(page_spare, (int32_t)g_cfg->screen_width);

	if (was_list == now_list) {
		// Instant and Slide paginate identically -- they differ only in how the
		// page arrives -- so the page on screen is already the right page and the
		// trail behind it is still true. Re-rendering here would make the book
		// appear to turn a page by itself the moment the mode changed.
		return;
	}

	// Crossing between pages and one long list. The trail means nothing in
	// either direction, and a scroll position is a block rather than a
	// character into one, so the block stays and the offset goes.
	page_stack_depth = 0;
	page_offset = 0;
	if (now_list) {
		lv_obj_add_flag(page_box, LV_OBJ_FLAG_SCROLLABLE);
	} else {
		lv_obj_remove_flag(page_box, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_scroll_to_y(page_box, 0, LV_ANIM_OFF);
	}
	// One long list has no pages; going back to pages means counting again.
	page_number_recount();
	show_page();
}

// One way of turning a page: the icon, its name beside it, and a tick on the
// right for the one in use.
static void turn_row(lv_obj_t *parent, int index, const lv_image_dsc_t *icon, const char *tag) {
	lv_obj_t *row = lv_btn_create(parent);
	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_height(row, 76);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_bg_color(row, theme()->accent, 0);
	lv_obj_set_style_radius(row, 12, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, 16, 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 14, 0);
	lv_obj_add_event_cb(row, turn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

	lv_obj_t *glyph = lv_image_create(row);
	lv_image_set_src(glyph, icon);
	lv_obj_set_style_image_recolor(glyph, theme()->text_primary, 0);
	lv_obj_set_style_image_recolor_opa(glyph, LV_OPA_COVER, 0);

	lv_obj_t *name = lv_label_create(row);
	lv_label_set_text(name, tr(tag));
	lv_obj_add_style(name, &theme_style_text, 0);
	lv_obj_set_style_text_font(name, &font_ui_22, 0);
	lv_obj_set_flex_grow(name, 1);

	lv_obj_t *tick = lv_image_create(row);
	lv_image_set_src(tick, &icon_check);
	lv_obj_set_style_image_recolor(tick, theme()->accent, 0);
	lv_obj_set_style_image_recolor_opa(tick, LV_OPA_COVER, 0);

	turn_rows[index] = row;
	turn_ticks[index] = tick;
}

static void build_theme(void) {
	lv_obj_t *pills = NULL;
	settingsrow_pills(menu_body, "ebookreader_reading_theme", &pills);
	theme_pills[0] = settingsrow_pill(pills, "ebookreader_theme_paper", THEME_LIGHT, theme_cb);
	theme_pills[1] = settingsrow_pill(pills, "ebookreader_theme_sepia", THEME_SEPIA, theme_cb);
	theme_pills[2] = settingsrow_pill(pills, "ebookreader_theme_night", THEME_DARK, theme_cb);
	theme_refresh();

	lv_obj_t *heading = lv_label_create(menu_body);
	lv_label_set_text(heading, tr("ebookreader_page_turn"));
	lv_obj_add_style(heading, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(heading, &font_ui_20, 0);
	lv_obj_set_style_margin_top(heading, 10, 0);

	turn_row(menu_body, TURN_INSTANT, &icon_ebook_turn_fast, "ebookreader_turn_instant");
	turn_row(menu_body, TURN_SLIDE, &icon_ebook_turn_slide, "ebookreader_turn_slide");
	turn_row(menu_body, TURN_VERTICAL, &icon_ebook_turn_vertical, "ebookreader_turn_vertical");
	turn_refresh();
}

// ---------------------------------------------------------------------------
// building the menu
// ---------------------------------------------------------------------------

// One of the three things on the bar: the icon above, its name under it.
static void bar_button(lv_obj_t *parent, const lv_image_dsc_t *icon, const char *tag, section_t which) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(btn, 1);
	lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
	lv_obj_add_style(btn, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(btn, 14, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_pad_ver(btn, 14, 0);
	lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(btn, 8, 0);
	lv_obj_add_event_cb(btn, section_open, LV_EVENT_CLICKED, (void *)(intptr_t)which);

	lv_obj_t *glyph = lv_image_create(btn);
	lv_image_set_src(glyph, icon);
	lv_obj_set_style_image_recolor(glyph, theme()->text_primary, 0);
	lv_obj_set_style_image_recolor_opa(glyph, LV_OPA_COVER, 0);
	remember_glyph(glyph);

	lv_obj_t *name = lv_label_create(btn);
	lv_label_set_text(name, tr(tag));
	lv_obj_add_style(name, &theme_style_text, 0);
	lv_obj_set_style_text_font(name, &font_ui_18, 0);
}

static void build_menu(gui_config_t *cfg) {
	// The same veil the pop-overs put over a page, for the same reason: what is
	// underneath stays visible and every tap that misses the menu closes it.
	menu_veil = lv_obj_create(ebookreader_screen);
	lv_obj_remove_style_all(menu_veil);
	lv_obj_set_size(menu_veil, cfg->screen_width, cfg->screen_height);
	lv_obj_set_style_bg_color(menu_veil, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(menu_veil, LV_OPA_60, 0);
	lv_obj_add_flag(menu_veil, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(menu_veil, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_event_cb(menu_veil, menu_veil_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_flag(menu_veil, LV_OBJ_FLAG_HIDDEN);

	// --- the bar along the bottom
	menu_bar = lv_obj_create(menu_veil);
	lv_obj_add_style(menu_bar, &theme_style_panel, 0);
	lv_obj_set_size(menu_bar, cfg->screen_width - 2 * MENU_INSET, LV_SIZE_CONTENT);
	lv_obj_align(menu_bar, LV_ALIGN_BOTTOM_MID, 0, -MENU_INSET);
	lv_obj_set_style_radius(menu_bar, 18, 0);
	lv_obj_set_style_border_width(menu_bar, 0, 0);
	lv_obj_set_style_shadow_width(menu_bar, 0, 0);
	lv_obj_set_style_pad_all(menu_bar, 10, 0);
	lv_obj_set_flex_flow(menu_bar, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(menu_bar, 8, 0);
	lv_obj_remove_flag(menu_bar, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *icons = lv_obj_create(menu_bar);
	lv_obj_remove_style_all(icons);
	lv_obj_set_size(icons, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(icons, LV_FLEX_FLOW_ROW);
	lv_obj_remove_flag(icons, LV_OBJ_FLAG_SCROLLABLE);
	bar_button(icons, &icon_ebook_chapter, "chapters", SECTION_CHAPTERS);
	bar_button(icons, &icon_ebook_font, "ebookreader_font_settings", SECTION_FONT);
	bar_button(icons, &icon_ebook_theme, "ebookreader_book_theme", SECTION_THEME);

	// Under the three, separated: leaving is not a fourth thing to look at
	// while reading, but it has to be somewhere and this is the only panel the
	// reader has.
	lv_obj_t *close_row = lv_btn_create(menu_bar);
	lv_obj_set_size(close_row, lv_pct(100), 68);
	// Same reason as the card below: on the light theme a card on a panel is
	// white on white, and the one row that has to be found is the one that is
	// not there.
	lv_obj_add_style(close_row, &theme_style_screen, 0);
	lv_obj_add_style(close_row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(close_row, 12, 0);
	lv_obj_set_style_border_width(close_row, 0, 0);
	lv_obj_set_style_shadow_width(close_row, 0, 0);
	lv_obj_add_event_cb(close_row, close_book_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *close_label = lv_label_create(close_row);
	lv_label_set_text(close_label, tr("ebookreader_close_the_book"));
	lv_obj_add_style(close_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(close_label, &font_ui_22, 0);
	lv_obj_center(close_label);

	// --- the card each of the three opens
	menu_card = lv_obj_create(menu_veil);
	// The background of a page and not of a panel, because what goes inside are
	// setting rows, and a setting row is drawn as a card: on the light theme
	// card and panel are the same white, and the rows vanish into the card they
	// sit on. Every other page in the player has that same relationship --
	// cards on the page colour -- so this is the shape, not a workaround.
	lv_obj_add_style(menu_card, &theme_style_screen, 0);
	lv_obj_set_size(menu_card, cfg->screen_width - 2 * MENU_INSET, cfg->screen_height * 3 / 4);
	lv_obj_center(menu_card);
	lv_obj_set_style_radius(menu_card, 18, 0);
	lv_obj_set_style_border_width(menu_card, 0, 0);
	lv_obj_set_style_shadow_width(menu_card, 0, 0);
	lv_obj_set_style_pad_all(menu_card, 12, 0);
	lv_obj_set_flex_flow(menu_card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(menu_card, 10, 0);
	// Only the body scrolls. Left scrollable the card scrolls too, and the
	// heading -- which carries the way back to the bar -- slides off the top.
	lv_obj_remove_flag(menu_card, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(menu_card, LV_OBJ_FLAG_HIDDEN);

	lv_obj_t *header = lv_obj_create(menu_card);
	lv_obj_remove_style_all(header);
	lv_obj_set_size(header, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(header, 10, 0);
	lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *back = lv_btn_create(header);
	lv_obj_set_size(back, 56, 56);
	lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
	lv_obj_add_style(back, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_border_width(back, 0, 0);
	lv_obj_set_style_shadow_width(back, 0, 0);
	lv_obj_add_event_cb(back, section_back_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *chev = lv_image_create(back);
	lv_image_set_src(chev, &icon_chevron_left);
	lv_obj_center(chev);
	lv_obj_set_style_image_recolor(chev, theme()->text_primary, 0);
	lv_obj_set_style_image_recolor_opa(chev, LV_OPA_COVER, 0);
	remember_glyph(chev);

	menu_heading = lv_label_create(header);
	lv_label_set_text(menu_heading, "");
	lv_obj_add_style(menu_heading, &theme_style_text, 0);
	lv_obj_set_style_text_font(menu_heading, &font_ui_24, 0);

	menu_body = lv_obj_create(menu_card);
	lv_obj_remove_style_all(menu_body);
	// Whatever the heading leaves, not the whole card: a full height here is a
	// body taller than the room it sits in, which is what makes a card scroll
	// when only its list should.
	lv_obj_set_width(menu_body, lv_pct(100));
	lv_obj_set_flex_grow(menu_body, 1);
	lv_obj_set_flex_flow(menu_body, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(menu_body, 8, 0);
	lv_obj_set_style_pad_right(menu_body, 6, 0);
	// The rest of a long chapter list arrives as it is scrolled to; the handler
	// leaves every other section alone.
	lv_obj_add_event_cb(menu_body, chapters_scrolled_cb, LV_EVENT_SCROLL, NULL);
}

// The page of a book wants the whole panel: the clock and the battery are
// forty-four rows of something else to look at, and the chevron sits exactly
// where the first line of text begins. Both come back when the book is put
// down.
static void reader_loaded_cb(lv_event_t *e) {
	(void)e;
	topbar_set_hidden(true);
	back_btn_force_hidden(true);
}

static void reader_unloaded_cb(lv_event_t *e) {
	(void)e;
	back_btn_force_hidden(false);
	topbar_set_hidden(false);

	// Everything the book owns goes here, and in this order: the objects that
	// point at the fonts first, then the fonts, then the book. The other way
	// round is a label drawing from a face that has been destroyed.
	lv_anim_delete(page_box, NULL);
	lv_anim_delete(page_spare, NULL);
	turning = false;
	if (page_box) {
		lv_obj_clean(page_box);
	}
	if (page_spare) {
		lv_obj_clean(page_spare);
	}
	menu_close();
	for (int i = 0; i < OPT_COUNT; i++) {
		stepper_values[i] = NULL;
	}
	theme_pills[0] = theme_pills[1] = theme_pills[2] = NULL;
	turn_rows[0] = turn_rows[1] = turn_rows[2] = NULL;
	turn_ticks[0] = turn_ticks[1] = turn_ticks[2] = NULL;
	ebookfonts_close();
	if (book) {
		position_save();
		ebook_log_memory(book, "before closing");
		ebook_close(book);
		book = NULL;
		ebook_log_memory(NULL, "closed");
	}
	power_hold_screen_on(false);
}

// Where the next open should land, when it is not "wherever it was left". Set
// by ebookreader_open_at() and consumed by the open it belongs to: a bookmark
// names a place, and the place has to survive the call that opens the book.
static bool want_position;
static uint32_t want_spine, want_block, want_offset;

void ebookreader_open_at(const char *path, uint32_t spine, uint32_t block, uint32_t offset) {
	want_position = true;
	want_spine = spine;
	want_block = block;
	want_offset = offset;
	ebookreader_open(path);
}

void ebookreader_open(const char *path) {
	if (!path || !path[0]) {
		return;
	}
	if (book) {
		ebook_close(book);
		book = NULL;
	}

	book = ebook_open(path);
	if (!book) {
		want_position = false; // it was for this open and this open is not happening
		gui_notify_popup("ebookreader_open_failed");
		return;
	}

	opt_size = (int)config_store_get_int(config_ebook_store(), "ebook", "font_size", 20);
	opt_line = (int)config_store_get_int(config_ebook_store(), "ebook", "line_space", 4);
	opt_margin = (int)config_store_get_int(config_ebook_store(), "ebook", "margin", 20);
	opt_word = (int)config_store_get_int(config_ebook_store(), "ebook", "word_space", 0);
	if (opt_word < READER_WORD_MIN || opt_word > READER_WORD_MAX) {
		opt_word = 0;
	}
	opt_theme = (reader_theme_t)config_store_get_int(config_ebook_store(), "ebook", "theme", THEME_LIGHT);
	opt_turn = (turn_mode_t)config_store_get_int(config_ebook_store(), "ebook", "turn", TURN_INSTANT);
	if (opt_size < READER_SIZE_MIN || opt_size > READER_SIZE_MAX) {
		opt_size = 20;
	}
	if (opt_turn > TURN_VERTICAL) {
		opt_turn = TURN_INSTANT;
	}

	ebookfonts_open(g_cfg->sd_root_path, opt_size);
	choose_word_gap();

	// The strip may have grown or shrunk since the last book: its options live
	// on their own page, reached from the shelf, so they can change while no
	// book is open.
	int32_t box_h = (int32_t)g_cfg->screen_height - bar_space();
	lv_obj_set_height(page_box, box_h);
	lv_obj_set_height(page_spare, box_h);
	lv_obj_set_style_pad_all(page_box, opt_margin, 0);
	lv_obj_set_style_pad_all(page_spare, opt_margin, 0);
	lv_obj_set_x(page_box, 0);
	lv_obj_set_x(page_spare, (int32_t)g_cfg->screen_width);
	turning = false;
	if (opt_turn == TURN_VERTICAL) {
		lv_obj_add_flag(page_box, LV_OBJ_FLAG_SCROLLABLE);
	} else {
		lv_obj_remove_flag(page_box, LV_OBJ_FLAG_SCROLLABLE);
	}

	uint32_t spine, block, offset;
	if (want_position) {
		spine = want_spine;
		block = want_block;
		offset = want_offset;
	} else {
		position_load(path, &spine, &block, &offset);
	}
	want_position = false;
	if (spine >= ebook_spine_count(book)) {
		spine = 0;
		block = offset = 0;
	}
	if (!load_chapter(spine, block, offset)) {
		load_chapter(0, 0, 0);
	}

	// A page of a book is looked at for minutes without a finger touching the
	// glass, which is exactly what the screen timeout is for -- so it is held
	// off while the reader is up.
	power_hold_screen_on(true);
	switch_screen(ebookreader_screen);
}

// The two page boxes are identical and built by the same lines, because the
// slide swaps them and a difference between them would be a difference that
// appears every other turn.
static lv_obj_t *make_page_box(gui_config_t *cfg) {
	lv_obj_t *box = lv_obj_create(ebookreader_screen);
	lv_obj_remove_style_all(box);
	lv_obj_set_size(box, cfg->screen_width, cfg->screen_height - bar_space());
	lv_obj_set_pos(box, 0, 0);
	lv_obj_set_style_pad_all(box, opt_margin, 0);
	lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
	lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(box, page_pressed_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(box, page_pressed_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(box, page_pressed_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(box, page_pressed_cb, LV_EVENT_PRESS_LOST, NULL);
	lv_obj_add_event_cb(box, vertical_scrolled_cb, LV_EVENT_SCROLL_END, NULL);
	return box;
}

void ebookreader_init(gui_config_t *cfg) {
	g_cfg = cfg;

	ebookreader_screen = lv_obj_create(NULL);
	lv_obj_remove_style_all(ebookreader_screen);
	lv_obj_set_size(ebookreader_screen, cfg->screen_width, cfg->screen_height);
	lv_obj_remove_flag(ebookreader_screen, LV_OBJ_FLAG_SCROLLABLE);

	page_box = make_page_box(cfg);
	page_spare = make_page_box(cfg);
	lv_obj_set_x(page_spare, (int32_t)cfg->screen_width);

	ebookbar_create(&status_bar, ebookreader_screen, cfg->screen_width);
	lv_obj_align(status_bar.root, LV_ALIGN_BOTTOM_MID, 0, -6);

	build_menu(cfg);

	// No switcher_attach_back_gesture() here, deliberately: the swipe-back and
	// the page turn are the same gesture, and the page turn is the one a reader
	// makes a thousand times an hour. Leaving is close_book_cb().
	theme_register_refresh(menu_theme_refresh);

	lv_obj_add_event_cb(ebookreader_screen, reader_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(ebookreader_screen, reader_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
}

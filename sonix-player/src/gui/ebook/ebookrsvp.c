#include "ebookrsvp.h"

#include <stdio.h>
#include <string.h>

#include "src/gui/ebook/ebookfonts.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/system/core/lang.h"
#include "src/system/ebook/ebook_utf8.h"

// ---------------------------------------------------------------------------
// The words
//
// The chapter is blocks of spans of text (ebook.h). A word is found from a
// block and a byte offset into it, lazily: nothing is tokenised ahead of
// time, so a chapter of forty thousand words costs nothing until it is read.
// A position is the same block and byte offset the pages keep, so a bookmark
// made on a page opens in this view at the same word and the other way round.
//
// Where a word ends: at ASCII white space and the newline, at the two spaces
// the reader itself puts into a page (U+00A0 and U+2009), and at the other
// Unicode spaces. Kana, kanji and hanzi have no spaces between them, so every
// character from U+3000 up -- a lead byte of 0xE3 or more, the rule the pages
// break lines by -- is a word of its own; a closing mark after it (the
// ideographic full stop and comma, a closing corner bracket) stays with it
// and an opening bracket before it goes with it. A word is never cut inside a
// character's bytes.
// ---------------------------------------------------------------------------

// The longest word kept whole, in bytes. A longer one is shown cut; the walk
// still passes over all of it.
#define WORD_MAX 96

typedef struct {
	uint32_t block;	 // where the word starts
	uint32_t offset; // the byte, from the start of the block
	uint32_t end;	 // the byte after it
	char text[WORD_MAX];
	uint32_t cps;		// how many characters
	uint8_t style;		// EBOOK_STYLE_* of the span it starts in
	bool cjk;			// a single kana, kanji or hanzi, with its marks
	bool clause_end;	// ends with a comma, a semicolon or a colon
	bool sentence_end;	// ends with a full stop, ! ? or an ellipsis, quotes after them included
	bool paragraph_end; // the last word of its block
} word_t;

// One character of a block as the walk sees it.
typedef struct {
	uint32_t cp;
	const char *bytes;
	uint32_t n;
	uint8_t style;
} ch_t;

static gui_config_t *g_cfg;
static const ebookrsvp_hooks_t *hooks;
static ebook_t *book;

static bool is_space(uint32_t cp) { return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0x00A0 || (cp >= 0x2000 && cp <= 0x200B) || cp == 0x202F || cp == 0x205F || cp == 0x3000; }

// Every character from U+3000 up, which is where the kana, the kanji and the
// hanzi are. Checked after is_space(), which takes U+3000 itself. Hangul is
// above U+3000 too and so is split a syllable at a time, on purpose: it is
// the same rule the pages break a line by (chars_that_fit), so a bookmark
// made there and one made here agree.
static bool is_cjk(uint32_t cp) { return cp >= 0x3000; }

// A mark that stays with the character before it: the ideographic comma and
// full stop, the closing brackets, and the full-width punctuation.
static bool cjk_closes(uint32_t cp) {
	switch (cp) {
	case 0x3001:
	case 0x3002:
	case 0x3009:
	case 0x300B:
	case 0x300D:
	case 0x300F:
	case 0x3011:
	case 0x3015:
	case 0xFF01:
	case 0xFF09:
	case 0xFF0C:
	case 0xFF0E:
	case 0xFF1A:
	case 0xFF1B:
	case 0xFF1F:
	case 0xFF3D:
	case 0xFF5D:
		return true;
	default:
		return false;
	}
}

// An opening bracket, which goes with the character after it.
static bool cjk_opens(uint32_t cp) {
	switch (cp) {
	case 0x3008:
	case 0x300A:
	case 0x300C:
	case 0x300E:
	case 0x3010:
	case 0x3014:
	case 0xFF08:
	case 0xFF3B:
	case 0xFF5B:
		return true;
	default:
		return false;
	}
}

// A closing quote or bracket, which a sentence may end with after its full
// stop: the stop is what counts, not the quote.
static bool closes_quote(uint32_t cp) {
	switch (cp) {
	case '"':
	case '\'':
	case ')':
	case ']':
	case 0x2019: // right single quotation mark
	case 0x201D: // right double quotation mark
	case 0x00BB: // right-pointing double angle quotation mark
	case 0x3009:
	case 0x300B:
	case 0x300D:
	case 0x300F:
	case 0x3011:
	case 0x3015:
	case 0xFF09:
	case 0xFF3D:
	case 0xFF5D:
		return true;
	default:
		return false;
	}
}

static bool ends_sentence(uint32_t cp) { return cp == '.' || cp == '!' || cp == '?' || cp == 0x2026 || cp == 0x3002 || cp == 0xFF01 || cp == 0xFF1F || cp == 0xFF0E; }

static bool ends_clause(uint32_t cp) { return cp == ',' || cp == ';' || cp == ':' || cp == 0x3001 || cp == 0xFF0C || cp == 0xFF1B || cp == 0xFF1A; }

// A letter from a right-to-left script. The pivot is a left-to-right idea,
// so such a word is centred whole instead.
static bool is_rtl(uint32_t cp) { return (cp >= 0x0590 && cp <= 0x08FF) || (cp >= 0xFB1D && cp <= 0xFDFF) || (cp >= 0xFE70 && cp <= 0xFEFF); }

// Reads the character of `b` at byte `*at` and moves past it. False at the
// end of the block. The spans of a block are walked from the first every
// time, which is a few steps for a paragraph with a run of italics in it.
static bool block_char(const ebook_block_t *b, uint32_t *at, ch_t *c) {
	const char *pool = ebook_chapter_text(book);
	uint32_t seen = 0;
	for (uint32_t s = 0; s < b->span_count; s++) {
		const ebook_span_t *sp = ebook_span(book, b->first_span + s);
		if (*at >= seen + sp->length) {
			seen += sp->length;
			continue;
		}
		uint32_t within = *at - seen;
		c->bytes = pool + sp->offset + within;
		c->cp = ebook_utf8_take(pool + sp->offset, sp->length, &within);
		c->n = (uint32_t)(pool + sp->offset + within - c->bytes);
		c->style = sp->style;
		*at = seen + within;
		return true;
	}
	return false;
}

static bool text_block(const ebook_block_t *b) { return b->type != EBOOK_BLOCK_IMAGE && b->type != EBOOK_BLOCK_RULE; }

static bool western_letter(uint32_t cp) { return cp >= 0x80 || (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z'); }

// The first word of block `index` that starts at or after byte `at`. False
// when the block has none left.
static bool scan_word(uint32_t index, uint32_t at, word_t *w) {
	const ebook_block_t *b = ebook_block(book, index);
	if (!text_block(b)) {
		return false;
	}
	ch_t c;
	uint32_t save = at;
	while (block_char(b, &at, &c) && is_space(c.cp)) {
		save = at;
	}
	at = save;

	memset(w, 0, sizeof(*w));
	w->block = index;
	w->offset = at;
	uint32_t len = 0;
	uint32_t last_mark = 0; // the last character that is not a closing quote
	bool only_openers = false;

	for (;;) {
		save = at;
		if (!block_char(b, &at, &c) || is_space(c.cp)) {
			at = save;
			break;
		}
		if (w->cps == 0) {
			w->style = c.style;
			w->cjk = is_cjk(c.cp);
			only_openers = cjk_opens(c.cp);
		} else if (is_cjk(c.cp)) {
			if (cjk_closes(c.cp)) {
				only_openers = false;
			} else if (only_openers) {
				w->cjk = true;
				only_openers = cjk_opens(c.cp);
			} else {
				at = save; // a character of its own
				break;
			}
		} else if (w->cjk && !only_openers && western_letter(c.cp)) {
			at = save; // a Western word after a character
			break;
		} else {
			only_openers = false;
		}
		if (len + c.n < WORD_MAX) {
			memcpy(w->text + len, c.bytes, c.n);
			len += c.n;
		}
		w->cps++;
		if (!closes_quote(c.cp)) {
			last_mark = c.cp;
		}
	}
	if (w->cps == 0) {
		return false;
	}
	w->text[len] = '\0';
	w->end = at;
	w->sentence_end = ends_sentence(last_mark);
	w->clause_end = ends_clause(last_mark);

	// Anything but space after it, in this block?
	w->paragraph_end = true;
	while (block_char(b, &at, &c)) {
		if (!is_space(c.cp)) {
			w->paragraph_end = false;
			break;
		}
	}
	return true;
}

// The first word at or after `block`/`offset`, in this block or a later one.
// False at the end of the chapter.
static bool next_word(uint32_t block, uint32_t offset, word_t *w) {
	uint32_t count = ebook_block_count(book);
	for (; block < count; block++, offset = 0) {
		if (scan_word(block, offset, w)) {
			return true;
		}
	}
	return false;
}

// The last word that starts before `block`/`offset`: the one before the word
// starting there. Found by walking its block from the start, which is a
// paragraph's worth of work. False at the start of the chapter.
static bool prev_word(uint32_t block, uint32_t offset, word_t *w) {
	uint32_t count = ebook_block_count(book);
	if (block >= count) {
		block = count;
		offset = 0;
	}
	for (;;) {
		if (block < count && offset > 0) {
			word_t probe;
			bool found = false;
			uint32_t at = 0;
			while (scan_word(block, at, &probe) && probe.offset < offset) {
				*w = probe;
				found = true;
				at = probe.end;
			}
			if (found) {
				return true;
			}
		}
		if (block == 0) {
			return false;
		}
		block--;
		offset = UINT32_MAX;
	}
}

// Moves `w` back to the first word of its sentence.
static void sentence_start(word_t *w) {
	for (;;) {
		word_t earlier;
		if (!prev_word(w->block, w->offset, &earlier) || earlier.sentence_end || earlier.paragraph_end) {
			return;
		}
		*w = earlier;
	}
}

// Which character of the word the eye is parked on: the first of a short
// word, then one step right for every four more letters, never past the
// fifth.
static uint32_t pivot_index(uint32_t cps) {
	if (cps <= 1) {
		return 0;
	}
	if (cps <= 5) {
		return 1;
	}
	if (cps <= 9) {
		return 2;
	}
	if (cps <= 13) {
		return 3;
	}
	return 4;
}

// ---------------------------------------------------------------------------
// The view
//
// Three labels on one row: what is left of the pivot, ending at the focal
// point; the pivot letter, in the accent colour, centred on it; and the rest,
// starting from it. They share a face, so their baselines agree. The face is
// a second, larger Bookerly than the page's, opened for this view alone --
// see ebookfonts_large_face() -- and a word too wide for the screen at that
// size drops to the page's face rather than off the edge.
//
// The one-line mode replaces the three labels with a spangroup holding as
// many words as fit one line at the page's size.
// ---------------------------------------------------------------------------

// Where the focal point sits: left of the middle, because the pivot is in
// the left half of a word and the rest of the word needs the room.
#define PIVOT_X_PCT 38
// And how far down the screen.
#define ROW_Y_PCT 40

// The large face: twice the page's size, within reason.
#define LARGE_MIN 28
#define LARGE_MAX 44

#define TICK_W 2
#define TICK_H 10
#define TICK_GAP 6

// How long a changed speed stays on the page.
#define FLASH_MS 900

// The middle dot between the parts of the status line, U+00B7.
#define MIDDLE_DOT "\xC2\xB7"

static rsvp_mode_t mode = RSVP_MODE_PAGES;
static int wpm = RSVP_WPM_DEFAULT;
static ebookrsvp_look_t look;

static lv_obj_t *box;  // the reader's page box, which the view lives in
static lv_obj_t *view; // everything here, deleted with the page
static lv_obj_t *label_left, *label_pivot, *label_right;
static lv_obj_t *tick_top, *tick_bottom;
static lv_obj_t *line_group; // the one-line mode, built afresh for every line
static lv_obj_t *status;	 // chapter, progress, speed
static lv_obj_t *play_glyph; // shown while paused
static lv_obj_t *flash;		 // the speed, for a moment after a swipe

static lv_timer_t *ticker;
static lv_timer_t *flash_timer;
static bool playing;

// The word on screen, or the first word of the line on screen.
static word_t cur;
static bool have_word;
// In the one-line mode: where the line ends and how it is paced.
static uint32_t line_end;
static int line_words;
static word_t line_last;

static int large_size(void) {
	int size = look.text_size * 2;
	return size < LARGE_MIN ? LARGE_MIN : (size > LARGE_MAX ? LARGE_MAX : size);
}

static bool word_bold(const word_t *w) {
	const ebook_block_t *b = ebook_block(book, w->block);
	return (w->style & EBOOK_STYLE_BOLD) || b->type == EBOOK_BLOCK_HEADING;
}

static bool word_italic(const word_t *w) {
	const ebook_block_t *b = ebook_block(book, w->block);
	return (w->style & EBOOK_STYLE_ITALIC) || b->type == EBOOK_BLOCK_QUOTE;
}

// How wide `len` bytes of `text` are in `font`.
static int32_t text_width(const lv_font_t *font, const char *text, uint32_t len) {
	int32_t width = 0;
	uint32_t at = 0;
	while (at < len) {
		uint32_t cp = ebook_utf8_take(text, len, &at);
		uint32_t peek = at;
		uint32_t next = peek < len ? ebook_utf8_take(text, len, &peek) : 0;
		width += lv_font_get_glyph_width(font, cp, next);
	}
	return width;
}

static int32_t view_width(void) { return lv_obj_get_content_width(box); }

// The row's middle, from the top of the view.
static int32_t row_y(void) { return (int32_t)g_cfg->screen_height * ROW_Y_PCT / 100 - lv_obj_get_style_pad_top(box, 0); }

static int32_t pivot_x(void) { return view_width() * PIVOT_X_PCT / 100; }

static void view_deleted_cb(lv_event_t *e) {
	(void)e;
	view = NULL;
	label_left = label_pivot = label_right = NULL;
	tick_top = tick_bottom = NULL;
	line_group = status = play_glyph = flash = NULL;
}

static lv_obj_t *make_label(const lv_font_t *font, lv_color_t colour) {
	lv_obj_t *label = lv_label_create(view);
	lv_label_set_text(label, "");
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, colour, 0);
	return label;
}

static lv_obj_t *make_tick(void) {
	lv_obj_t *tick = lv_obj_create(view);
	lv_obj_remove_style_all(tick);
	lv_obj_set_size(tick, TICK_W, TICK_H);
	lv_obj_set_style_bg_color(tick, look.accent, 0);
	lv_obj_set_style_bg_opa(tick, LV_OPA_80, 0);
	lv_obj_set_clickable(tick, false);
	return tick;
}

static void build_view(void) {
	view = lv_obj_create(box);
	lv_obj_remove_style_all(view);
	// The page box lays its children out as a column; this is not one of them.
	lv_obj_set_ignore_layout(view, true);
	lv_obj_set_size(view, lv_pct(100), lv_pct(100));
	lv_obj_set_pos(view, 0, 0);
	lv_obj_set_clickable(view, false);
	lv_obj_set_scrollable(view, false);
	lv_obj_add_event_cb(view, view_deleted_cb, LV_EVENT_DELETE, NULL);

	const lv_font_t *font = ebookfonts_face(false, false);
	label_left = make_label(font, look.ink);
	label_pivot = make_label(font, look.accent);
	label_right = make_label(font, look.ink);
	tick_top = make_tick();
	tick_bottom = make_tick();

	status = lv_label_create(view);
	lv_label_set_text(status, "");
	lv_obj_set_style_text_font(status, &font_ui_18, 0);
	lv_obj_set_style_text_color(status, look.ink, 0);
	lv_obj_set_style_text_opa(status, LV_OPA_60, 0);
	lv_obj_align(status, LV_ALIGN_BOTTOM_MID, 0, -8);

	play_glyph = lv_image_create(view);
	lv_image_set_src(play_glyph, &icon_play_large);
	lv_obj_set_style_image_recolor(play_glyph, look.ink, 0);
	lv_obj_set_style_image_recolor_opa(play_glyph, LV_OPA_COVER, 0);
	lv_obj_set_style_image_opa(play_glyph, LV_OPA_50, 0);
	lv_obj_set_clickable(play_glyph, false);

	flash = lv_label_create(view);
	lv_label_set_text(flash, "");
	lv_obj_set_style_text_font(flash, &font_ui_36_bold, 0);
	lv_obj_set_style_text_color(flash, look.ink, 0);
	lv_obj_set_hidden(flash, true);
}

static void show_status(void) {
	if (!status || !book) {
		return;
	}
	uint32_t blocks = ebook_block_count(book);
	int percent = blocks && have_word ? (int)((uint64_t)cur.block * 100u / blocks) : 0;
	lv_label_set_text_fmt(status, "%u/%u  " MIDDLE_DOT "  %d%%  " MIDDLE_DOT "  %d %s", (unsigned)ebook_chapter_index(book) + 1u, (unsigned)ebook_spine_count(book), percent, wpm, tr("ebookreader_rsvp_unit"));
	lv_obj_align(status, LV_ALIGN_BOTTOM_MID, 0, -8);
}

static void show_playing(void) {
	if (play_glyph) {
		lv_obj_set_hidden(play_glyph, playing);
	}
}

static void drop_line(void) {
	if (line_group) {
		lv_obj_delete(line_group);
		line_group = NULL;
	}
	line_words = 0;
}

// Puts the three labels and the ticks where the word on screen wants them.
static void render_word(void) {
	// The one place ebookfonts_large_face() is asked, and so the one place the
	// large set may be closed and reopened at another size. It is always
	// reached right after build_view() made fresh labels that point at the
	// page face, so no live label ever points at a large face being deleted.
	drop_line();
	if (!have_word) {
		lv_label_set_text(label_left, "");
		lv_label_set_text(label_pivot, "");
		lv_label_set_text(label_right, "");
		return;
	}

	bool bold = word_bold(&cur);
	bool italic = word_italic(&cur);
	uint32_t len = (uint32_t)strlen(cur.text);
	uint32_t first_at = 0;
	uint32_t first_cp = ebook_utf8_take(cur.text, len, &first_at);
	bool centred = look.no_pivot || is_rtl(first_cp);

	// The pivot's bytes.
	uint32_t pivot_from = 0, pivot_to = len;
	if (!centred) {
		uint32_t at = 0;
		for (uint32_t i = 0; i < pivot_index(cur.cps) && at < len; i++) {
			ebook_utf8_take(cur.text, len, &at);
		}
		pivot_from = at;
		if (at < len) {
			ebook_utf8_take(cur.text, len, &at);
		}
		pivot_to = at;
	}

	// The large face, unless the word does not fit the screen in it.
	const lv_font_t *font = ebookfonts_large_face(bold, italic, large_size());
	int32_t width = view_width();
	int32_t px = pivot_x();
	int32_t w_left = 0, w_pivot = 0, w_right = 0;
	for (int pass = 0; pass < 2; pass++) {
		w_left = text_width(font, cur.text, pivot_from);
		w_pivot = text_width(font, cur.text + pivot_from, pivot_to - pivot_from);
		w_right = text_width(font, cur.text + pivot_to, len - pivot_to);
		bool fits = centred ? w_pivot <= width : (w_left + w_pivot / 2 <= px && w_pivot / 2 + w_right <= width - px);
		if (fits) {
			break;
		}
		font = ebookfonts_face(bold, italic);
	}

	lv_obj_set_style_text_font(label_left, font, 0);
	lv_obj_set_style_text_font(label_pivot, font, 0);
	lv_obj_set_style_text_font(label_right, font, 0);
	lv_obj_set_style_text_color(label_pivot, centred ? look.ink : look.accent, 0);

	char piece[WORD_MAX];
	memcpy(piece, cur.text, pivot_from);
	piece[pivot_from] = '\0';
	lv_label_set_text(label_left, piece);
	memcpy(piece, cur.text + pivot_from, pivot_to - pivot_from);
	piece[pivot_to - pivot_from] = '\0';
	lv_label_set_text(label_pivot, piece);
	lv_label_set_text(label_right, cur.text + pivot_to);

	int32_t line_h = lv_font_get_line_height(font);
	int32_t top = row_y() - line_h / 2;
	int32_t pivot_left = px - w_pivot / 2;
	lv_obj_set_pos(label_left, pivot_left - w_left, top);
	lv_obj_set_pos(label_pivot, pivot_left, top);
	lv_obj_set_pos(label_right, pivot_left + w_pivot, top);

	lv_obj_set_pos(tick_top, px - TICK_W / 2, top - TICK_GAP - TICK_H);
	lv_obj_set_pos(tick_bottom, px - TICK_W / 2, top + line_h + TICK_GAP);
	lv_obj_set_hidden(tick_top, false);
	lv_obj_set_hidden(tick_bottom, false);

	lv_obj_align(play_glyph, LV_ALIGN_TOP_MID, 0, top + line_h + TICK_GAP + TICK_H + 28);
}

// Fills a spangroup with as many words as fit one line from the word on
// screen, measured the way the page measures a line: each word in its own
// face, a space in the regular one between them. The line stops at the end
// of a paragraph, so two paragraphs never share one.
static void render_line(void) {
	lv_label_set_text(label_left, "");
	lv_label_set_text(label_pivot, "");
	lv_label_set_text(label_right, "");
	lv_obj_set_hidden(tick_top, true);
	lv_obj_set_hidden(tick_bottom, true);
	drop_line();
	if (!have_word) {
		return;
	}

	line_group = lv_spangroup_create(view);
	lv_obj_set_clickable(line_group, false);
	// A fixed size, so the group wraps at the width and clips what is below
	// its one line should the measure above ever be a pixel out.
	lv_spangroup_set_overflow(line_group, LV_SPAN_OVERFLOW_CLIP);
	lv_obj_set_style_text_line_space(line_group, look.line_space, 0);

	const lv_font_t *regular = ebookfonts_face(false, false);
	int32_t avail = view_width();
	int32_t space_w = lv_font_get_glyph_width(regular, ' ', 0);
	int32_t used = 0;
	word_t w = cur;
	bool prev_cjk = false;
	for (;;) {
		const lv_font_t *font = ebookfonts_face(word_bold(&w), word_italic(&w));
		int32_t ww = text_width(font, w.text, (uint32_t)strlen(w.text));
		bool gap = line_words > 0 && !prev_cjk && !w.cjk;
		int32_t need = (gap ? space_w : 0) + ww;
		if (line_words > 0 && used + need > avail) {
			break;
		}
		char text[WORD_MAX + 1];
		snprintf(text, sizeof(text), "%s%s", gap ? " " : "", w.text);
		lv_span_t *span = lv_spangroup_new_span(line_group);
		lv_span_set_text(span, text);
		lv_style_t *style = lv_span_get_style(span);
		lv_style_set_text_font(style, font);
		lv_style_set_text_color(style, look.ink);

		used += need;
		line_words++;
		line_last = w;
		line_end = w.end;
		prev_cjk = w.cjk;
		if (w.paragraph_end || !scan_word(w.block, w.end, &w)) {
			break;
		}
	}
	lv_spangroup_refresh(line_group);

	int32_t line_h = lv_font_get_line_height(regular) + look.line_space;
	int32_t top = row_y() - line_h / 2;
	lv_obj_set_size(line_group, avail, line_h);
	lv_obj_set_pos(line_group, 0, top);
	lv_obj_align(play_glyph, LV_ALIGN_TOP_MID, 0, top + line_h + 28);
}

static void render(void) {
	if (!view) {
		return;
	}
	if (mode == RSVP_MODE_LINE) {
		render_line();
	} else {
		render_word();
	}
	show_status();
	show_playing();
}

// ---------------------------------------------------------------------------
// The ticker
// ---------------------------------------------------------------------------

// How long a word stays: the base time, longer for a long word, and a rest
// after a comma, a longer one after a full stop and the longest at the end of
// a paragraph.
static uint32_t word_period(const word_t *w) {
	uint32_t base = 60000u / (uint32_t)wpm;
	uint32_t period = base;
	if (w->paragraph_end) {
		period = base * 4;
	} else if (w->sentence_end) {
		period = base * 3;
	} else if (w->clause_end) {
		period = base * 2;
	}
	if (w->cps >= 10) {
		period = period * 3 / 2;
	}
	return period;
}

// A line is paced by its words, with the rest its last word asks for.
static uint32_t period_now(void) {
	if (!have_word) {
		return 60000u / (uint32_t)wpm;
	}
	if (mode == RSVP_MODE_LINE && line_words > 0) {
		return (uint32_t)(line_words - 1) * (60000u / (uint32_t)wpm) + word_period(&line_last);
	}
	return word_period(&cur);
}

static void schedule(void) {
	if (ticker) {
		lv_timer_set_period(ticker, period_now());
	}
}

// The block the view was shown at, for a chapter that has no word to stand
// on: a cover page, a chapter of illustrations.
static uint32_t shown_block;

// Tells the reader where the reading is: the word on screen or, when the
// chapter has none, the block the view was shown at -- always a block the
// chapter has, so that no position outside it is ever written down.
static void moved(void) {
	if (!hooks || !hooks->moved) {
		return;
	}
	if (have_word) {
		hooks->moved(cur.block, cur.offset);
	} else {
		hooks->moved(shown_block, 0);
	}
}

// Loads the chapters in `direction` one after another until one with a word
// in it is up, so a cover page or a chapter of illustrations is passed over
// rather than stopped on. Each load calls ebookrsvp_show(). False when the
// book ran out that way; the chapter and the word that were up are then put
// back, so nothing is lost by trying.
static bool go_chapter(int direction) {
	if (!hooks || !hooks->load_chapter) {
		return false;
	}
	uint32_t origin = ebook_chapter_index(book);
	bool had = have_word;
	word_t was = cur;
	uint32_t count = ebook_spine_count(book);
	uint32_t spine = origin;
	for (;;) {
		if (direction > 0 ? spine + 1u >= count : spine == 0) {
			break;
		}
		spine = direction > 0 ? spine + 1u : spine - 1u;
		// A chapter that cannot be read is passed over like one with no words.
		if (hooks->load_chapter(spine, direction > 0 ? 0 : UINT32_MAX, 0, direction) && have_word) {
			return true;
		}
	}
	hooks->load_chapter(origin, had ? was.block : 0, had ? was.offset : 0, 0);
	return false;
}

static void set_playing(bool on) {
	if (!ticker) {
		return;
	}
	if (on && book && !have_word) {
		// Started on a page with no words: the first chapter after it that has
		// some, which is where a book that opens on its cover begins.
		go_chapter(+1);
	}
	playing = on && book && have_word;
	if (playing) {
		schedule();
		lv_timer_reset(ticker);
		lv_timer_resume(ticker);
	} else {
		lv_timer_pause(ticker);
	}
	show_playing();
}

// Moves to the word after this one, or the line after this one, and on into
// the next chapter with words at the end of this one. At the end of the book
// the ticker stops where it is.
static void advance(void) {
	uint32_t from = mode == RSVP_MODE_LINE ? line_end : cur.end;
	word_t next;
	if (next_word(cur.block, from, &next)) {
		cur = next;
		render();
		moved();
		schedule();
		return;
	}
	if (go_chapter(+1)) {
		return; // ebookrsvp_show() has run and the ticker goes on
	}
	set_playing(false);
	if (hooks && hooks->save) {
		hooks->save();
	}
}

static void tick_cb(lv_timer_t *timer) {
	(void)timer;
	if (!book || !view || !have_word) {
		set_playing(false);
		return;
	}
	advance();
}

static void flash_cb(lv_timer_t *timer) {
	(void)timer;
	lv_timer_pause(flash_timer);
	if (flash) {
		lv_obj_set_hidden(flash, true);
	}
}

static void flash_wpm(void) {
	if (!flash || !flash_timer) {
		return;
	}
	lv_label_set_text_fmt(flash, "%d %s", wpm, tr("ebookreader_rsvp_unit"));
	int32_t y = row_y() - 110;
	lv_obj_align(flash, LV_ALIGN_TOP_MID, 0, y > 8 ? y : 8);
	lv_obj_set_hidden(flash, false);
	lv_timer_set_period(flash_timer, FLASH_MS);
	lv_timer_reset(flash_timer);
	lv_timer_resume(flash_timer);
}

// ---------------------------------------------------------------------------
// What the reader calls
// ---------------------------------------------------------------------------

void ebookrsvp_init(gui_config_t *cfg, const ebookrsvp_hooks_t *reader_hooks) {
	g_cfg = cfg;
	hooks = reader_hooks;
	ticker = lv_timer_create(tick_cb, 60000u / RSVP_WPM_DEFAULT, NULL);
	lv_timer_pause(ticker);
	flash_timer = lv_timer_create(flash_cb, FLASH_MS, NULL);
	lv_timer_pause(flash_timer);
}

void ebookrsvp_set_mode(rsvp_mode_t want) { mode = want; }

rsvp_mode_t ebookrsvp_mode(void) { return mode; }

void ebookrsvp_set_wpm(int want, bool show) {
	wpm = want < RSVP_WPM_MIN ? RSVP_WPM_MIN : (want > RSVP_WPM_MAX ? RSVP_WPM_MAX : want);
	if (playing) {
		// Felt at once rather than after the word already on screen.
		schedule();
		lv_timer_reset(ticker);
	}
	show_status();
	if (show) {
		flash_wpm();
	}
}

void ebookrsvp_show(lv_obj_t *page_box, ebook_t *open_book, uint32_t block, uint32_t offset, const ebookrsvp_look_t *page_look) {
	box = page_box;
	book = open_book;
	look = *page_look;
	if (!box || !book) {
		return;
	}
	// Whatever the box held -- a page, or the view before this one, which
	// nulls `view` and the rest through view_deleted_cb -- goes.
	lv_obj_clean(box);
	// The box may have just been resized for this book, before its screen is
	// up: its width has to be real before the labels are placed by it.
	lv_obj_update_layout(box);
	uint32_t count = ebook_block_count(book);
	bool at_end = block == UINT32_MAX;
	if (block >= count) {
		block = count ? count - 1u : 0;
		offset = 0;
	}
	shown_block = block;
	build_view();
	if (at_end) {
		// Arrived from the chapter after this one: its last sentence, which is
		// what going back a sentence from there means.
		have_word = prev_word(count, 0, &cur);
		if (have_word) {
			sentence_start(&cur);
		}
	} else {
		have_word = next_word(block, offset, &cur);
	}
	render();
	moved();
	// A chapter with no word in it leaves the ticker as it is: whoever loaded
	// the chapter -- go_chapter() -- is on its way past it, and should a tick
	// come first, tick_cb() stops.
	if (playing && have_word) {
		schedule();
		lv_timer_reset(ticker);
	}
}

void ebookrsvp_settle(void) {
	if (book && !have_word) {
		go_chapter(+1);
	}
}

void ebookrsvp_hide(void) {
	set_playing(false);
	if (flash_timer) {
		lv_timer_pause(flash_timer);
	}
	book = NULL;
	box = NULL;
	// The reader deletes the objects with the page; the pointers go now so
	// that nothing here draws on them in between.
	view = NULL;
	label_left = label_pivot = label_right = NULL;
	tick_top = tick_bottom = NULL;
	line_group = status = play_glyph = flash = NULL;
	have_word = false;
}

void ebookrsvp_play(void) { set_playing(true); }

void ebookrsvp_pause(void) {
	bool was = playing;
	set_playing(false);
	if (was && hooks && hooks->save) {
		hooks->save();
	}
}

void ebookrsvp_toggle(void) {
	if (playing) {
		ebookrsvp_pause();
	} else {
		ebookrsvp_play();
	}
}

bool ebookrsvp_playing(void) { return playing; }

// The word that begins the sentence after this one -- after the line on
// screen, in the one-line mode -- or false at the end of the chapter.
static bool sentence_after(word_t *w) {
	word_t probe = mode == RSVP_MODE_LINE && line_words > 0 ? line_last : cur;
	for (;;) {
		bool ends = probe.sentence_end || probe.paragraph_end;
		if (!next_word(probe.block, probe.end, &probe)) {
			return false;
		}
		if (ends) {
			*w = probe;
			return true;
		}
	}
}

// The word that begins this sentence -- or the one before it, when this one
// has only just begun. False at the start of the chapter.
static bool sentence_before(word_t *w) {
	word_t probe;
	if (!prev_word(cur.block, cur.offset, &probe)) {
		return false;
	}
	sentence_start(&probe);
	*w = probe;
	return true;
}

void ebookrsvp_skip_sentence(int direction) {
	if (!book) {
		return;
	}
	word_t target;
	if (have_word && (direction > 0 ? sentence_after(&target) : sentence_before(&target))) {
		cur = target;
		render();
		moved();
		if (playing) {
			schedule();
			lv_timer_reset(ticker);
		}
		if (hooks && hooks->save) {
			hooks->save();
		}
		return;
	}
	// Over the chapter's edge: the first sentence of the next chapter that
	// has one, or the last sentence of the nearest one before.
	go_chapter(direction);
	if (hooks && hooks->save) {
		hooks->save();
	}
}

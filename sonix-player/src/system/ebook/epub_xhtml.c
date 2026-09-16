#include "src/system/ebook/epub_css.h"
#include "src/system/ebook/epub_internal.h"
#include "src/system/ebook/epub_xml.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// One chapter's XHTML, turned into blocks of styled text
//
// Not a DOM. The scanner walks the document once and this keeps a stack of the
// styles currently in force -- bold, italic, quote depth, list depth -- with a
// block being closed whenever a block-level tag ends. What comes out is a flat
// array of blocks, each a run of spans, all of it in the chapter's arena and
// all of it thrown away at the next chapter.
//
// An element this does not know is transparent: its children are rendered as
// if it were not there. That is what makes the reader survive real books, which
// are full of <section>, <aside>, <figure> and whatever the converter felt like
// emitting. A reader that only renders a whitelist shows a blank page for a
// book that every other reader opens.
//
// What is skipped entirely, contents and all: <head>, <title> and <script>.
// The text of <style> is read too, but as a stylesheet rather than as prose.
// ---------------------------------------------------------------------------

// The ceilings for one chapter. A chapter with more blocks than this is a book
// that has put itself in one file, and it is cut rather than refused: half a
// very long chapter is better than nothing at all.
//
// They are ceilings and not reservations: the arrays grow by doubling, so a
// short chapter costs what a short chapter is.
#define MAX_BLOCKS 20000u
#define MAX_SPANS 60000u
#define FIRST_BLOCKS 128u
#define FIRST_SPANS 256u

// How deep the tags may nest before this stops tracking them. Real books do not
// come close; a converter's output sometimes does.
#define STACK_MAX 64

typedef struct {
	uint8_t style;	 // the styles in force at this depth
	uint8_t list;	 // list nesting: 0 none
	uint8_t quote;	 // blockquote nesting
	uint8_t align;	 // ebook_align_t, from the stylesheet
	bool hidden;	 // display:none somewhere at or above this depth
	bool block;		 // this element closes a block when it ends
	bool skip;		 // this element and everything in it is not read
	char name[16];	 // so the matching CLOSE can be recognised
} tag_state_t;

typedef struct {
	ebook_t *book;

	ebook_block_t *blocks;
	uint32_t block_count;
	uint32_t block_cap;
	ebook_span_t *spans;
	uint32_t span_count;
	uint32_t span_cap;
	char *text;
	uint32_t text_len;
	uint32_t text_cap;

	tag_state_t stack[STACK_MAX];
	int depth;

	// The block being filled. -1 when the parser is between blocks, which is
	// where the text of a book that forgot its <p> tags ends up: see
	// ensure_block().
	int32_t open_block;
	uint32_t open_first_span;

	bool pending_space; // whitespace was seen and one space is owed
	int skip_depth;		// >0 while inside <script>, <style> or <head>

	epub_css_t css;
	bool in_style;	  // inside a <style>: its text is a stylesheet, not prose
	int hidden_depth; // >0 while inside something the stylesheet hides
} parse_t;

static bool tag_is(const epub_xml_t *x, const char *name) { return epub_xml_is(x, name); }

// strstr over a run that is not NUL-terminated: attribute values point into the
// document and end where the quote is, not where a zero byte is.
static bool strstr_n(const char *hay, size_t hay_len, const char *needle) {
	size_t n = strlen(needle);
	if (n > hay_len) {
		return false;
	}
	for (size_t i = 0; i + n <= hay_len; i++) {
		size_t k = 0;
		while (k < n) {
			char a = hay[i + k], b = needle[k];
			if (a >= 'A' && a <= 'Z') {
				a = (char)(a + 32);
			}
			if (a != b) {
				break;
			}
			k++;
		}
		if (k == n) {
			return true;
		}
	}
	return false;
}

// The block-level elements this recognises. Everything else is inline, which
// for an unknown element is the right guess: making it a block would break a
// sentence in half, while making it inline at worst runs two together.
static bool block_type_of(const epub_xml_t *x, uint8_t *type, uint8_t *level) {
	static const char *const HEADINGS[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
	for (int i = 0; i < 6; i++) {
		if (tag_is(x, HEADINGS[i])) {
			*type = EBOOK_BLOCK_HEADING;
			*level = (uint8_t)(i + 1);
			return true;
		}
	}
	*level = 0;
	if (tag_is(x, "p") || tag_is(x, "div") || tag_is(x, "pre")) {
		*type = EBOOK_BLOCK_PARAGRAPH;
		return true;
	}
	if (tag_is(x, "li")) {
		*type = EBOOK_BLOCK_LIST_ITEM;
		return true;
	}
	if (tag_is(x, "blockquote")) {
		*type = EBOOK_BLOCK_QUOTE;
		return true;
	}
	return false;
}

// The three arrays of a chapter all grow the same way: double, copy, and let
// the old block sit in the arena until the chapter is thrown away. An arena has
// no free, so the copies left behind are the price of never calling one -- and
// they are bounded by the final size, because doubling means the abandoned
// blocks together are smaller than the one in use.
static bool grow_blocks(parse_t *p) {
	if (p->block_count < p->block_cap) {
		return true;
	}
	if (p->block_cap >= MAX_BLOCKS) {
		return false;
	}
	uint32_t want = p->block_cap ? p->block_cap * 2u : FIRST_BLOCKS;
	if (want > MAX_BLOCKS) {
		want = MAX_BLOCKS;
	}
	ebook_block_t *bigger = arena_calloc(&p->book->chapter_arena, want, sizeof(ebook_block_t));
	if (!bigger) {
		return false;
	}
	if (p->block_count) {
		memcpy(bigger, p->blocks, (size_t)p->block_count * sizeof(ebook_block_t));
	}
	p->blocks = bigger;
	p->block_cap = want;
	return true;
}

static bool grow_spans(parse_t *p) {
	if (p->span_count < p->span_cap) {
		return true;
	}
	if (p->span_cap >= MAX_SPANS) {
		return false;
	}
	uint32_t want = p->span_cap ? p->span_cap * 2u : FIRST_SPANS;
	if (want > MAX_SPANS) {
		want = MAX_SPANS;
	}
	ebook_span_t *bigger = arena_calloc(&p->book->chapter_arena, want, sizeof(ebook_span_t));
	if (!bigger) {
		return false;
	}
	if (p->span_count) {
		memcpy(bigger, p->spans, (size_t)p->span_count * sizeof(ebook_span_t));
	}
	p->spans = bigger;
	p->span_cap = want;
	return true;
}

static void close_block(parse_t *p) {
	if (p->open_block < 0) {
		return;
	}
	ebook_block_t *b = &p->blocks[p->open_block];
	b->span_count = p->span_count - p->open_first_span;
	// A block with no text at all is dropped: converters emit empty <p> by the
	// dozen, and each one would be a blank line on the page.
	if (b->span_count == 0 && b->type != EBOOK_BLOCK_RULE) {
		p->block_count--;
	}
	p->open_block = -1;
	p->pending_space = false;
}

static bool start_block(parse_t *p, uint8_t type, uint8_t level) {
	close_block(p);
	if (!grow_blocks(p)) {
		return false;
	}

	uint8_t indent = 0;
	if (p->depth > 0) {
		const tag_state_t *top = &p->stack[p->depth - 1];
		indent = (uint8_t)(top->list + top->quote);
		// A paragraph inside a blockquote IS the quote. Books write the quote
		// as <blockquote><p>...</p></blockquote>, so the blockquote's own block
		// ends up empty and dropped, and without this the quoted text would
		// arrive as an ordinary paragraph that merely happens to be indented.
		if (type == EBOOK_BLOCK_PARAGRAPH && top->quote > 0) {
			type = EBOOK_BLOCK_QUOTE;
		}
	}

	ebook_block_t *b = &p->blocks[p->block_count];
	b->type = type;
	b->level = level;
	b->first_span = p->span_count;
	b->span_count = 0;
	b->indent = indent > 6 ? 6 : indent;
	b->align = p->depth > 0 ? p->stack[p->depth - 1].align : EBOOK_ALIGN_DEFAULT;
	p->open_block = (int32_t)p->block_count;
	p->open_first_span = p->span_count;
	p->block_count++;
	return true;
}

// Text outside any block still has to go somewhere. Books that come out of a
// converter routinely put a whole paragraph directly inside <body>.
static bool ensure_block(parse_t *p) {
	if (p->open_block >= 0) {
		return true;
	}
	return start_block(p, EBOOK_BLOCK_PARAGRAPH, 0);
}

static uint8_t current_style(const parse_t *p) { return p->depth > 0 ? p->stack[p->depth - 1].style : 0; }

// Appends `len` bytes to the chapter's text pool, growing it if needed. The
// pool is one allocation that doubles, so a chapter is a handful of mappings
// and not one per paragraph.
static bool text_append(parse_t *p, const char *data, uint32_t len) {
	if (p->text_len + len + 1u > p->text_cap) {
		uint32_t want = p->text_cap ? p->text_cap * 2u : 32u * 1024u;
		while (want < p->text_len + len + 1u) {
			want *= 2u;
		}
		char *bigger = arena_alloc(&p->book->chapter_arena, want);
		if (!bigger) {
			return false;
		}
		if (p->text_len) {
			memcpy(bigger, p->text, p->text_len);
		}
		// The old block is not returned: an arena has no free. It is the price
		// of never calling free(), and it is bounded by the pool's final size.
		p->text = bigger;
		p->text_cap = want;
	}
	memcpy(p->text + p->text_len, data, len);
	p->text_len += len;
	p->text[p->text_len] = '\0';
	return true;
}

// Adds one run of text in the style now in force, joining it to the previous
// span when the style has not changed -- which is most of the time, and which
// keeps a paragraph one span instead of one per scanner event.
static bool add_text(parse_t *p, const char *data, uint32_t len) {
	if (!len || !ensure_block(p)) {
		return len == 0;
	}
	uint8_t style = current_style(p);
	uint32_t at = p->text_len;
	if (!text_append(p, data, len)) {
		return false;
	}

	if (p->span_count > p->open_first_span) {
		ebook_span_t *last = &p->spans[p->span_count - 1];
		if (last->style == style && last->offset + last->length == at) {
			last->length += len;
			return true;
		}
	}
	if (!grow_spans(p)) {
		return false;
	}
	ebook_span_t *s = &p->spans[p->span_count++];
	s->offset = at;
	s->length = len;
	s->style = style;
	return true;
}

// XHTML whitespace is not significant: any run of it is one space, and a run at
// the start of a block is nothing at all. Doing this here rather than at layout
// time is what keeps the text pool the size of the words.
static bool add_run(parse_t *p, const char *decoded, size_t len) {
	size_t i = 0;
	while (i < len) {
		if (decoded[i] == ' ' || decoded[i] == '\t' || decoded[i] == '\n' || decoded[i] == '\r') {
			p->pending_space = true;
			i++;
			continue;
		}
		size_t start = i;
		while (i < len && decoded[i] != ' ' && decoded[i] != '\t' && decoded[i] != '\n' && decoded[i] != '\r') {
			i++;
		}
		if (p->pending_space) {
			p->pending_space = false;
			// Not at the very start of a block: a leading space indents the
			// first line by a space for no reason.
			if (p->open_block >= 0 && p->span_count > p->open_first_span) {
				if (!add_text(p, " ", 1)) {
					return false;
				}
			}
		}
		if (!add_text(p, decoded + start, (uint32_t)(i - start))) {
			return false;
		}
	}
	return true;
}

static void push(parse_t *p, const epub_xml_t *x, uint8_t add_style, bool is_block, bool is_list, bool is_quote,
				 bool skip, uint8_t align, bool hidden) {
	if (p->depth >= STACK_MAX) {
		return;
	}
	tag_state_t *top = &p->stack[p->depth];
	const tag_state_t *below = p->depth > 0 ? &p->stack[p->depth - 1] : NULL;
	top->style = (uint8_t)((below ? below->style : 0) | add_style);
	// Alignment is inherited: a centred <div> centres the paragraphs inside it,
	// which is how a title page is written.
	top->align = align != EBOOK_ALIGN_DEFAULT ? align : (below ? below->align : EBOOK_ALIGN_DEFAULT);
	top->hidden = hidden || (below ? below->hidden : false);
	top->list = (uint8_t)((below ? below->list : 0) + (is_list ? 1 : 0));
	top->quote = (uint8_t)((below ? below->quote : 0) + (is_quote ? 1 : 0));
	top->block = is_block;
	top->skip = skip;
	size_t len = x->name_len < sizeof(top->name) - 1 ? x->name_len : sizeof(top->name) - 1;
	memcpy(top->name, x->name, len);
	top->name[len] = '\0';
	p->depth++;
}

// Reads a stylesheet the document links to.
//
// The rules keep pointers into the text they were parsed from rather than
// copying every selector, so the sheet is extracted into the chapter's arena and
// not into a scratch one: it has to stay readable for as long as the chapter
// does. It costs one copy of a file that is a few kilobytes.
static void load_linked_sheet(parse_t *p, const epub_xml_t *x) {
	const char *rel = NULL, *type = NULL, *href = NULL;
	size_t rel_len = 0, type_len = 0, href_len = 0;
	epub_xml_attr(x, "rel", &rel, &rel_len);
	epub_xml_attr(x, "type", &type, &type_len);
	if (!epub_xml_attr(x, "href", &href, &href_len) || !href_len) {
		return;
	}

	// Either mark is enough: books written by hand say rel="stylesheet" and
	// leave the type out, and a few converters do the opposite.
	bool is_sheet = (rel && rel_len >= 10 && strstr_n(rel, rel_len, "stylesheet")) ||
					(type && type_len >= 8 && strstr_n(type, type_len, "text/css"));
	if (!is_sheet) {
		return;
	}

	char path[512];
	if (!epub_resolve_path(path, sizeof(path), p->book->chapter_dir, href, href_len)) {
		return;
	}
	int index = epub_zip_find(&p->book->zip, path);
	if (index < 0) {
		return;
	}
	uint32_t size = 0;
	const char *sheet = epub_zip_extract(&p->book->zip, index, &p->book->chapter_arena, EPUB_MAX_STYLESHEET, &size);
	if (sheet && size) {
		epub_css_add(&p->css, sheet, size);
	}
}

// A picture in the flow of the text.
//
// What goes into the block is the path inside the ZIP, not the bytes: the parser
// has no business decoding a JPEG, and the reader only wants the two or three
// that are on screen. <image> as well as <img> because a cover page is very
// often an <svg> with an <image> in it, and a reader that only knows <img> shows
// a blank first page for a great many books.
static void add_image(parse_t *p, const epub_xml_t *x) {
	const char *href = NULL;
	size_t href_len = 0;
	// epub_xml strips the namespace, so xlink:href arrives as href.
	if (!epub_xml_attr(x, "src", &href, &href_len) && !epub_xml_attr(x, "href", &href, &href_len)) {
		return;
	}
	if (!href_len) {
		return;
	}

	char path[512];
	if (!epub_resolve_path(path, sizeof(path), p->book->chapter_dir, href, href_len)) {
		return;
	}

	close_block(p);
	if (!start_block(p, EBOOK_BLOCK_IMAGE, 0)) {
		return;
	}
	// The path is the block's only span. Written through add_text() so it lands
	// in the same pool as everything else and needs no second lifetime.
	add_text(p, path, (uint32_t)strlen(path));
	close_block(p);
}

bool epub_parse_chapter(ebook_t *book, const char *xhtml) {
	parse_t p;
	memset(&p, 0, sizeof(p));
	p.book = book;
	p.open_block = -1;
	epub_css_init(&p.css, &book->chapter_arena);

	if (!grow_blocks(&p) || !grow_spans(&p)) {
		return false;
	}

	epub_xml_t x;
	epub_xml_init(&x, xhtml);

	char decoded[4096];

	while (epub_xml_next(&x)) {
		if (x.event == EPUB_XML_TEXT) {
			// The text of a <style> is a stylesheet and not something to read.
			// It has to be parsed here rather than skipped, because it is where
			// most books keep the only CSS they have.
			if (p.in_style) {
				epub_css_add(&p.css, x.text, x.text_len);
				continue;
			}
			if (p.skip_depth > 0 || p.hidden_depth > 0) {
				continue;
			}
			size_t len = epub_xml_unescape(decoded, sizeof(decoded), x.text, x.text_len);
			if (!add_run(&p, decoded, len)) {
				break;
			}
			continue;
		}

		if (x.event == EPUB_XML_OPEN) {
			// The stylesheets the document links to, read before anything that
			// might be styled by them: <link> lives in <head>, which comes
			// first in every document there is.
			if (tag_is(&x, "link")) {
				load_linked_sheet(&p, &x);
				if (!x.self_closing) {
					push(&p, &x, 0, false, false, false, false, EBOOK_ALIGN_DEFAULT, false);
				}
				continue;
			}
			if (tag_is(&x, "style")) {
				if (!x.self_closing) {
					p.in_style = true;
					push(&p, &x, 0, false, false, false, true, EBOOK_ALIGN_DEFAULT, false);
				}
				continue;
			}

			bool skip = tag_is(&x, "script") || tag_is(&x, "head") || tag_is(&x, "title");
			if (skip && !x.self_closing) {
				p.skip_depth++;
				push(&p, &x, 0, false, false, false, true, EBOOK_ALIGN_DEFAULT, false);
				continue;
			}
			if (p.skip_depth > 0) {
				if (!x.self_closing) {
					push(&p, &x, 0, false, false, false, true, EBOOK_ALIGN_DEFAULT, false);
				}
				continue;
			}

			// What the stylesheet says about this element, by tag and by class.
			const char *class_attr = NULL;
			size_t class_len = 0;
			epub_xml_attr(&x, "class", &class_attr, &class_len);
			uint8_t css = epub_css_lookup(&p.css, x.name, x.name_len, class_attr, class_len);

			bool hidden = (css & EPUB_CSS_HIDDEN) != 0;
			uint8_t align = EBOOK_ALIGN_DEFAULT;
			if (css & EPUB_CSS_CENTRE) {
				align = EBOOK_ALIGN_CENTRE;
			} else if (css & EPUB_CSS_RIGHT) {
				align = EBOOK_ALIGN_RIGHT;
			}

			if (hidden || p.hidden_depth > 0) {
				// Hidden and everything inside it. Counted rather than flagged,
				// because the close that ends it may be several levels up.
				if (!x.self_closing) {
					p.hidden_depth++;
					push(&p, &x, 0, false, false, false, false, align, true);
				}
				continue;
			}

			// A picture. Its own block, so pagination can measure it and a page
			// can be the picture alone when that is all that fits.
			if (tag_is(&x, "img") || tag_is(&x, "image")) {
				add_image(&p, &x);
				if (!x.self_closing) {
					push(&p, &x, 0, false, false, false, false, align, false);
				}
				continue;
			}

			// The two that are content in themselves.
			if (tag_is(&x, "br")) {
				// A line break inside a paragraph. Kept as a real newline in the
				// text so the layout can honour it; the pending space is dropped
				// because a space before a break is not a space.
				p.pending_space = false;
				if (ensure_block(&p)) {
					add_text(&p, "\n", 1);
				}
				if (!x.self_closing) {
					push(&p, &x, 0, false, false, false, false, align, false);
				}
				continue;
			}
			if (tag_is(&x, "hr")) {
				close_block(&p);
				if (start_block(&p, EBOOK_BLOCK_RULE, 0)) {
					close_block(&p);
				}
				if (!x.self_closing) {
					push(&p, &x, 0, false, false, false, false, align, false);
				}
				continue;
			}

			uint8_t add_style = 0;
			if (tag_is(&x, "b") || tag_is(&x, "strong")) {
				add_style |= EBOOK_STYLE_BOLD;
			}
			if (tag_is(&x, "i") || tag_is(&x, "em") || tag_is(&x, "cite")) {
				add_style |= EBOOK_STYLE_ITALIC;
			}
			// And whatever the stylesheet adds, which for most converted books
			// is where the emphasis actually lives: <span class="calibre5">
			// rather than <em>.
			if (css & EPUB_CSS_BOLD) {
				add_style |= EBOOK_STYLE_BOLD;
			}
			if (css & EPUB_CSS_ITALIC) {
				add_style |= EBOOK_STYLE_ITALIC;
			}

			bool is_list = tag_is(&x, "ul") || tag_is(&x, "ol");
			bool is_quote = tag_is(&x, "blockquote");

			uint8_t type = 0, level = 0;
			bool is_block = block_type_of(&x, &type, &level);

			if (!x.self_closing) {
				push(&p, &x, add_style, is_block, is_list, is_quote, false, align, false);
			}
			if (is_block) {
				start_block(&p, type, level);
				// start_block() reads the alignment off the stack, so a block
				// whose own tag carried it has to be told again: a self-closing
				// one never got pushed, and one that did was pushed on the line
				// above this.
				if (p.open_block >= 0 && align != EBOOK_ALIGN_DEFAULT) {
					p.blocks[p.open_block].align = align;
				}
			} else if (is_list || is_quote) {
				close_block(&p);
			}
			continue;
		}

		// EPUB_XML_CLOSE. Unwind to the matching open tag rather than assuming
		// the document is balanced: an unclosed <i> in the middle of a chapter
		// would otherwise italicise the rest of the book.
		if (p.in_style && epub_xml_is(&x, "style")) {
			p.in_style = false;
		}
		int found = -1;
		for (int i = p.depth - 1; i >= 0; i--) {
			if (strncmp(p.stack[i].name, x.name, x.name_len) == 0 && p.stack[i].name[x.name_len] == '\0') {
				found = i;
				break;
			}
		}
		if (found < 0) {
			continue; // a close with no open: ignore it
		}
		for (int i = p.depth - 1; i >= found; i--) {
			if (p.stack[i].skip && p.skip_depth > 0) {
				p.skip_depth--;
			}
			if (p.stack[i].hidden && p.hidden_depth > 0) {
				p.hidden_depth--;
			}
			if (p.stack[i].block) {
				close_block(&p);
			}
		}
		p.depth = found;
	}

	close_block(&p);

	book->blocks = p.blocks;
	book->block_count = p.block_count;
	book->spans = p.spans;
	book->span_count = p.span_count;
	book->text = p.text ? p.text : (char *)"";
	book->text_len = p.text_len;
	return true;
}

#ifndef EBOOK_H
#define EBOOK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// An EPUB, opened one chapter at a time
//
// The whole of this subsystem is built around one promise: closing a book gives
// its memory back to the kernel, not to the allocator. Everything a book owns
// lives in one of two arenas (see epub_arena.h) -- the book's, which holds the
// metadata, the spine and the table of contents and stays a few tens of
// kilobytes, and the chapter's, which holds the text of the one chapter being
// read and is thrown away whole at every chapter change. Nothing here ever
// holds the whole book.
//
// There is no LVGL in this half. It hands out blocks of styled text and knows
// nothing about pages, fonts or pixels: how many words fit on a screen is a
// question only the side that owns the fonts can answer, so pagination lives in
// src/gui/ebookreader.c and this side stays measurable, testable and free of
// the interface. tools/test_ebook.c drives it without a display.
//
// What is here: EPUB 2 and 3 reflowable books, paragraphs, headings, emphasis,
// lists, block quotes, images, as much of the stylesheet as changes what is on
// screen (see epub_css.h), the table of contents, and the reading position.
// What is not, on purpose: embedded fonts, JavaScript, fixed layout, DRM.
// ---------------------------------------------------------------------------

typedef struct ebook ebook_t;

// How a block of text is meant to look. One byte, because a chapter has
// thousands of them.
typedef enum {
	EBOOK_BLOCK_PARAGRAPH = 0,
	EBOOK_BLOCK_HEADING,	// `level` says which
	EBOOK_BLOCK_QUOTE,		// blockquote
	EBOOK_BLOCK_LIST_ITEM,	// li, with the bullet or number already in the text
	EBOOK_BLOCK_RULE,		// hr: a line, no text
	EBOOK_BLOCK_IMAGE,		// img: the span text is the path inside the ZIP
} ebook_block_type_t;

// How a block sits across the page. From the stylesheet, which is the only
// place a book says so -- there is no element that means "centred".
typedef enum {
	EBOOK_ALIGN_DEFAULT = 0,
	EBOOK_ALIGN_CENTRE,
	EBOOK_ALIGN_RIGHT,
} ebook_align_t;

#define EBOOK_STYLE_BOLD 0x01u
#define EBOOK_STYLE_ITALIC 0x02u

// A run of text in one style. The text itself is in the chapter's string pool,
// which is one block rather than one allocation per span.
typedef struct {
	uint32_t offset; // into ebook_chapter_text()
	uint32_t length;
	uint8_t style; // EBOOK_STYLE_*
} ebook_span_t;

typedef struct {
	uint32_t first_span;
	uint32_t span_count;
	uint8_t type;  // ebook_block_type_t
	uint8_t level; // 1..6 for a heading, 0 otherwise
	uint8_t indent; // nesting depth of lists and quotes
	uint8_t align;	// ebook_align_t
} ebook_block_t;

// Where the reader is. Small on purpose: it is written to the config at every
// chapter change and read back at the next boot.
typedef struct {
	uint32_t spine;	 // which chapter
	uint32_t block;	 // which block in it
	uint32_t offset; // how far into that block's text
} ebook_position_t;

typedef struct {
	const char *label;
	uint32_t spine; // the chapter it points at
	uint8_t depth;	// 0 for a top-level entry
} ebook_toc_entry_t;

// ---------------------------------------------------------------------------
// The book
// ---------------------------------------------------------------------------

// Opens `path`, reads container.xml, the OPF and the table of contents, and
// stops. No chapter is loaded. NULL when the file is not an EPUB this can read,
// and nothing is left allocated in that case.
ebook_t *ebook_open(const char *path);

// Closes everything: the chapter, the book, the ZIP handle, and every mapping.
// Safe with NULL.
void ebook_close(ebook_t *book);

const char *ebook_title(const ebook_t *book);
const char *ebook_author(const ebook_t *book);
const char *ebook_path(const ebook_t *book);

uint32_t ebook_spine_count(const ebook_t *book);

uint32_t ebook_toc_count(const ebook_t *book);
const ebook_toc_entry_t *ebook_toc(const ebook_t *book, uint32_t index);

// How much of the book each chapter is, for saying how far in a position is
// without opening every chapter to count its words: the unpacked size of the
// chapter's file, the sum of the ones before it, and the sum of them all. The
// figures include the XHTML markup, which is why they are weights and not
// character counts -- but markup grows with the text, so a chapter twice the
// size really does hold about twice the reading.
//
// Zero for a book whose chapters are not in its ZIP, in which case the caller
// has nothing to weigh with and should fall back to counting chapters.
//
// Not const: the table is built on the first call and kept in the book.
uint32_t ebook_chapter_weight(ebook_t *book, uint32_t spine_index);
uint64_t ebook_weight_before(ebook_t *book, uint32_t spine_index);
uint64_t ebook_book_weight(ebook_t *book);

// The cover image as it sits in the EPUB -- JPEG or PNG bytes, not decoded.
// The bytes live in the book's arena and die with it. NULL when the book names
// no cover, which plenty do not.
const void *ebook_cover_bytes(ebook_t *book, uint32_t *size_out);

// The bytes of one image named by an EBOOK_BLOCK_IMAGE block, as they are in the
// file -- PNG or JPEG, for whoever decodes them. malloc'd and the caller's to
// free: images are not in the chapter arena because a page holds two or three of
// them and a chapter can name fifty, and because the decoded picture outlives the
// compressed bytes by a long way. NULL when the book does not have it.
void *ebook_load_image(ebook_t *book, const char *zip_path, uint32_t *size_out);

// The same without opening a book: for the shelf, which needs a cover from
// every file in the folder and none of the rest. The bytes are malloc'd and the
// caller frees them -- there is no book here to own them.
void *ebook_peek_cover(const char *path, uint32_t *size_out, char *title, size_t title_size);

// ---------------------------------------------------------------------------
// The chapter
// ---------------------------------------------------------------------------

// Throws away whatever chapter was loaded and loads this one. False when the
// chapter cannot be read; the previous one is gone either way, because holding
// it would defeat the point.
bool ebook_open_chapter(ebook_t *book, uint32_t spine_index);

// Unloads the chapter and unmaps its arena. Idempotent.
void ebook_close_chapter(ebook_t *book);

uint32_t ebook_chapter_index(const ebook_t *book);
bool ebook_chapter_loaded(const ebook_t *book);

uint32_t ebook_block_count(const ebook_t *book);

// How many spans the chapter has in total. For walking every run of text
// without going block by block.
uint32_t ebook_span_count(const ebook_t *book);
const ebook_block_t *ebook_block(const ebook_t *book, uint32_t index);
const ebook_span_t *ebook_span(const ebook_t *book, uint32_t index);

// The chapter's string pool. Spans index into it; it is NUL-terminated but
// holds NULs of its own, so it is not one string.
const char *ebook_chapter_text(const ebook_t *book);

// ---------------------------------------------------------------------------
// What it costs
//
// Live bytes held by each arena. All zero for a book that is closed.
// ---------------------------------------------------------------------------
size_t ebook_book_bytes(const ebook_t *book);
size_t ebook_chapter_bytes(const ebook_t *book);

// Writes one line per arena to stderr, for diagnostics.
void ebook_log_memory(const ebook_t *book, const char *when);

#endif /* EBOOK_H */

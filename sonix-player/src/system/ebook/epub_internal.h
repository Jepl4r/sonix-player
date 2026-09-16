#ifndef EPUB_INTERNAL_H
#define EPUB_INTERNAL_H

#include "src/system/ebook/ebook.h"
#include "src/system/ebook/epub_arena.h"
#include "src/system/ebook/epub_zip.h"

// What the engine's .c files share. Not a public header: the UI talks to
// ebook.h and knows none of this.

// The ceilings. They exist so that a damaged or hostile file asks for a bounded
// amount of memory rather than whatever its header claims.
#define EPUB_MAX_SPINE 4096u
#define EPUB_MAX_TOC 2048u
#define EPUB_MAX_OPF (4u * 1024u * 1024u)
#define EPUB_MAX_CHAPTER (8u * 1024u * 1024u)
#define EPUB_MAX_COVER (8u * 1024u * 1024u)
// One picture inside a chapter. Smaller than a cover on purpose: a cover is the
// one big image a book is entitled to, an illustration in the text is not.
#define EPUB_MAX_IMAGE (4u * 1024u * 1024u)
#define EPUB_IMAGE_ARENA_FIRST (128u * 1024u)
#define EPUB_MAX_STYLESHEET (512u * 1024u)

// How much the two arenas ask for first. The book's is small because a book's
// own data is small; the chapter's is a page of text and a little over.
#define EPUB_BOOK_ARENA_FIRST (64u * 1024u)
#define EPUB_CHAPTER_ARENA_FIRST (256u * 1024u)

typedef struct {
	const char *path; // inside the ZIP, already resolved against the OPF folder
} epub_spine_entry_t;

struct ebook {
	// Everything in here that is a pointer points into one of the two arenas,
	// except `zip.file`, which is a FILE* and is closed by ebook_close().
	epub_arena_t book_arena;
	epub_arena_t chapter_arena;
	epub_zip_t zip;

	char *path;	 // the .epub on the card
	char *title;
	char *author;
	char *opf_dir; // "OEBPS/" or "" -- what hrefs in the OPF are relative to
	// The folder the CURRENT chapter is in, which is what its own hrefs -- a
	// picture, a stylesheet -- are relative to. Not the same as opf_dir: books
	// routinely keep their text in OEBPS/text/ and their pictures in
	// OEBPS/images/, and resolving against the wrong one finds neither.
	char chapter_dir[512];

	epub_spine_entry_t *spine;
	uint32_t spine_count;

	// How big each chapter's file is unpacked, and the sum. Built on the first
	// question and then kept: it is one linear search of the ZIP directory per
	// chapter, which is nothing once and too much per page turn. NULL until
	// then; `weights_total` of zero also means "not built yet", which is the
	// same answer as a book whose chapters all measure zero.
	uint32_t *weights;
	uint64_t weights_total;

	ebook_toc_entry_t *toc;
	uint32_t toc_count;

	char *cover_path; // inside the ZIP, or NULL
	const void *cover_bytes;
	uint32_t cover_size;

	// The chapter, all of it in chapter_arena.
	bool chapter_loaded;
	uint32_t chapter_index;
	ebook_block_t *blocks;
	uint32_t block_count;
	ebook_span_t *spans;
	uint32_t span_count;
	char *text;
	uint32_t text_len;
};

// epub_opf.c: fills in title, author, opf_dir, spine, toc and cover_path.
// False when the book has no readable spine, which is the one thing that makes
// an EPUB unopenable.
bool epub_read_structure(ebook_t *book);

// epub_xhtml.c: turns one chapter's XHTML into blocks, spans and text, all in
// `book->chapter_arena`. The document is NUL-terminated and belongs to the
// chapter arena too, so it does not have to be kept after this returns.
bool epub_parse_chapter(ebook_t *book, const char *xhtml);

// Resolves `href` against `base` (a folder ending in '/' or ""), collapsing
// "../" and "./", and strips any "#fragment". Into `out`, which is `out_size`
// bytes. False when it does not fit.
bool epub_resolve_path(char *out, size_t out_size, const char *base, const char *href, size_t href_len);

#endif /* EPUB_INTERNAL_H */

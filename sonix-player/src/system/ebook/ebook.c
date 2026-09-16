#include "src/system/ebook/epub_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The book handle itself is the one thing that cannot live in an arena: the
// arenas live inside it. It is a few hundred bytes of malloc per open book, and
// it is freed in ebook_close() after the mappings have gone.

ebook_t *ebook_open(const char *path) {
	if (!path || !path[0]) {
		return NULL;
	}

	ebook_t *book = calloc(1, sizeof(*book));
	if (!book) {
		return NULL;
	}
	arena_init(&book->book_arena, "book", EPUB_BOOK_ARENA_FIRST);
	arena_init(&book->chapter_arena, "chapter", EPUB_CHAPTER_ARENA_FIRST);

	book->path = arena_strdup(&book->book_arena, path);
	if (!book->path) {
		goto fail;
	}
	if (!epub_zip_open(&book->zip, path, &book->book_arena)) {
		goto fail;
	}
	if (!epub_read_structure(book)) {
		goto fail;
	}

	if (!book->title) {
		// A book with no title is shelved under its file name, which is what
		// the person who put it on the card called it.
		const char *slash = strrchr(path, '/');
		book->title = arena_strdup(&book->book_arena, slash ? slash + 1 : path);
	}
	return book;

fail:
	// One way out for every failure, which is what makes a malformed book
	// impossible to leak: whatever was built so far is in the arenas, and the
	// arenas go whole.
	ebook_close(book);
	return NULL;
}

void ebook_close(ebook_t *book) {
	if (!book) {
		return;
	}
	epub_zip_close(&book->zip);
	arena_destroy(&book->chapter_arena);
	arena_destroy(&book->book_arena);
	free(book);
}

const char *ebook_title(const ebook_t *book) { return book && book->title ? book->title : ""; }
const char *ebook_author(const ebook_t *book) { return book && book->author ? book->author : ""; }
const char *ebook_path(const ebook_t *book) { return book && book->path ? book->path : ""; }
uint32_t ebook_spine_count(const ebook_t *book) { return book ? book->spine_count : 0; }
uint32_t ebook_toc_count(const ebook_t *book) { return book ? book->toc_count : 0; }

// ---------------------------------------------------------------------------
// How much of the book a chapter is
//
// The unpacked size of the chapter's file. It is XHTML and not prose, so it
// counts the markup too -- but markup grows with the text it marks up, so as a
// weight it is close enough, and it is the only measure available without
// opening every chapter in the book. Treating chapters as equal instead puts
// the reader far into the book after a two-page preface.
//
// Built once, into the book's arena, and consulted from there.
// ---------------------------------------------------------------------------

static void weights_build(ebook_t *book) {
	if (book->weights || !book->spine_count) {
		return;
	}
	book->weights = arena_alloc(&book->book_arena, book->spine_count * sizeof(*book->weights));
	if (!book->weights) {
		return;
	}
	book->weights_total = 0;
	for (uint32_t i = 0; i < book->spine_count; i++) {
		int index = epub_zip_find(&book->zip, book->spine[i].path);
		book->weights[i] = index >= 0 ? epub_zip_size(&book->zip, index) : 0;
		book->weights_total += book->weights[i];
	}
}

uint32_t ebook_chapter_weight(ebook_t *book, uint32_t spine_index) {
	if (!book || spine_index >= book->spine_count) {
		return 0;
	}
	weights_build(book);
	return book->weights ? book->weights[spine_index] : 0;
}

uint64_t ebook_weight_before(ebook_t *book, uint32_t spine_index) {
	if (!book || !book->spine_count) {
		return 0;
	}
	weights_build(book);
	if (!book->weights) {
		return 0;
	}
	if (spine_index > book->spine_count) {
		spine_index = book->spine_count;
	}
	uint64_t sum = 0;
	for (uint32_t i = 0; i < spine_index; i++) {
		sum += book->weights[i];
	}
	return sum;
}

uint64_t ebook_book_weight(ebook_t *book) {
	if (!book) {
		return 0;
	}
	weights_build(book);
	return book->weights_total;
}

const ebook_toc_entry_t *ebook_toc(const ebook_t *book, uint32_t index) {
	if (!book || index >= book->toc_count) {
		return NULL;
	}
	return &book->toc[index];
}

const void *ebook_cover_bytes(ebook_t *book, uint32_t *size_out) {
	if (size_out) {
		*size_out = 0;
	}
	if (!book || !book->cover_path) {
		return NULL;
	}
	if (!book->cover_bytes) {
		int index = epub_zip_find(&book->zip, book->cover_path);
		book->cover_bytes = epub_zip_extract(&book->zip, index, &book->book_arena, EPUB_MAX_COVER, &book->cover_size);
	}
	if (size_out) {
		*size_out = book->cover_size;
	}
	return book->cover_bytes;
}

// An image the chapter names.
//
// Extracted into a scratch arena of its own and copied out to malloc, rather
// than into the chapter arena: a chapter can name fifty pictures and only the
// two or three on screen are ever wanted, so putting them in the chapter's arena
// would mean the arena grew to hold every picture in the chapter and stayed that
// big until the chapter changed. The arena here is created and destroyed around
// the one extraction, so the compressed bytes go straight back to the kernel.
void *ebook_load_image(ebook_t *book, const char *zip_path, uint32_t *size_out) {
	if (size_out) {
		*size_out = 0;
	}
	if (!book || !zip_path || !zip_path[0]) {
		return NULL;
	}
	int index = epub_zip_find(&book->zip, zip_path);
	if (index < 0) {
		return NULL;
	}

	epub_arena_t scratch;
	arena_init(&scratch, "image", EPUB_IMAGE_ARENA_FIRST);

	uint32_t size = 0;
	const void *bytes = epub_zip_extract(&book->zip, index, &scratch, EPUB_MAX_IMAGE, &size);
	void *copy = NULL;
	if (bytes && size) {
		copy = malloc(size);
		if (copy) {
			memcpy(copy, bytes, size);
			if (size_out) {
				*size_out = size;
			}
		}
	}

	arena_destroy(&scratch);
	return copy;
}

void *ebook_peek_cover(const char *path, uint32_t *size_out, char *title, size_t title_size) {
	if (size_out) {
		*size_out = 0;
	}
	if (title && title_size) {
		title[0] = '\0';
	}

	// A whole book is opened and closed for one picture. That is the cheap way
	// round: the alternative is a second path through the OPF that would have
	// to be kept correct alongside this one, and the shelf builds its thumbnails
	// once and then reads them from the card.
	ebook_t *book = ebook_open(path);
	if (!book) {
		return NULL;
	}
	if (title && title_size) {
		snprintf(title, title_size, "%s", ebook_title(book));
	}

	uint32_t size = 0;
	const void *bytes = ebook_cover_bytes(book, &size);
	void *copy = NULL;
	if (bytes && size) {
		// Copied out of the arena, because the arena is about to be unmapped.
		copy = malloc(size);
		if (copy) {
			memcpy(copy, bytes, size);
			if (size_out) {
				*size_out = size;
			}
		}
	}
	ebook_close(book);
	return copy;
}


bool ebook_open_chapter(ebook_t *book, uint32_t spine_index) {
	if (!book || spine_index >= book->spine_count) {
		return false;
	}

	// The old chapter goes first, and unconditionally. Loading the new one on
	// top of it would mean holding two, which is exactly the thing this design
	// exists to prevent -- and the failure path below would then leave the old
	// one half-replaced.
	ebook_close_chapter(book);

	int index = epub_zip_find(&book->zip, book->spine[spine_index].path);
	if (index < 0) {
		return false;
	}

	// The document is extracted into the chapter's own arena, so it is freed by
	// the same munmap that frees what was parsed out of it. It is not needed
	// after the parse, but keeping it costs one copy of the chapter's bytes and
	// removes a second allocator from the picture.
	char *doc = epub_zip_extract(&book->zip, index, &book->chapter_arena, EPUB_MAX_CHAPTER, NULL);
	if (!doc) {
		goto fail;
	}

	// Where this chapter sits inside the ZIP, so its own hrefs resolve. Set
	// before the parse because the parser is what needs it.
	{
		const char *path = book->spine[spine_index].path;
		const char *slash = strrchr(path, '/');
		size_t dir_len = slash ? (size_t)(slash - path) + 1u : 0u;
		if (dir_len >= sizeof(book->chapter_dir)) {
			dir_len = sizeof(book->chapter_dir) - 1u;
		}
		memcpy(book->chapter_dir, path, dir_len);
		book->chapter_dir[dir_len] = '\0';
	}

	if (!epub_parse_chapter(book, doc)) {
		goto fail;
	}

	book->chapter_loaded = true;
	book->chapter_index = spine_index;
	return true;

fail:
	ebook_close_chapter(book);
	return false;
}

void ebook_close_chapter(ebook_t *book) {
	if (!book) {
		return;
	}
	book->chapter_loaded = false;
	book->blocks = NULL;
	book->block_count = 0;
	book->spans = NULL;
	book->span_count = 0;
	book->text = NULL;
	book->text_len = 0;
	arena_destroy(&book->chapter_arena);
}

uint32_t ebook_chapter_index(const ebook_t *book) { return book ? book->chapter_index : 0; }
bool ebook_chapter_loaded(const ebook_t *book) { return book && book->chapter_loaded; }
uint32_t ebook_block_count(const ebook_t *book) { return book ? book->block_count : 0; }
uint32_t ebook_span_count(const ebook_t *book) { return book ? book->span_count : 0; }

const ebook_block_t *ebook_block(const ebook_t *book, uint32_t index) {
	if (!book || index >= book->block_count) {
		return NULL;
	}
	return &book->blocks[index];
}

const ebook_span_t *ebook_span(const ebook_t *book, uint32_t index) {
	if (!book || index >= book->span_count) {
		return NULL;
	}
	return &book->spans[index];
}

const char *ebook_chapter_text(const ebook_t *book) { return book && book->text ? book->text : ""; }

size_t ebook_book_bytes(const ebook_t *book) { return book ? arena_bytes(&book->book_arena) : 0; }
size_t ebook_chapter_bytes(const ebook_t *book) { return book ? arena_bytes(&book->chapter_arena) : 0; }

void ebook_log_memory(const ebook_t *book, const char *when) {
	fprintf(stderr, "ebook memory (%s): book %zu KB, chapter %zu KB, total %zu KB\n", when ? when : "",
			ebook_book_bytes(book) / 1024u, ebook_chapter_bytes(book) / 1024u,
			(ebook_book_bytes(book) + ebook_chapter_bytes(book)) / 1024u);
}

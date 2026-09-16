#ifndef EPUB_ZIP_H
#define EPUB_ZIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "src/system/ebook/epub_arena.h"

// ---------------------------------------------------------------------------
// One member of a ZIP at a time
//
// An EPUB is a ZIP, and the whole architecture rests on never having more of it
// in memory than the chapter being read. So this reads the central directory
// once -- names and offsets, a few dozen bytes per entry, in the book's arena
// -- and then extracts exactly one member on demand.
//
// It is written here rather than taken from a library because the player
// already has the expensive half: tinfl, miniz's inflate, which decodes every
// PNG cover on the card (src/system/image/miniz). What a ZIP adds on top of
// inflate is a directory format, and that is a few hundred lines. Linking a
// second copy of inflate -- which is what adding minizip would do -- would cost
// more code than this file is.
//
// What is deliberately missing: writing, encryption, ZIP64 beyond the size
// fields, multi-disk archives, and anything to do with the data descriptor.
// None of it appears in an EPUB.
// ---------------------------------------------------------------------------

typedef struct {
	uint32_t name_offset; // into the archive's name pool
	uint32_t local_offset; // where the local header sits in the file
	uint32_t comp_size;
	uint32_t uncomp_size;
	uint16_t method; // 0 stored, 8 deflate; nothing else is accepted
} epub_zip_entry_t;

typedef struct {
	FILE *file;
	epub_zip_entry_t *entries;
	uint32_t count;
	char *names; // every name, NUL-separated, in one block
	epub_arena_t *arena; // where the directory lives; NOT owned
} epub_zip_t;

// Reads the central directory of `path` into `arena`. False when the file is
// not a ZIP, is damaged, or is too big to be an EPUB anyone meant to read.
bool epub_zip_open(epub_zip_t *zip, const char *path, epub_arena_t *arena);

// Closes the file. The directory dies with the arena, not here.
void epub_zip_close(epub_zip_t *zip);

// The index of `name`, or -1. Names are matched exactly, and ZIP names always
// use '/'.
int epub_zip_find(const epub_zip_t *zip, const char *name);

const char *epub_zip_name(const epub_zip_t *zip, uint32_t index);

// The size the member at `index` will be once decompressed, or 0 when there is
// no such member. For deciding whether a member is worth reading before reading
// it.
uint32_t epub_zip_size(const epub_zip_t *zip, int index);

// Extracts one member into `arena`, NUL-terminated so the result can be
// treated as text without a copy. `size_out` may be NULL. NULL on any failure,
// including a member larger than `limit` -- which is how a book with a
// hundred-megabyte chapter is refused rather than fatal.
void *epub_zip_extract(const epub_zip_t *zip, int index, epub_arena_t *arena, uint32_t limit, uint32_t *size_out);

#endif /* EPUB_ZIP_H */

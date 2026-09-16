#include "epub_zip.h"

#include <stdlib.h>
#include <string.h>

// The inflate the player already has. It is compiled into png_scaled.c, which
// includes miniz's tinfl_impl.inc to decode the PNG covers; the symbols are
// global, so this file declares what it needs and links against that one copy.
// Including the .inc again here would be a second inflate in the binary and a
// duplicate-symbol error at the link.
#include "src/system/image/miniz/miniz_tinfl.h"

// The end-of-central-directory record, and how far back from the end of the
// file it may sit. It is 22 bytes plus a comment of up to 64 KB; EPUBs have no
// comment, but reading the last 66 KB costs one seek and removes the question.
#define EOCD_SIGNATURE 0x06054b50u
#define EOCD_MIN 22u
#define EOCD_SEARCH (66u * 1024u)

#define CDH_SIGNATURE 0x02014b50u
#define CDH_MIN 46u
#define LFH_SIGNATURE 0x04034b50u
#define LFH_MIN 30u

// What this refuses to open at all. An EPUB with more than this many members,
// or a directory bigger than this, is not a book anyone is reading on a music
// player -- and the numbers exist so that a damaged file cannot ask for an
// arena the size of the card.
#define ZIP_MAX_ENTRIES 8192u
#define ZIP_MAX_DIRECTORY (4u * 1024u * 1024u)

static uint16_t read16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t read32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool read_at(FILE *f, long offset, void *into, size_t size) {
	return fseek(f, offset, SEEK_SET) == 0 && fread(into, 1, size, f) == size;
}

// Finds the end-of-central-directory record by scanning backwards for its
// signature. Backwards because a stored file could contain those four bytes,
// and the real record is the last one.
static bool find_eocd(FILE *f, long file_size, uint32_t *count, uint32_t *dir_size, uint32_t *dir_offset) {
	size_t window = (size_t)(file_size < (long)EOCD_SEARCH ? file_size : (long)EOCD_SEARCH);
	if (window < EOCD_MIN) {
		return false;
	}

	uint8_t *tail = malloc(window);
	if (!tail) {
		return false;
	}
	if (!read_at(f, file_size - (long)window, tail, window)) {
		free(tail);
		return false;
	}

	bool found = false;
	for (size_t back = EOCD_MIN; back <= window; back++) {
		const uint8_t *p = tail + window - back;
		if (read32(p) != EOCD_SIGNATURE) {
			continue;
		}
		*count = read16(p + 10);
		*dir_size = read32(p + 12);
		*dir_offset = read32(p + 16);
		found = true;
		break;
	}

	free(tail);
	return found;
}

bool epub_zip_open(epub_zip_t *zip, const char *path, epub_arena_t *arena) {
	memset(zip, 0, sizeof(*zip));
	zip->arena = arena;

	zip->file = fopen(path, "rb");
	if (!zip->file) {
		return false;
	}
	if (fseek(zip->file, 0, SEEK_END) != 0) {
		epub_zip_close(zip);
		return false;
	}
	long file_size = ftell(zip->file);
	if (file_size < (long)EOCD_MIN) {
		epub_zip_close(zip);
		return false;
	}

	uint32_t count = 0, dir_size = 0, dir_offset = 0;
	if (!find_eocd(zip->file, file_size, &count, &dir_size, &dir_offset)) {
		epub_zip_close(zip);
		return false;
	}
	if (count == 0 || count > ZIP_MAX_ENTRIES || dir_size > ZIP_MAX_DIRECTORY ||
		(long)dir_offset + (long)dir_size > file_size) {
		epub_zip_close(zip);
		return false;
	}

	// The whole directory at once: it is small, and walking it with one read
	// per entry would be a few hundred seeks on a microSD.
	uint8_t *dir = malloc(dir_size);
	if (!dir) {
		epub_zip_close(zip);
		return false;
	}
	if (!read_at(zip->file, (long)dir_offset, dir, dir_size)) {
		free(dir);
		epub_zip_close(zip);
		return false;
	}

	zip->entries = arena_calloc(arena, count, sizeof(epub_zip_entry_t));
	// One block for every name, so a book with two hundred resources is one
	// allocation and not two hundred. dir_size bounds it: a name cannot be
	// longer than the record that holds it.
	zip->names = arena_alloc(arena, dir_size + 1u);
	if (!zip->entries || !zip->names) {
		free(dir);
		epub_zip_close(zip);
		return false;
	}

	uint32_t name_used = 0;
	uint32_t kept = 0;
	size_t at = 0;
	for (uint32_t i = 0; i < count && at + CDH_MIN <= dir_size; i++) {
		const uint8_t *h = dir + at;
		if (read32(h) != CDH_SIGNATURE) {
			break;
		}
		uint16_t method = read16(h + 10);
		uint32_t comp = read32(h + 20);
		uint32_t uncomp = read32(h + 24);
		uint16_t name_len = read16(h + 28);
		uint16_t extra_len = read16(h + 30);
		uint16_t comment_len = read16(h + 32);
		uint32_t local = read32(h + 42);

		size_t record = CDH_MIN + (size_t)name_len + extra_len + comment_len;
		if (at + record > dir_size) {
			break;
		}

		// A directory entry, or something this cannot decompress. Skipped
		// rather than refused: a book with one odd member is still a book.
		bool usable = name_len > 0 && h[CDH_MIN + name_len - 1] != '/' && (method == 0 || method == 8);
		if (usable) {
			epub_zip_entry_t *e = &zip->entries[kept];
			e->name_offset = name_used;
			e->local_offset = local;
			e->comp_size = comp;
			e->uncomp_size = uncomp;
			e->method = method;

			memcpy(zip->names + name_used, h + CDH_MIN, name_len);
			name_used += name_len;
			zip->names[name_used++] = '\0';
			kept++;
		}

		at += record;
	}

	free(dir);
	zip->count = kept;
	if (!kept) {
		epub_zip_close(zip);
		return false;
	}
	return true;
}

void epub_zip_close(epub_zip_t *zip) {
	if (zip->file) {
		fclose(zip->file);
		zip->file = NULL;
	}
	// entries and names belong to the arena; whoever owns that frees them.
	zip->entries = NULL;
	zip->names = NULL;
	zip->count = 0;
}

const char *epub_zip_name(const epub_zip_t *zip, uint32_t index) {
	if (index >= zip->count) {
		return NULL;
	}
	return zip->names + zip->entries[index].name_offset;
}

int epub_zip_find(const epub_zip_t *zip, const char *name) {
	if (!name || !zip->count) {
		return -1;
	}
	for (uint32_t i = 0; i < zip->count; i++) {
		if (strcmp(zip->names + zip->entries[i].name_offset, name) == 0) {
			return (int)i;
		}
	}
	return -1;
}

uint32_t epub_zip_size(const epub_zip_t *zip, int index) {
	if (index < 0 || (uint32_t)index >= zip->count) {
		return 0;
	}
	return zip->entries[index].uncomp_size;
}

void *epub_zip_extract(const epub_zip_t *zip, int index, epub_arena_t *arena, uint32_t limit, uint32_t *size_out) {
	if (size_out) {
		*size_out = 0;
	}
	if (index < 0 || (uint32_t)index >= zip->count || !zip->file) {
		return NULL;
	}
	const epub_zip_entry_t *e = &zip->entries[index];
	if (e->uncomp_size > limit) {
		return NULL; // bigger than the caller is willing to hold
	}

	// The local header repeats the name and the extra field, and its lengths
	// are the ones that count: some writers put a different extra field here
	// than in the directory, so the data does not start where the directory's
	// lengths would say.
	uint8_t local[LFH_MIN];
	if (!read_at(zip->file, (long)e->local_offset, local, sizeof(local)) || read32(local) != LFH_SIGNATURE) {
		return NULL;
	}
	long data_at = (long)e->local_offset + (long)LFH_MIN + read16(local + 26) + read16(local + 28);

	// Room for a NUL after the data: every caller of this either parses text or
	// hands the bytes to an image decoder, and the first is much easier when
	// the buffer is already a C string.
	uint8_t *out = arena_alloc(arena, (size_t)e->uncomp_size + 1u);
	if (!out) {
		return NULL;
	}

	if (e->method == 0) {
		if (e->comp_size != e->uncomp_size || !read_at(zip->file, data_at, out, e->uncomp_size)) {
			return NULL;
		}
	} else {
		// The compressed bytes are read into a plain malloc and freed before
		// this returns. They are the one thing here that does not belong to the
		// book: keeping them in the arena would hold the compressed copy for as
		// long as the decompressed one.
		uint8_t *packed = malloc(e->comp_size ? e->comp_size : 1u);
		if (!packed) {
			return NULL;
		}
		if (!read_at(zip->file, data_at, packed, e->comp_size)) {
			free(packed);
			return NULL;
		}

		// A raw deflate stream, not a zlib one: a ZIP member has no zlib
		// header. The output buffer holds the whole result, whose size the
		// directory already gave, so the non-wrapping flag applies and tinfl
		// needs no window of its own.
		size_t written = tinfl_decompress_mem_to_mem(out, e->uncomp_size, packed, e->comp_size,
													 TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
		free(packed);
		if (written != e->uncomp_size) {
			return NULL;
		}
	}

	out[e->uncomp_size] = '\0';
	if (size_out) {
		*size_out = e->uncomp_size;
	}
	return out;
}

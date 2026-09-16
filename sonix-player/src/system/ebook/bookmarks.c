#include "src/system/ebook/bookmarks.h"

#include <stdio.h>
#include <string.h>

#include "src/system/core/config.h"

// See bookmarks.h for the shape of what is written.

#define SECTION "bookmarks"

void bookmark_key(const char *book_path, char *out, size_t out_size) {
	uint64_t h = 1469598103934665603ull;
	for (const unsigned char *p = (const unsigned char *)(book_path ? book_path : ""); *p; p++) {
		h ^= *p;
		h *= 1099511628211ull;
	}
	snprintf(out, out_size, "%08x%08x", (unsigned)(h >> 32), (unsigned)(h & 0xFFFFFFFFu));
}

static void count_key(const char *book_path, char *out, size_t out_size) {
	char key[24];
	bookmark_key(book_path, key, sizeof(key));
	snprintf(out, out_size, "n_%s", key);
}

static void entry_key(const char *book_path, int index, char *out, size_t out_size) {
	char key[24];
	bookmark_key(book_path, key, sizeof(key));
	// Clamped so the key has a bounded length: "b_" plus sixteen hex digits plus
	// "_" plus at most two, which is twenty-one, and a config key has room for
	// thirty-one.
	if (index < 0) {
		index = 0;
	}
	if (index > BOOKMARK_MAX_PER_BOOK) {
		index = BOOKMARK_MAX_PER_BOOK;
	}
	snprintf(out, out_size, "b_%s_%d", key, index);
}

static void set_count(const char *book_path, int count) {
	char key[48];
	count_key(book_path, key, sizeof(key));
	config_store_set_int(config_ebook_store(), SECTION, key, count);
}

int bookmark_count(const char *book_path) {
	if (!book_path || !book_path[0]) {
		return 0;
	}
	char key[48];
	count_key(book_path, key, sizeof(key));
	long n = config_store_get_int(config_ebook_store(), SECTION, key, 0);
	if (n < 0) {
		return 0;
	}
	return n > BOOKMARK_MAX_PER_BOOK ? BOOKMARK_MAX_PER_BOOK : (int)n;
}

bool bookmark_get(const char *book_path, int index, bookmark_t *out) {
	if (!out || index < 0 || index >= bookmark_count(book_path)) {
		return false;
	}
	memset(out, 0, sizeof(*out));

	char key[48];
	entry_key(book_path, index, key, sizeof(key));
	const char *value = config_store_get(config_ebook_store(), SECTION, key, "");
	if (!value || !value[0]) {
		return false;
	}

	unsigned a = 0, b = 0, c = 0;
	int consumed = 0;
	if (sscanf(value, "%u:%u:%u%n", &a, &b, &c, &consumed) < 3) {
		return false;
	}
	out->spine = a;
	out->block = b;
	out->offset = c;

	// Everything after the separator is the line of text, which may hold
	// colons of its own -- hence reading the position with %n and taking the
	// rest as it is rather than splitting on the last colon.
	const char *bar = value[consumed] == '|' ? value + consumed + 1 : NULL;
	if (bar) {
		snprintf(out->text, sizeof(out->text), "%s", bar);
	}
	return true;
}

static void write_entry(const char *book_path, int index, const bookmark_t *bm) {
	char key[48], value[160];
	entry_key(book_path, index, key, sizeof(key));
	snprintf(value, sizeof(value), "%u:%u:%u|%s", bm->spine, bm->block, bm->offset, bm->text);
	config_store_set(config_ebook_store(), SECTION, key, value);
}

bool bookmark_add(const char *book_path, uint32_t spine, uint32_t block, uint32_t offset, const char *text) {
	if (!book_path || !book_path[0]) {
		return false;
	}
	int count = bookmark_count(book_path);
	if (count >= BOOKMARK_MAX_PER_BOOK) {
		return false;
	}

	// The same page marked twice is a reader who pressed twice.
	for (int i = 0; i < count; i++) {
		bookmark_t existing;
		if (bookmark_get(book_path, i, &existing) && existing.spine == spine && existing.block == block &&
			existing.offset == offset) {
			return false;
		}
	}

	bookmark_t bm;
	memset(&bm, 0, sizeof(bm));
	bm.spine = spine;
	bm.block = block;
	bm.offset = offset;
	if (text) {
		snprintf(bm.text, sizeof(bm.text), "%s", text);
		// A snippet that ends mid-word reads as a broken string; back up to the
		// last space when the cut was not at one anyway.
		size_t len = strlen(bm.text);
		if (len == sizeof(bm.text) - 1) {
			char *space = strrchr(bm.text, ' ');
			if (space && space > bm.text + sizeof(bm.text) / 2) {
				*space = '\0';
			}
		}
	}

	write_entry(book_path, count, &bm);
	set_count(book_path, count + 1);
	config_store_save(config_ebook_store());
	return true;
}

bool bookmark_remove(const char *book_path, int index) {
	int count = bookmark_count(book_path);
	if (index < 0 || index >= count) {
		return false;
	}

	// Shifted down rather than left with a hole: the numbering is what the
	// keys are made of, and a sparse one would need a separate list of which
	// numbers are real.
	for (int i = index; i + 1 < count; i++) {
		bookmark_t next;
		if (bookmark_get(book_path, i + 1, &next)) {
			write_entry(book_path, i, &next);
		}
	}
	char key[48];
	entry_key(book_path, count - 1, key, sizeof(key));
	config_store_set(config_ebook_store(), SECTION, key, "");
	set_count(book_path, count - 1);
	config_store_save(config_ebook_store());
	return true;
}

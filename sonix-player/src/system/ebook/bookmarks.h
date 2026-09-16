#ifndef EBOOK_BOOKMARKS_H
#define EBOOK_BOOKMARKS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Where a reader put a finger in a book
//
// A bookmark is a reading position plus a line of the text that was on screen,
// so the list of them reads as places in a book rather than as numbers.
//
// They live in the reader's own file (/usr/data/ebook_config.ini, see config.h)
// under the same hash-of-the-path key the reading position uses, so a book is
// identified by where it is on the card and nothing has to store a path:
//
//     [bookmarks]
//     n_1a2b3c4d5e6f7a8b = 2
//     b_1a2b3c4d5e6f7a8b_0 = 3:41:120|Nel mezzo del cammin di nostra vita
//     b_1a2b3c4d5e6f7a8b_1 = 5:2:0|Lo giorno se n'andava
//
// Which book each hash belongs to is not written down anywhere, and does not
// need to be: the page that lists them walks the Ebook folder and hashes what
// is there. A book deleted from the card simply stops appearing, and its
// entries cost forty bytes until a factory reset.
// ---------------------------------------------------------------------------

// How many a single book may hold. The config table has room for far more than
// anyone will make, but a list that cannot be scrolled to the end is not a
// list -- and an unbounded one would be a book that can fill the file by
// itself.
#define BOOKMARK_MAX_PER_BOOK 64

// The snippet is cut to fit a config value (128 bytes) with the position and
// the separator in front of it.
#define BOOKMARK_TEXT_MAX 96

typedef struct {
	uint32_t spine;
	uint32_t block;
	uint32_t offset;
	char text[BOOKMARK_TEXT_MAX];
} bookmark_t;

// The key a book is known by: the FNV-1a of its path, as sixteen hex digits.
// `out` needs 17 bytes.
void bookmark_key(const char *book_path, char *out, size_t out_size);

// How many `book_path` has.
int bookmark_count(const char *book_path);

// Reads one, oldest first. False when `index` is past the end.
bool bookmark_get(const char *book_path, int index, bookmark_t *out);

// Adds one and saves. `text` may be NULL. False when the book is full or the
// same position is already marked -- marking the same page twice is a reader
// pressing twice, not a second bookmark.
bool bookmark_add(const char *book_path, uint32_t spine, uint32_t block, uint32_t offset, const char *text);

// Removes the one at `index`, closing the gap so the numbering stays dense.
bool bookmark_remove(const char *book_path, int index);

#endif /* EBOOK_BOOKMARKS_H */

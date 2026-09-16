#include "epub_arena.h"

#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

// How big a mapping gets. The first one is chosen by the caller -- a book's
// metadata needs far less than a chapter's text -- and each one after is twice
// the last up to this, because a book that outgrew one block is not going to
// fit in another of the same size either.
#define ARENA_BLOCK_MAX (1024u * 1024u)

// Every allocation is aligned to this. Eight is enough for every type this
// subsystem uses, including a double, on both the device and the host.
#define ARENA_ALIGN 8u

struct arena_block {
	arena_block_t *next;
	size_t size; // the whole mapping, this header included
	size_t used; // how far into it the bump pointer has got
};

static size_t page_size(void) {
	static size_t cached;
	if (!cached) {
		long got = sysconf(_SC_PAGESIZE);
		cached = got > 0 ? (size_t)got : 4096u;
	}
	return cached;
}

static size_t round_up(size_t value, size_t to) { return (value + to - 1u) / to * to; }

void arena_init(epub_arena_t *a, const char *name, size_t first_block) {
	memset(a, 0, sizeof(*a));
	a->name = name;
	a->first_block = first_block ? first_block : 64u * 1024u;
	a->block_size = a->first_block;
}

// Takes another mapping, big enough for `need` bytes on top of its own header.
static arena_block_t *block_new(epub_arena_t *a, size_t need) {
	size_t want = a->block_size;
	if (want < need + sizeof(arena_block_t)) {
		want = need + sizeof(arena_block_t);
	}
	want = round_up(want, page_size());

	// MAP_ANONYMOUS: pages from the kernel, not from the heap, and therefore
	// pages the kernel takes back when they are unmapped. They arrive zeroed
	// and are not touched until they are handed out, so a block bigger than
	// the book needs costs address space and not memory.
	void *mem = mmap(NULL, want, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mem == MAP_FAILED) {
		fprintf(stderr, "ebook: the %s arena cannot have %zu KB\n", a->name ? a->name : "?", want / 1024u);
		return NULL;
	}

	arena_block_t *block = mem;
	block->size = want;
	block->used = sizeof(arena_block_t);
	block->next = a->first;
	a->first = block;
	a->mapped += want;

	// The next one is bigger, up to the ceiling: a chapter that needed two
	// blocks says more about the book than the first block's size did.
	if (a->block_size < ARENA_BLOCK_MAX) {
		a->block_size *= 2u;
		if (a->block_size > ARENA_BLOCK_MAX) {
			a->block_size = ARENA_BLOCK_MAX;
		}
	}
	return block;
}

void *arena_alloc(epub_arena_t *a, size_t size) {
	if (!size) {
		size = 1; // so that two allocations never share an address
	}
	size = round_up(size, ARENA_ALIGN);

	// Only the newest block is ever allocated from. The older ones have a tail
	// nobody will use, and walking the list to find it would make allocation a
	// search rather than an addition -- for a few hundred bytes per book.
	arena_block_t *block = a->first;
	if (!block || block->size - block->used < size) {
		block = block_new(a, size);
		if (!block) {
			return NULL;
		}
	}

	void *out = (char *)block + block->used;
	block->used += size;
	a->used += size;
	return out;
}

void *arena_calloc(epub_arena_t *a, size_t count, size_t size) {
	if (count && size > (size_t)-1 / count) {
		return NULL; // the multiplication would wrap
	}
	size_t total = count * size;
	void *out = arena_alloc(a, total);
	if (out) {
		// Not always necessary -- a page straight from mmap is zero -- but a
		// block that has been used before is not, and telling the two apart here
		// would be fragile.
		memset(out, 0, total);
	}
	return out;
}

char *arena_strndup(epub_arena_t *a, const char *text, size_t len) {
	if (!text) {
		return NULL;
	}
	char *out = arena_alloc(a, len + 1u);
	if (!out) {
		return NULL;
	}
	memcpy(out, text, len);
	out[len] = '\0';
	return out;
}

char *arena_strdup(epub_arena_t *a, const char *text) {
	return text ? arena_strndup(a, text, strlen(text)) : NULL;
}

void arena_destroy(epub_arena_t *a) {
	arena_block_t *block = a->first;
	while (block) {
		// The header is inside the mapping, so `next` has to be read before
		// the mapping goes away.
		arena_block_t *next = block->next;
		munmap(block, block->size);
		block = next;
	}
	// Back to how arena_init left it, and not to the size the blocks had grown
	// to: the next book is a different book, and starting it with the largest
	// block the last one needed would hold that memory for no reason.
	const char *name = a->name;
	size_t first = a->first_block;
	memset(a, 0, sizeof(*a));
	a->name = name;
	a->first_block = first;
	a->block_size = first;
}

size_t arena_bytes(const epub_arena_t *a) { return a->mapped; }

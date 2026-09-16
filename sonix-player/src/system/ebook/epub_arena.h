#ifndef EPUB_ARENA_H
#define EPUB_ARENA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Where everything an open book owns is allocated
//
// A bump allocator over blocks taken straight from the kernel with mmap(), and
// given straight back with munmap(). Nothing here is ever freed on its own:
// the arena is thrown away whole, and that is the point.
//
// Why not malloc. The question this subsystem has to answer is not "is the
// memory freed" but "is the memory GONE" -- back with the kernel, out of this
// process's resident set, the moment the book is closed. glibc does not
// promise that: free() puts the block on a free list, and whether the pages go
// back depends on where in the heap they landed. A reader that leaves megabytes
// of grown heap behind after every book eventually kills the player on a device
// with this much RAM. munmap() has no such
// question: the mapping is unmapped, RSS drops, and it is not a matter of
// timing or of which allocator bin the block came from.
//
// The block descriptors live at the front of each mapping rather than in a
// malloc'd list, so an arena costs exactly its mappings and nothing else.
// ---------------------------------------------------------------------------

typedef struct arena_block arena_block_t;

typedef struct {
	arena_block_t *first;
	size_t first_block; // what arena_init was told, so a reused arena starts small again
	size_t block_size;	// what the next block asks the kernel for
	size_t mapped;	   // total bytes mapped, descriptors included
	size_t used;	   // total bytes handed out
	const char *name;  // for the accounting line in the log
} epub_arena_t;

// Sets an arena up. Nothing is mapped yet: an arena nobody allocates from
// costs nothing at all. `first_block` is the size of the first mapping;
// later blocks grow up to ARENA_BLOCK_MAX, because a book that needs a second
// block usually needs a third.
void arena_init(epub_arena_t *a, const char *name, size_t first_block);

// `size` bytes, aligned for anything. NULL when the kernel will not give the
// memory -- which a caller must handle, because on this device it happens.
void *arena_alloc(epub_arena_t *a, size_t size);

// The same, zeroed. Fresh mmap pages are already zero; this only pays for the
// part of a block that has been handed out before.
void *arena_calloc(epub_arena_t *a, size_t count, size_t size);

// A copy of `text` inside the arena, which dies with it. NULL propagates.
char *arena_strdup(epub_arena_t *a, const char *text);
char *arena_strndup(epub_arena_t *a, const char *text, size_t len);

// Unmaps every block. The arena is usable again afterwards, empty.
void arena_destroy(epub_arena_t *a);

// What this arena has taken from the kernel. Zero after arena_destroy().
size_t arena_bytes(const epub_arena_t *a);

#endif /* EPUB_ARENA_H */

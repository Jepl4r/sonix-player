#ifndef BOOKMARKGUARD_H
#define BOOKMARKGUARD_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct {
	bool valid;
	char book[512], file[512];
	double seconds;
	uint32_t tick;
} bookmark_guard_t;
static inline bool bookmark_guard_duplicate(const bookmark_guard_t *g, const char *book, const char *file, double seconds, uint32_t tick) {
	double delta = seconds - g->seconds;
	return g->valid && (uint32_t)(tick - g->tick) < 2000 && delta >= -1 && delta <= 1 && !strcmp(book, g->book) && !strcmp(file, g->file);
}
static inline void bookmark_guard_saved(bookmark_guard_t *g, const char *book, const char *file, double seconds, uint32_t tick) {
	snprintf(g->book, sizeof(g->book), "%s", book);
	snprintf(g->file, sizeof(g->file), "%s", file);
	g->seconds = seconds;
	g->tick = tick;
	g->valid = true;
}
#endif

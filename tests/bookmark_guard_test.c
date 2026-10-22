#include "src/gui/library/bookmarkguard.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
	bookmark_guard_t guard = {0};
	assert(!bookmark_guard_duplicate(&guard, "book", "file", 10, 100));
	bookmark_guard_saved(&guard, "book", "file", 10, 100);
	assert(bookmark_guard_duplicate(&guard, "book", "file", 11, 2099));
	assert(!bookmark_guard_duplicate(&guard, "book", "file", 10, 2100));
	assert(!bookmark_guard_duplicate(&guard, "other", "file", 10, 101));
	assert(!bookmark_guard_duplicate(&guard, "book", "other", 10, 101));
	assert(!bookmark_guard_duplicate(&guard, "book", "file", 11.1, 101));
	puts("PASS bookmark-guard");
}

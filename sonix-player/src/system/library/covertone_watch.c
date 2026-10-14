#include "covertone_watch.h"

#include <stddef.h>

#include "src/system/library/covertone.h"

static void (*watchers[COVERTONE_WATCH_MAX])(void);
static int watcher_count;

// Called by covertone. A callback may remove itself (or another) while this
// runs, so the list is walked from a copy.
static void fan_out(void) {
	void (*now[COVERTONE_WATCH_MAX])(void);
	int n = watcher_count;
	for (int i = 0; i < n; i++) {
		now[i] = watchers[i];
	}
	for (int i = 0; i < n; i++) {
		if (now[i]) {
			now[i]();
		}
	}
}

void covertone_watch_add(void (*changed)(void)) {
	if (!changed) {
		return;
	}
	for (int i = 0; i < watcher_count; i++) {
		if (watchers[i] == changed) {
			return;
		}
	}
	if (watcher_count >= COVERTONE_WATCH_MAX) {
		return;
	}
	watchers[watcher_count++] = changed;
	if (watcher_count == 1) {
		covertone_set_listener(fan_out);
	}
}

void covertone_watch_remove(void (*changed)(void)) {
	for (int i = 0; i < watcher_count; i++) {
		if (watchers[i] == changed) {
			watchers[i] = watchers[--watcher_count];
			watchers[watcher_count] = NULL;
			break;
		}
	}
	if (watcher_count == 0) {
		covertone_set_listener(NULL);
	}
}

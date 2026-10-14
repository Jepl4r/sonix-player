#include "covertonefan.h"

#include <stddef.h>

#include "src/system/library/covertone.h"

static void (*listeners[COVERTONEFAN_MAX])(void);
static int listener_count;
static bool installed;

static void fan_out(void) {
	// A copy, so a callback that adds or removes a listener does not shift the
	// table under the loop.
	void (*now[COVERTONEFAN_MAX])(void);
	int count = listener_count;
	for (int i = 0; i < count; i++) {
		now[i] = listeners[i];
	}
	for (int i = 0; i < count; i++) {
		if (now[i]) {
			now[i]();
		}
	}
}

bool covertonefan_add(void (*cb)(void)) {
	if (!cb) {
		return false;
	}
	for (int i = 0; i < listener_count; i++) {
		if (listeners[i] == cb) {
			return true;
		}
	}
	if (listener_count >= COVERTONEFAN_MAX) {
		return false;
	}
	listeners[listener_count++] = cb;
	if (!installed) {
		installed = true;
		covertone_set_listener(fan_out);
	}
	return true;
}

void covertonefan_remove(void (*cb)(void)) {
	for (int i = 0; i < listener_count; i++) {
		if (listeners[i] == cb) {
			for (int k = i; k + 1 < listener_count; k++) {
				listeners[k] = listeners[k + 1];
			}
			listener_count--;
			return;
		}
	}
}

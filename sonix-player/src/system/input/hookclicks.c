#include "hookclicks.h"

// Times are compared as a signed difference and never as `now >= due`. The
// clock these come from is a millisecond counter that wraps roughly every
// forty-nine days, and the plain comparison reads backwards for the window
// straddling the wrap -- a click that would then wait out the whole counter.
static bool reached(uint32_t now, uint32_t deadline) { return (int32_t)(now - deadline) >= 0; }

void hook_clicks_press(hook_clicks_t *h, uint32_t now_ms) {
	if (!h) {
		return;
	}
	// The window runs from the LAST click, not the first: three clicks at the
	// pace a thumb manages must not have the third fall outside it.
	h->clicks++;
	h->due = now_ms + HOOK_MULTI_CLICK_MS;
}

bool hook_clicks_pending(const hook_clicks_t *h) { return h && h->clicks > 0; }

int hook_clicks_timeout(const hook_clicks_t *h, uint32_t now_ms) {
	if (!hook_clicks_pending(h)) {
		return -1;
	}
	return reached(now_ms, h->due) ? 0 : (int)(h->due - now_ms);
}

bool hook_clicks_expired(const hook_clicks_t *h, uint32_t now_ms) {
	return hook_clicks_pending(h) && reached(now_ms, h->due);
}

hook_gesture_t hook_clicks_take(hook_clicks_t *h) {
	if (!hook_clicks_pending(h)) {
		return HOOK_GESTURE_NONE;
	}
	int clicks = h->clicks;
	hook_clicks_cancel(h);

	switch (clicks) {
	case 1:
		return HOOK_GESTURE_PLAY_PAUSE;
	case 2:
		return HOOK_GESTURE_NEXT;
	default:
		// Three or more. A fourth click is a thumb that slipped, not a fourth
		// meaning, so it stays previous rather than becoming nothing.
		return HOOK_GESTURE_PREV;
	}
}

void hook_clicks_cancel(hook_clicks_t *h) {
	if (h) {
		h->clicks = 0;
		h->due = 0;
	}
}

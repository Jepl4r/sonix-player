#ifndef HOOKCLICKS_H
#define HOOKCLICKS_H

#include <stdbool.h>
#include <stdint.h>

// One, two or three clicks on the centre button of a cable remote.
//
// A three-button remote has no next and no previous: the centre button carries
// them as a double and a triple click, the way the Apple remote this device's
// driver is named after always has. sa_earpods_adc says it counts the clicks
// itself -- one KEY_PLAYPAUSE, two KEY_NEXTSONG, three KEY_PREVIOUSSONG -- but
// on the remote in hand only the single click ever arrives, so the count is
// done here as well.
//
// Counting here costs a delay: a click cannot become play/pause until it is
// known that no second one is coming. HOOK_MULTI_CLICK_MS is that wait, and it
// is why every remote of this kind pauses a moment before the music does.
//
// If the driver does emit its own 163 or 165, that is acted on at once and the
// pending count is thrown away (hook_clicks_cancel), so one gesture can never
// be counted twice.

// Long enough that a deliberate double click lands inside it, short enough that
// the pause before the music stops is not read as the button being missed.
#define HOOK_MULTI_CLICK_MS 300

typedef enum {
	HOOK_GESTURE_NONE = 0,
	HOOK_GESTURE_PLAY_PAUSE,
	HOOK_GESTURE_NEXT,
	HOOK_GESTURE_PREV,
} hook_gesture_t;

// Zero-initialised is the idle state, so a caller needs no constructor.
typedef struct {
	int clicks;
	uint32_t due; // when the window closes; meaningless with no clicks counted
} hook_clicks_t;

// A press of the centre button. Never decides anything on its own: a click can
// always turn out to be the first of two.
void hook_clicks_press(hook_clicks_t *h, uint32_t now_ms);

bool hook_clicks_pending(const hook_clicks_t *h);

// Milliseconds until the window closes, or -1 with nothing pending -- what a
// poll() deadline is built from.
int hook_clicks_timeout(const hook_clicks_t *h, uint32_t now_ms);

// True once no further click can join the ones already counted.
bool hook_clicks_expired(const hook_clicks_t *h, uint32_t now_ms);

// The gesture the counted clicks add up to, and the counter goes back to idle.
// HOOK_GESTURE_NONE when there was nothing to take.
hook_gesture_t hook_clicks_take(hook_clicks_t *h);

// Back to idle with no gesture: the driver counted this one itself.
void hook_clicks_cancel(hook_clicks_t *h);

#endif /* HOOKCLICKS_H */

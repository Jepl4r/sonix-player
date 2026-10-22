#ifndef SLEEPFADE_H
#define SLEEPFADE_H

#include <stdbool.h>
#include <stdint.h>

#define SLEEPFADE_SPAN_MS 20000
#define SLEEPFADE_RETURN_MS 500

static inline int sleepfade_gain_q10(uint32_t now_ms, uint32_t deadline_ms, bool armed, uint32_t disarmed_at_ms, int gain_at_disarm) {
	if (armed) {
		int32_t left = (int32_t)(deadline_ms - now_ms);
		if (left <= 0)
			return 0;
		if (left >= SLEEPFADE_SPAN_MS)
			return 1024;
		return left * 1024 / SLEEPFADE_SPAN_MS;
	}
	uint32_t elapsed = now_ms - disarmed_at_ms;
	if (elapsed >= SLEEPFADE_RETURN_MS)
		return 1024;
	return gain_at_disarm + (1024 - gain_at_disarm) * (int)elapsed / SLEEPFADE_RETURN_MS;
}

// The deadline and the return ramp both limit the gain of this snapshot.
static inline int sleep_fade_gain(uint32_t now_ms, uint32_t deadline_ms, bool armed, uint32_t disarmed_at_ms, int gain_at_disarm) {
	int returning = sleepfade_gain_q10(now_ms, 0, false, disarmed_at_ms, gain_at_disarm);
	int timed = sleepfade_gain_q10(now_ms, deadline_ms, armed, disarmed_at_ms, gain_at_disarm);
	return timed < returning ? timed : returning;
}

#endif /* SLEEPFADE_H */

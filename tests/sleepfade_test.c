#include "../sonix-player/src/system/audio/sleepfade.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
	assert(sleepfade_gain_q10(10000, 40000, true, 0, 1024) == 1024);
	assert(sleepfade_gain_q10(30000, 40000, true, 0, 1024) == 512);
	assert(sleepfade_gain_q10(40000, 40000, true, 0, 1024) == 0);
	assert(sleepfade_gain_q10(45000, 40000, true, 0, 1024) == 0);
	assert(sleepfade_gain_q10(1000, 0, false, 1000, 512) == 512);
	assert(sleepfade_gain_q10(1250, 0, false, 1000, 512) == 768);
	assert(sleepfade_gain_q10(1500, 0, false, 1000, 512) == 1024);
	assert(sleepfade_gain_q10(2000, 0, false, 1000, 1024) == 1024);
	assert(sleepfade_gain_q10(UINT32_MAX - 4999, 5000, true, 0, 1024) == 512);
	assert(sleepfade_gain_q10(5000, UINT32_MAX - 4999, true, 0, 1024) == 0);
	assert(sleepfade_gain_q10(249, 0, false, UINT32_MAX - 249, 512) == 1022);
	// Extending a deadline while fading returns from the gain in effect.
	int gain = sleepfade_gain_q10(30000, 40000, true, 0, 1024);
	assert(gain == 512);
	assert(sleep_fade_gain(30000, 90000, true, 30000, gain) == 512);
	assert(sleep_fade_gain(30250, 90000, true, 30000, gain) == 768);
	assert(sleep_fade_gain(30500, 90000, true, 30000, gain) == 1024);
	// A nearer deadline still wins over the return ramp.
	assert(sleep_fade_gain(30000, 35000, true, 30000, gain) == 256);
	puts("PASS sleepfade");
	return 0;
}

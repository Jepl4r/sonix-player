#include "swvolume.h"

#include "src/system/audio/audio.h"
#include "src/system/audio/usbaudio.h"

#include <math.h>
#include <stdio.h>

// SW/HDB, the one software curve the R3 Pro II's ot_devices.json defines, in
// tenths of a dB. Index 0 is -150 dB and not silence: it is far below anything
// a converter can render, but it is not a zero coefficient, so a real mute has
// to be someone else's job.
//
// The shape is deliberate -- huge steps at the bottom, 0.3 dB steps at the top,
// so the last few points of the scale are fine adjustment rather than the
// blunt 1% of a linear percentage.
static const int SW_HDB[101] = {
	-1500,													  //
	-1200, -600, -510, -470, -420, -395, -375, -361, -358, -346, //
	-335,  -325, -316, -308, -300, -292, -285, -278, -271, -264, //
	-258,  -252, -246, -240, -235, -230, -225, -220, -215, -210, //
	-207,  -204, -201, -198, -195, -192, -189, -186, -183, -180, //
	-177,  -174, -171, -168, -165, -162, -159, -156, -153, -150, //
	-147,  -144, -141, -138, -135, -132, -129, -126, -123, -120, //
	-117,  -114, -111, -108, -105, -102, -99,  -96,  -93,  -90,  //
	-87,   -84,  -81,  -78,  -75,  -72,  -69,  -66,  -63,  -60,  //
	-57,   -54,  -51,  -48,  -45,  -42,  -39,  -36,  -33,  -30,  //
	-27,   -24,  -21,  -18,  -15,  -12,  -9,   -6,   -3,   0};

// The coefficients, worked out once. pow() is a poor thing to call per sample
// and an unnecessary one to call per volume change: there are 101 answers and
// they never change.
static int64_t coefficient[101];
static bool table_ready;

// The index in force. A plain int, written by whoever moves the volume and read
// by the playback thread; a word-sized load and store, so the worst a reader
// can see is the level from a moment ago.
static int sw_index = 100;

// The setting, read the same way and for the same reason.
static bool sw_disabled;

static int clamp_index(int percent) {
	if (percent < 0) {
		return 0;
	}
	if (percent > 100) {
		return 100;
	}
	return percent;
}

static void build_table(void) {
	if (table_ready) {
		return;
	}
	for (int i = 0; i <= 100; i++) {
		coefficient[i] = (int64_t)(pow(10.0, (double)SW_HDB[i] / 200.0) * 2147483648.0);
	}
	table_ready = true;
}

void swvolume_set_index(int percent) {
	build_table();
	sw_index = clamp_index(percent);
}

int swvolume_millibel(int percent) { return SW_HDB[clamp_index(percent)]; }

int64_t swvolume_coefficient(int percent) {
	build_table();
	return coefficient[clamp_index(percent)];
}

// On the analogue path this runs alongside the CS43198's own attenuation, and
// not instead of it. The stock player's configuration says so in its first
// line:
//
//     "SET_HW_SW_VOL_BOTH": 1
//
// and the two curves are drawn to match. Neither is regular alone -- the
// hardware one steps 0.5 dB here and 1 dB there, the software one 0.3 dB and
// then 0.9 -- but added together they come out at exactly 0.8 dB per index
// from 100 down to 45. They are one volume law, split across the converter and
// the samples; taking either half away leaves a scale that is neither even nor
// the stock player's.
//
// Two routes are out of it. Over Bluetooth the level belongs to the headphones
// (A2DP_FIXED_GAIN in the same file, and AVRCP absolute volume here), and over
// USB-C to a device that publishes a volume control of its own -- there the
// CS43198 is not in the path at all, so there is no pair to complete and the
// device's own control is written instead. On a USB device with no control at
// all, this curve is the only thing the volume keys can move.
bool swvolume_active(void) {
	if (audio_output_is_bluetooth()) {
		return false;
	}
	if (usbaudio_active() && usbaudio_has_volume_control()) {
		return false;
	}
	// Switched off by hand, and only where there is a converter register left
	// to carry the volume on its own. On a USB device with no control of its
	// own there is not: see the note over swvolume_set_disabled().
	if (sw_disabled && !usbaudio_active()) {
		return false;
	}
	return true;
}

void swvolume_set_disabled(bool disabled) {
	if (sw_disabled == disabled) {
		return;
	}
	sw_disabled = disabled;
	fprintf(stderr, "swvolume: software attenuation %s\n", disabled ? "off" : "on");
}

bool swvolume_disabled(void) { return sw_disabled; }

// The gain to apply now, or 0 for "leave the samples alone". Zero is free as a
// sentinel: the quietest real coefficient on the curve is 67.
static int64_t gain_now(void) {
	if (!table_ready || !swvolume_active()) {
		return 0;
	}
	int idx = clamp_index(sw_index);
	if (idx >= 100) {
		return 0; // unity
	}

	static int said = -1;
	if (said != idx) {
		said = idx;
		fprintf(stderr, "swvolume: index %d -> %.1f dB (Q31 %lld)\n", idx, SW_HDB[idx] / 10.0,
				(long long)coefficient[idx]);
	}
	return coefficient[idx];
}

// Rounded, not truncated. A shift alone rounds towards minus infinity, which on
// a signed sample is a half-bit of DC and an asymmetric error either side of
// zero; half a step added first costs one instruction and removes both.
#define SWVOLUME_SCALE(sample, gain) (((int64_t)(sample) * (gain) + (1 << 30)) >> 31)

void swvolume_apply_s16(short *samples, int count) {
	int64_t gain = gain_now();
	if (gain <= 0) {
		return;
	}
	for (int i = 0; i < count; i++) {
		samples[i] = (short)SWVOLUME_SCALE(samples[i], gain);
	}
}

void swvolume_apply_s32(int32_t *samples, int count) {
	int64_t gain = gain_now();
	if (gain <= 0) {
		return;
	}
	for (int i = 0; i < count; i++) {
		samples[i] = (int32_t)SWVOLUME_SCALE(samples[i], gain);
	}
}

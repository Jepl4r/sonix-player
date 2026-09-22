#include "swvolume.h"

#include "src/system/audio/alsa-controls.h"
#include "src/system/audio/audio.h"
#include "src/system/audio/usbaudio.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

// SW/HDB, the one software curve the R3 Pro II's ot_devices.json defines, in
// tenths of a dB. Index 0 is -150 dB and not silence: it is far below anything
// a converter can render, but it is not a zero coefficient, so a real mute is
// handled separately below.
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

// What the stock engine adds to the curve for each of its three hardware
// tables, in tenths of a dB. Only two of them are reachable here: the gain
// switch sends MDB or HDB and never LDB.
//
// It is applied because SW_MIX_GAIN_ENABLE is on. The key is absent from this
// device's ot_devices.json, and the stock parser's default for it is 1 -- the
// field is written before the file is read and only overwritten if the key
// turns up.
#define SW_OFFSET_MDB (-60)
#define SW_OFFSET_HDB 0

// The coefficients, worked out once, one set per gain. pow() is a poor thing to
// call per sample and an unnecessary one to call per volume change: there are
// 101 answers per gain and they never change.
static int64_t coefficient_hdb[101];
static int64_t coefficient_mdb[101];
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

// The stock binary's own arithmetic, down to the intermediate type. The
// division is done in float and only the result is widened for pow(): dividing
// in double instead moves about ninety of the 101 coefficients by up to 35
// counts, which is inaudible but is not the same number.
static int64_t stock_q31(int effective_tenths) {
	float exponent = (float)effective_tenths / 200.0f;
	return (int64_t)(pow(10.0, (double)exponent) * 2147483648.0);
}

static void build_table(void) {
	if (table_ready) {
		return;
	}
	for (int i = 0; i <= 100; i++) {
		coefficient_hdb[i] = stock_q31(SW_HDB[i] + SW_OFFSET_HDB);
		coefficient_mdb[i] = stock_q31(SW_HDB[i] + SW_OFFSET_MDB);
	}
	// Index 0 is silence rather than the -150 dB the curve carries. The stock
	// engine sets both channel coefficients to zero there and the samples go
	// out as a zeroed buffer; -150 dB leaves a coefficient of 67, which is a
	// signal, and the one place a volume control must be exact is the bottom.
	coefficient_hdb[0] = 0;
	coefficient_mdb[0] = 0;
	table_ready = true;
}

void swvolume_set_index(int percent) {
	build_table();
	sw_index = clamp_index(percent);
}

int swvolume_millibel(int percent) { return SW_HDB[clamp_index(percent)]; }

int64_t swvolume_coefficient(int percent, bool high_gain) {
	build_table();
	int idx = clamp_index(percent);
	return high_gain ? coefficient_hdb[idx] : coefficient_mdb[idx];
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

// Which half of the volume law this stream is getting, and so whether the gain
// offset belongs on it.
//
// The offset is the software share of the difference between two HARDWARE
// tables, so it goes wherever those tables go and nowhere else. On a USB device
// the converter is not in the path, the hardware curve is not written, and the
// gain switch moves nothing; adding -6 dB there would be six decibels of
// attenuation for a setting that has no other effect on that route.
//
// Read from alsa-controls on every buffer rather than kept here: one integer
// load, and the gain in force cannot then be a copy that a gain change has not
// reached yet.
static bool gain_offset_applies(void) { return !usbaudio_active(); }

// What to do with this buffer: the Q31 coefficient, or one of two answers that
// are not a multiply.
typedef enum {
	SW_PASS,  // leave the samples alone
	SW_MUTE,  // fill with silence
	SW_SCALE, // multiply by `gain`
} sw_action_t;

static sw_action_t gain_now(int64_t *gain) {
	*gain = 0;
	if (!table_ready || !swvolume_active()) {
		return SW_PASS;
	}

	int idx = clamp_index(sw_index);
	if (idx == 0) {
		return SW_MUTE;
	}

	bool high = gain_offset_applies() ? (get_high_gain() != 0) : true;
	int64_t q31 = high ? coefficient_hdb[idx] : coefficient_mdb[idx];

	// Unity, and only where it really is unity: index 100 on HDB. On MDB the
	// same index is the -6 dB offset, which is a multiply like any other -- the
	// stock engine calls the scaler there too, so the curve has no step at its
	// top end.
	if (q31 == 2147483648LL) {
		return SW_PASS;
	}

	static int said = -1;
	static int said_gain = -1;
	if (said != idx || said_gain != (int)high) {
		said = idx;
		said_gain = (int)high;
		fprintf(stderr, "swvolume: index %d %s -> %.1f dB (Q31 %lld)\n", idx, high ? "HDB" : "MDB",
				(SW_HDB[idx] + (high ? SW_OFFSET_HDB : SW_OFFSET_MDB)) / 10.0, (long long)q31);
	}

	*gain = q31;
	return SW_SCALE;
}

// Truncating, because that is what the stock binary does: the product is shifted
// down by 31 with nothing added first. Rounding to nearest would cost one
// instruction and be marginally more correct, and would also stop the output
// being the stock player's sample for sample.
#define SWVOLUME_SCALE(sample, gain) (((int64_t)(sample) * (gain)) >> 31)

void swvolume_apply_u8(unsigned char *samples, int count) {
	int64_t gain = 0;
	sw_action_t action = gain_now(&gain);
	if (action == SW_PASS) {
		return;
	}
	if (action == SW_MUTE) {
		// Not zero: unsigned 8-bit PCM has silence at the middle of its range.
		memset(samples, 0x80, (size_t)count);
		return;
	}
	for (int i = 0; i < count; i++) {
		int sample = (int)samples[i] - 128;
		samples[i] = (unsigned char)(SWVOLUME_SCALE(sample, gain) + 128);
	}
}

void swvolume_apply_s16(short *samples, int count) {
	int64_t gain = 0;
	sw_action_t action = gain_now(&gain);
	if (action == SW_PASS) {
		return;
	}
	if (action == SW_MUTE) {
		memset(samples, 0, (size_t)count * sizeof(*samples));
		return;
	}
	for (int i = 0; i < count; i++) {
		samples[i] = (short)SWVOLUME_SCALE(samples[i], gain);
	}
}

// S24_3LE: three bytes per sample, little-endian, signed, and nothing between
// one sample and the next. The sample has to be sign-extended out of 24 bits
// before the multiply and only three bytes written back -- the byte after the
// last sample belongs to the next frame, or to whatever follows the PCM in the
// buffer.
void swvolume_apply_s24_3le(unsigned char *samples, int count) {
	int64_t gain = 0;
	sw_action_t action = gain_now(&gain);
	if (action == SW_PASS) {
		return;
	}
	if (action == SW_MUTE) {
		memset(samples, 0, (size_t)count * 3);
		return;
	}
	for (int i = 0; i < count; i++) {
		unsigned char *p = samples + (size_t)i * 3;
		int32_t sample = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
		if (sample & 0x800000) {
			sample -= 0x1000000;
		}
		int32_t scaled = (int32_t)SWVOLUME_SCALE(sample, gain);
		p[0] = (unsigned char)(scaled & 0xFF);
		p[1] = (unsigned char)((scaled >> 8) & 0xFF);
		p[2] = (unsigned char)((scaled >> 16) & 0xFF);
	}
}

void swvolume_apply_s32(int32_t *samples, int count) {
	int64_t gain = 0;
	sw_action_t action = gain_now(&gain);
	if (action == SW_PASS) {
		return;
	}
	if (action == SW_MUTE) {
		memset(samples, 0, (size_t)count * sizeof(*samples));
		return;
	}
	for (int i = 0; i < count; i++) {
		samples[i] = (int32_t)SWVOLUME_SCALE(samples[i], gain);
	}
}

#include "swvolume.h"

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

// The coefficients, worked out once. pow() is a poor thing to call per sample
// and an unnecessary one to call per volume change: there are 101 answers and
// they never change.
static int64_t coefficient[101];
static bool table_ready;

// The index in force. A plain int, written by whoever moves the volume and read
// by the playback thread; a word-sized load and store, so the worst a reader
// can see is the level from a moment ago.
static int sw_index = 100;

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
		coefficient[i] = stock_q31(SW_HDB[i]);
	}
	// Index 0 is silence rather than the -150 dB the curve carries. The stock
	// engine sets both channel coefficients to zero there and the samples go
	// out as a zeroed buffer; -150 dB leaves a coefficient of 67, which is a
	// signal, and the one place a volume control must be exact is the bottom.
	coefficient[0] = 0;
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

// The one route this curve belongs to: a device on the USB-C port with no
// volume control of its own. There the CS43198 is not in the path at all, so
// the volume keys have nothing else to move -- the stream is the only place
// left to put the level.
//
// Everywhere else something better holds it. On the jacks it is the converter's
// own attenuation, which is the whole volume law there and needs no partner:
// each hardware table steps 0.5 dB per index from 100 down to 45, 1 dB down to
// 10, then 2 and 5, and MDB sits exactly 6.0 dB under HDB at every index.
// Over Bluetooth the headphones hold it (A2DP_FIXED_GAIN in the stock
// configuration, and AVRCP absolute volume here). On a USB device that
// publishes a control of its own, that control is written directly.
//
// Bluetooth is asked first and separately: usbaudio_poll() gives up as soon as
// Bluetooth is playing, so a USB card that was in use keeps saying so while the
// sound is going over the air.
bool swvolume_active(void) {
	if (audio_output_is_bluetooth()) {
		return false;
	}
	return usbaudio_active() && !usbaudio_has_volume_control();
}

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

	int64_t q31 = coefficient[idx];
	if (q31 == 2147483648LL) {
		return SW_PASS; // unity: index 100
	}

	static int said = -1;
	if (said != idx) {
		said = idx;
		fprintf(stderr, "swvolume: index %d -> %.1f dB (Q31 %lld)\n", idx, SW_HDB[idx] / 10.0, (long long)q31);
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

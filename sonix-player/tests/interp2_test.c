// Desktop check of the x2 interpolator (src/system/audio/decimate.c): gain of
// a tone through it, and how much of its image (at the input rate minus the
// tone) is left, at 48 -> 96 kHz. Then the 44.1 -> 48 -> 96 kHz chain the
// player runs for a CD-rate track on an LDAC link held at 96 kHz.
//
//   cc -std=gnu11 -O2 -Wall -Wextra -I. tests/interp2_test.c src/system/audio/decimate.c src/system/audio/resample.c -lm -o /tmp/interp2_test
//   /tmp/interp2_test
//
// The images are all ultrasonic (28 to 48 kHz); the equiripple design keeps
// them 77 to 88 dB down up to 19.2 kHz, the edge of its passband.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/system/audio/decimate.h"
#include "src/system/audio/resample.h"

// resample.c reads its setting through the configuration; not needed here.
const char *config_get(const char *section, const char *key, const char *fallback) {
	(void)section;
	(void)key;
	return fallback;
}

static int failures;

// Amplitude of `hz` in `x` (mono, `n` samples at `rate`), by a least-squares
// fit of a sine and a cosine.
static double tone_level(const double *x, int n, double rate, double hz) {
	double ss = 0, sc = 0, cc = 0, xs = 0, xc = 0;
	for (int i = 0; i < n; i++) {
		double s = sin(2 * M_PI * hz * i / rate), c = cos(2 * M_PI * hz * i / rate);
		ss += s * s;
		sc += s * c;
		cc += c * c;
		xs += x[i] * s;
		xc += x[i] * c;
	}
	double det = ss * cc - sc * sc;
	double a = (xs * cc - xc * sc) / det, b = (xc * ss - xs * sc) / det;
	return hypot(a, b);
}

#define IN_FRAMES 48000
#define AMP 0.5

static void one_tone(double hz) {
	static int32_t in[IN_FRAMES * 2], out[IN_FRAMES * 4];
	static double mono[IN_FRAMES * 2];
	interp2_t p;
	interp2_init(&p, 2);
	for (int i = 0; i < IN_FRAMES; i++) {
		int32_t v = (int32_t)lrint(AMP * sin(2 * M_PI * hz * i / 48000.0) * 8388607.0) * 256;
		in[2 * i] = v;
		in[2 * i + 1] = -v;
	}
	int made = 0;
	// In uneven blocks, the way the playback loop hands them over.
	for (int at = 0; at < IN_FRAMES;) {
		int n = (at / 7) % 1000 + 1;
		if (at + n > IN_FRAMES) {
			n = IN_FRAMES - at;
		}
		made += interp2_s32(&p, in + 2 * at, n, out + 2 * made);
		at += n;
	}
	int skip = 2000; // the delay line filling
	int n = made - 2 * skip;
	for (int i = 0; i < n; i++) {
		mono[i] = out[2 * (i + skip)] / 2147483648.0;
	}
	double gain = tone_level(mono, n, 96000.0, hz) / AMP;
	double image = tone_level(mono, n, 96000.0, 48000.0 - hz) / AMP;
	double gain_db = 20 * log10(gain), image_db = 20 * log10(image + 1e-12);
	bool ok = (hz > 19200 || fabs(gain_db) < 0.01) && (hz > 19200 || image_db < -76);
	printf("%s %6.0f Hz: gain %+.4f dB, image at %5.0f Hz %6.1f dB\n", ok ? "ok  " : "FAIL", hz, gain_db, 48000.0 - hz,
		   image_db);
	if (!ok) {
		failures++;
	}
}

// 44.1 -> 48 -> 96 kHz: what is left of a 1 kHz and a 15 kHz tone besides
// the tone itself, over the audible band.
static void chain_tone(double hz) {
	enum { N = 44100 };
	static int32_t in[N * 2], mid[N * 2 * 2], out[N * 2 * 5];
	static double mono[N * 5];
	resampler_t r;
	interp2_t p;
	resample_init(&r, 2);
	interp2_init(&p, 2);
	for (int i = 0; i < N; i++) {
		int32_t v = (int32_t)lrint(AMP * sin(2 * M_PI * hz * i / 44100.0) * 8388607.0) * 256;
		in[2 * i] = v;
		in[2 * i + 1] = v;
	}
	int made = 0;
	for (int at = 0; at < N; at += 1000) {
		int n = at + 1000 > N ? N - at : 1000;
		int m = resample_s32(&r, in + 2 * at, n, mid);
		made += interp2_s32(&p, mid, m, out + 2 * made);
	}
	int skip = 4000;
	int n = made - 2 * skip;
	for (int i = 0; i < n; i++) {
		mono[i] = out[2 * (i + skip)] / 2147483648.0;
	}
	double level = tone_level(mono, n, 96000.0, hz);
	// The residue: everything but the tone, as a level below it.
	double w = 2 * M_PI * hz / 96000.0, ss = 0, sc = 0, cc = 0, xs = 0, xc = 0;
	for (int i = 0; i < n; i++) {
		double s = sin(w * i), c = cos(w * i);
		ss += s * s;
		sc += s * c;
		cc += c * c;
		xs += mono[i] * s;
		xc += mono[i] * c;
	}
	double det = ss * cc - sc * sc;
	double a = (xs * cc - xc * sc) / det, b = (xc * ss - xs * sc) / det;
	double res = 0;
	for (int i = 0; i < n; i++) {
		double e = mono[i] - a * sin(w * i) - b * cos(w * i);
		res += e * e;
	}
	double resid_db = 10 * log10(res / n) - 20 * log10(level / sqrt(2));
	bool ok = fabs(20 * log10(level / AMP)) < 0.05 && resid_db < -80;
	printf("%s chain 44.1->96 %5.0f Hz: gain %+.4f dB, everything else %6.1f dB\n", ok ? "ok  " : "FAIL", hz,
		   20 * log10(level / AMP), resid_db);
	if (!ok) {
		failures++;
	}
	resample_free();
}

int main(void) {
	const double tones[] = {100, 1000, 5000, 10000, 15000, 18000, 19000, 20000};
	for (size_t i = 0; i < sizeof(tones) / sizeof(tones[0]); i++) {
		one_tone(tones[i]);
	}
	chain_tone(1000);
	chain_tone(15000);
	printf("%s\n", failures ? "FAILED" : "all passed");
	return failures ? 1 : 0;
}

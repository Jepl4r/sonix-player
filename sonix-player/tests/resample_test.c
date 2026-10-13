// Desktop check of the polyphase converters (src/system/audio/resample.c),
// 44.1 -> 48 kHz and 48 -> 44.1 kHz: the gain of tones across the audible
// band, what is left besides the tone (images, aliases, rounding), that a
// stream fed in uneven blocks comes out bit for bit as one fed whole, and what
// a second of stereo costs on this machine.
//
//   cc -std=gnu11 -O2 -Wall -Wextra -I. tests/resample_test.c src/system/audio/resample.c -lm -lpthread -o /tmp/resample_test
//   /tmp/resample_test
//
// The residue is measured over the whole band of the output, so tones whose
// image is allowed to stay (it lands above 20 kHz, see resample.h) are not
// among the ones checked for it.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/system/audio/resample.h"

// resample.c reads its setting through the configuration; not needed here.
const char *config_get(const char *section, const char *key, const char *fallback) {
	(void)section;
	(void)key;
	return fallback;
}

static int failures;

#define AMP 0.5
#define SECONDS 1

// Level of `hz` in `x` (n samples at `rate`) and of everything else, as a
// least-squares fit of a sine and a cosine and what is left over.
static void tone_fit(const double *x, int n, double rate, double hz, double *level, double *resid_db) {
	double w = 2 * M_PI * hz / rate, ss = 0, sc = 0, cc = 0, xs = 0, xc = 0;
	for (int i = 0; i < n; i++) {
		double s = sin(w * i), c = cos(w * i);
		ss += s * s;
		sc += s * c;
		cc += c * c;
		xs += x[i] * s;
		xc += x[i] * c;
	}
	double det = ss * cc - sc * sc;
	double a = (xs * cc - xc * sc) / det, b = (xc * ss - xs * sc) / det;
	double res = 0;
	for (int i = 0; i < n; i++) {
		double e = x[i] - a * sin(w * i) - b * cos(w * i);
		res += e * e;
	}
	*level = hypot(a, b);
	*resid_db = 10 * log10(res / n + 1e-30) - 20 * log10(*level / sqrt(2));
}

// One tone through `in_rate` -> `out_rate`, fed in blocks of 1000 frames.
static void one_tone(int in_rate, int out_rate, double hz, bool check_resid, double resid_limit) {
	const int n_in = in_rate * SECONDS;
	int32_t *in = malloc(sizeof(int32_t) * (size_t)n_in * 2);
	int32_t *out = malloc(sizeof(int32_t) * ((size_t)n_in * 2 * 2 + 64));
	double *mono = malloc(sizeof(double) * (size_t)n_in * 2);
	resampler_t r;
	if (resample_init(&r, in_rate, out_rate, 2) != 0) {
		printf("FAIL no converter for %d -> %d\n", in_rate, out_rate);
		failures++;
		return;
	}
	for (int i = 0; i < n_in; i++) {
		int32_t v = (int32_t)lrint(AMP * sin(2 * M_PI * hz * i / in_rate) * 8388607.0) * 256;
		in[2 * i] = v;
		in[2 * i + 1] = -v;
	}
	int made = 0;
	for (int at = 0; at < n_in; at += 1000) {
		int n = at + 1000 > n_in ? n_in - at : 1000;
		made += resample_s32(&r, in + 2 * at, n, out + 2 * made);
	}
	// The filter's start-up and the tail left in it are not the steady state.
	int skip = out_rate / 50;
	int n = made - 2 * skip;
	for (int i = 0; i < n; i++) {
		mono[i] = out[2 * (i + skip)] / 2147483648.0;
	}
	double level, resid;
	tone_fit(mono, n, out_rate, hz, &level, &resid);
	double gain = 20 * log10(level / AMP);
	bool ok = fabs(gain) < 0.05 && (!check_resid || resid < resid_limit);
	printf("%s %5d -> %5d %5.0f Hz: gain %+.4f dB", ok ? "ok  " : "FAIL", in_rate, out_rate, hz, gain);
	if (check_resid) {
		printf(", everything else %6.1f dB", resid);
	}
	printf("\n");
	if (!ok) {
		failures++;
	}
	resample_close(&r);
	free(in);
	free(out);
	free(mono);
}

// The same noise through in one block and in blocks of 1 to 997 frames: the
// state that carries between calls must make the two identical.
static void blocks(int in_rate, int out_rate) {
	const int n_in = 20000;
	int32_t *in = malloc(sizeof(int32_t) * (size_t)n_in * 2);
	int32_t *whole = malloc(sizeof(int32_t) * (size_t)n_in * 4);
	int32_t *parts = malloc(sizeof(int32_t) * (size_t)n_in * 4);
	unsigned seed = 1;
	for (int i = 0; i < n_in * 2; i++) {
		seed = seed * 1103515245u + 12345u;
		in[i] = (int32_t)(seed & 0xffffff00u) / 4;
	}
	resampler_t a, b;
	resample_init(&a, in_rate, out_rate, 2);
	resample_init(&b, in_rate, out_rate, 2);
	int made_a = resample_s32(&a, in, n_in, whole);
	int made_b = 0;
	int step = 1;
	for (int at = 0; at < n_in; at += step, step = step % 997 + 7) {
		int n = at + step > n_in ? n_in - at : step;
		made_b += resample_s32(&b, in + 2 * at, n, parts + 2 * made_b);
	}
	bool ok = made_a == made_b && memcmp(whole, parts, sizeof(int32_t) * 2 * (size_t)made_a) == 0 &&
			  made_a <= resample_out_max(&a, n_in);
	printf("%s %5d -> %5d uneven blocks: %d frames, identical to one block\n", ok ? "ok  " : "FAIL", in_rate, out_rate,
		   made_b);
	if (!ok) {
		failures++;
	}
	resample_close(&a);
	resample_close(&b);
	free(in);
	free(whole);
	free(parts);
}

static void cost(int in_rate, int out_rate) {
	const int n_in = in_rate * 10;
	int32_t *in = calloc((size_t)n_in * 2, sizeof(int32_t));
	int32_t *out = malloc(sizeof(int32_t) * ((size_t)n_in * 2 * 2 + 64));
	for (int i = 0; i < n_in * 2; i++) {
		in[i] = (int32_t)(i * 2654435761u) & ~0xff;
	}
	resampler_t r;
	resample_init(&r, in_rate, out_rate, 2);
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	int made = 0;
	for (int at = 0; at < n_in; at += 4096) {
		int n = at + 4096 > n_in ? n_in - at : 4096;
		made += resample_s32(&r, in + 2 * at, n, out);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double ms = ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / 10.0;
	printf("     %5d -> %5d: %.2f ms of CPU per second of stereo here (%d taps x %d outputs x 2)\n", in_rate,
		   out_rate, ms, r.taps, made / 10);
	resample_close(&r);
	free(in);
	free(out);
}

int main(void) {
	const double tones[] = {100, 1000, 5000, 10000, 15000, 18000, 19000, 20000};
	for (size_t i = 0; i < sizeof(tones) / sizeof(tones[0]); i++) {
		// Up: the image of a tone above 16 kHz folds to 20-24 kHz and is
		// allowed to stay (resample.h), so the residue is checked below it.
		one_tone(44100, 48000, tones[i], tones[i] <= 16000, -85.0);
	}
	for (size_t i = 0; i < sizeof(tones) / sizeof(tones[0]); i++) {
		// Down: every image of the audible band lands in it, so all of them.
		one_tone(48000, 44100, tones[i], true, -88.0);
	}
	blocks(44100, 48000);
	blocks(48000, 44100);
	cost(44100, 48000);
	cost(48000, 44100);
	resampler_t none;
	bool refused = resample_init(&none, 32000, 48000, 2) != 0 && !resample_supported(96000, 44100);
	printf("%s other rates refused\n", refused ? "ok  " : "FAIL");
	failures += !refused;
	printf(failures ? "%d FAILED\n" : "all passed\n", failures);
	return failures != 0;
}

#include "resample.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "src/system/core/config.h"

// The prototype is one long lowpass at the upsampled rate (44100 x 160 =
// 7.056 MHz), cut by phase: phase p takes taps p, p + L, p + 2L, ... Stored per
// phase in the order the delay line is read (oldest sample first), so the inner
// loop is a straight dot product.
#define TABLE_LEN (RESAMPLE_L * RESAMPLE_TAPS)
#define CUTOFF_HZ 24000.0
#define KAISER_BETA 9.0
#define UP_RATE (44100.0 * RESAMPLE_L)

static int32_t *table; // [phase][tap], Q30

// Modified Bessel function of the first kind, order 0, by its series: the
// Kaiser window is built from it. Twenty-five terms are far past double
// precision for beta = 9.
static double bessel_i0(double x) {
	double sum = 1.0;
	double term = 1.0;
	for (int k = 1; k < 25; k++) {
		double f = x / (2.0 * k);
		term *= f * f;
		sum += term;
	}
	return sum;
}

static int build_table(void) {
	if (table) {
		return 0;
	}
	int32_t *t = malloc(sizeof(int32_t) * TABLE_LEN);
	double *h = malloc(sizeof(double) * TABLE_LEN);
	if (!t || !h) {
		free(t);
		free(h);
		return -1;
	}

	double centre = (TABLE_LEN - 1) / 2.0;
	double wc = 2.0 * CUTOFF_HZ / UP_RATE; // cut-off as a fraction of the upsampled Nyquist
	double i0_beta = bessel_i0(KAISER_BETA);
	for (int n = 0; n < TABLE_LEN; n++) {
		double x = n - centre;
		double sinc = x == 0.0 ? 1.0 : sin(M_PI * wc * x) / (M_PI * wc * x);
		double r = x / centre;
		double window = bessel_i0(KAISER_BETA * sqrt(1.0 - r * r)) / i0_beta;
		h[n] = RESAMPLE_L * wc * sinc * window;
	}

	// Each phase scaled to a gain of exactly 1 at DC. The phases differ from
	// one another by a few parts in a million otherwise, which is a faint
	// 6 kHz-periodic ripple on silence with an offset -- cheap to take away.
	for (int p = 0; p < RESAMPLE_L; p++) {
		double sum = 0.0;
		for (int k = 0; k < RESAMPLE_TAPS; k++) {
			sum += h[p + k * RESAMPLE_L];
		}
		for (int k = 0; k < RESAMPLE_TAPS; k++) {
			// Tap k multiplies x[i - k]; the delay line is read oldest first.
			double c = h[p + k * RESAMPLE_L] / sum;
			t[p * RESAMPLE_TAPS + (RESAMPLE_TAPS - 1 - k)] = (int32_t)lrint(c * (double)(1 << 30));
		}
	}
	free(h);
	table = t;
	return 0;
}

void resample_free(void) {
	free(table);
	table = NULL;
}

int resample_init(resampler_t *r, int channels) {
	if (!r || channels < 1 || channels > RESAMPLE_CHANNELS_MAX) {
		return -1;
	}
	if (build_table() != 0) {
		return -1;
	}
	r->channels = channels;
	resample_reset(r);
	return 0;
}

void resample_reset(resampler_t *r) {
	r->phase = 0;
	r->need_input = true;
	r->pos = 0;
	memset(r->line, 0, sizeof(r->line));
}

int resample_out_max(int in_frames) { return (int)(((long long)in_frames * RESAMPLE_L) / RESAMPLE_M) + 2; }

static inline int32_t saturate24(int64_t v) {
	if (v > 0x7fffff) {
		return 0x7fffff;
	}
	if (v < -0x800000) {
		return -0x800000;
	}
	return (int32_t)v;
}

int resample_s32(resampler_t *r, const int32_t *in, int in_frames, int32_t *out) {
	const int channels = r->channels;
	int used = 0;
	int made = 0;
	for (;;) {
		if (r->need_input) {
			if (used >= in_frames) {
				break;
			}
			// Twice, T apart, so the last T samples are always contiguous
			// from line[pos] whatever pos is.
			for (int c = 0; c < channels; c++) {
				int32_t s = in[used * channels + c] >> 8;
				r->line[c][r->pos] = s;
				r->line[c][r->pos + RESAMPLE_TAPS] = s;
			}
			r->pos = (r->pos + 1) % RESAMPLE_TAPS;
			used++;
			r->need_input = false;
		}

		const int32_t *coef = table + r->phase * RESAMPLE_TAPS;
		for (int c = 0; c < channels; c++) {
			const int32_t *x = &r->line[c][r->pos];
			int64_t acc = 1 << 29; // rounding
			for (int k = 0; k < RESAMPLE_TAPS; k++) {
				acc += (int64_t)coef[k] * x[k];
			}
			out[made * channels + c] = saturate24(acc >> 30) * 256;
		}
		made++;

		r->phase += RESAMPLE_M;
		if (r->phase >= RESAMPLE_L) {
			r->phase -= RESAMPLE_L;
			r->need_input = true;
		}
	}
	return made;
}

bool resample_quality_wanted(void) { return strcmp(config_get("bluetooth", "resampler", "quality"), "fast") != 0; }

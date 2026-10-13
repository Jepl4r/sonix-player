#include "resample.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "src/system/core/config.h"

// The prototype of each design is one long lowpass at the upsampled rate
// (7.056 MHz both ways), cut by phase: phase p takes taps p, p + L, p + 2L, ...
// Stored per phase in the order the delay line is read (oldest sample first),
// so the inner loop is a straight dot product.
#define KAISER_BETA 9.0

typedef struct {
	int in_rate, out_rate;
	int l, m;
	int taps;
	double cutoff_hz;
	int32_t *table; // [phase][tap], Q30; NULL until a stream needs it
	int users;
} design_t;

// See resample.h for why each is cut where it is.
static design_t designs[] = {
	{44100, 48000, 160, 147, 32, 24000.0, NULL, 0},
	{48000, 44100, 147, 160, 48, 22500.0, NULL, 0},
};
#define DESIGN_COUNT ((int)(sizeof(designs) / sizeof(designs[0])))

static pthread_mutex_t tables_lock = PTHREAD_MUTEX_INITIALIZER;

static design_t *design_for(int in_rate, int out_rate) {
	for (int i = 0; i < DESIGN_COUNT; i++) {
		if (designs[i].in_rate == in_rate && designs[i].out_rate == out_rate) {
			return &designs[i];
		}
	}
	return NULL;
}

bool resample_supported(int in_rate, int out_rate) { return design_for(in_rate, out_rate) != NULL; }

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

// Under tables_lock.
static int build_table(design_t *d) {
	if (d->table) {
		return 0;
	}
	const int len = d->l * d->taps;
	int32_t *t = malloc(sizeof(int32_t) * (size_t)len);
	double *h = malloc(sizeof(double) * (size_t)len);
	if (!t || !h) {
		free(t);
		free(h);
		return -1;
	}

	double centre = (len - 1) / 2.0;
	double up_rate = (double)d->in_rate * d->l;
	double wc = 2.0 * d->cutoff_hz / up_rate; // cut-off as a fraction of the upsampled Nyquist
	double i0_beta = bessel_i0(KAISER_BETA);
	for (int n = 0; n < len; n++) {
		double x = n - centre;
		double sinc = x == 0.0 ? 1.0 : sin(M_PI * wc * x) / (M_PI * wc * x);
		double r = x / centre;
		double window = bessel_i0(KAISER_BETA * sqrt(1.0 - r * r)) / i0_beta;
		h[n] = d->l * wc * sinc * window;
	}

	// Each phase scaled to a gain of exactly 1 at DC. The phases differ from
	// one another by a few parts in a million otherwise, which is a faint
	// periodic ripple on silence with an offset -- cheap to take away.
	for (int p = 0; p < d->l; p++) {
		double sum = 0.0;
		for (int k = 0; k < d->taps; k++) {
			sum += h[p + k * d->l];
		}
		for (int k = 0; k < d->taps; k++) {
			// Tap k multiplies x[i - k]; the delay line is read oldest first.
			double c = h[p + k * d->l] / sum;
			t[p * d->taps + (d->taps - 1 - k)] = (int32_t)lrint(c * (double)(1 << 30));
		}
	}
	free(h);
	d->table = t;
	return 0;
}

int resample_init(resampler_t *r, int in_rate, int out_rate, int channels) {
	if (!r) {
		return -1;
	}
	memset(r, 0, sizeof(*r));
	design_t *d = design_for(in_rate, out_rate);
	if (!d || channels < 1 || channels > RESAMPLE_CHANNELS_MAX) {
		return -1;
	}
	pthread_mutex_lock(&tables_lock);
	int err = build_table(d);
	if (err == 0) {
		d->users++;
	}
	pthread_mutex_unlock(&tables_lock);
	if (err != 0) {
		return -1;
	}
	r->channels = channels;
	r->l = d->l;
	r->m = d->m;
	r->taps = d->taps;
	r->coef = d->table;
	resample_reset(r);
	return 0;
}

void resample_close(resampler_t *r) {
	if (!r || !r->coef) {
		return;
	}
	pthread_mutex_lock(&tables_lock);
	for (int i = 0; i < DESIGN_COUNT; i++) {
		design_t *d = &designs[i];
		if (d->table == r->coef) {
			if (d->users > 0 && --d->users == 0) {
				free(d->table);
				d->table = NULL;
			}
			break;
		}
	}
	pthread_mutex_unlock(&tables_lock);
	r->coef = NULL;
}

void resample_reset(resampler_t *r) {
	r->phase = 0;
	r->need = 1;
	r->pos = 0;
	memset(r->line, 0, sizeof(r->line));
}

int resample_out_max(const resampler_t *r, int in_frames) {
	return (int)(((long long)in_frames * r->l) / r->m) + 2;
}

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
	const int taps = r->taps;
	int used = 0;
	int made = 0;
	for (;;) {
		// One input sample per output going up, one or two going down.
		while (r->need > 0) {
			if (used >= in_frames) {
				return made;
			}
			// Twice, `taps` apart, so the last `taps` samples are always
			// contiguous from line[pos] whatever pos is.
			for (int c = 0; c < channels; c++) {
				int32_t s = in[used * channels + c] >> 8;
				r->line[c][r->pos] = s;
				r->line[c][r->pos + taps] = s;
			}
			r->pos = (r->pos + 1 == taps) ? 0 : r->pos + 1;
			used++;
			r->need--;
		}

		const int32_t *coef = r->coef + r->phase * taps;
		for (int c = 0; c < channels; c++) {
			const int32_t *x = &r->line[c][r->pos];
			int64_t acc = 1 << 29; // rounding
			for (int k = 0; k < taps; k++) {
				acc += (int64_t)coef[k] * x[k];
			}
			out[made * channels + c] = saturate24(acc >> 30) * 256;
		}
		made++;

		r->phase += r->m;
		while (r->phase >= r->l) {
			r->phase -= r->l;
			r->need++;
		}
	}
}

bool resample_quality_wanted(void) { return strcmp(config_get("bluetooth", "resampler", "quality"), "fast") != 0; }

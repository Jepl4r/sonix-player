#include "decimate.h"

#include <string.h>

// Equiripple half-band designs (Parks-McClellan, then the even offsets set to
// exactly zero and the centre trimmed so the taps sum to 1.0 in Q30). Only the
// odd offsets from the centre are stored; the filter is symmetric.
//
// WIDE, 23 taps: passband 0-0.15 of the input rate, stopband from 0.35,
// 82 dB. Every stage but the last: its transition band is wide because the
// stage after it removes the rest.
static const int32_t WIDE[] = {
	334050879, -92446195, 37693894, -14460381, 4416382, -836730,
};
#define WIDE_TAPS 23
#define WIDE_CENTRE 536906126

// NARROW, 47 taps: passband 0-0.2, stopband from 0.3, 81 dB. The last stage,
// whose output is the rate the sink plays.
static const int32_t NARROW[] = {
	339822367, -108088562, 59000622, -36487509, 23314909, -14805048,
	9125700,   -5355264,   2929807,	 -1451240,	618994,	   -212298,
};
#define NARROW_TAPS 47
#define NARROW_CENTRE 536916868

int decimate_factor_for(int rate, int sink_rate) {
	int factor = 1;
	while (factor < DECIMATE_MAX && rate % (factor * 2) == 0) {
		int next = rate / (factor * 2);
		if (next < 44100 || (long long)next * 10 < (long long)sink_rate * 9) {
			break;
		}
		factor *= 2;
	}
	return factor;
}

static void stage_setup(halfband_t *s, const int32_t *coef, int taps, int32_t centre) {
	s->coef = coef;
	s->taps = taps;
	s->centre = centre;
}

int decimate_init(decimator_t *d, int factor, int channels) {
	memset(d, 0, sizeof(*d));
	if (channels < 1 || channels > DECIMATE_CHANNELS_MAX) {
		return -1;
	}
	int stages = factor == 2 ? 1 : factor == 4 ? 2 : factor == 8 ? 3 : 0;
	if (stages == 0) {
		return -1;
	}
	d->factor = factor;
	d->channels = channels;
	d->stages = stages;
	for (int i = 0; i < stages - 1; i++) {
		stage_setup(&d->stage[i], WIDE, WIDE_TAPS, WIDE_CENTRE);
	}
	stage_setup(&d->stage[stages - 1], NARROW, NARROW_TAPS, NARROW_CENTRE);
	return 0;
}

void decimate_reset(decimator_t *d) {
	for (int i = 0; i < d->stages; i++) {
		halfband_t *s = &d->stage[i];
		s->pos = 0;
		s->phase = 0;
		memset(s->line, 0, sizeof(s->line));
	}
}

// The delay lines hold samples shifted down by SAMPLE_SHIFT: the decoder's S32
// is 24 bits left-justified, so nothing is lost, and the two samples a
// coefficient multiplies can be added first without overflowing.
#define SAMPLE_SHIFT 8
#define SAMPLE_MAX ((1 << (31 - SAMPLE_SHIFT)) - 1)
#define SAMPLE_MIN (-(1 << (31 - SAMPLE_SHIFT)))

static inline int32_t clamp_sample(int64_t v) {
	if (v > SAMPLE_MAX) {
		return SAMPLE_MAX;
	}
	if (v < SAMPLE_MIN) {
		return SAMPLE_MIN;
	}
	return (int32_t)v;
}

// One stage over an interleaved block of shifted samples, in place. Each
// delay line is stored twice over so the window [pos, pos + taps) is always
// contiguous, oldest sample first. Writes land at or behind the frame being
// read.
static int stage_run(halfband_t *s, int channels, int32_t *buf, int frames) {
	const int taps = s->taps;
	const int half = (taps - 1) / 2;
	const int pairs = (taps + 1) / 4;
	const int32_t *coef = s->coef;
	int out = 0;
	for (int i = 0; i < frames; i++) {
		const int32_t *in = buf + i * channels;
		for (int c = 0; c < channels; c++) {
			s->line[c][s->pos] = in[c];
			s->line[c][s->pos + taps] = in[c];
		}
		s->pos = (s->pos + 1 == taps) ? 0 : s->pos + 1;
		s->phase ^= 1;
		if (s->phase) {
			continue;
		}
		int32_t *dst = buf + out * channels;
		for (int c = 0; c < channels; c++) {
			const int32_t *mid = &s->line[c][s->pos + half];
			int64_t acc = (int64_t)mid[0] * s->centre;
			for (int k = 0; k < pairs; k++) {
				int off = 2 * k + 1;
				acc += (int64_t)(mid[-off] + mid[off]) * coef[k];
			}
			dst[c] = clamp_sample((acc + (1 << 29)) >> 30);
		}
		out++;
	}
	return out;
}

int decimate_s32(decimator_t *d, int32_t *buf, int frames) {
	int samples = frames * d->channels;
	for (int i = 0; i < samples; i++) {
		buf[i] >>= SAMPLE_SHIFT;
	}
	for (int i = 0; i < d->stages && frames > 0; i++) {
		frames = stage_run(&d->stage[i], d->channels, buf, frames);
	}
	samples = frames * d->channels;
	for (int i = 0; i < samples; i++) {
		buf[i] = (int32_t)((uint32_t)buf[i] << SAMPLE_SHIFT);
	}
	return frames;
}

// ---------------------------------------------------------------------------
// Interpolation by 2
//
// Zeros between the input samples, then NARROW at the output rate with a gain
// of 2. Of the 47 taps only the centre lands on an input sample at an even
// output position, and it is exactly a half: those outputs are the input
// itself, twelve frames late. At an odd position only the odd taps land on
// input samples, as twelve symmetric pairs around the point halfway between
// two of them.
// ---------------------------------------------------------------------------

#define INTERP2_PAIRS 12
#define INTERP2_DELAY (INTERP2_SPAN / 2)

int interp2_init(interp2_t *p, int channels) {
	memset(p, 0, sizeof(*p));
	if (channels < 1 || channels > DECIMATE_CHANNELS_MAX) {
		return -1;
	}
	p->channels = channels;
	return 0;
}

void interp2_reset(interp2_t *p) {
	p->pos = 0;
	memset(p->line, 0, sizeof(p->line));
}

int interp2_s32(interp2_t *p, const int32_t *in, int frames, int32_t *out) {
	const int channels = p->channels;
	for (int i = 0; i < frames; i++) {
		for (int c = 0; c < channels; c++) {
			int32_t s = in[i * channels + c] >> SAMPLE_SHIFT;
			p->line[c][p->pos] = s;
			p->line[c][p->pos + INTERP2_SPAN] = s;
		}
		p->pos = (p->pos + 1 == INTERP2_SPAN) ? 0 : p->pos + 1;
		for (int c = 0; c < channels; c++) {
			// The last INTERP2_SPAN samples, oldest first: x[n-23] .. x[n].
			const int32_t *x = &p->line[c][p->pos];
			// Halfway between x[n-12] and x[n-11]: coefficient k pairs
			// x[n-12-k] with x[n-11+k].
			int64_t acc = 0;
			for (int k = 0; k < INTERP2_PAIRS; k++) {
				acc += (int64_t)(x[INTERP2_DELAY - 1 - k] + x[INTERP2_DELAY + k]) * NARROW[k];
			}
			// Gain 2: shifted by 29, not 30.
			int32_t mid = clamp_sample((acc + (1 << 28)) >> 29);
			int32_t *dst = out + (size_t)(2 * i) * (size_t)channels;
			dst[c] = (int32_t)((uint32_t)x[INTERP2_DELAY - 1] << SAMPLE_SHIFT);
			dst[channels + c] = (int32_t)((uint32_t)mid << SAMPLE_SHIFT);
		}
	}
	return 2 * frames;
}

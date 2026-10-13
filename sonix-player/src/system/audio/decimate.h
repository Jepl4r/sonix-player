#ifndef DECIMATE_H
#define DECIMATE_H

#include <stdint.h>

// Integer downsampling by 2, 4 or 8 on interleaved S32 frames, as a cascade of
// half-band FIR stages. Used ahead of the effects chain when the output is a
// Bluetooth sink running at a fraction of the track's rate, so the chain runs
// at the sink's rate and alsa's linear converter never sees a hi-res stream.
//
// Passband flat to 0.2 of the input rate of the last stage (19.2 kHz from
// 96 kHz, 17.6 kHz from 88.2 kHz); every stage rejects its alias band by more
// than 80 dB. Works at 24 bits: the low 8 bits of each S32 sample are dropped
// on the way in and come back as zero.

#define DECIMATE_MAX 8
#define DECIMATE_CHANNELS_MAX 2

// Taps of the longest stage.
#define DECIMATE_TAPS_MAX 47

typedef struct {
	const int32_t *coef; // Q30, the odd offsets 1, 3, 5, ... from the centre
	int32_t centre;		 // Q30
	int taps;
	int pos;
	int phase;
	int32_t line[DECIMATE_CHANNELS_MAX][2 * DECIMATE_TAPS_MAX];
} halfband_t;

typedef struct {
	int factor;
	int channels;
	int stages;
	halfband_t stage[3];
} decimator_t;

// The largest power of two, up to DECIMATE_MAX, that divides `rate` and leaves
// a rate of at least 44100 Hz and at least nine tenths of `sink_rate`. 1 when
// no reduction applies.
int decimate_factor_for(int rate, int sink_rate);

// `factor` 1, 2, 4 or 8; `channels` 1 or 2. Returns 0 on success.
int decimate_init(decimator_t *d, int factor, int channels);

// Clears the delay lines: after a seek, or a pause that rewound the decoder.
void decimate_reset(decimator_t *d);

// Filters `frames` frames in place and returns how many come out. The count
// carries across calls: a block with an odd number of frames is fine, and
// the result can be 0 for a very short block.
int decimate_s32(decimator_t *d, int32_t *buf, int frames);

// ---------------------------------------------------------------------------
// The other way: upsampling by 2, for a Bluetooth link at twice the stream's
// rate (LDAC on "96 kHz" with a 48 kHz stream). The same NARROW half-band as
// the decimator's last stage, run as an interpolator: every input frame comes
// out twice, once as itself and once as the filtered point halfway to the
// next one. Passband flat to 0.2 of the OUTPUT rate (19.2 kHz at 96 kHz),
// images of the stream rejected by more than 80 dB from 0.3 of it. Twelve
// multiply-adds per input sample, and a delay of twelve input frames.
//
// Interleaved S32 at 24-bit precision, like the decimator.
// ---------------------------------------------------------------------------

#define INTERP2_SPAN 24 // input frames the filter looks at

typedef struct {
	int channels;
	int pos;
	int32_t line[DECIMATE_CHANNELS_MAX][2 * INTERP2_SPAN];
} interp2_t;

// `channels` 1 or 2. Returns 0 on success.
int interp2_init(interp2_t *p, int channels);

// Clears the delay line: after a seek, or a pause that rewound the decoder.
void interp2_reset(interp2_t *p);

// `frames` frames of `in` into 2 * `frames` frames of `out`, which must not
// overlap it. Returns the number of output frames.
int interp2_s32(interp2_t *p, const int32_t *in, int frames, int32_t *out);

#endif

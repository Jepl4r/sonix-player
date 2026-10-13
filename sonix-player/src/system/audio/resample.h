#ifndef RESAMPLE_H
#define RESAMPLE_H

#include <stdbool.h>
#include <stdint.h>

// 44.1 kHz to 48 kHz, as a polyphase FIR: up by 160, down by 147.
//
// Why it exists: a CD-rate track going to headphones that run at 48 kHz is
// converted today by alsa-lib's plug, and the plug on this firmware has no
// rate_converter plugin -- only its built-in linear interpolator, whose images
// of the music come back into the audible band at around -30 to -40 dB (a
// 10 kHz cymbal leaves a 13.9 kHz ghost). libasound_module_rate_speexrate
// would fix it from the outside, but needs speexdsp cross-built. This does the
// same job in the player, ahead of the bluealsa PCM, so the plug sees matching
// rates and passes the stream through.
//
// The filter. Images of the input sit at 44.1 kHz - f; once the output is
// sampled at 48 kHz, anything above 24 kHz folds back to 48 kHz - (44.1 - f) =
// 3.9 kHz + f. So the images that would land in the audible band (below 20 kHz)
// are those above 28 kHz, and the filter has from 20 kHz to 28 kHz to get from
// pass to stop -- a gentle transition, which is what makes 32 taps per phase
// enough. Kaiser-windowed sinc, cut-off 24 kHz, beta 9: about 0.1 dB of droop
// at 20 kHz and close to 90 dB of rejection of every image that can be heard
// (resample_test.c measures both). Images between 22.05 and 28 kHz are
// attenuated less and fold to 20-24 kHz, where nobody hears them.
//
// Cost: 32 multiply-accumulates per output sample and channel, 3.1 million a
// second for stereo, in 64-bit integer arithmetic (one `madd` each on MIPS32).
// The table is 160 x 32 Q30 coefficients, 20 KB, allocated on first use and
// freed by resample_free(): nothing is spent while the option is off.
//
// Samples are interleaved S32 at 24-bit precision, like decimate.h: the low 8
// bits are dropped on the way in and come back as zero.
//
// Used by audio.c for a 44.1 kHz stream going to a Bluetooth link at 48 kHz
// (see bt_resample_t there), under the [bluetooth] resampler setting below.
// Any thread, one resampler per stream.

#define RESAMPLE_L 160
#define RESAMPLE_M 147
#define RESAMPLE_TAPS 32 // per phase
#define RESAMPLE_CHANNELS_MAX 2

typedef struct {
	int channels;
	int phase;		 // position of the next output between two inputs, in 1/L of an input sample
	bool need_input; // the next output needs one more input sample first
	int pos;		 // write position in the delay lines
	int32_t line[RESAMPLE_CHANNELS_MAX][2 * RESAMPLE_TAPS];
} resampler_t;

// Builds the shared coefficient table if it is not there yet. Returns 0 on
// success, -1 when the table could not be allocated or `channels` is not 1 or 2.
int resample_init(resampler_t *r, int channels);

// Clears the delay lines: after a seek, or a new track.
void resample_reset(resampler_t *r);

// The most frames resample_s32() can produce from `in_frames` input frames.
int resample_out_max(int in_frames);

// Converts `in_frames` frames from `in` into `out` and returns how many came
// out. Every input frame is consumed; the fraction of a frame left over carries
// to the next call. `out` must hold resample_out_max(in_frames) frames and must
// not overlap `in`.
int resample_s32(resampler_t *r, const int32_t *in, int in_frames, int32_t *out);

// Gives back the coefficient table. Only when no resampler is in use.
void resample_free(void);

// [bluetooth] resampler = quality | fast. "quality" (the default, and anything
// that is not "fast") is this filter; "fast" leaves 44.1 to 48 kHz to
// alsa-lib's linear converter, as before 1.2.4.
bool resample_quality_wanted(void);

#endif

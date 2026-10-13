#ifndef RESAMPLE_H
#define RESAMPLE_H

#include <stdbool.h>
#include <stdint.h>

// Between 44.1 kHz and 48 kHz, either way, as a polyphase FIR: up by L, down
// by M (160/147 one way, 147/160 the other).
//
// Why it exists: a stream going to an output that runs at the other family's
// rate -- headphones held at 48 kHz, a link forced to 44.1 kHz, a USB DAC that
// lacks the track's rate -- was converted by alsa-lib's plug, and the plug on
// this firmware has no rate_converter plugin, only its built-in linear
// interpolator: the top octave droops (6 dB at 20 kHz) and images of the music
// come back into the audible band at around -30 to -40 dB (a 10 kHz cymbal
// leaves a 13.9 kHz ghost). libasound_module_rate_speexrate would fix it from
// the outside, but needs speexdsp cross-built. This does the same job in the
// player, so the plug sees matching rates and passes the stream through.
//
// The filters. Kaiser-windowed sincs at the upsampled rate (7.056 MHz both
// ways), beta 9, cut by phase.
//
//   44.1 -> 48 kHz   32 taps per phase, cut-off 24 kHz. Images of the input
//                    sit at 44.1 kHz - f; at 48 kHz anything above 24 kHz
//                    folds back to 3.9 kHz + f, so the images that would be
//                    heard are those above 28 kHz, and the filter has from 20
//                    to 28 kHz to get from pass to stop. About 0.1 dB of droop
//                    at 20 kHz and close to 90 dB of rejection from 28 kHz.
//                    Images between 22.05 and 28 kHz are attenuated less and
//                    fold to 20-24 kHz, where nobody hears them.
//
//   48 -> 44.1 kHz   48 taps per phase, cut-off 22.5 kHz. The image of an
//                    input tone at f lands at 48 kHz - f and, sampled at
//                    44.1 kHz, comes back at f - 3.9 kHz: the image of a 10 kHz
//                    tone would be heard at 6.1 kHz, so everything from 28 kHz
//                    up (the images of the whole audible band) has to go, and
//                    goes by 96 dB or more. What is left is the image of the
//                    stream's own top, 20 to 24 kHz, which falls to 16-20 kHz:
//                    62 dB down at 25 kHz (19.1 kHz once folded) and 90 dB from
//                    26 kHz (18.1). Music at 22 kHz is itself 50 to 80 dB down,
//                    so what reaches the ear from there is under the 16-bit
//                    floor. Flat within 0.02 dB to 20 kHz. The 48 taps are what
//                    the shorter transition costs: 32 would leave the images of
//                    the audible band only 67 dB down.
//
// Cost: taps x output rate multiply-accumulates per channel, in 64-bit integer
// arithmetic (one `madd` each on MIPS32): 3.1 million a second for stereo at
// 48 kHz out, 4.2 million at 44.1 kHz out. Each table (20 or 28 KB of Q30
// coefficients) is built on first use, shared by every stream converting the
// same way, and freed when the last of them closes: nothing is spent while no
// stream needs it.
//
// Samples are interleaved S32 at 24-bit precision, like decimate.h: the low 8
// bits are dropped on the way in and come back as zero.
//
// Used by audio.c wherever a stream at one of the two rates has to reach a
// device at the other (see bt_resample_t there), under the [bluetooth]
// resampler setting below. Any thread, one resampler per stream; the tables
// are shared under a lock taken only by init and close.

#define RESAMPLE_TAPS_MAX 48 // per phase
#define RESAMPLE_CHANNELS_MAX 2

typedef struct {
	int channels;
	int l, m;			 // up by l, down by m
	int taps;			 // per phase
	const int32_t *coef; // [phase][tap], Q30, shared; NULL when not open
	int phase;			 // position of the next output between two inputs, in 1/l of an input sample
	int need;			 // input samples to take in before the next output
	int pos;			 // write position in the delay lines
	int32_t line[RESAMPLE_CHANNELS_MAX][2 * RESAMPLE_TAPS_MAX];
} resampler_t;

// Whether there is a filter for `in_rate` to `out_rate`: 44100 to 48000 and
// back.
bool resample_supported(int in_rate, int out_rate);

// Takes a hold on the table for the conversion, building it if it is not
// there yet. Returns 0 on success, -1 when the pair is not supported, the table
// could not be allocated, or `channels` is not 1 or 2.
int resample_init(resampler_t *r, int in_rate, int out_rate, int channels);

// Clears the delay lines: after a seek, or a new track.
void resample_reset(resampler_t *r);

// The most frames resample_s32() can produce from `in_frames` input frames.
int resample_out_max(const resampler_t *r, int in_frames);

// Converts `in_frames` frames from `in` into `out` and returns how many came
// out. Every input frame is consumed; the fraction of a frame left over carries
// to the next call. `out` must hold resample_out_max() frames and must not
// overlap `in`.
int resample_s32(resampler_t *r, const int32_t *in, int in_frames, int32_t *out);

// Gives the hold on the table back; the last stream to close frees it. Safe on
// a resampler that was never opened (zeroed) or is already closed.
void resample_close(resampler_t *r);

// [bluetooth] resampler = quality | fast. "quality" (the default, and anything
// that is not "fast") is these filters; "fast" leaves every conversion between
// the two families to alsa-lib's linear converter, as before 1.2.4. The key
// covers USB DACs and the external sources too.
bool resample_quality_wanted(void);

#endif

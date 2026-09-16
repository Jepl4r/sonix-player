#ifndef SPEED_H
#define SPEED_H

#include <stdbool.h>
#include <stdint.h>

// Playing faster or slower without shifting the pitch.
//
// The obvious way to play a book at 1.5x -- feed the samples out 1.5 times as
// fast -- raises the pitch by a fifth. What is wanted is the same voice in
// fewer seconds.
//
// So this is a time-domain stretcher (WSOLA, the same shape SoundTouch uses):
// the stream is cut into overlapping windows and the windows are laid back
// down closer together or further apart. Where exactly the next window is
// taken from is not fixed -- it is searched for, within a few milliseconds,
// so that it lines up with the waveform already written. That search is the
// whole trick: get it wrong and every window boundary clicks.
//
// The search runs first on a decimated copy and is then refined, which keeps
// it to well under a million multiply-accumulates a second. At a factor of
// exactly 1.0 the whole thing is bypassed, so normal playback pays nothing.

typedef struct speed speed_t;

// 16-bit interleaved, 1 or 2 channels. NULL if the rate or channel count is
// out of range or memory ran out.
speed_t *speed_open(int channels, int sample_rate);
void speed_close(speed_t *s);

// 1.0 is untouched. Clamped to [0.25, 4.0]; anything outside is nonsense for
// speech. Changing it takes effect on the next block, without a click.
void speed_set_factor(speed_t *s, double factor);
double speed_factor(const speed_t *s);

// Throws away what is buffered: after a seek, the samples held back belong to
// a part of the book that is no longer playing.
void speed_reset(speed_t *s);

// Pulls exactly `want` output frames (fewer only at the end of the stream).
//
// Input is taken through `fill`, which must behave like a decoder read: write
// up to `frames` interleaved frames into `dst` and return how many it wrote,
// 0 at the end. `input_frames` receives how many input frames were consumed to
// make this output -- which is what the position in the book advances by, and
// is not the same as the number returned.
int speed_pull(speed_t *s, short *out, int want, int (*fill)(void *user, short *dst, int frames), void *user,
			   uint64_t *input_frames);

#endif /* SPEED_H */

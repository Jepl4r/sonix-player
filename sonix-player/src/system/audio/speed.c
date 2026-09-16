#include "speed.h"

#include <stdlib.h>
#include <string.h>

// See speed.h for what this is and why it is not a resampler.
//
// The window sizes are the ones that work for speech. A forty-millisecond
// sequence is long enough that the search has something to lock onto and short
// enough that a stretched syllable does not smear; the eight-millisecond
// overlap is about one pitch period of a low male voice, which is what keeps
// the crossfade from beating.

#define SEQUENCE_MS 40
#define SEEK_MS 15
#define OVERLAP_MS 8

// The search is done on every fourth sample first, then refined either side of
// the winner. Full resolution everywhere would cost roughly fifteen times as
// much for an answer that differs by a sample or two.
#define COARSE_STEP 4
#define REFINE_RANGE 4

#define FACTOR_MIN 0.25
#define FACTOR_MAX 4.0

struct speed {
	int channels;
	int rate;

	int sequence; // frames laid down per iteration, before overlap
	int seek;	  // how far the search may wander, in frames
	int overlap;  // crossfade length, in frames

	double factor;
	double skip_fraction; // the fractional part of the input step, carried over

	short *input;	 // interleaved, `input_frames` valid
	int input_frames;
	int input_capacity;

	short *mid; // the tail of what was last written, `overlap` frames

	short *output; // finished frames waiting to be handed out
	int output_frames;
	int output_read;
	int output_capacity;

	bool primed; // `mid` holds something real
};

static int clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

speed_t *speed_open(int channels, int sample_rate) {
	if (channels < 1 || channels > 2 || sample_rate < 4000 || sample_rate > 384000) {
		return NULL;
	}

	speed_t *s = calloc(1, sizeof(*s));
	if (!s) {
		return NULL;
	}

	s->channels = channels;
	s->rate = sample_rate;
	s->factor = 1.0;

	s->sequence = clamp_int(SEQUENCE_MS * sample_rate / 1000, 64, 8192);
	s->seek = clamp_int(SEEK_MS * sample_rate / 1000, 16, 4096);
	s->overlap = clamp_int(OVERLAP_MS * sample_rate / 1000, 16, s->sequence / 2);

	// Room for one full iteration's worth of lookahead plus a spare window, so
	// a short read from the decoder never forces a partial iteration.
	s->input_capacity = s->sequence + s->seek + s->sequence;
	s->output_capacity = s->sequence * 2;

	s->input = calloc((size_t)s->input_capacity * channels, sizeof(short));
	s->output = calloc((size_t)s->output_capacity * channels, sizeof(short));
	s->mid = calloc((size_t)s->overlap * channels, sizeof(short));

	if (!s->input || !s->output || !s->mid) {
		speed_close(s);
		return NULL;
	}
	return s;
}

void speed_close(speed_t *s) {
	if (!s) {
		return;
	}
	free(s->input);
	free(s->output);
	free(s->mid);
	free(s);
}

void speed_set_factor(speed_t *s, double factor) {
	if (!s) {
		return;
	}
	if (factor < FACTOR_MIN) {
		factor = FACTOR_MIN;
	}
	if (factor > FACTOR_MAX) {
		factor = FACTOR_MAX;
	}
	s->factor = factor;
}

double speed_factor(const speed_t *s) { return s ? s->factor : 1.0; }

void speed_reset(speed_t *s) {
	if (!s) {
		return;
	}
	s->input_frames = 0;
	s->output_frames = 0;
	s->output_read = 0;
	s->skip_fraction = 0;
	s->primed = false;
	memset(s->mid, 0, (size_t)s->overlap * s->channels * sizeof(short));
}

// ---------------------------------------------------------------------------
// the search
// ---------------------------------------------------------------------------

// How well the input at `offset` continues what `mid` ended with. Normalised,
// because a loud stretch would otherwise always beat a quiet one regardless of
// how badly it lines up.
//
// `step` walks both windows in the same stride: 4 for the coarse pass, 1 for
// the refinement.
static double correlation(const speed_t *s, int offset, int step) {
	const short *a = s->mid;
	const short *b = s->input + (size_t)offset * s->channels;
	int taps = s->overlap * s->channels;
	int stride = step * s->channels;

	long long cross = 0, energy = 0;
	for (int i = 0; i < taps; i += stride) {
		for (int c = 0; c < s->channels; c++) {
			int x = a[i + c];
			int y = b[i + c];
			cross += (long long)x * y;
			energy += (long long)y * y;
		}
	}

	if (energy <= 0) {
		return cross > 0 ? 0 : -1e18;
	}
	// cross / sqrt(energy), without the square root: comparing c^2/e keeps the
	// ordering for positive c, and a negative correlation is never the answer.
	if (cross <= 0) {
		return -1e18;
	}
	return (double)cross * (double)cross / (double)energy;
}

static int best_offset(const speed_t *s) {
	if (!s->primed) {
		return 0; // nothing to line up with yet
	}

	int limit = s->seek;
	if (limit + s->sequence > s->input_frames) {
		limit = s->input_frames - s->sequence;
	}
	if (limit <= 0) {
		return 0;
	}

	int best = 0;
	double best_score = -1e18;
	for (int offset = 0; offset < limit; offset += COARSE_STEP) {
		double score = correlation(s, offset, COARSE_STEP);
		if (score > best_score) {
			best_score = score;
			best = offset;
		}
	}

	int from = best - REFINE_RANGE;
	int to = best + REFINE_RANGE;
	if (from < 0) {
		from = 0;
	}
	if (to >= limit) {
		to = limit - 1;
	}

	best_score = -1e18;
	int refined = best;
	for (int offset = from; offset <= to; offset++) {
		double score = correlation(s, offset, 1);
		if (score > best_score) {
			best_score = score;
			refined = offset;
		}
	}
	return refined;
}

// Linear crossfade from what was last written into what comes next.
static void blend(const speed_t *s, short *dst, const short *next) {
	int n = s->overlap;
	for (int i = 0; i < n; i++) {
		int fade = (i << 12) / n; // 0 .. 4096
		for (int c = 0; c < s->channels; c++) {
			int a = s->mid[i * s->channels + c];
			int b = next[i * s->channels + c];
			dst[i * s->channels + c] = (short)((a * (4096 - fade) + b * fade) >> 12);
		}
	}
}

// ---------------------------------------------------------------------------
// the engine
// ---------------------------------------------------------------------------

// Runs one window: writes `sequence - overlap` frames into the output buffer
// and consumes its share of the input. Returns how many input frames went.
static int iterate(speed_t *s) {
	int offset = best_offset(s);

	short *dst = s->output + (size_t)s->output_frames * s->channels;
	const short *src = s->input + (size_t)offset * s->channels;

	if (s->primed) {
		blend(s, dst, src);
		dst += (size_t)s->overlap * s->channels;
		s->output_frames += s->overlap;

		int straight = s->sequence - 2 * s->overlap;
		if (straight > 0) {
			memcpy(dst, src + (size_t)s->overlap * s->channels, (size_t)straight * s->channels * sizeof(short));
			s->output_frames += straight;
		}
	} else {
		// The first window has nothing to crossfade with, so it is simply
		// written out and the machine is primed from its tail.
		int straight = s->sequence - s->overlap;
		memcpy(dst, src, (size_t)straight * s->channels * sizeof(short));
		s->output_frames += straight;
		s->primed = true;
	}

	// Keep the tail: it is what the next window has to line up with.
	memcpy(s->mid, s->input + (size_t)(offset + s->sequence - s->overlap) * s->channels,
		   (size_t)s->overlap * s->channels * sizeof(short));

	// And step the input along by the factor. The fractional part is carried
	// rather than rounded away, or 1.5x would drift into 1.49x over an hour.
	double nominal = s->factor * (double)(s->sequence - s->overlap);
	s->skip_fraction += nominal;
	int skip = (int)s->skip_fraction;
	s->skip_fraction -= skip;

	if (skip > s->input_frames) {
		skip = s->input_frames;
	}
	if (skip > 0) {
		memmove(s->input, s->input + (size_t)skip * s->channels,
				(size_t)(s->input_frames - skip) * s->channels * sizeof(short));
		s->input_frames -= skip;
	}
	return skip;
}

int speed_pull(speed_t *s, short *out, int want, int (*fill)(void *user, short *dst, int frames), void *user,
			   uint64_t *input_frames) {
	if (input_frames) {
		*input_frames = 0;
	}
	if (!s || !out || want <= 0 || !fill) {
		return 0;
	}

	// At 1.0 there is nothing to do, and doing nothing must also mean no
	// buffering: an untouched track should not gain a window of latency.
	if (s->factor > 0.999 && s->factor < 1.001 && s->output_read >= s->output_frames) {
		int got = fill(user, out, want);
		if (input_frames) {
			*input_frames = (uint64_t)(got > 0 ? got : 0);
		}
		return got > 0 ? got : 0;
	}

	int written = 0;
	uint64_t consumed = 0;

	while (written < want) {
		// Hand out whatever is already finished.
		if (s->output_read < s->output_frames) {
			int available = s->output_frames - s->output_read;
			int take = want - written;
			if (take > available) {
				take = available;
			}
			memcpy(out + (size_t)written * s->channels, s->output + (size_t)s->output_read * s->channels,
				   (size_t)take * s->channels * sizeof(short));
			s->output_read += take;
			written += take;
			continue;
		}

		s->output_frames = 0;
		s->output_read = 0;

		// Top the input up to a full window plus its search room.
		int needed = s->sequence + s->seek;
		bool eof = false;
		while (s->input_frames < needed) {
			int room = s->input_capacity - s->input_frames;
			int got = fill(user, s->input + (size_t)s->input_frames * s->channels, room);
			if (got <= 0) {
				eof = true;
				break;
			}
			s->input_frames += got;
		}

		if (s->input_frames < s->sequence) {
			// The stream has ended. What is left is shorter than a window, so
			// there is nothing sensible to stretch: it goes out as it is.
			if (eof && s->input_frames > 0) {
				// ...but it still has to be crossfaded onto the tail already
				// written, or the last moment of every chapter ends in a click.
				if (s->primed) {
					int n = s->overlap < s->input_frames ? s->overlap : s->input_frames;
					for (int i = 0; i < n; i++) {
						int fade = (i << 12) / n;
						for (int c = 0; c < s->channels; c++) {
							int a = s->mid[i * s->channels + c];
							int b = s->input[i * s->channels + c];
							s->input[i * s->channels + c] = (short)((a * (4096 - fade) + b * fade) >> 12);
						}
					}
					s->primed = false; // done only once, on the way out
				}

				int take = s->input_frames;
				if (take > want - written) {
					take = want - written;
				}
				memcpy(out + (size_t)written * s->channels, s->input, (size_t)take * s->channels * sizeof(short));
				written += take;
				consumed += (uint64_t)take;
				memmove(s->input, s->input + (size_t)take * s->channels,
						(size_t)(s->input_frames - take) * s->channels * sizeof(short));
				s->input_frames -= take;
				continue;
			}
			break;
		}

		consumed += (uint64_t)iterate(s);
	}

	if (input_frames) {
		*input_frames = consumed;
	}
	return written;
}

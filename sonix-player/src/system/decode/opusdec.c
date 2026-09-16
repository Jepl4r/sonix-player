#include "opusdec.h"

// opusfile.h itself includes <opus_multistream.h> unprefixed, so the opus/
// directory must be on the -I path: on the host pkg-config handles it, on the
// target the Makefile points at $(OPUS_PREFIX)/include/opus.
#include <opusfile.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

// A gap in the stream (OP_HOLE) is not an error: it is a missing or corrupt
// piece, and opusfile expects the read to be retried. But a broken file
// returning gaps forever would hang the player inside a while loop, so they are
// counted.
#define MAX_HOLES 64

struct opusdec {
	OggOpusFile *of;
	int channels;
	bool force_stereo; // more than two channels: take the stereo downmix
	uint64_t frames;
};

// opusfile has two reads: op_read() gives the channels the file really has, and
// op_read_stereo() always gives two, mixing as needed. Above two channels the
// latter is used: the rest of the player is built on mono and stereo, and the
// library's downmix beats a hand-rolled one.
static int channels_of(const OggOpusFile *of) {
	int ch = op_channel_count(of, -1);
	return (ch >= 1 && ch <= 8) ? ch : 0;
}

opusdec_t *opusdec_open(const char *filepath) {
	int err = 0;
	OggOpusFile *of = op_open_file(filepath, &err);
	if (!of) {
		fprintf(stderr, "opus: %s does not open (error %d)\n", filepath, err);
		return NULL;
	}

	int ch = channels_of(of);
	if (ch == 0) {
		fprintf(stderr, "opus: %s has a channel count we do not know how to handle\n", filepath);
		op_free(of);
		return NULL;
	}

	opusdec_t *o = calloc(1, sizeof(*o));
	if (!o) {
		op_free(of);
		return NULL;
	}
	o->of = of;
	o->force_stereo = (ch > 2);
	o->channels = o->force_stereo ? 2 : ch;

	// op_pcm_total() is -1 on a non-seekable stream (a pipe): there the duration
	// simply is not known, and zero is how the rest of the player says so.
	ogg_int64_t total = op_pcm_total(of, -1);
	o->frames = total > 0 ? (uint64_t)total : 0;

	printf("opus: %s -- 48000 Hz, %d ch%s, %llu frame\n", filepath, o->channels,
		   o->force_stereo ? " (downmixed from multichannel)" : "", (unsigned long long)o->frames);
	return o;
}

void opusdec_close(opusdec_t *o) {
	if (!o) {
		return;
	}
	if (o->of) {
		op_free(o->of);
	}
	free(o);
}

int opusdec_channels(const opusdec_t *o) { return o ? o->channels : 0; }

// Not read from a field: Opus decodes at 48 kHz and nothing else.
int opusdec_sample_rate(const opusdec_t *o) { return o ? 48000 : 0; }

uint64_t opusdec_total_frames(const opusdec_t *o) { return o ? o->frames : 0; }

// On the 16-bit path opusfile scales by 32753 instead of 32768 (`OP_GAIN` in
// opusfile.c) to keep headroom against clipping, since decoded Opus can exceed
// full scale between samples. That is -0.004 dB against a full-scale decode,
// which on a lossy codec costs less than the headroom is worth.
uint64_t opusdec_read_s16(opusdec_t *o, uint64_t frames, short *out) {
	if (!o || !out || frames == 0) {
		return 0;
	}

	uint64_t done = 0;
	int holes = 0;

	// op_read() returns how many frames per channel it produced, and produces as
	// many as suits it -- normally one packet at a time. It has to be called
	// again until the caller's buffer is full.
	while (done < frames) {
		uint64_t want_frames = frames - done;
		// The count opusfile wants is in total samples, channels included, and
		// has to fit in an int.
		uint64_t want_samples = want_frames * (uint64_t)o->channels;
		if (want_samples > (uint64_t)INT_MAX) {
			want_samples = (uint64_t)INT_MAX;
		}

		short *dst = out + done * (uint64_t)o->channels;
		int got = o->force_stereo ? op_read_stereo(o->of, dst, (int)want_samples)
								  : op_read(o->of, dst, (int)want_samples, NULL);

		if (got == OP_HOLE) {
			// Missing piece: retry, but not forever.
			if (++holes > MAX_HOLES) {
				fprintf(stderr, "opus: too many consecutive gaps; stopping here\n");
				break;
			}
			continue;
		}
		if (got <= 0) {
			break; // 0 = end of stream, negative = a real error
		}

		holes = 0;
		done += (uint64_t)got;
	}

	return done;
}

bool opusdec_seek(opusdec_t *o, uint64_t frame) {
	if (!o) {
		return false;
	}
	return op_pcm_seek(o->of, (ogg_int64_t)frame) == 0;
}

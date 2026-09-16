#include "alacdec.h"

#include "alac/decomp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// alac.c declares this extern and leaves the definition to the caller. Without
// a definition the file does not link, and a wrong value silently byte-swaps
// every sample, so the real value is probed at runtime in alacdec_open().
int host_bigendian = 0;

// Exact length of ALACSpecificConfig, big-endian.
#define COOKIE_SIZE 24

// alac.c reads bits from the access unit without knowing where it ends, so the
// access unit is copied into a buffer with this much zero padding behind it. A
// decoder that overruns stays inside the allocation and reads zeros instead of
// someone else's memory.
#define INPUT_SLACK 4096

struct alacdec {
	alac_file *alac;

	int channels;
	int sample_rate;
	int bits;
	int frame_samples; // frameLength: maximum samples per access unit

	unsigned char *in; // access unit + zero padding
	int in_capacity;

	unsigned char *raw; // raw decoder output, packed 16 or 24 bit
	int raw_capacity;
};

static uint32_t be32(const unsigned char *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// alac_set_info() expects 24 bytes of QuickTime headers (`frma`, `alac`, the
// sizes) that it skips without looking, and only then the ALACSpecificConfig.
// mp4.c supplies a clean cookie, so the expected preamble is built in front of
// it. The buffer is an array of uint32_t on purpose: that function reads 32-
// and 16-bit integers straight off the pointer, and on MIPS an unaligned read
// is not merely slow.
static void feed_config(alac_file *alac, const unsigned char *cookie) {
	uint32_t buf[(24 + COOKIE_SIZE) / 4];
	memset(buf, 0, 24);
	memcpy((unsigned char *)buf + 24, cookie, COOKIE_SIZE);
	alac_set_info(alac, (char *)buf);
}

alacdec_t *alacdec_open(const unsigned char *cookie, int cookie_len) {
	if (!cookie || cookie_len < COOKIE_SIZE) {
		fprintf(stderr, "alac: configuration missing or too short (%d bytes)\n", cookie_len);
		return NULL;
	}

	// ALACSpecificConfig, big-endian:
	//   0..3  frameLength      8..8   kb
	//   4     compatVersion    9      numChannels
	//   5     bitDepth        10..11  maxRun
	//   6     pb              12..15  maxFrameBytes
	//   7     mb              16..19  avgBitRate      20..23 sampleRate
	uint32_t frame_length = be32(cookie);
	int bits = cookie[5];
	int channels = cookie[9];
	uint32_t sample_rate = be32(cookie + 20);

	if (bits != 16 && bits != 24) {
		// The decoder does not implement 20- and 32-bit depths.
		fprintf(stderr, "alac: %d bit not supported (16 and 24 only)\n", bits);
		return NULL;
	}
	if (channels < 1 || channels > 2) {
		fprintf(stderr, "alac: %d channels not supported (mono and stereo only)\n", channels);
		return NULL;
	}
	if (frame_length == 0 || frame_length > 65536 || sample_rate == 0) {
		fprintf(stderr, "alac: implausible configuration (frameLength %u, %u Hz)\n", frame_length, sample_rate);
		return NULL;
	}

	// Probed once, rather than trusting a macro: a wrong value here produces
	// noise, not an error.
	const uint16_t probe = 0x0001;
	host_bigendian = (*(const unsigned char *)&probe == 0) ? 1 : 0;

	alacdec_t *a = calloc(1, sizeof(*a));
	if (!a) {
		return NULL;
	}
	a->channels = channels;
	a->bits = bits;
	a->sample_rate = (int)sample_rate;
	a->frame_samples = (int)frame_length;

	a->alac = create_alac(bits, channels);
	if (!a->alac) {
		free(a);
		return NULL;
	}
	feed_config(a->alac, cookie);

	// The decoder writes (bits/8) bytes per sample per channel, at most one
	// frameLength at a time.
	a->raw_capacity = a->frame_samples * channels * (bits / 8);
	a->raw = malloc((size_t)a->raw_capacity);

	// A compressed access unit can only be marginally larger than the
	// uncompressed one; maxFrameBytes would be the right measure but not every
	// tagger writes it, so start generous and grow if needed.
	a->in_capacity = a->raw_capacity + INPUT_SLACK;
	a->in = calloc(1, (size_t)a->in_capacity);

	if (!a->raw || !a->in) {
		alacdec_close(a);
		return NULL;
	}

	printf("alac: %u Hz, %d ch, %d bit, %d samples per frame\n", sample_rate, channels, bits, a->frame_samples);
	return a;
}

void alacdec_close(alacdec_t *a) {
	if (!a) {
		return;
	}
	// free_alac() is a local addition to alac.c, releasing the six internal
	// buffers create_alac() allocates. See alac/README.md.
	free_alac(a->alac);
	free(a->raw);
	free(a->in);
	free(a);
}

int alacdec_channels(const alacdec_t *a) { return a ? a->channels : 0; }
int alacdec_sample_rate(const alacdec_t *a) { return a ? a->sample_rate : 0; }
int alacdec_bits(const alacdec_t *a) { return a ? a->bits : 16; }
int alacdec_frame_samples(const alacdec_t *a) { return a ? a->frame_samples : 0; }

int alacdec_decode(alacdec_t *a, const unsigned char *au, int au_len, int32_t *out, int out_frames) {
	if (!a || !au || au_len <= 0 || !out || out_frames <= 0) {
		return 0;
	}

	// The access unit must fit in the buffer together with its zero padding.
	if (au_len + INPUT_SLACK > a->in_capacity) {
		int want = au_len + INPUT_SLACK;
		unsigned char *grown = realloc(a->in, (size_t)want);
		if (!grown) {
			return 0;
		}
		a->in = grown;
		a->in_capacity = want;
	}
	memcpy(a->in, au, (size_t)au_len);
	memset(a->in + au_len, 0, (size_t)(a->in_capacity - au_len));

	int raw_bytes = 0;
	decode_frame(a->alac, a->in, a->raw, &raw_bytes);

	int bytes_per_frame = a->channels * (a->bits / 8);
	if (raw_bytes <= 0 || bytes_per_frame <= 0) {
		return 0;
	}
	if (raw_bytes > a->raw_capacity) {
		// The frameLength clamp inside alac.c should prevent this, but if it
		// happens the decoder has already written past the buffer, and the only
		// sane response is not to compound it by reading.
		fprintf(stderr, "alac: %d byte frame beyond the %d expected; skipping it\n", raw_bytes, a->raw_capacity);
		return 0;
	}

	int frames = raw_bytes / bytes_per_frame;
	if (frames > out_frames) {
		frames = out_frames;
	}
	int samples = frames * a->channels;

	if (a->bits == 16) {
		// The decoder writes int16 in host byte order.
		const int16_t *src = (const int16_t *)(const void *)a->raw;
		for (int i = 0; i < samples; i++) {
			out[i] = (int32_t)src[i] << 16;
		}
	} else {
		// 24 bit: three bytes per sample, little-endian, as alac.c writes them.
		const unsigned char *src = a->raw;
		for (int i = 0; i < samples; i++) {
			uint32_t v = (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16);
			// Left-justify into the word: the 24 bits land at the top and the
			// shift into the sign bit extends the sign.
			out[i] = (int32_t)(v << 8);
			src += 3;
		}
	}

	return frames;
}

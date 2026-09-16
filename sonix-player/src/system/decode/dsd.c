#include "dsd.h"

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// See dsd.h for the two routes out. This file does the container, the bit
// order, and either the DoP packing or the filter.

#define MAX_CHANNELS 2
#define DSD64_RATE 2822400u

// How much of the bitstream is pulled in at a time, per channel.
#define READ_BYTES 16384

// ---------------------------------------------------------------------------
// The filter, for the conversion route
// ---------------------------------------------------------------------------
//
// Two stages, because one filter long enough to go from 2.8 MHz to 176.4 kHz
// in a single step would be hundreds of taps and this is a 1 GHz MIPS.
//
//   1. eight-to-one, done a byte at a time through a lookup table -- the
//      dsd2pcm trick: a byte of the stream is eight taps of the filter, so the
//      256 possible bytes are pre-summed for each byte position and the inner
//      loop becomes six table reads and five adds instead of forty-eight
//      multiplies;
//   2. two-to-one as many times as needed, with a short symmetric filter.
//
// DSD64 goes 2822400 -> 352800 -> 176400 (one halving), DSD128 takes two and
// DSD256 three.

#define STAGE1_BYTES 6 // 48 taps
#define STAGE1_TAPS (STAGE1_BYTES * 8)
#define HALF_TAPS 23
#define MAX_HALVINGS 3

static int32_t stage1_table[STAGE1_BYTES][256];
static int32_t half_coef[HALF_TAPS];
static bool tables_ready;

// Half-band: sinc(n/2) windowed, so every even tap but the centre is zero and
// the multiplies are half what the length suggests. It runs at the decimated
// rate, which is why it can afford to be longer than the first stage.
static void build_halfband(void) {
	double taps[HALF_TAPS];
	double sum = 0;
	int mid = (HALF_TAPS - 1) / 2;
	for (int i = 0; i < HALF_TAPS; i++) {
		int n = i - mid;
		double w = 0.42 - 0.5 * cos(2 * M_PI * i / (HALF_TAPS - 1)) + 0.08 * cos(4 * M_PI * i / (HALF_TAPS - 1));
		if (n == 0) {
			taps[i] = 1.0;
		} else if (n % 2 == 0) {
			taps[i] = 0.0; // the half-band's own zeros
		} else {
			taps[i] = sin(M_PI * n / 2.0) / (M_PI * n / 2.0) * w;
		}
		sum += taps[i];
	}

	int32_t total = 0;
	for (int i = 0; i < HALF_TAPS; i++) {
		half_coef[i] = (int32_t)lrint(taps[i] / sum * 32768.0);
		total += half_coef[i];
	}
	// Forced to exactly unity at DC: rounding leaves the sum a few counts out,
	// and DSD256 runs three of these in series, so a per-stage gain error
	// compounds.
	half_coef[mid] += 32768 - total;
}

// A windowed sinc with its corner at a sixteenth of the DSD rate, which is the
// Nyquist of what comes out of this stage.
static void build_tables(void) {
	if (tables_ready) {
		return;
	}

	build_halfband();

	double taps[STAGE1_TAPS];
	double sum = 0;
	for (int i = 0; i < STAGE1_TAPS; i++) {
		double x = i - (STAGE1_TAPS - 1) / 2.0;
		double sinc = (fabs(x) < 1e-9) ? 1.0 : sin(M_PI * x / 8.0) / (M_PI * x / 8.0);
		// Blackman window: simple, and its stopband is deep enough that the
		// DSD hiss that survives is far below anything audible.
		double w = 0.42 - 0.5 * cos(2 * M_PI * i / (STAGE1_TAPS - 1)) + 0.08 * cos(4 * M_PI * i / (STAGE1_TAPS - 1));
		taps[i] = sinc * w;
		sum += taps[i];
	}
	for (int i = 0; i < STAGE1_TAPS; i++) {
		taps[i] /= sum; // unity at DC
	}

	// Each entry is the filter's answer to one byte in one position, with a
	// bit worth +1 and a clear bit -1, in Q24.
	for (int b = 0; b < STAGE1_BYTES; b++) {
		for (int v = 0; v < 256; v++) {
			double acc = 0;
			for (int k = 0; k < 8; k++) {
				double bit = (v & (0x80 >> k)) ? 1.0 : -1.0;
				// Bit 7 is the earliest of the eight in time and bit 0 the
				// latest, while hist[0] is the newest byte and hist[5] the
				// oldest -- so the tap index runs backwards inside a byte
				// and forwards between them. Reversing it reverses the
				// filter in groups of eight, which is not a filter at all.
				acc += bit * taps[b * 8 + (7 - k)];
			}
			stage1_table[b][v] = (int32_t)lrint(acc * 16777216.0);
		}
	}
	tables_ready = true;
}

struct dsd_file {
	int fd;
	dsd_output_t mode;

	int channels;
	uint32_t rate;	 // the DSD rate
	int multiple;	 // 64 / 128 / 256
	bool lsb_first;	 // .dsf writes the earliest bit in bit 0
	bool planar;	 // .dsf: blocks per channel; .dff: byte-interleaved
	uint32_t block;	 // .dsf block size per channel
	uint64_t data_offset;
	uint64_t data_bytes;	// total, all channels
	uint64_t bytes_per_ch;	// audio bytes per channel, padding excluded

	uint64_t pos_bytes; // per channel, where the next read starts
	int out_rate;
	int halvings;
	uint64_t total_out;

	// The bitstream, de-interleaved into one buffer per channel.
	unsigned char *chan[MAX_CHANNELS];
	unsigned char *raw; // .dff only: the interleaved slab before splitting
	int chan_len;
	int chan_read;

	// Filter state, per channel.
	//
	// Neither delay line is an array that gets shifted: at DSD256 the chain
	// runs 2.8 million first-stage evaluations and 2.5 million half-band ones
	// every second, and a memmove inside each of those dominates the cost on
	// a 1 GHz MIPS. So:
	//
	//   * the first stage keeps its six bytes in one 64-bit word and shifts it,
	//   * the half-band writes each sample twice into a buffer of twice the
	//     length, so the taps it needs are always contiguous from `half_pos`
	//     and nothing ever has to be moved.
	uint64_t hist[MAX_CHANNELS];
	int32_t half_hist[MAX_CHANNELS][MAX_HALVINGS][HALF_TAPS * 2];
	int half_pos[MAX_CHANNELS][MAX_HALVINGS];
	int half_phase[MAX_CHANNELS][MAX_HALVINGS];

	// Finished output frames waiting to be handed over.
	int32_t *pcm;
	int pcm_frames;
	int pcm_read;
	int pcm_capacity;

	// DoP alternates its marker every frame.
	int dop_phase;
};

// ---------------------------------------------------------------------------
// containers
// ---------------------------------------------------------------------------

static bool read_at(int fd, uint64_t off, void *buf, size_t len) {
	size_t done = 0;
	while (done < len) {
		ssize_t n = pread(fd, (char *)buf + done, len - done, (off_t)(off + done));
		if (n <= 0) {
			return false;
		}
		done += (size_t)n;
	}
	return true;
}

static uint32_t le32(const unsigned char *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t le64(const unsigned char *p) { return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32); }
static uint32_t be32d(const unsigned char *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t be64d(const unsigned char *p) { return ((uint64_t)be32d(p) << 32) | be32d(p + 4); }

static bool parse_dsf(dsd_file_t *d) {
	unsigned char head[28];
	if (!read_at(d->fd, 0, head, 28) || memcmp(head, "DSD ", 4) != 0) {
		return false;
	}

	unsigned char fmt[52];
	if (!read_at(d->fd, 28, fmt, 52) || memcmp(fmt, "fmt ", 4) != 0) {
		return false;
	}

	d->channels = (int)le32(fmt + 24);
	d->rate = le32(fmt + 28);
	uint32_t bits = le32(fmt + 32);
	uint64_t sample_count = le64(fmt + 36); // per channel, in bits
	d->block = le32(fmt + 44);
	d->lsb_first = (bits == 1);
	d->planar = true;

	if (d->block == 0 || d->block > 65536) {
		return false;
	}

	uint64_t data_at = 28 + le64(fmt + 4);
	unsigned char dh[12];
	if (!read_at(d->fd, data_at, dh, 12) || memcmp(dh, "data", 4) != 0) {
		return false;
	}
	d->data_offset = data_at + 12;
	d->data_bytes = le64(dh + 4) - 12;
	// The count in the header is what is really there; the last block of each
	// channel is padded out and that padding is not audio.
	d->bytes_per_ch = sample_count / 8;
	return true;
}

static bool parse_dff(dsd_file_t *d) {
	unsigned char head[16];
	if (!read_at(d->fd, 0, head, 16) || memcmp(head, "FRM8", 4) != 0 || memcmp(head + 12, "DSD ", 4) != 0) {
		return false;
	}

	d->lsb_first = false;
	d->planar = false;
	d->channels = 2; // until CHNL says otherwise
	d->rate = 0;

	uint64_t pos = 16;
	uint64_t end = 12 + be64d(head + 4);

	// PROP is a container and DSD comes after it, so descending into it has to
	// be undone again: one saved level of position is all DSDIFF needs.
	uint64_t outer_pos = 0, outer_end = 0;
	bool inside = false;

	for (;;) {
		if (pos + 12 > end) {
			if (inside) {
				// Out of PROP; carry on where the outer level left off.
				inside = false;
				pos = outer_pos;
				end = outer_end;
				continue;
			}
			break;
		}

		// Sixteen, not twelve: PROP carries a four-byte type after its header
		// and it has to be read, not looked for past the end of the buffer.
		unsigned char ch[16] = {0};
		if (!read_at(d->fd, pos, ch, 12)) {
			return false;
		}
		uint64_t size = be64d(ch + 4);
		uint64_t body = pos + 12;
		if (memcmp(ch, "PROP", 4) == 0 && size >= 4) {
			(void)read_at(d->fd, body, ch + 12, 4);
		}

		if (!inside && memcmp(ch, "PROP", 4) == 0 && memcmp(ch + 12, "SND ", 4) == 0) {
			// Walk into it: FS, CHNL and the rest live one level down.
			outer_pos = body + size + (size & 1);
			outer_end = end;
			inside = true;
			pos = body + 4; // past the "SND " type
			end = body + size;
			continue;
		}
		if (memcmp(ch, "FS  ", 4) == 0 && size >= 4) {
			unsigned char v[4];
			if (read_at(d->fd, body, v, 4)) {
				d->rate = be32d(v);
			}
		} else if (memcmp(ch, "CHNL", 4) == 0 && size >= 2) {
			unsigned char v[2];
			if (read_at(d->fd, body, v, 2)) {
				d->channels = (v[0] << 8) | v[1];
			}
		} else if (memcmp(ch, "DSD ", 4) == 0) {
			d->data_offset = body;
			d->data_bytes = size;
			if (d->channels > 0) {
				d->bytes_per_ch = size / (uint64_t)d->channels;
			}
			return d->rate != 0;
		} else if (memcmp(ch, "DST ", 4) == 0) {
			return false; // compressed DSD; not unpacked here
		}

		// Every chunk is padded to an even length, and the size does not say so.
		pos = body + size + (size & 1);
	}
	return false;
}

// ---------------------------------------------------------------------------
// filtering
// ---------------------------------------------------------------------------

static int32_t half_step(dsd_file_t *d, int ch, int stage, int32_t in, bool *have_out) {
	int32_t *h = d->half_hist[ch][stage];

	// Walk the write position backwards and store the sample in both halves;
	// the taps then read forwards from it without a wrap test.
	int pos = d->half_pos[ch][stage] - 1;
	if (pos < 0) {
		pos = HALF_TAPS - 1;
	}
	d->half_pos[ch][stage] = pos;
	h[pos] = in;
	h[pos + HALF_TAPS] = in;

	d->half_phase[ch][stage] ^= 1;
	if (d->half_phase[ch][stage] == 0) {
		*have_out = false;
		return 0; // every other input produces an output
	}

	const int32_t *x = h + pos;
	int64_t acc = 0;
	// HALF_TAPS is odd, so the centre sits at an odd index while every other
	// non-zero tap sits at an even one: the loop steps over the even indices
	// and the centre is added on its own.
	//
	// The parity matters: stepping over the odd indices instead would leave
	// nothing but the centre tap, counted twice. That is still exactly unity
	// at DC, so a tone comes out at the right level while the filter has
	// silently stopped filtering.
	for (int i = 0; i < HALF_TAPS; i += 2) {
		acc += (int64_t)x[i] * half_coef[i];
	}
	acc += (int64_t)x[HALF_TAPS / 2] * half_coef[HALF_TAPS / 2];
	*have_out = true;
	return (int32_t)(acc >> 15);
}

// Turns one byte of one channel's bitstream into however many output frames it
// produced (0 or 1 at these ratios), writing into `sample`.
static bool feed_byte(dsd_file_t *d, int ch, unsigned char byte, int32_t *sample) {
	// Stage one: shift the byte into the low end of the register -- byte b of
	// the old history is now at bit 8*b -- and take one 8:1 output.
	uint64_t hist = (d->hist[ch] << 8) | byte;
	d->hist[ch] = hist;

	int32_t acc = 0;
	for (int b = 0; b < STAGE1_BYTES; b++) {
		acc += stage1_table[b][(hist >> (8 * b)) & 0xFF];
	}
	int32_t value = acc; // Q24: full scale is 1<<24

	for (int s = 0; s < d->halvings; s++) {
		bool have = false;
		value = half_step(d, ch, s, value, &have);
		if (!have) {
			return false;
		}
	}

	// Q24 up to left-justified 32-bit, which is what every other decoder hands
	// out. Saturating rather than wrapping: the filter's own overshoot can push
	// a loud passage past full scale, and a wrap there is a crack, not a hiss.
	int64_t wide = (int64_t)value << 7;
	if (wide > INT32_MAX) {
		wide = INT32_MAX;
	} else if (wide < INT32_MIN) {
		wide = INT32_MIN;
	}
	*sample = (int32_t)wide;
	return true;
}

// ---------------------------------------------------------------------------
// reading
// ---------------------------------------------------------------------------

// Pulls the next slab of bitstream and de-interleaves it into d->chan.
static bool fill_channels(dsd_file_t *d) {
	d->chan_read = 0;
	d->chan_len = 0;

	if (d->pos_bytes >= d->bytes_per_ch) {
		return false;
	}

	uint64_t left = d->bytes_per_ch - d->pos_bytes;
	int want = (int)(left < READ_BYTES ? left : READ_BYTES);

	if (d->planar) {
		// .dsf: whole blocks per channel, one channel after another. Read only
		// as far as the block boundary so the de-interleave stays simple.
		uint64_t in_block = d->pos_bytes % d->block;
		uint64_t to_edge = d->block - in_block;
		if ((uint64_t)want > to_edge) {
			want = (int)to_edge;
		}

		uint64_t block_index = d->pos_bytes / d->block;
		for (int c = 0; c < d->channels; c++) {
			uint64_t off = d->data_offset + (block_index * d->channels + c) * (uint64_t)d->block + in_block;
			if (!read_at(d->fd, off, d->chan[c], (size_t)want)) {
				return false;
			}
		}
	} else {
		// .dff: one byte per channel, round and round.
		unsigned char *raw = d->raw;
		size_t bytes = (size_t)want * d->channels;
		if (!read_at(d->fd, d->data_offset + d->pos_bytes * d->channels, raw, bytes)) {
			return false;
		}
		for (int i = 0; i < want; i++) {
			for (int c = 0; c < d->channels; c++) {
				d->chan[c][i] = raw[i * d->channels + c];
			}
		}
	}

	d->pos_bytes += (uint64_t)want;
	d->chan_len = want;
	return true;
}

static unsigned char to_msb_first(unsigned char v, bool lsb_first) {
	if (!lsb_first) {
		return v;
	}
	// The earliest bit has to end up at the top, because that is the order
	// both the filter table and DoP expect.
	v = (unsigned char)(((v & 0xF0) >> 4) | ((v & 0x0F) << 4));
	v = (unsigned char)(((v & 0xCC) >> 2) | ((v & 0x33) << 2));
	v = (unsigned char)(((v & 0xAA) >> 1) | ((v & 0x55) << 1));
	return v;
}

// Produces one buffer's worth of output frames from the bitstream.
static bool produce(dsd_file_t *d) {
	d->pcm_frames = 0;
	d->pcm_read = 0;

	while (d->pcm_frames == 0) {
		if (d->chan_read >= d->chan_len && !fill_channels(d)) {
			return false;
		}

		int available = d->chan_len - d->chan_read;

		if (d->mode == DSD_OUT_DOP) {
			// Two bytes of stream per frame, sixteen bits, with the marker on
			// top and the two bytes below it -- the earliest bit first.
			int pairs = available / 2;
			if (pairs > d->pcm_capacity) {
				pairs = d->pcm_capacity;
			}
			for (int i = 0; i < pairs; i++) {
				uint32_t marker = d->dop_phase ? 0xFA : 0x05;
				d->dop_phase ^= 1;
				for (int c = 0; c < d->channels; c++) {
					unsigned char hi = to_msb_first(d->chan[c][d->chan_read + i * 2], d->lsb_first);
					unsigned char lo = to_msb_first(d->chan[c][d->chan_read + i * 2 + 1], d->lsb_first);
					uint32_t word = (marker << 16) | ((uint32_t)hi << 8) | lo;
					// Left-justified in 32 bits, which is how every other
					// 24-bit source reaches the output.
					d->pcm[i * d->channels + c] = (int32_t)(word << 8);
				}
			}
			d->chan_read += pairs * 2;
			d->pcm_frames = pairs;
		} else {
			// One byte in is one stage-one output, and each halving keeps every
			// other one: so capacity << halvings bytes is exactly the buffer.
			// Getting this wrong does not overflow -- it silently throws
			// samples away, which is worse, because it sounds like a fast tape.
			int max_bytes = d->pcm_capacity << d->halvings;
			int count = available > max_bytes ? max_bytes : available;
			for (int i = 0; i < count; i++) {
				int32_t sample[MAX_CHANNELS];
				bool got = false;
				for (int c = 0; c < d->channels; c++) {
					got = feed_byte(d, c, to_msb_first(d->chan[c][d->chan_read + i], d->lsb_first), &sample[c]);
				}
				if (got) {
					for (int c = 0; c < d->channels; c++) {
						d->pcm[d->pcm_frames * d->channels + c] = sample[c];
					}
					d->pcm_frames++;
				}
			}
			d->chan_read += count;
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------

dsd_file_t *dsd_open(const char *path, dsd_output_t mode) {
	build_tables();

	dsd_file_t *d = calloc(1, sizeof(*d));
	if (!d) {
		return NULL;
	}
	d->mode = mode;
	d->fd = open(path, O_RDONLY);
	if (d->fd < 0) {
		free(d);
		return NULL;
	}

	bool ok = parse_dsf(d);
	if (!ok) {
		ok = parse_dff(d);
	}
	if (!ok || d->channels < 1 || d->channels > MAX_CHANNELS || d->bytes_per_ch == 0) {
		dsd_close(d);
		return NULL;
	}

	// 64, 128 or 256, and nothing else: a rate that is not 44.1 kHz times one
	// of those is not a file this decoder can time.
	d->multiple = (int)(d->rate / (DSD64_RATE / 64));
	if (d->multiple != 64 && d->multiple != 128 && d->multiple != 256) {
		fprintf(stderr, "dsd: %u Hz is not DSD64/128/256\n", d->rate);
		dsd_close(d);
		return NULL;
	}

	if (mode == DSD_OUT_DOP) {
		d->out_rate = (int)(d->rate / 16);
		d->halvings = 0;
	} else {
		d->out_rate = DSD_PCM_RATE;
		// /8 in the first stage, then halve until the rate is reached: one
		// halving for DSD64, two for DSD128, three for DSD256.
		int factor = (int)(d->rate / 8) / DSD_PCM_RATE;
		d->halvings = 0;
		while (factor > 1 && d->halvings < MAX_HALVINGS) {
			factor /= 2;
			d->halvings++;
		}
	}

	d->total_out = (d->bytes_per_ch * 8ull) / (uint64_t)(d->rate / (uint32_t)d->out_rate);

	d->pcm_capacity = 4096;
	d->pcm = calloc((size_t)d->pcm_capacity * d->channels, sizeof(int32_t));
	for (int c = 0; c < d->channels; c++) {
		d->chan[c] = malloc(READ_BYTES);
		if (!d->chan[c]) {
			dsd_close(d);
			return NULL;
		}
	}
	if (!d->planar) {
		d->raw = malloc((size_t)READ_BYTES * d->channels);
		if (!d->raw) {
			dsd_close(d);
			return NULL;
		}
	}
	if (!d->pcm) {
		dsd_close(d);
		return NULL;
	}

	printf("dsd: %s DSD%d %u Hz %d ch -> %s %d Hz\n", d->planar ? "dsf" : "dff", d->multiple, d->rate, d->channels,
		   mode == DSD_OUT_DOP ? "DoP" : "PCM", d->out_rate);
	return d;
}

void dsd_close(dsd_file_t *d) {
	if (!d) {
		return;
	}
	for (int c = 0; c < MAX_CHANNELS; c++) {
		free(d->chan[c]);
	}
	free(d->raw);
	free(d->pcm);
	if (d->fd >= 0) {
		close(d->fd);
	}
	free(d);
}

int dsd_channels(const dsd_file_t *d) { return d ? d->channels : 0; }
uint32_t dsd_rate(const dsd_file_t *d) { return d ? d->rate : 0; }
int dsd_multiple(const dsd_file_t *d) { return d ? d->multiple : 0; }
int dsd_output_rate(const dsd_file_t *d) { return d ? d->out_rate : 0; }
bool dsd_is_dop(const dsd_file_t *d) { return d && d->mode == DSD_OUT_DOP; }
uint64_t dsd_total_frames(const dsd_file_t *d) { return d ? d->total_out : 0; }

uint64_t dsd_read(dsd_file_t *d, uint64_t frames, int32_t *out) {
	if (!d || !out || frames == 0) {
		return 0;
	}

	uint64_t written = 0;
	while (written < frames) {
		if (d->pcm_read >= d->pcm_frames && !produce(d)) {
			break;
		}
		uint64_t available = (uint64_t)(d->pcm_frames - d->pcm_read);
		uint64_t take = frames - written;
		if (take > available) {
			take = available;
		}
		memcpy(out + written * (uint64_t)d->channels, d->pcm + (size_t)d->pcm_read * d->channels,
			   (size_t)take * d->channels * sizeof(int32_t));
		d->pcm_read += (int)take;
		written += take;
	}
	return written;
}

bool dsd_seek(dsd_file_t *d, uint64_t frame) {
	if (!d) {
		return false;
	}
	if (frame > d->total_out) {
		frame = d->total_out;
	}

	uint64_t bytes_per_frame = (uint64_t)(d->rate / (uint32_t)d->out_rate) / 8;
	uint64_t target = frame * bytes_per_frame;
	if (target > d->bytes_per_ch) {
		target = d->bytes_per_ch;
	}

	// On the DoP route a frame is exactly two bytes, so landing on an odd byte
	// would swap the halves of every word from here on.
	if (d->mode == DSD_OUT_DOP) {
		target &= ~1ull;
	}

	d->pos_bytes = target;
	d->chan_len = 0;
	d->chan_read = 0;
	d->pcm_frames = 0;
	d->pcm_read = 0;
	d->dop_phase = 0;

	memset(d->hist, 0, sizeof(d->hist));
	memset(d->half_hist, 0, sizeof(d->half_hist));
	memset(d->half_pos, 0, sizeof(d->half_pos));
	memset(d->half_phase, 0, sizeof(d->half_phase));
	return true;
}

#include "dsd.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// The container, the bit order, and the two ways out: DoP packing, and the
// lookup-table filter to PCM (see dsd.h).

#define MAX_CHANNELS 2
#define DSD64_RATE 2822400u

// How much of one channel's bitstream a read pulls in.
//
// At DSD256 the stream is 2.8 MB a second off the card, and a .dsf keeps each
// channel's blocks separately: asking for one block at a time is two reads at
// two places in the file for every four kilobytes, several hundred times a
// second, which is the worst shape a card can be asked for. This much per
// channel is one sequential read covering every channel's block at once.
#define READ_BYTES 32768

struct dsd_file {
	int fd;

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
	uint64_t total_out;

	// The bitstream, de-interleaved into one buffer per channel.
	unsigned char *chan[MAX_CHANNELS];
	unsigned char *raw; // the slab as it comes off the card, before splitting
	int chan_len;
	int chan_read;

	// Finished output frames waiting to be handed over.
	int32_t *pcm;
	int pcm_frames;
	int pcm_read;
	int pcm_capacity;

	// DoP alternates its marker every frame.
	int dop_phase;

	// PCM instead of DoP (dsd_to_pcm). Each channel's buffer then has the
	// last `taps` bytes of the slab before it in front of `chan[c]`.
	bool pcm_mode;
	int step;	  // bytes of stream per channel per output frame
	int taps;	  // filter length in bytes
	int sum_bits; // the filter's coefficients sum to 1 << sum_bits
	const int32_t (*table)[256];
	int32_t comp[MAX_CHANNELS][2]; // the droop compensator's last two inputs, per channel
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
		// .dsf: whole blocks, one channel's after another's. A group of them
		// starting on a block boundary is contiguous on disk across every
		// channel, so it comes in with one read and is split afterwards;
		// anything short of that (the first read after a seek, the tail of the
		// file) falls back to a read per channel.
		uint64_t in_block = d->pos_bytes % d->block;
		uint64_t block_index = d->pos_bytes / d->block;
		int blocks = (in_block == 0) ? want / (int)d->block : 0;

		if (blocks > 0) {
			size_t span = (size_t)blocks * d->block;
			uint64_t off = d->data_offset + block_index * d->channels * (uint64_t)d->block;
			if (!read_at(d->fd, off, d->raw, span * d->channels)) {
				return false;
			}
			for (int b = 0; b < blocks; b++) {
				for (int c = 0; c < d->channels; c++) {
					memcpy(d->chan[c] + (size_t)b * d->block,
						   d->raw + ((size_t)b * d->channels + c) * d->block, d->block);
				}
			}
			want = (int)span;
		} else {
			uint64_t to_edge = d->block - in_block;
			if ((uint64_t)want > to_edge) {
				want = (int)to_edge;
			}
			for (int c = 0; c < d->channels; c++) {
				uint64_t off = d->data_offset + (block_index * d->channels + c) * (uint64_t)d->block + in_block;
				if (!read_at(d->fd, off, d->chan[c], (size_t)want)) {
					return false;
				}
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
	// DoP expects.
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

		// Two bytes of stream per frame, sixteen bits, with the marker on top
		// and the two bytes below it -- the earliest bit first.
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
				// Left-justified in 32 bits, which is how every other 24-bit
				// source reaches the output.
				d->pcm[i * d->channels + c] = (int32_t)(word << 8);
			}
		}
		d->chan_read += pairs * 2;
		d->pcm_frames = pairs;
	}
	return true;
}

// ---------------------------------------------------------------------------
// to PCM
//
// A linear-phase FIR over the one-bit stream, decimating straight to
// PCM_RATE. Every bit is +1 or -1, so the filter's work on one byte of stream
// is one of 256 sums, worked out once per byte position: an output sample is
// then one table lookup and one add per byte of the filter's span, with no
// multiplications. The table is built for the file's bit order, so the stream
// is read where it lies, byte for byte.
//
// The filter is a fourth-order CIC: four moving averages of one output period
// (16, 32 or 64 bits) in a row. Its zeros sit exactly on the multiples of
// 176.4 kHz, which are the only bands that fold back under 20 kHz here, so it
// rejects them by 77 dB or more while spanning 8, 16 or 32 bytes, half of
// what a windowed sinc needs for the same rejection. What it lets through
// between those bands, and its droop of 1.6 dB at 20 kHz, are the half-band
// decimator's to deal with (decimate.h), which takes the result down to the
// headphones' rate anyway. The coefficients are integers summing to a power
// of two, so the tables are exact.
//
// Per second of stereo that is 2.8, 5.6 or 11.3 million lookups, into a table
// of 8, 16 or 32 KB.
//
// Gain +6 dB: a full-scale DSD signal is 50 % modulation, half of what the
// stream can carry, so without it DSD plays 6 dB quieter than PCM. What goes
// past full scale is clamped.
//
// The droop. A CIC's passband is not flat: (sin(pi f / F) / (R sin(pi f /
// RF)))^4 for an output rate F, 0.18 dB down at 10 kHz, 0.41 at 15 and 0.74
// at 20 -- the same at DSD64, 128 and 256, since it depends only on f / F.
// It is taken back by a three-tap FIR at 176.4 kHz, [-c, 1 + 2c, -c] with
// c = 0.1793 (Q14: -2938, 22260, -2938), a least-squares fit of the CIC's
// inverse up to 20 kHz: the two together are flat within 0.012 dB to 20 kHz.
// Two multiplications per sample, a sixth of what the CIC itself costs at
// DSD64. Its rise above the audible band (+4.7 dB at 88.2 kHz) lifts the
// ultrasonic noise a little on the way into the decimator, which removes it.
// What is left at 20 kHz once the stream reaches 44.1 kHz is the half-band
// decimator's own transition (0.85 dB), which every 88.2 kHz track has too.
// ---------------------------------------------------------------------------

#define COMP_SIDE (-2938)
#define COMP_CENTRE 22260 // with the sides, 1 << 14 at DC

#define PCM_RATE 176400
#define CIC_ORDER 4

// DSD64, 128, 256, each in the two bit orders.
static const int32_t (*tables[3][2])[256];
static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;

// `period` is the bits per output sample; `taps` the span in bytes, enough for
// CIC_ORDER * (period - 1) + 1 coefficients.
static const int32_t (*table_for(int multiple, int period, int taps, bool lsb_first))[256] {
	int which = multiple == 64 ? 0 : multiple == 128 ? 1 : 2;
	pthread_mutex_lock(&table_lock);
	if (!tables[which][lsb_first]) {
		int n = taps * 8;
		int32_t *h = calloc((size_t)n, sizeof(int32_t));
		int32_t *next = calloc((size_t)n, sizeof(int32_t));
		int32_t (*t)[256] = malloc(sizeof(int32_t) * 256 * (size_t)taps);
		if (h && next && t) {
			// One moving average, convolved with itself CIC_ORDER - 1 times.
			for (int i = 0; i < period; i++) {
				h[i] = 1;
			}
			int len = period;
			for (int order = 1; order < CIC_ORDER; order++) {
				memset(next, 0, sizeof(int32_t) * (size_t)n);
				for (int i = 0; i < len; i++) {
					for (int j = 0; j < period; j++) {
						next[i + j] += h[i];
					}
				}
				len += period - 1;
				memcpy(h, next, sizeof(int32_t) * (size_t)n);
			}
			for (int k = 0; k < taps; k++) {
				for (int v = 0; v < 256; v++) {
					int32_t acc = 0;
					for (int b = 0; b < 8; b++) {
						// The earliest bit of the byte is bit 0 in a .dsf, bit 7
						// in a .dff.
						int bit = lsb_first ? (v >> b) & 1 : (v >> (7 - b)) & 1;
						acc += bit ? h[k * 8 + b] : -h[k * 8 + b];
					}
					t[k][v] = acc;
				}
			}
			tables[which][lsb_first] = (const int32_t (*)[256])t;
			t = NULL;
		}
		free(t);
		free(next);
		free(h);
	}
	const int32_t (*t)[256] = tables[which][lsb_first];
	pthread_mutex_unlock(&table_lock);
	return t;
}

// Fills the history in front of each channel's bytes with DSD's idle pattern,
// which the filter reads as silence.
static void history_clear(dsd_file_t *d) {
	for (int c = 0; c < d->channels; c++) {
		memset(d->chan[c] - d->taps, 0x69, (size_t)d->taps);
	}
	memset(d->comp, 0, sizeof(d->comp));
}

static bool produce_pcm(dsd_file_t *d) {
	d->pcm_frames = 0;
	d->pcm_read = 0;
	// The coefficients sum to 1 << sum_bits, which is 100 % modulation; with
	// the gain of two, full scale is half of that.
	const int shift = 32 - d->sum_bits;
	const int32_t top = (1 << (d->sum_bits - 1)) - 1;

	while (d->pcm_frames == 0) {
		if (d->chan_read >= d->chan_len) {
			// The last `taps` bytes become the history of the next slab.
			for (int c = 0; c < d->channels; c++) {
				memmove(d->chan[c] - d->taps, d->chan[c] + d->chan_len - d->taps, (size_t)d->taps);
			}
			if (!fill_channels(d)) {
				return false;
			}
		}
		int frames = (d->chan_len - d->chan_read) / d->step;
		if (frames > d->pcm_capacity) {
			frames = d->pcm_capacity;
		}
		if (frames == 0) {
			d->chan_read = d->chan_len; // a tail shorter than one frame
			continue;
		}
		const int32_t (*t)[256] = d->table;
		int taps = d->taps;
		for (int c = 0; c < d->channels; c++) {
			// The window of an output frame ends with the last byte it takes in.
			const unsigned char *p = d->chan[c] + d->chan_read + d->step - taps;
			int32_t *out = d->pcm + c;
			int32_t x1 = d->comp[c][0], x2 = d->comp[c][1];
			for (int i = 0; i < frames; i++, p += d->step, out += d->channels) {
				int32_t acc = 0;
				for (int k = 0; k < taps; k += 4) {
					acc += t[k][p[k]] + t[k + 1][p[k + 1]] + t[k + 2][p[k + 2]] + t[k + 3][p[k + 3]];
				}
				// The droop compensator, one sample behind the CIC.
				int64_t y = ((int64_t)COMP_CENTRE * x1 + (int64_t)COMP_SIDE * ((int64_t)acc + x2) + (1 << 13)) >> 14;
				x2 = x1;
				x1 = acc;
				if (y > top) {
					y = top;
				} else if (y < -top) {
					y = -top;
				}
				*out = (int32_t)((uint32_t)y << shift);
			}
			d->comp[c][0] = x1;
			d->comp[c][1] = x2;
		}
		d->chan_read += frames * d->step;
		d->pcm_frames = frames;
	}
	return true;
}

bool dsd_to_pcm(dsd_file_t *d) {
	if (!d) {
		return false;
	}
	if (d->pcm_mode) {
		return true;
	}
	int step = d->multiple / 32;					   // 2, 4 or 8 bytes: 176.4 kHz out
	int period = step * 8;							   // 16, 32 or 64 bits
	int taps = (CIC_ORDER * (period - 1) + 1 + 7) / 8; // 8, 16 or 32 bytes
	const int32_t (*table)[256] = table_for(d->multiple, period, taps, d->lsb_first);
	if (!table) {
		return false;
	}
	// Each channel's buffer grows a history of `taps` bytes in front.
	unsigned char *lines[MAX_CHANNELS] = {0};
	for (int c = 0; c < d->channels; c++) {
		lines[c] = malloc((size_t)READ_BYTES + (size_t)taps);
		if (!lines[c]) {
			for (int k = 0; k < c; k++) {
				free(lines[k]);
			}
			return false;
		}
	}
	for (int c = 0; c < d->channels; c++) {
		free(d->chan[c]);
		d->chan[c] = lines[c] + taps;
	}
	d->step = step;
	d->taps = taps;
	d->sum_bits = CIC_ORDER * (period == 16 ? 4 : period == 32 ? 5 : 6);
	d->table = table;
	d->pcm_mode = true;
	d->out_rate = PCM_RATE;
	d->total_out = d->bytes_per_ch / (uint64_t)step;
	d->chan_len = 0;
	d->chan_read = 0;
	d->pcm_frames = 0;
	d->pcm_read = 0;
	history_clear(d);
	printf("dsd: DSD%d filtered to PCM %d Hz (%d-byte filter, droop compensated)\n", d->multiple, PCM_RATE, taps);
	return true;
}

bool dsd_is_pcm(const dsd_file_t *d) { return d && d->pcm_mode; }

// ---------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------

dsd_file_t *dsd_open(const char *path) {
	dsd_file_t *d = calloc(1, sizeof(*d));
	if (!d) {
		return NULL;
	}
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

	d->out_rate = (int)(d->rate / 16);

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
	// Both containers need it now: .dff to split the round-robin bytes, .dsf to
	// hold the block group before it is split.
	d->raw = malloc((size_t)READ_BYTES * d->channels);
	if (!d->raw) {
		dsd_close(d);
		return NULL;
	}
	if (!d->pcm) {
		dsd_close(d);
		return NULL;
	}

	printf("dsd: %s DSD%d %u Hz %d ch -> DoP %d Hz\n", d->planar ? "dsf" : "dff", d->multiple, d->rate,
		   d->channels, d->out_rate);
	return d;
}

void dsd_close(dsd_file_t *d) {
	if (!d) {
		return;
	}
	for (int c = 0; c < MAX_CHANNELS; c++) {
		free(d->chan[c] && d->pcm_mode ? d->chan[c] - d->taps : d->chan[c]);
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
uint64_t dsd_total_frames(const dsd_file_t *d) { return d ? d->total_out : 0; }

uint64_t dsd_read(dsd_file_t *d, uint64_t frames, int32_t *out) {
	if (!d || !out || frames == 0) {
		return 0;
	}

	uint64_t written = 0;
	while (written < frames) {
		if (d->pcm_read >= d->pcm_frames && !(d->pcm_mode ? produce_pcm(d) : produce(d))) {
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

	// A DoP frame is exactly two bytes, so landing on an odd one would swap
	// the halves of every word from here on. A PCM frame is `step` bytes.
	uint64_t step = d->pcm_mode ? (uint64_t)d->step : 2;
	uint64_t target = frame * step;
	if (target > d->bytes_per_ch) {
		target = d->bytes_per_ch;
	}
	target -= target % step;
	if (d->pcm_mode) {
		history_clear(d);
	}

	d->pos_bytes = target;
	d->chan_len = 0;
	d->chan_read = 0;
	d->pcm_frames = 0;
	d->pcm_read = 0;
	d->dop_phase = 0;
	return true;
}

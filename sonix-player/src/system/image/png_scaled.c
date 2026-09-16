#include "png_scaled.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// miniz's streaming inflate, compiled into this translation unit. Nothing
// else links it, so there are no symbol clashes to manage.
#include "miniz/tinfl_impl.inc"

// ---------------------------------------------------------------------------
// PNG plumbing
// ---------------------------------------------------------------------------

typedef struct {
	int width, height;
	int bit_depth, color_type, interlace;
	int channels; // bytes per pixel in the raw scanline (a palette index is 1)

	const uint8_t *palette; // PLTE, 3 bytes per entry, or NULL
	int palette_entries;
} png_info_t;

static uint32_t be32(const uint8_t *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int paeth(int a, int b, int c) {
	int p = a + b - c;
	int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
	if (pa <= pb && pa <= pc)
		return a;
	if (pb <= pc)
		return b;
	return c;
}

// The row consumer: unfilters one scanline, expands it to RGB, and folds it
// into the box-average accumulators; every k-th row it emits one output row.
typedef struct {
	png_info_t info;
	int k;		  // downscale factor
	int out_w, out_h;

	uint8_t *row;	   // current filtered scanline, info.channels * width
	uint8_t *prev_row; // previous unfiltered scanline
	size_t row_bytes;

	uint32_t *acc;	// out_w * 3 accumulators
	int rows_in_acc;
	int src_row;	// next source row index
	int out_row;	// next output row index

	uint8_t *out; // out_w * out_h * 3
} png_stream_t;

// Turns one unfiltered scanline into RGB samples added to the accumulators.
static void accumulate_row(png_stream_t *s, const uint8_t *line) {
	const png_info_t *in = &s->info;
	int k = s->k;

	for (int x = 0; x < in->width; x++) {
		int ox = x / k;
		if (ox >= s->out_w) {
			break;
		}

		uint8_t r, g, b;
		switch (in->color_type) {
		case 0: // grayscale
			r = g = b = line[x];
			break;
		case 2: // RGB
			r = line[x * 3];
			g = line[x * 3 + 1];
			b = line[x * 3 + 2];
			break;
		case 3: { // palette
			int idx = line[x];
			if (idx >= in->palette_entries) {
				idx = 0;
			}
			r = in->palette[idx * 3];
			g = in->palette[idx * 3 + 1];
			b = in->palette[idx * 3 + 2];
			break;
		}
		case 4: // gray + alpha (alpha dropped: covers are opaque)
			r = g = b = line[x * 2];
			break;
		case 6: // RGBA
		default:
			r = line[x * 4];
			g = line[x * 4 + 1];
			b = line[x * 4 + 2];
			break;
		}

		uint32_t *a = &s->acc[(size_t)ox * 3];
		a[0] += r;
		a[1] += g;
		a[2] += b;
	}
}

// Finishes one band of k source rows into one output row.
static void flush_band(png_stream_t *s) {
	if (s->out_row >= s->out_h || s->rows_in_acc == 0) {
		return;
	}

	uint8_t *dst = s->out + (size_t)s->out_row * s->out_w * 3;
	for (int ox = 0; ox < s->out_w; ox++) {
		// Cells at the right edge may cover fewer than k columns; using the
		// nominal k*rows divisor darkens them slightly, so count for real.
		int cols = s->k;
		if ((ox + 1) * s->k > s->info.width) {
			cols = s->info.width - ox * s->k;
		}
		uint32_t div = (uint32_t)cols * s->rows_in_acc;

		uint32_t *a = &s->acc[(size_t)ox * 3];
		dst[ox * 3] = (uint8_t)(a[0] / div);
		dst[ox * 3 + 1] = (uint8_t)(a[1] / div);
		dst[ox * 3 + 2] = (uint8_t)(a[2] / div);
	}

	memset(s->acc, 0, (size_t)s->out_w * 3 * sizeof(uint32_t));
	s->rows_in_acc = 0;
	s->out_row++;
}

// Consumes one complete filtered scanline (filter byte + data).
static void process_scanline(png_stream_t *s, uint8_t filter, uint8_t *data) {
	int bpp = s->info.channels; // bytes per pixel at 8-bit depth
	size_t n = s->row_bytes;

	switch (filter) {
	case 0:
		break;
	case 1: // Sub
		for (size_t i = bpp; i < n; i++) {
			data[i] = (uint8_t)(data[i] + data[i - bpp]);
		}
		break;
	case 2: // Up
		for (size_t i = 0; i < n; i++) {
			data[i] = (uint8_t)(data[i] + s->prev_row[i]);
		}
		break;
	case 3: // Average
		for (size_t i = 0; i < n; i++) {
			int left = i >= (size_t)bpp ? data[i - bpp] : 0;
			data[i] = (uint8_t)(data[i] + ((left + s->prev_row[i]) >> 1));
		}
		break;
	case 4: // Paeth
		for (size_t i = 0; i < n; i++) {
			int left = i >= (size_t)bpp ? data[i - bpp] : 0;
			int up_left = i >= (size_t)bpp ? s->prev_row[i - bpp] : 0;
			data[i] = (uint8_t)(data[i] + paeth(left, s->prev_row[i], up_left));
		}
		break;
	default:
		break; // corrupt filter byte: keep going, the row will just look wrong
	}

	accumulate_row(s, data);
	memcpy(s->prev_row, data, n);

	s->rows_in_acc++;
	s->src_row++;
	if (s->rows_in_acc == s->k || s->src_row == s->info.height) {
		flush_band(s);
	}
}

uint8_t *png_scaled_decode(const uint8_t *data, size_t size, int min_box, int *out_w, int *out_h) {
	static const uint8_t magic[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	if (!data || size < 45 || memcmp(data, magic, 8) != 0 || !out_w || !out_h) {
		return NULL;
	}

	// --- chunk walk: IHDR, PLTE, and where the IDAT stream lives ---
	png_info_t info;
	memset(&info, 0, sizeof(info));

	// First pass: find IHDR and PLTE, and confirm at least one IDAT exists.
	// The IDAT chunks themselves are walked lazily during the inflate below.
	// Writers emit them in slices of a few kilobytes, so one large image can carry
	// hundreds; there is no fixed bound on how many.
	size_t pos = 8;
	bool have_ihdr = false;
	bool have_idat = false;

	while (pos + 8 <= size) {
		uint32_t len = be32(data + pos);
		const uint8_t *type = data + pos + 4;
		size_t body = pos + 8;
		if (body + len > size) {
			break;
		}

		if (memcmp(type, "IHDR", 4) == 0 && len >= 13) {
			info.width = (int)be32(data + body);
			info.height = (int)be32(data + body + 4);
			info.bit_depth = data[body + 8];
			info.color_type = data[body + 9];
			info.interlace = data[body + 12];
			have_ihdr = true;
		} else if (memcmp(type, "PLTE", 4) == 0) {
			info.palette = data + body;
			info.palette_entries = (int)(len / 3);
		} else if (memcmp(type, "IDAT", 4) == 0) {
			have_idat = true;
		} else if (memcmp(type, "IEND", 4) == 0) {
			break;
		}

		pos = body + len + 4; // skip CRC
	}

	if (!have_ihdr || !have_idat || info.width < 1 || info.height < 1) {
		return NULL;
	}
	// 8-bit non-interlaced only; everything else is rare enough for stb.
	if (info.bit_depth != 8 || info.interlace != 0) {
		return NULL;
	}
	switch (info.color_type) {
	case 0:
		info.channels = 1;
		break;
	case 2:
		info.channels = 3;
		break;
	case 3:
		if (!info.palette) {
			return NULL;
		}
		info.channels = 1;
		break;
	case 4:
		info.channels = 2;
		break;
	case 6:
		info.channels = 4;
		break;
	default:
		return NULL;
	}

	// --- output geometry ---
	if (min_box < 1) {
		min_box = 1;
	}
	int short_side = info.width < info.height ? info.width : info.height;
	int k = short_side / min_box;
	if (k < 1) {
		k = 1;
	}

	// The same ceiling as the JPEG path. The caller already doubles min_box for
	// quality headroom, but on a huge source that headroom yields an output of
	// many megabytes. Past this short side the factor is raised further, never
	// going below half of min_box -- that is, the box actually asked for.
	#define PNG_OUT_MAX_SHORT 1024
	while (short_side / k > PNG_OUT_MAX_SHORT && short_side / (k + 1) >= min_box / 2) {
		k++;
	}

	png_stream_t s;
	memset(&s, 0, sizeof(s));
	s.info = info;
	s.k = k;
	s.out_w = info.width / k;
	s.out_h = info.height / k;
	if (s.out_w < 1 || s.out_h < 1) {
		return NULL;
	}
	s.row_bytes = (size_t)info.width * info.channels;

	s.row = malloc(s.row_bytes);
	s.prev_row = calloc(1, s.row_bytes);
	s.acc = calloc((size_t)s.out_w * 3, sizeof(uint32_t));
	s.out = malloc((size_t)s.out_w * s.out_h * 3);

	tinfl_decompressor *inflator = malloc(sizeof(tinfl_decompressor));
	uint8_t *window = malloc(TINFL_LZ_DICT_SIZE); // 32 KB inflate window

	bool ok = s.row && s.prev_row && s.acc && s.out && inflator && window;

	if (ok) {
		tinfl_init(inflator);

		// The IDAT walk, restartable: `chunk_pos` is where the next chunk search
		// resumes, so the inflate can be fed one chunk at a time.
		size_t chunk_pos = 8;
		const uint8_t *in = NULL;
		size_t in_left = 0;
		bool last_chunk = false;

		// The inflate window fills and drains into scanlines as decoding runs.
		size_t window_pos = 0;
		size_t line_have = 0; // bytes of the current scanline gathered so far
		uint8_t filter = 0;
		bool have_filter = false;
		int rows_done = 0;

		while (ok && rows_done < info.height) {
			// Refill the input from the next IDAT chunk when drained.
			if (in_left == 0 && !last_chunk) {
				in = NULL;
				while (chunk_pos + 8 <= size) {
					uint32_t clen = be32(data + chunk_pos);
					const uint8_t *ctype = data + chunk_pos + 4;
					size_t cbody = chunk_pos + 8;
					if (cbody + clen > size) {
						break;
					}
					chunk_pos = cbody + clen + 4;

					if (memcmp(ctype, "IDAT", 4) == 0) {
						in = data + cbody;
						in_left = clen;
						break;
					}
					if (memcmp(ctype, "IEND", 4) == 0) {
						break;
					}
				}
				if (!in) {
					last_chunk = true; // no more compressed input anywhere
				}
			}

			{
				size_t in_bytes = in_left;
				size_t out_bytes = TINFL_LZ_DICT_SIZE - window_pos;

				tinfl_status status =
					tinfl_decompress(inflator, in, &in_bytes, window, window + window_pos, &out_bytes,
									 (!last_chunk ? TINFL_FLAG_HAS_MORE_INPUT : 0) | TINFL_FLAG_PARSE_ZLIB_HEADER);

				in += in_bytes;
				in_left -= in_bytes;

				// Drain the fresh window bytes into scanlines.
				uint8_t *fresh = window + window_pos;
				size_t fresh_len = out_bytes;
				while (fresh_len > 0 && rows_done < info.height) {
					if (!have_filter) {
						filter = *fresh++;
						fresh_len--;
						have_filter = true;
						continue;
					}
					size_t need = s.row_bytes - line_have;
					size_t take = fresh_len < need ? fresh_len : need;
					memcpy(s.row + line_have, fresh, take);
					line_have += take;
					fresh += take;
					fresh_len -= take;

					if (line_have == s.row_bytes) {
						process_scanline(&s, filter, s.row);
						line_have = 0;
						have_filter = false;
						rows_done++;
					}
				}

				window_pos = (window_pos + out_bytes) & (TINFL_LZ_DICT_SIZE - 1);

				if (status == TINFL_STATUS_DONE) {
					break;
				}
				if (status < TINFL_STATUS_DONE) {
					ok = false; // corrupt stream
					break;
				}
				if (status == TINFL_STATUS_NEEDS_MORE_INPUT && last_chunk) {
					break; // truncated: emit whatever arrived
				}
				// NEEDS_MORE_INPUT refills from the next chunk at the loop
				// top; HAS_MORE_OUTPUT loops again on the now-drained window.
			}
		}
		if (rows_done < info.height) {
			// Truncated file: emit what accumulated so the last band shows.
			flush_band(&s);
		}
		ok = ok && s.out_row > 0;
		// A partial image is worse than none: anything short of the full height
		// is discarded.
		if (ok && s.out_row < s.out_h) {
			ok = false;
		}
	}

	free(window);
	free(inflator);
	free(s.acc);
	free(s.prev_row);
	free(s.row);

	if (!ok) {
		free(s.out);
		return NULL;
	}

	*out_w = s.out_w;
	*out_h = s.out_h;
	return s.out;
}

#include "albumtone.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// OKLab
//
// Björn Ottosson's OKLab: perceptual, so that "how colourful" and "which
// colour" are distances and angles a person would agree with, and the same
// space the palette engine builds the screen in (sx_oklch_to_565 in the
// design handoff). The two have to agree, or a cover analysed here as orange
// is painted as a red there.
// ---------------------------------------------------------------------------

// sRGB byte to linear light, once: a pow() per channel per pixel would be the
// most expensive thing in the loop.
static float srgb_linear[256];
static pthread_once_t srgb_once = PTHREAD_ONCE_INIT;

static void srgb_table_fill(void) {
	for (int i = 0; i < 256; i++) {
		float c = (float)i / 255.0f;
		srgb_linear[i] = c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
	}
}

void albumtone_oklab(uint8_t r, uint8_t g, uint8_t b, float *L, float *a, float *bb) {
	pthread_once(&srgb_once, srgb_table_fill);
	float lr = srgb_linear[r], lg = srgb_linear[g], lb = srgb_linear[b];
	float l = 0.4122214708f * lr + 0.5363325363f * lg + 0.0514459929f * lb;
	float m = 0.2119034982f * lr + 0.6806995451f * lg + 0.1073969566f * lb;
	float s = 0.0883024619f * lr + 0.2817188376f * lg + 0.6299787005f * lb;
	float l_ = cbrtf(l), m_ = cbrtf(m), s_ = cbrtf(s);
	*L = 0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_;
	*a = 1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_;
	*bb = 0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_;
}

// ---------------------------------------------------------------------------
// The hue
// ---------------------------------------------------------------------------

// At most this many sample points each way. 64 x 64 is four thousand points:
// enough that a family covering 2% of the sleeve is eighty of them, few enough
// that the cube roots cost a few milliseconds on the device.
#define TONE_GRID 64

// 36 bins of 10 degrees; a family is a bin and its two neighbours.
#define TONE_BINS 36

// A point with less chroma than this has no hue worth counting: the rounding
// noise of a JPEG grey, a paper texture, a skin tone in shadow. OKLab chroma of
// a mid grey's noise is under 0.01, of a dull beige about 0.03, of a clear
// colour 0.08 and up.
#define TONE_MIN_CHROMA 0.035f

// Nor does a point too dark or too light for its hue to show: near black, a
// JPEG's chroma is mostly quantisation, and near white every hue looks the
// same.
#define TONE_MIN_LIGHT 0.20f
#define TONE_MAX_LIGHT 0.96f

// A family has to cover this much of the sleeve to be noticed at all: a red
// logo the size of a stamp on a grey photograph is not the sleeve's colour.
#define TONE_NOTICE_SHARE 0.02f

typedef struct {
	double weight;		 // sum of chroma^2: what the bins compete with
	double sum_c;		 // sum of chroma
	double sum_a, sum_b; // a and b weighted by chroma: the family's direction
	double sum_l;		 // lightness weighted by chroma
	double sum_cc;		 // chroma weighted by chroma
	unsigned count;		 // points
} tone_bin_t;

bool albumtone_extract(const uint8_t *rgb, int w, int h, albumtone_t *out) {
	memset(out, 0, sizeof(*out));
	if (!rgb || w <= 0 || h <= 0) {
		return false;
	}

	int step = ((w > h ? w : h) + TONE_GRID - 1) / TONE_GRID;
	if (step < 1) {
		step = 1;
	}

	tone_bin_t bins[TONE_BINS];
	memset(bins, 0, sizeof(bins));
	unsigned samples = 0;
	double all_l = 0, all_c = 0;

	for (int y = step / 2; y < h; y += step) {
		const uint8_t *row = rgb + (size_t)y * w * 3;
		for (int x = step / 2; x < w; x += step) {
			const uint8_t *px = row + (size_t)x * 3;
			float L, a, b;
			albumtone_oklab(px[0], px[1], px[2], &L, &a, &b);
			float c = sqrtf(a * a + b * b);
			samples++;
			all_l += L;
			all_c += c;
			if (c < TONE_MIN_CHROMA || L < TONE_MIN_LIGHT || L > TONE_MAX_LIGHT) {
				continue;
			}
			float deg = atan2f(b, a) * (180.0f / (float)M_PI);
			if (deg < 0) {
				deg += 360.0f;
			}
			int bin = (int)(deg / (360.0f / TONE_BINS));
			if (bin >= TONE_BINS) {
				bin = TONE_BINS - 1;
			}
			tone_bin_t *t = &bins[bin];
			t->weight += (double)c * c;
			t->sum_c += c;
			t->sum_a += (double)a * c;
			t->sum_b += (double)b * c;
			t->sum_l += (double)L * c;
			t->sum_cc += (double)c * c;
			t->count++;
		}
	}
	if (!samples) {
		return false;
	}

	// The strongest noticeable family.
	unsigned notice = (unsigned)ceilf(TONE_NOTICE_SHARE * (float)samples);
	if (notice < 1) {
		notice = 1;
	}
	int best = -1;
	double best_weight = 0;
	for (int i = 0; i < TONE_BINS; i++) {
		const tone_bin_t *p = &bins[(i + TONE_BINS - 1) % TONE_BINS];
		const tone_bin_t *n = &bins[(i + 1) % TONE_BINS];
		unsigned count = p->count + bins[i].count + n->count;
		double weight = p->weight + bins[i].weight + n->weight;
		if (count >= notice && weight > best_weight) {
			best = i;
			best_weight = weight;
		}
	}

	if (best < 0) {
		out->coloured = false;
		out->chroma = (float)(all_c / samples);
		out->light = (float)(all_l / samples);
		return true;
	}

	double sum_c = 0, sum_a = 0, sum_b = 0, sum_l = 0, sum_cc = 0;
	unsigned count = 0;
	for (int d = -1; d <= 1; d++) {
		const tone_bin_t *t = &bins[(best + d + TONE_BINS) % TONE_BINS];
		sum_c += t->sum_c;
		sum_a += t->sum_a;
		sum_b += t->sum_b;
		sum_l += t->sum_l;
		sum_cc += t->sum_cc;
		count += t->count;
	}
	float deg = (float)(atan2(sum_b, sum_a) * (180.0 / M_PI));
	if (deg < 0) {
		deg += 360.0f;
	}
	if (deg >= 360.0f) {
		deg -= 360.0f;
	}
	out->coloured = true;
	out->hue = deg;
	// Weighted by chroma, so the vivid points of the family say how vivid it
	// is and its shaded edges do not pull it towards grey.
	out->chroma = (float)(sum_cc / sum_c);
	out->light = (float)(sum_l / sum_c);
	out->share = (float)count / (float)samples;
	return true;
}

uint16_t albumtone_hash_hue(const char *title) {
	// FNV-1a over the title folded to lower case in ASCII, which is how SQLite's
	// NOCASE -- the library's album comparison -- folds it: two spellings the
	// library takes for one album get one hue.
	uint32_t h = 2166136261u;
	for (const unsigned char *p = (const unsigned char *)(title ? title : ""); *p; p++) {
		unsigned c = *p;
		if (c >= 'A' && c <= 'Z') {
			c += 'a' - 'A';
		}
		h ^= c;
		h *= 16777619u;
	}
	// The low bits of FNV are its weakest: fold the top in before the modulo.
	h ^= h >> 15;
	h *= 0x2c1b3c6du;
	h ^= h >> 12;
	return (uint16_t)(h % 360u);
}

// ---------------------------------------------------------------------------
// The square
// ---------------------------------------------------------------------------

// Rounded to the nearest 5- and 6-bit level, as cover.c packs its covers: on a
// flat grey the difference between this and a plain cut is the difference
// between grey and a faint tint.
static inline uint16_t pack565(uint32_t r, uint32_t g, uint32_t b) {
	uint32_t r5 = (r * 249 + 1014) >> 11;
	uint32_t g6 = (g * 253 + 505) >> 10;
	uint32_t b5 = (b * 249 + 1014) >> 11;
	return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

// Weights in 1/AREA_ONE of an output pixel along each axis, adding up to
// exactly AREA_ONE (see cover.c's resample_area, which this follows): 12 bits
// an axis and 8-bit samples stay within 32 bits.
#define AREA_SHIFT 12
#define AREA_ONE (1 << AREA_SHIFT)

typedef struct {
	int first;
	int count;
	int at; // where its weights start
} span_t;

static bool area_axis(int src_len, int dst_len, span_t *spans, uint16_t *weights) {
	int at = 0;
	for (int d = 0; d < dst_len; d++) {
		int64_t lo = (int64_t)d * src_len;
		int64_t hi = lo + src_len;
		int first = (int)(lo / dst_len);
		int last = (int)((hi - 1) / dst_len);
		if (last >= src_len) {
			last = src_len - 1;
		}
		spans[d].first = first;
		spans[d].count = last - first + 1;
		spans[d].at = at;
		int sum = 0, biggest = at;
		for (int s = first; s <= last; s++) {
			int64_t s_lo = (int64_t)s * dst_len;
			int64_t s_hi = s_lo + dst_len;
			int64_t overlap = (hi < s_hi ? hi : s_hi) - (lo > s_lo ? lo : s_lo);
			int wt = (int)((overlap * AREA_ONE + src_len / 2) / src_len);
			weights[at] = (uint16_t)wt;
			if (wt > weights[biggest]) {
				biggest = at;
			}
			sum += wt;
			at++;
		}
		int fixed = weights[biggest] + (AREA_ONE - sum);
		if (fixed < 0 || fixed > AREA_ONE) {
			return false;
		}
		weights[biggest] = (uint16_t)fixed;
	}
	return true;
}

static bool square_by_area(const uint8_t *rgb, int w, int x0, int y0, int src, int side, uint16_t *out) {
	size_t row_len = (size_t)side * 3;
	span_t *spans = malloc((size_t)side * sizeof(*spans));
	uint16_t *weights = malloc((size_t)(src + side) * sizeof(*weights));
	uint32_t *acc = malloc(row_len * sizeof(*acc));
	uint32_t *line = malloc(row_len * sizeof(*line));
	// The crop is square, so one table serves both axes.
	bool ok = spans && weights && acc && line && area_axis(src, side, spans, weights);
	if (ok) {
		int cached = -1;
		for (int y = 0; y < side; y++) {
			memset(acc, 0, row_len * sizeof(*acc));
			for (int k = 0; k < spans[y].count; k++) {
				int s = spans[y].first + k;
				uint32_t wy = weights[spans[y].at + k];
				if (s != cached) {
					const uint8_t *in = rgb + ((size_t)(y0 + s) * w + x0) * 3;
					uint32_t *o = line;
					for (int x = 0; x < side; x++) {
						const uint8_t *px = in + (size_t)spans[x].first * 3;
						const uint16_t *wx = weights + spans[x].at;
						uint32_t r = 0, g = 0, b = 0;
						for (int i = 0; i < spans[x].count; i++) {
							r += wx[i] * (uint32_t)px[0];
							g += wx[i] * (uint32_t)px[1];
							b += wx[i] * (uint32_t)px[2];
							px += 3;
						}
						o[0] = r;
						o[1] = g;
						o[2] = b;
						o += 3;
					}
					cached = s;
				}
				for (size_t i = 0; i < row_len; i++) {
					acc[i] += wy * line[i];
				}
			}
			uint16_t *dst = out + (size_t)y * side;
			const uint32_t half = 1u << (2 * AREA_SHIFT - 1);
			for (int x = 0; x < side; x++) {
				dst[x] = pack565((acc[x * 3] + half) >> (2 * AREA_SHIFT), (acc[x * 3 + 1] + half) >> (2 * AREA_SHIFT),
								 (acc[x * 3 + 2] + half) >> (2 * AREA_SHIFT));
			}
		}
	}
	free(spans);
	free(weights);
	free(acc);
	free(line);
	return ok;
}

// A source smaller than the square: bilinear, sampled at the centre of each
// output pixel, in 8-bit fixed point.
static void square_by_interpolation(const uint8_t *rgb, int w, int x0, int y0, int src, int side, uint16_t *out) {
	for (int y = 0; y < side; y++) {
		int fy = (int)(((int64_t)(2 * y + 1) * src * 256) / (2 * side)) - 128;
		if (fy < 0) {
			fy = 0;
		}
		int sy = fy >> 8, ty = fy & 255;
		int sy1 = sy + 1 < src ? sy + 1 : sy;
		for (int x = 0; x < side; x++) {
			int fx = (int)(((int64_t)(2 * x + 1) * src * 256) / (2 * side)) - 128;
			if (fx < 0) {
				fx = 0;
			}
			int sx = fx >> 8, tx = fx & 255;
			int sx1 = sx + 1 < src ? sx + 1 : sx;
			const uint8_t *p00 = rgb + ((size_t)(y0 + sy) * w + x0 + sx) * 3;
			const uint8_t *p10 = rgb + ((size_t)(y0 + sy) * w + x0 + sx1) * 3;
			const uint8_t *p01 = rgb + ((size_t)(y0 + sy1) * w + x0 + sx) * 3;
			const uint8_t *p11 = rgb + ((size_t)(y0 + sy1) * w + x0 + sx1) * 3;
			uint32_t c[3];
			for (int i = 0; i < 3; i++) {
				uint32_t top = (uint32_t)p00[i] * (256 - tx) + (uint32_t)p10[i] * tx;
				uint32_t bottom = (uint32_t)p01[i] * (256 - tx) + (uint32_t)p11[i] * tx;
				c[i] = (top * (256 - ty) + bottom * ty + 32768) >> 16;
			}
			out[(size_t)y * side + x] = pack565(c[0], c[1], c[2]);
		}
	}
}

bool albumtone_square565(const uint8_t *rgb, int w, int h, int side, uint16_t *out) {
	if (!rgb || !out || w <= 0 || h <= 0 || side <= 0) {
		return false;
	}
	int src = w < h ? w : h;
	int x0 = (w - src) / 2;
	int y0 = (h - src) / 2;
	if (src >= side) {
		return square_by_area(rgb, w, x0, y0, src, side, out);
	}
	square_by_interpolation(rgb, w, x0, y0, src, side, out);
	return true;
}

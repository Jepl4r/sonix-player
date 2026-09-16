#include "jpeg_scaled.h"

#include <stdlib.h>
#include <string.h>

// The vendored TJpgDec is compiled into this translation unit under renamed
// entry points, because LVGL links its own copy of the same library (with
// scaling compiled OUT) and the two would otherwise collide at link time.
#define jd_prepare sonix_jd_prepare
#define jd_decomp sonix_jd_decomp
#define jd_mcu_load sonix_jd_mcu_load
#define jd_mcu_output sonix_jd_mcu_output
#define jd_restart sonix_jd_restart
#include "tjpgd/tjpgd_impl.inc"

// Work pool for the decoder's tables and MCU buffers. The documented minimum
// is ~3.1 KB; double it for comfort with 4:2:0 files.
#define JPEG_POOL_BYTES 8192

typedef struct {
	const uint8_t *data;
	size_t size;
	size_t pos;

	uint8_t *out; // RGB888, out_w * out_h * 3
	int out_w;
	int out_h;
} jpeg_io_t;

// TJpgDec pulls compressed bytes through this. A NULL buffer means "skip".
static size_t in_func(JDEC *jd, uint8_t *buf, size_t len) {
	jpeg_io_t *io = jd->device;

	size_t remaining = io->size - io->pos;
	if (len > remaining) {
		len = remaining;
	}
	if (buf) {
		memcpy(buf, io->data + io->pos, len);
	}
	io->pos += len;
	return len;
}

// Receives one decoded MCU rectangle, already scaled.
//
// NOTE the channel order: LVGL's copy of TJpgDec is patched to emit B,G,R
// (see the /*B*/ /*G*/ /*R*/ lines in tjpgd_impl.inc's mcu_output) where the
// upstream library emits R,G,B. The swap below turns it back into the RGB888
// everything downstream expects.
static int out_func(JDEC *jd, void *bitmap, JRECT *rect) {
	jpeg_io_t *io = jd->device;

	int rect_w = rect->right - rect->left + 1;
	int rect_h = rect->bottom - rect->top + 1;
	const uint8_t *src = bitmap;

	for (int y = 0; y < rect_h; y++) {
		int out_y = rect->top + y;
		if (out_y >= io->out_h) {
			break;
		}

		int copy_w = rect_w;
		if (rect->left + copy_w > io->out_w) {
			copy_w = io->out_w - rect->left;
		}
		if (copy_w <= 0) {
			continue;
		}

		const uint8_t *s = src + (size_t)y * rect_w * 3;
		uint8_t *d = io->out + ((size_t)out_y * io->out_w + rect->left) * 3;
		for (int x = 0; x < copy_w; x++) {
			d[0] = s[2]; // R
			d[1] = s[1]; // G
			d[2] = s[0]; // B
			s += 3;
			d += 3;
		}
	}

	return 1; // keep going
}

uint8_t *jpeg_scaled_decode(const uint8_t *data, size_t size, int min_box, int *out_w, int *out_h,
							int *src_w, int *src_h) {
	if (!data || size == 0 || !out_w || !out_h) {
		return NULL;
	}

	void *pool = malloc(JPEG_POOL_BYTES);
	if (!pool) {
		return NULL;
	}

	jpeg_io_t io = {.data = data, .size = size};

	JDEC jd;
	if (sonix_jd_prepare(&jd, in_func, pool, JPEG_POOL_BYTES, &io) != JDR_OK) {
		free(pool);
		return NULL; // progressive, broken, or not a JPEG: caller falls back
	}

	// The strongest reduction whose output still gives the caller's resampler
	// at least two source pixels per destination pixel. Below that ratio the
	// area-averaging filter degenerates towards nearest-neighbour and the result
	// looks grainy; beyond it is wasted memory and decode time.
	if (min_box < 1) {
		min_box = 1;
	}
	int quality_floor = min_box * 2;
	int short_side = jd.width < jd.height ? jd.width : jd.height;
	uint8_t scale = 0;
	while (scale < 3) {
		int next_short = short_side >> (scale + 1);
		if (next_short < quality_floor) {
			break;
		}
		scale++;
	}

	// The memory ceiling wins over quality: the two-source-pixels rule on its own
	// lets a very large source produce an output too big to allocate in RGB888.
	// Above this short side the reduction steps up again, as long as that keeps
	// the output at or above the requested box.
	#define JPEG_OUT_MAX_SHORT 1024
	while (scale < 3) {
		int cur_short = short_side >> scale;
		int next_short = short_side >> (scale + 1);
		if (cur_short <= JPEG_OUT_MAX_SHORT || next_short < min_box) {
			break; // either it fits already, or halving would fall below the requested box
		}
		scale++;
	}

	io.out_w = jd.width >> scale;
	io.out_h = jd.height >> scale;
	if (io.out_w < 1 || io.out_h < 1) {
		free(pool);
		return NULL;
	}

	io.out = malloc((size_t)io.out_w * io.out_h * 3);
	if (!io.out) {
		free(pool);
		return NULL;
	}

	JRESULT res = sonix_jd_decomp(&jd, out_func, scale);
	free(pool);

	if (res != JDR_OK) {
		free(io.out);
		return NULL;
	}

	*out_w = io.out_w;
	*out_h = io.out_h;
	if (src_w) {
		*src_w = jd.width;
	}
	if (src_h) {
		*src_h = jd.height;
	}
	return io.out;
}

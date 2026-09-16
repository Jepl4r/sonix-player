#ifndef JPEG_SCALED_H
#define JPEG_SCALED_H

#include <stddef.h>
#include <stdint.h>

// Scaled baseline-JPEG decoding, via a vendored TJpgDec with JD_USE_SCALE on.
//
// This is how a very large cover becomes drawable on 64 MB of RAM: instead of
// stb_image's full-resolution pass, TJpgDec decodes each MCU and downsamples it
// on the way out, so a 1/8-scale decode needs only a few hundred kilobytes of
// output and an 8 KB work pool.
//
// Baseline JPEG only -- progressive files make TJpgDec return an error, and
// the caller falls back to stb_image with its memory budget.

// Decodes `data` to RGB888 at one of the built-in reductions (1/1, 1/2, 1/4,
// 1/8). The reduction chosen keeps the short side at or above twice `min_box`
// where memory allows, so the caller's own resampler has at least two source
// pixels per destination pixel; a very large picture is reduced further, but
// never below `min_box` on the short side. Returns a malloc'd w*h*3 buffer
// (caller frees) or NULL when the file is not a JPEG TJpgDec can read.
//
// `src_w` and `src_h` report the picture's own size, before any reduction, and
// may be NULL. They exist for the log: the output size alone cannot say whether
// anything was reduced.
uint8_t *jpeg_scaled_decode(const uint8_t *data, size_t size, int min_box, int *out_w, int *out_h,
							int *src_w, int *src_h);

#endif /* JPEG_SCALED_H */

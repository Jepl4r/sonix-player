#ifndef JPEG_PLANES_H
#define JPEG_PLANES_H

#include <stddef.h>
#include <stdint.h>

// Decodes a JPEG straight from its component planes into a small RGB888 image,
// without ever building the full-resolution picture.
//
// It exists for progressive JPEGs. TJpgDec (see jpeg_scaled.h) cannot read them
// at all, and stb_image's own loader interleaves a full-size RGB buffer that the
// resampler then throws away -- several megabytes of peak for a large cover, on a
// device with 64 MB. This asks stb for the decode and then averages the Y/Cb/Cr
// planes down to the wanted size itself, so the output stays small whatever the
// source measures.
//
// `box` is the longest edge anybody will ask of the result; the picture keeps
// its aspect ratio and is never enlarged. `peak_bytes` (may be NULL) receives
// what the decode is going to need at its worst moment, so a caller with a
// memory budget can refuse before allocating anything -- it is filled in even
// when the decode is not attempted.
//
// Returns a malloc'd out_w * out_h * 3 buffer, or NULL.
uint8_t *jpeg_planes_decode(const uint8_t *data, size_t size, int box, size_t budget_bytes, int *out_w, int *out_h,
							size_t *peak_bytes);

#endif // JPEG_PLANES_H

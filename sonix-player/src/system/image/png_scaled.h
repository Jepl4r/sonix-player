#ifndef PNG_SCALED_H
#define PNG_SCALED_H

#include <stddef.h>
#include <stdint.h>

// Streaming, downscaling PNG decoding.
//
// stb_image inflates the whole image before handing it over, which for a large
// PNG is tens of megabytes of pixels plus the filtered stream -- over any budget
// a 64 MB device can offer. Unlike progressive JPEG, PNG can be decoded one
// scanline at a time: this decoder inflates the IDAT stream through miniz's
// tinfl, unfilters each row against the previous one, and box-averages k x k
// blocks straight into the output. Peak memory is two rows, a 32 KB inflate
// window and the (small) output, whatever the source size.
//
// Handles 8-bit non-interlaced PNGs in grayscale, gray+alpha, RGB, RGBA and
// palette form -- which is every album cover in practice. Interlaced
// (Adam7) and 16-bit files return NULL and the caller falls back to
// stb_image under its memory budget.

// Decodes `data` to RGB888, downscaled by the largest integer factor that
// keeps the short side at or above min_box (factor 1 if it already is). A very
// large source is reduced further, to a short side of about 1024, but never
// below half of min_box. Returns a malloc'd w*h*3 buffer (caller frees) or NULL
// when the file is not a PNG this decoder reads.
uint8_t *png_scaled_decode(const uint8_t *data, size_t size, int min_box, int *out_w, int *out_h);

#endif /* PNG_SCALED_H */

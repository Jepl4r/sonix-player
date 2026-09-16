#ifndef PNG_WRITE_H
#define PNG_WRITE_H

#include <stdbool.h>
#include <stdint.h>

// Writes a truecolour PNG from an RGB565 frame, that is from a framebuffer page
// as it stands.
//
// It serves screenshots and nothing else, and is written accordingly: one input
// format, no transparency, no interlacing, filter zero on every row. The
// resulting PNG opens anywhere.
//
// Compression uses the device's libz, opened at run time as with libfdk-aac and
// libsndfile. Without it the file is still written, using "stored" deflate
// blocks: a valid PNG, as large as the pixels.
bool png_write_rgb565(const char *path, const uint16_t *pixels, int width, int height);

#endif /* PNG_WRITE_H */

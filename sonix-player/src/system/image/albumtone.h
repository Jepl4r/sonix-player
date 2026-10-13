#ifndef ALBUMTONE_H
#define ALBUMTONE_H

#include <stdbool.h>
#include <stdint.h>

// The pixel work behind the cover pipeline of Sonix UI 2.0 (covertone.h): the
// one hue a screen is painted in, and the small square pictures a list row and
// an album page draw before the real cover is decoded.
//
// Pure functions over a decoded RGB888 buffer -- no files, no threads, no
// LVGL -- so they can be tested on a desktop on their own
// (tests/covertone_test.c) and called from whichever thread decoded the
// picture.

// What a sleeve looks like, as one OKLCH colour.
//
// The hue is the palette engine's input: every colour of a screen is that hue
// at the lightness and chroma the design gives it (sonix_ui_tokens.h, SX_T_*),
// so the number has to be an OKLab hue -- HSV's red sits about 30 degrees away
// from OKLab's, and a palette built on the wrong one comes out in the wrong
// colour.
typedef struct {
	bool coloured; // a hue worth painting with was found; false for a grey,
				   // black-and-white or nearly colourless sleeve
	float hue;	   // OKLCH hue of that colour, degrees 0..360 (0 when !coloured)
	float chroma;  // its chroma, 0..~0.37 (the sleeve's mean chroma when !coloured)
	float light;   // its lightness, 0..1 (the sleeve's mean lightness when !coloured)
	float share;   // how much of the sleeve the chosen family of hues covers, 0..1
} albumtone_t;

// The most saturated noticeable hue of a picture, not its average.
//
// An average of a sleeve is mud: a red and blue cover averages to a grey
// purple that is on neither, and the large dull areas every photograph has
// drown the colour a person would name. So the picture is sampled on a grid
// of at most 64 x 64 points, each point taken to OKLab, and the points with
// enough colour (chroma) at a lightness where a hue means something vote into
// 36 bins of 10 degrees with a weight of chroma squared: a vivid patch counts
// for more than a larger washed-out one, but a speck does not count at all --
// a family of three neighbouring bins has to cover at least 2% of the picture
// to be noticed. The strongest family wins, and its colour is the
// chroma-weighted mean of its points.
//
// A picture where no family is noticeable is grey: `coloured` false, and the
// caller paints with the title's hash instead (albumtone_hash_hue).
//
// `rgb` is w * h * 3 bytes. False only when there is nothing to look at.
bool albumtone_extract(const uint8_t *rgb, int w, int h, albumtone_t *out);

// The hue an album without a usable picture is painted in: a hash of its
// title, folded to lower case in ASCII (the library compares album names
// that way), spread over the circle. The same title always gives the same
// hue, on every device and in every release that keeps this function.
uint16_t albumtone_hash_hue(const char *title);

// A square picture of side x side in RGB565 (LVGL's native layout: one
// uint16_t per pixel, red in the top bits) made from the centre square of
// `rgb` (w x h, RGB888): averaged by area when shrinking, interpolated when the
// source is smaller than the square. `out` holds side * side values. False on
// a bad size or when the scratch memory cannot be had (a few kilobytes).
bool albumtone_square565(const uint8_t *rgb, int w, int h, int side, uint16_t *out);

// One sRGB colour in OKLab. For the tests, and for anything that wants to
// compare a pixel with a stored tone.
void albumtone_oklab(uint8_t r, uint8_t g, uint8_t b, float *L, float *a, float *bb);

#endif // ALBUMTONE_H

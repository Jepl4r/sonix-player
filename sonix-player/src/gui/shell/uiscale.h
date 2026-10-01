#ifndef UISCALE_H
#define UISCALE_H

#include <stdint.h>

// The interface is laid out in the pixels of a 480-wide panel. A player whose
// panel is smaller (the original R3's, 360x480) scales every one of those
// numbers by the same factor instead of shrinking the finished picture:
// the text is then drawn at its real size, sharp, and nothing is resampled on
// the way to the panel.
//
// ui_px() is that scaling. Every pixel quantity in the pages goes through it
// -- sizes, positions, pads, radii, border widths -- and nothing else does:
// not percentages, not opacities, not counts, not durations.
//
// At 1:1, which is every player but the R3, ui_px() returns its argument
// untouched, so those players are laid out exactly as before, to the pixel.

// Set once, before the first font is opened or the first object is built.
// num/den of 3/4 draws the interface at three quarters.
void ui_scale_set(int num, int den);

extern int ui_scale_num;
extern int ui_scale_den;

// Rounded to the nearest pixel, halves away from zero, so a 1-pixel border
// stays one pixel and a negative offset shrinks like a positive one.
static inline int32_t ui_px(int32_t v) {
	if (ui_scale_num == ui_scale_den) {
		return v;
	}
	int32_t half = ui_scale_den / 2;
	return (v * ui_scale_num + (v < 0 ? -half : half)) / ui_scale_den;
}

#endif /* UISCALE_H */

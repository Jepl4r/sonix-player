#ifndef UISCALE_H
#define UISCALE_H

#include <stdint.h>

// The interface's fixed sizes -- paddings, radii, gaps, the size of a button,
// a row or an icon, the fonts -- were drawn for a panel 480 px wide, which the
// R3 Pro II and the R1 both are. ui_px() turns one of them into the panel's
// own: unchanged at 480 px, two thirds of it on the R3 II 2025's 320.
//
// Sizes worked out from the panel itself -- cfg->screen_width, the height left
// under a bar, a percentage -- are the panel's already and do not go through
// it.
#define UI_DESIGN_WIDTH 480

// Before the fonts are opened and before any widget is made.
void ui_scale_init(int panel_width);

// One of those sizes, on this panel. Rounded to the nearest pixel, the same
// way for a negative offset as for a positive one.
int32_t ui_px(int32_t px);

#endif /* UISCALE_H */

#ifndef ICONSCALE_H
#define ICONSCALE_H

#include <stdbool.h>

// The icons in icons.c are drawn for the 480-wide design. A player laid out at
// another scale (uiscale.h) gets them from a set drawn at that scale instead,
// tools/svg_to_lvgl.py's icons-<num>-<den>.bin, kept beside the other
// resources on that player alone: redrawn from the SVGs rather than shrunk, so
// they stay as sharp as the rest, and nothing in the binary grows for the
// players that never use it.
//
// Points every icon_<name> at its drawing in that set, so the pages keep naming
// the same icons whatever the scale. Once, at startup, before any icon is
// shown. False, with the icons left as they are, when there is no set for
// this scale or it was drawn from another list of icons than this build's.
bool icons_load_scaled(int num, int den);

#endif /* ICONSCALE_H */

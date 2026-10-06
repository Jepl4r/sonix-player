// board_profile.h -- the small set of dimensions that differ on the
// TempoTec V1.
//
// The V1 uses the same Linux/LVGL application shape as the HiBy targets but
// has a much smaller 240x320 panel.  Keep that distinction in one place so a
// V1 build can still be tested on the host and the same source remains usable
// on the 480-pixel HiBy players.

#ifndef SRC_GUI_BOARD_PROFILE_H_
#define SRC_GUI_BOARD_PROFILE_H_

#include <stdbool.h>

#include "lvgl/lvgl.h"

// BOARD=tempotec_v1 selects the profile.  BOARD=hiby_r1/hiby_r3proii forces
// the regular profile.  BOARD_DEFAULT_TEMPOTEC_V1 may be defined by a dedicated
// firmware build.  SONIX_PANEL=240x320 also selects it for host simulation.
bool bp_is_tempotec_v1(void);

int bp_screen_w(void);
int bp_screen_h(void);
int bp_padding(void);
int bp_status_bar_h(void);
int bp_tile_radius(void);

// Common compact-shell geometry.  Callers which already have gui_config_t can
// still key off screen_width < 320; these helpers are for code built before the
// display exists (notably fonts_init()).
int bp_header_button_size(void);
int bp_header_button_gap(void);

// Fonts used by the most crowded player/list labels.  On the V1 fonts.c also
// maps the remaining large named UI fonts to compact raster sizes, so pages
// which use font_ui_24 directly do not keep 24 physical pixels on a 240-pixel
// display.
const lv_font_t *bp_title_font(void);
const lv_font_t *bp_artist_font(void);
const lv_font_t *bp_time_font(void);
const lv_font_t *bp_tile_label_font(void);
const lv_font_t *bp_queue_label_font(void);
const lv_font_t *bp_format_label_font(void);

#endif // SRC_GUI_BOARD_PROFILE_H_

// board_profile.h -- runtime detection of the device so the GUI can pick
// sizes, fonts and paddings that fit it.
//
// Why this file exists
// --------------------
// sonix-player is authored against the HiBy R1 (480x640, 3.06 inch). The
// Tempotec V1 keeps the same Ingenic X1000 SoC and the same LVGL stack,
// but ships a 2 inch 240x320 panel. The patches in the tempotec-v1 branch
// are gated on the helpers here so the same binary still runs on the R1.
//
// Detection
// ---------
// The launcher most commonly sets `BOARD` in /etc/init.d before exec'ing
// sonix_player. We honour that. When nothing is set, the build-time
// `-DBOARD_DEFAULT_...` decides. The default is `hiby_r1` so upstream
// behaviour is preserved when this file is simply included.
//
// What callers use
// ----------------
//   bp_is_tempotec_v1()    -- true on the Tempotec V1
//   bp_screen_w() / bp_screen_h()  -- panel pixels
//   bp_title_font()        -- &font_ui_14 on V1, &font_ui_26 on R1
//   bp_artist_font()       -- &font_ui_14 on V1, &font_ui_24 on R1
//   bp_time_font()         -- &font_ui_14 on V1, &font_ui_22 on R1
//   bp_tile_radius()       -- 14 on V1, 20 on R1
//   bp_tile_label_font()   -- &font_ui_14 on V1, default on R1
//   bp_padding()           -- outer page padding in pixels
//   bp_status_bar_h()      -- top status strip height
//
// Anything not overridden here stays at its R1 default; the runtime helper
// for each knob decides whether V1 needs a different value.

#ifndef SRC_GUI_BOARD_PROFILE_H_
#define SRC_GUI_BOARD_PROFILE_H_

#include "lvgl/lvgl.h"

const lv_font_t *bp_title_font(void);
const lv_font_t *bp_artist_font(void);
const lv_font_t *bp_time_font(void);
const lv_font_t *bp_tile_label_font(void);
const lv_font_t *bp_queue_label_font(void);
const lv_font_t *bp_format_label_font(void);

bool bp_is_tempotec_v1(void);
int  bp_screen_w(void);
int  bp_screen_h(void);
int  bp_tile_radius(void);
int  bp_padding(void);
int  bp_status_bar_h(void);

#endif  // SRC_GUI_BOARD_PROFILE_H_

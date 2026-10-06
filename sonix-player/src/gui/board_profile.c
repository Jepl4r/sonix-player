// board_profile.c -- runtime knobs for the Tempotec V1.
//
// The Tempotec V1 (Ingenic X1000, 2" 240x320) shares the OS and most of
// the player code with the HiBy R1. The launcher (or /etc/init.d/S80*)
// sets `BOARD=tempotec_v1`, then exec's the player. Detection in this
// file reads that, falls back to the compile-time default, and exposes
// the per-knob helpers declared in board_profile.h.
//
// The Tempotec V1 has no LV_CONF knobs of its own: everything that is
// board-specific in this patch sits behind the bp_*() helpers. Anywhere
// the player previously hard-coded a font/style, the V1 branch in the
// caller now picks one of these helpers.
//
// No assets are touched here; the Tempotec V1 stock firmware image, when
// dropped under sonix-packer/assets/TV1/, becomes the deployed rootfs.

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"

// ---------- board identity ------------------------------------------------

static bool detect_tempotec_v1(void) {
    const char *b = getenv("BOARD");
    if (b && strcmp(b, "tempotec_v1") == 0) return true;
    if (b && strcmp(b, "hiby_r1")     == 0) return false;
#if defined(BOARD_DEFAULT_TEMPOTEC_V1)
    return true;
#else
    return false;           // default: HiBy R1 -- upstream behaviour
#endif
}

// ---------- per-knob helpers ---------------------------------------------

const lv_font_t *bp_title_font(void)        { return bp_is_tempotec_v1() ? &font_ui_16 : &font_ui_26; }
const lv_font_t *bp_artist_font(void)       { return bp_is_tempotec_v1() ? &font_ui_14 : &font_ui_24; }
const lv_font_t *bp_time_font(void)         { return bp_is_tempotec_v1() ? &font_ui_14 : &font_ui_22; }
const lv_font_t *bp_tile_label_font(void)   { return bp_is_tempotec_v1() ? &font_ui_14 : &font_ui_20_bold; }
const lv_font_t *bp_queue_label_font(void)  { return bp_is_tempotec_v1() ? &font_ui_14 : &font_ui_18; }
const lv_font_t *bp_format_label_font(void) { return bp_is_tempotec_v1() ? &font_ui_14 : &font_ui_18; }

bool bp_is_tempotec_v1(void)      { return detect_tempotec_v1(); }
int  bp_screen_w(void)            { return bp_is_tempotec_v1() ? 240 : 480; }
int  bp_screen_h(void)            { return bp_is_tempotec_v1() ? 320 : 640; }
int  bp_tile_radius(void)         { return bp_is_tempotec_v1() ? 14  : 20;  }
int  bp_padding(void)             { return bp_is_tempotec_v1() ? 6   : 14;  }
int  bp_status_bar_h(void)        { return bp_is_tempotec_v1() ? 24  : 32;  }

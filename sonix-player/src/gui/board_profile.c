// Runtime board profile for the TempoTec V1.

#include "src/gui/board_profile.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "src/gui/fonts/fonts.h"

static bool value_is(const char *value, const char *wanted) {
	return value && strcasecmp(value, wanted) == 0;
}

bool bp_is_tempotec_v1(void) {
	// The environment does not change after exec.  Caching this also keeps the
	// many font/layout queries below from repeatedly parsing it during startup.
	static int cached = -1;
	if (cached >= 0) {
		return cached != 0;
	}

	const char *board = getenv("BOARD");
	if (value_is(board, "tempotec_v1") || value_is(board, "tempotec-v1") || value_is(board, "tv1")) {
		cached = 1;
		return true;
	}
	if (value_is(board, "hiby_r1") || value_is(board, "hiby-r1") || value_is(board, "hiby_r3proii") ||
		value_is(board, "hiby-r3proii")) {
		cached = 0;
		return false;
	}

	// Convenient and unambiguous for the simulator.  On target BOARD should be
	// exported by the launcher, but honouring this also makes a hand-written
	// launcher which only sets the panel size behave consistently.
	const char *panel = getenv("SONIX_PANEL");
	if (panel && (strcasecmp(panel, "240x320") == 0 || strcasecmp(panel, "240X320") == 0)) {
		cached = 1;
		return true;
	}

#if defined(BOARD_DEFAULT_TEMPOTEC_V1)
	cached = 1;
#else
	cached = 0;
#endif
	return cached != 0;
}

int bp_screen_w(void) { return bp_is_tempotec_v1() ? 240 : 480; }
int bp_screen_h(void) { return bp_is_tempotec_v1() ? 320 : 720; }
int bp_padding(void) { return bp_is_tempotec_v1() ? 6 : 15; }
int bp_status_bar_h(void) { return bp_is_tempotec_v1() ? 24 : 44; }
int bp_tile_radius(void) { return bp_is_tempotec_v1() ? 10 : 12; }
int bp_header_button_size(void) { return bp_is_tempotec_v1() ? 36 : 56; }
int bp_header_button_gap(void) { return bp_is_tempotec_v1() ? 4 : 6; }

const lv_font_t *bp_title_font(void) { return bp_is_tempotec_v1() ? &font_ui_16 : &font_ui_26; }
const lv_font_t *bp_artist_font(void) { return bp_is_tempotec_v1() ? &font_ui_14 : &font_ui_24; }
const lv_font_t *bp_time_font(void) { return bp_is_tempotec_v1() ? &font_ui_14 : &font_ui_22; }
const lv_font_t *bp_tile_label_font(void) { return bp_is_tempotec_v1() ? &font_ui_14 : &font_ui_20_bold; }
const lv_font_t *bp_queue_label_font(void) { return bp_is_tempotec_v1() ? &font_ui_14 : &font_ui_18; }
const lv_font_t *bp_format_label_font(void) { return bp_is_tempotec_v1() ? &font_ui_14 : &font_ui_18; }

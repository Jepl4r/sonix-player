#include "parentalpage.h"

#include "lvgl/lvgl.h"

#include "src/gui/shell/morepage.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"

lv_obj_t *parentalpage_screen;

static lv_obj_t *games_switch;

// Five taps, each within this long of the one before. A pause starts the count
// again, so taps spread over a few visits never add up to an opening.
#define PARENTAL_TAPS 5
#define PARENTAL_TAP_GAP_MS 1000

static int taps;
static uint32_t last_tap;

static void row_tapped_cb(lv_event_t *e) {
	(void)e;

	if (taps > 0 && lv_tick_elaps(last_tap) > PARENTAL_TAP_GAP_MS) {
		taps = 0;
	}
	last_tap = lv_tick_get();
	if (++taps < PARENTAL_TAPS) {
		return;
	}
	taps = 0;
	switch_screen(parentalpage_screen);
}

static void games_toggled_cb(lv_event_t *e) {
	(void)e;
	morepage_set_games_shown(lv_obj_has_state(games_switch, LV_STATE_CHECKED));
}

void parentalpage_add_row(lv_obj_t *container) { settingsrow_add(container, "parental_controls", NULL, row_tapped_cb, NULL); }

void parentalpage_init(gui_config_t *cfg) {
	parentalpage_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(parentalpage_screen, cfg, "parental_controls");

	settingsrow_toggle(container, "parental_show_games", &games_switch, games_toggled_cb);
	if (morepage_games_shown()) {
		lv_obj_add_state(games_switch, LV_STATE_CHECKED);
	}

	switcher_attach_back_gesture(parentalpage_screen);
}

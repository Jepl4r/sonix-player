#include "speedpage.h"

#include <stdint.h>
#include <stdio.h>

#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/system/core/config.h"

// ---------------------------------------------------------------------------
// Playback speed
//
// One page for every speed the engine can play at: 0.25x to 4.00x in steps of
// 0.05x on the slider, plus the handful of values people reach for -- a
// podcast at 1.5x, a book slowed down at 0.75x -- as pills. The setting is
// stored in thousandths, the way podcast.c and audiobook.c keep it, and each
// one remembers its own: changing an episode's speed leaves the books' alone.
// ---------------------------------------------------------------------------

#define SPEED_MIN_PERMILLE 250
#define SPEED_MAX_PERMILLE 4000
#define SPEED_STEP_PERMILLE 50
#define SPEED_STEPS ((SPEED_MAX_PERMILLE - SPEED_MIN_PERMILLE) / SPEED_STEP_PERMILLE + 1)

static lv_obj_t *speed_screen;
static lv_obj_t *speed_slider;
static lv_obj_t *speed_value;
static lv_obj_t *step_minus;
static lv_obj_t *step_plus;

#define SPEED_PRESET_COUNT 5
static const int SPEED_PRESETS[SPEED_PRESET_COUNT] = {750, 1000, 1250, 1500, 2000};
static const char *const SPEED_PRESET_LABELS[SPEED_PRESET_COUNT] = {"0.75x", "1.0x", "1.25x", "1.5x", "2.0x"};
static lv_obj_t *speed_presets[SPEED_PRESET_COUNT];

// "1.0x", "1.5x", "2.0x" on tenths, "1.05x", "1.25x", "2.55x" when there are
// hundredths. Integer arithmetic on purpose: the value is a thousandths
// setting, not a float, and the formatting has to agree with it.
static void speed_text(int permille, char *out, size_t size) {
	int whole = permille / 1000;
	int fraction = permille % 1000;
	if (fraction % 100 == 0) {
		snprintf(out, size, "%d.%dx", whole, fraction / 100);
	} else {
		snprintf(out, size, "%d.%02dx", whole, fraction / 10);
	}
}

// Paints the page from the stored setting. Programmatic lv_slider_set_value()
// with LV_ANIM_OFF does not emit VALUE_CHANGED in LVGL 9.6, so writing the
// slider here cannot bounce back through slider_cb().
static void refresh(void) {
	if (!speed_slider) {
		return;
	}

	int permille = player_speed_permille();
	int step = (permille - SPEED_MIN_PERMILLE + SPEED_STEP_PERMILLE / 2) / SPEED_STEP_PERMILLE;
	if (step < 0) {
		step = 0;
	}
	if (step > SPEED_STEPS - 1) {
		step = SPEED_STEPS - 1;
	}
	lv_slider_set_value(speed_slider, step, LV_ANIM_OFF);

	char text[16];
	speed_text(permille, text, sizeof(text));
	lv_label_set_text(speed_value, text);

	for (int i = 0; i < SPEED_PRESET_COUNT; i++) {
		settingsrow_pill_active(speed_presets[i], SPEED_PRESETS[i] == permille);
	}
	// The two step pills are actions, not choices: they never light up.
	settingsrow_pill_active(step_minus, false);
	settingsrow_pill_active(step_plus, false);
}

static void apply(int permille) {
	if (permille < SPEED_MIN_PERMILLE) {
		permille = SPEED_MIN_PERMILLE;
	}
	if (permille > SPEED_MAX_PERMILLE) {
		permille = SPEED_MAX_PERMILLE;
	}
	player_speed_set_permille(permille);
	refresh();
}

// The setters only touch memory; this is the one write to the card. The slider
// saves once at the end of a drag, the pills once per tap.
static void save_cb(lv_event_t *e) {
	(void)e;
	config_save();
}

static void slider_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	(void)e;
	apply(SPEED_MIN_PERMILLE + (int)lv_slider_get_value(speed_slider) * SPEED_STEP_PERMILLE);
}

static void preset_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	apply((int)(intptr_t)lv_event_get_user_data(e));
	config_save();
}

// The "-0.05x" and "+0.05x" pills: user data is -1 or +1.
static void step_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	int direction = (int)(intptr_t)lv_event_get_user_data(e);
	apply(player_speed_permille() + direction * SPEED_STEP_PERMILLE);
	config_save();
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	refresh();
}

void speedpage_open(void) {
	player_sheet_close(false);
	switch_screen(speed_screen);
	switcher_set_player_return(speed_screen);
}

void speedpage_init(gui_config_t *cfg) {
	speed_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(speed_screen, cfg, "playback_speed");
	// No corner buttons on this page, so the whole width belongs to the title.
	settingsrow_title_corner_slots(settingsrow_page_title(speed_screen), cfg, 0);
	lv_obj_set_style_pad_row(container, 10, 0);

	settingsrow_slider(container, "speed", SPEED_STEPS, &speed_value, &speed_slider, slider_cb);
	lv_obj_add_event_cb(speed_slider, save_cb, LV_EVENT_RELEASED, NULL);

	// Below the card, a bare wrapping row: the two single-step pills and the
	// presets. No card of its own, so the page stays two blocks tall and the
	// numbers read as controls rather than a second setting.
	lv_obj_t *pills = lv_obj_create(container);
	lv_obj_set_width(pills, lv_pct(100));
	lv_obj_set_height(pills, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(pills, 0, 0);
	lv_obj_set_style_border_width(pills, 0, 0);
	lv_obj_set_style_radius(pills, 0, 0);
	lv_obj_set_style_pad_all(pills, 0, 0);
	lv_obj_set_style_pad_gap(pills, 10, 0);
	lv_obj_set_scrollable(pills, false);
	lv_obj_set_event_bubble(pills, true);
	lv_obj_set_flex_flow(pills, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(pills, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

	// Plain text pills, not translated tags: "0.75x" and "-0.05x" read the same
	// in every language, and seven tags per label would only invite mistakes.
	step_minus = settingsrow_pill_text(pills, "-0.05x", -1, step_cb);
	step_plus = settingsrow_pill_text(pills, "+0.05x", 1, step_cb);
	for (int i = 0; i < SPEED_PRESET_COUNT; i++) {
		speed_presets[i] = settingsrow_pill_text(pills, SPEED_PRESET_LABELS[i], SPEED_PRESETS[i], preset_cb);
	}

	lv_obj_add_event_cb(speed_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(speed_screen);

	// So the card and the pills carry a value even before the first visit.
	refresh();
}

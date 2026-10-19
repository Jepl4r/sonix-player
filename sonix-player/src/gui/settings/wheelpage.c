#include "wheelpage.h"

#include <stdint.h>

#include "lvgl/lvgl.h"

#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/device/sysinfo.h"
#include "src/system/device/system.h"

lv_obj_t *wheelpage_screen;

// [wheel] in the settings file. Clockwise both ways by default: it is the way
// the R3 II 2025's wheel raises the volume out of the box.
#define WHEEL_SECTION "wheel"

// The pills' values: what the wheel turns, and a direction.
#define WHEEL_MODE_VOLUME 0
#define WHEEL_MODE_SCRUB 1
#define WHEEL_CLOCKWISE 1
#define WHEEL_ANTICLOCKWISE 0

static lv_obj_t *mode_pill[2];
static lv_obj_t *up_pill[2];	  // clockwise, anti-clockwise
static lv_obj_t *forward_pill[2]; // the same
static lv_obj_t *up_card, *forward_card, *step_card;
static lv_obj_t *step_value, *step_slider;

static bool scrub_setting(void) { return config_get_bool(WHEEL_SECTION, "scrub", false); }

static bool up_clockwise_setting(void) { return config_get_bool(WHEEL_SECTION, "volume_up_clockwise", true); }

static bool forward_clockwise_setting(void) { return config_get_bool(WHEEL_SECTION, "scrub_forward_clockwise", true); }

static int step_setting(void) {
	int seconds = (int)config_get_int(WHEEL_SECTION, "scrub_step", WHEEL_SCRUB_DEFAULT_SECONDS);
	if (seconds < WHEEL_SCRUB_MIN_SECONDS) {
		return WHEEL_SCRUB_MIN_SECONDS;
	}
	return seconds > WHEEL_SCRUB_MAX_SECONDS ? WHEEL_SCRUB_MAX_SECONDS : seconds;
}

void wheelpage_apply(void) { input_set_wheel(scrub_setting(), up_clockwise_setting(), forward_clockwise_setting(), step_setting()); }

// Paints the pills from the settings, and greys out what does not apply.
// Called after every change and after a theme switch: the pills' colours are
// hand-set.
static void refresh(void) {
	if (!wheelpage_screen) {
		return;
	}
	bool scrub = scrub_setting();
	bool up = up_clockwise_setting();
	bool forward = forward_clockwise_setting();

	settingsrow_pill_active(mode_pill[WHEEL_MODE_VOLUME], !scrub);
	settingsrow_pill_active(mode_pill[WHEEL_MODE_SCRUB], scrub);
	settingsrow_pill_active(up_pill[0], up);
	settingsrow_pill_active(up_pill[1], !up);
	settingsrow_pill_active(forward_pill[0], forward);
	settingsrow_pill_active(forward_pill[1], !forward);

	// The cards for the mode not in use are greyed out.
	settingsrow_set_enabled(up_card, up_pill, 2, !scrub);
	settingsrow_set_enabled(forward_card, forward_pill, 2, scrub);
	settingsrow_set_enabled(step_card, &step_slider, 1, scrub);
}

static void changed(void) {
	config_save();
	wheelpage_apply();
	refresh();
}

static void mode_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	config_set_bool(WHEEL_SECTION, "scrub", (int)(intptr_t)lv_event_get_user_data(e) == WHEEL_MODE_SCRUB);
	changed();
}

static void up_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	config_set_bool(WHEEL_SECTION, "volume_up_clockwise", (int)(intptr_t)lv_event_get_user_data(e) == WHEEL_CLOCKWISE);
	changed();
}

static void forward_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	config_set_bool(WHEEL_SECTION, "scrub_forward_clockwise", (int)(intptr_t)lv_event_get_user_data(e) == WHEEL_CLOCKWISE);
	changed();
}

static void show_step(int seconds) { lv_label_set_text_fmt(step_value, "%d s", seconds); }

static void step_cb(lv_event_t *e) {
	(void)e;
	int seconds = WHEEL_SCRUB_MIN_SECONDS + (int)lv_slider_get_value(step_slider);
	show_step(seconds);
	if (seconds == step_setting()) {
		return;
	}
	config_set_int(WHEEL_SECTION, "scrub_step", seconds);
	config_save();
	wheelpage_apply();
}

void wheelpage_init(gui_config_t *cfg) {
	const sysinfo_model_t *model = sysinfo_model();
	if (!model || !model->volume_wheel) {
		return;
	}

	wheelpage_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(wheelpage_screen, cfg, "wheel");

	lv_obj_t *pills;
	settingsrow_pills(container, "wheel_controls", &pills);
	mode_pill[WHEEL_MODE_VOLUME] = settingsrow_pill(pills, "wheel_volume", WHEEL_MODE_VOLUME, mode_cb);
	mode_pill[WHEEL_MODE_SCRUB] = settingsrow_pill(pills, "wheel_scrubbing", WHEEL_MODE_SCRUB, mode_cb);

	up_card = settingsrow_pills(container, "wheel_volume_up", &pills);
	up_pill[0] = settingsrow_pill(pills, "wheel_clockwise", WHEEL_CLOCKWISE, up_cb);
	up_pill[1] = settingsrow_pill(pills, "wheel_anticlockwise", WHEEL_ANTICLOCKWISE, up_cb);

	forward_card = settingsrow_pills(container, "wheel_scrub_forward", &pills);
	forward_pill[0] = settingsrow_pill(pills, "wheel_clockwise", WHEEL_CLOCKWISE, forward_cb);
	forward_pill[1] = settingsrow_pill(pills, "wheel_anticlockwise", WHEEL_ANTICLOCKWISE, forward_cb);

	step_card = settingsrow_slider(container, "wheel_scrub_step", WHEEL_SCRUB_MAX_SECONDS - WHEEL_SCRUB_MIN_SECONDS + 1, &step_value, &step_slider, step_cb);
	int seconds = step_setting();
	lv_slider_set_value(step_slider, seconds - WHEEL_SCRUB_MIN_SECONDS, LV_ANIM_OFF);
	show_step(seconds);

	refresh();
	theme_register_refresh(refresh);
	switcher_attach_back_gesture(wheelpage_screen);
}

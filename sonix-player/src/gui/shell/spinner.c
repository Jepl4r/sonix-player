#include "spinner.h"

#include "src/gui/shell/theme.h"

// One full turn. A second and a half: faster looks frantic, slower looks stalled.
#define SPIN_PERIOD_MS 1500

// LVGL angles are in tenths of a degree, so a full turn is 3600.
#define SPIN_FULL_TURN 3600

static void spin_start(lv_obj_t *img);

// Turning only while it can be seen. LVGL's animation timer runs every 16 ms
// for as long as any animation is alive, and spinners are left behind on pages
// that are built once and kept (a service page's loader, hidden between loads;
// a sign-in box on a screen not shown since): each one kept the whole
// interface waking sixty times a second. So a spinner that finds itself out of
// sight -- another screen, a hidden parent, scrolled away -- stops, and the
// next time it is drawn it starts again (spin_draw_cb). Being drawn is the one
// sure sign of being seen again, and a hidden object coming back is
// invalidated by LVGL, so it is drawn at once.
static void spin_cb(void *obj, int32_t angle) {
	lv_obj_t *img = obj;
	if (!lv_obj_is_visible(img)) {
		lv_anim_delete(img, spin_cb); // safe from inside the animation's own callback
		return;
	}
	lv_image_set_rotation(img, angle);
}

static void spin_start(lv_obj_t *img) {
	if (lv_anim_get(img, spin_cb)) {
		return;
	}
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, img);
	lv_anim_set_exec_cb(&a, spin_cb);
	lv_anim_set_values(&a, 0, SPIN_FULL_TURN);
	lv_anim_set_duration(&a, SPIN_PERIOD_MS);
	lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
	// Linear: a spinner that speeds up and slows down each turn looks broken.
	lv_anim_set_path_cb(&a, lv_anim_path_linear);
	// Not applied on the spot: this can run while a frame is being drawn
	// (spin_draw_cb), where nothing may be invalidated.
	lv_anim_set_early_apply(&a, false);
	lv_anim_start(&a);
}

static void spin_draw_cb(lv_event_t *e) { spin_start(lv_event_get_target_obj(e)); }

lv_obj_t *spinner_create(lv_obj_t *parent, const lv_image_dsc_t *icon) {
	lv_obj_t *img = lv_image_create(parent);
	lv_image_set_src(img, icon);
	lv_obj_add_style(img, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);

	// Pivot at the image centre; otherwise it spins around the top-left corner
	// and appears to fly off.
	lv_image_set_pivot(img, (int32_t)icon->header.w / 2, (int32_t)icon->header.h / 2);

	// Rotation pushes pixels outside the image rectangle: without this LVGL
	// draws within the original bounds and clips the circle's corners every
	// quarter turn.
	lv_obj_set_overflow_visible(img, true);

	lv_obj_add_event_cb(img, spin_draw_cb, LV_EVENT_DRAW_MAIN_BEGIN, NULL);
	spin_start(img);

	return img;
}

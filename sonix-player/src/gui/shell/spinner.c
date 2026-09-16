#include "spinner.h"

#include "src/gui/shell/theme.h"

// One full turn. A second and a half: faster looks frantic, slower looks stalled.
#define SPIN_PERIOD_MS 1500

// LVGL angles are in tenths of a degree, so a full turn is 3600.
#define SPIN_FULL_TURN 3600

static void spin_cb(void *obj, int32_t angle) { lv_image_set_rotation((lv_obj_t *)obj, angle); }

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
	lv_obj_add_flag(img, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, img);
	lv_anim_set_exec_cb(&a, spin_cb);
	lv_anim_set_values(&a, 0, SPIN_FULL_TURN);
	lv_anim_set_duration(&a, SPIN_PERIOD_MS);
	lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
	// Linear: a spinner that speeds up and slows down each turn looks broken.
	lv_anim_set_path_cb(&a, lv_anim_path_linear);
	lv_anim_start(&a);

	return img;
}

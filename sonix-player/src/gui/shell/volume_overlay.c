#include "volume_overlay.h"

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/sx_palette.h"
#include "src/gui/shell/sx_tokens.h"
#include "src/gui/shell/theme.h"
#include "src/system/audio/alsa-controls.h"

// The 2.0 volume bar (sx_tokens.h): a 64 x 300 pill against the right edge,
// near-black whatever the theme, the level as a number at the top, a slim
// vertical scale filled in the playing album's accent, the speaker glyph at
// the bottom. It slides in from the edge, stays SX_VOL_OSD_MS after the last
// key press, and slides out again: the page under it is not covered, only its
// right edge for a moment.
//
// The scale is a real slider, so the level can still be dragged with a finger
// while the bar is up, as on the pill it replaces.
#define OVERLAY_WIDTH SX_VOL_OSD_W
#define OVERLAY_HEIGHT SX_VOL_OSD_H
#define OVERLAY_RADIUS (OVERLAY_WIDTH / 2)
#define OVERLAY_HIDE_MS SX_VOL_OSD_MS
#define OVERLAY_SLIDE_MS 160
// Off screen: its width and the margin, and a little more.
#define OVERLAY_AWAY (SX_VOL_OSD_W + SX_VOL_OSD_RIGHT + 32)
#define SCALE_W 14

// Where the icon changes: silent, quiet, loud.
#define VOLUME_LOW_PERCENT 1
#define VOLUME_HIGH_PERCENT 55

static lv_obj_t *overlay;
static lv_obj_t *overlay_icon;
static lv_obj_t *overlay_slider;
static lv_obj_t *overlay_label;
static lv_timer_t *hide_timer;

// What the bar is currently showing, so a theme switch can recolour the
// number without having to be told the level again.
static int shown_percent;

// The number: white, or the warning red past the point where every further
// step is loud.
static void apply_label_color(int percent) {
	lv_color_t colour = percent > VOLUME_WARN_PERCENT ? VOLUME_WARN_COLOR : lv_color_white();
	lv_obj_set_style_text_color(overlay_label, colour, 0);
}

// The one colour from the palette, set by hand: the fill, in the accent of the
// always-dark surfaces. Called when built, on every show and after a palette
// change.
static void apply_theme(void) {
	if (!overlay) {
		return;
	}
	lv_obj_set_style_bg_color(overlay_slider, sx_pal()->np_accent, LV_PART_INDICATOR);
	apply_label_color(shown_percent);
}

static void slide_exec_cb(void *obj, int32_t v) { lv_obj_set_style_translate_x((lv_obj_t *)obj, v, 0); }

static void slid_out_cb(lv_anim_t *a) {
	(void)a;
	lv_obj_set_hidden(overlay, true);
}

static void slide(bool in) {
	lv_anim_delete(overlay, slide_exec_cb);
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, overlay);
	lv_anim_set_exec_cb(&a, slide_exec_cb);
	lv_anim_set_values(&a, lv_obj_get_style_translate_x(overlay, LV_PART_MAIN), in ? 0 : OVERLAY_AWAY);
	lv_anim_set_duration(&a, OVERLAY_SLIDE_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	if (!in) {
		lv_anim_set_completed_cb(&a, slid_out_cb);
	}
	lv_anim_start(&a);
}

static void hide_cb(lv_timer_t *timer) {
	(void)timer;
	// Not while a finger is on the slider: the bar must never vanish
	// mid-drag.
	if (overlay_slider && lv_obj_has_state(overlay_slider, LV_STATE_PRESSED)) {
		lv_timer_reset(hide_timer);
		return;
	}
	lv_timer_pause(hide_timer);
	slide(false);
}

static void set_icon_for(int percent) {
	const lv_image_dsc_t *icon = &icon_volume_high;
	if (percent < VOLUME_LOW_PERCENT) {
		icon = &icon_volume_mute;
	} else if (percent < VOLUME_HIGH_PERCENT) {
		icon = &icon_volume_low;
	}
	lv_image_set_src(overlay_icon, icon);
}

// The finger moved the slider: apply it and keep the bar up.
static void slider_changed_cb(lv_event_t *e) {
	(void)e;
	int percent = (int)lv_slider_get_value(overlay_slider);

	set_volume_percent(percent);

	// What the output is really at, which is not always what was asked for:
	// line out holds a fixed level, and a bar showing a number the hardware
	// ignored would be the one thing on screen saying otherwise.
	percent = get_volume_percent();
	lv_slider_set_value(overlay_slider, percent, LV_ANIM_OFF);
	set_icon_for(percent);
	shown_percent = percent;
	lv_label_set_text_fmt(overlay_label, "%d", percent);
	apply_label_color(percent);

	lv_timer_reset(hide_timer);
	lv_timer_resume(hide_timer);
}

// Kept for its callers. Before 2.0 the bar showed only on the player (and
// where this allowed it), the status bar's number standing in everywhere
// else; the 2.0 bar shows on every page, so this changes nothing any more.
void volume_overlay_allow_outside_player(bool allow) { (void)allow; }

void volume_overlay_show(int percent) {
	if (!overlay) {
		return;
	}
	if (percent < 0) {
		percent = 0;
	}
	if (percent > 100) {
		percent = 100;
	}

	set_icon_for(percent);
	shown_percent = percent;
	apply_theme();

	// Never yank the knob out from under a finger.
	if (!lv_obj_has_state(overlay_slider, LV_STATE_PRESSED)) {
		lv_slider_set_value(overlay_slider, percent, LV_ANIM_OFF);
	}
	lv_label_set_text_fmt(overlay_label, "%d", percent);
	apply_label_color(percent);

	if (lv_obj_is_hidden(overlay)) {
		lv_obj_set_style_translate_x(overlay, OVERLAY_AWAY, 0);
		lv_obj_set_hidden(overlay, false);
	}
	lv_obj_move_foreground(overlay);
	slide(true);

	// Every key press pushes the dismissal back, so holding the key keeps the
	// bar up for as long as the level is moving.
	lv_timer_reset(hide_timer);
	lv_timer_resume(hide_timer);
}

void volume_overlay_init(gui_config_t *cfg) {
	overlay = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(overlay);
	lv_obj_set_size(overlay, OVERLAY_WIDTH, OVERLAY_HEIGHT);
	lv_obj_set_pos(overlay, (int32_t)cfg->screen_width - SX_VOL_OSD_RIGHT - OVERLAY_WIDTH,
				   ((int32_t)cfg->screen_height - OVERLAY_HEIGHT) / 2);
	lv_obj_set_style_radius(overlay, OVERLAY_RADIUS, 0);
	lv_obj_set_style_bg_color(overlay, lv_color_hex(SX_VOL_OSD_BG), 0);
	lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_ver(overlay, 16, 0);
	lv_obj_set_style_pad_row(overlay, 12, 0);
	lv_obj_set_scrollable(overlay, false);
	lv_obj_set_clickable(overlay, true);
	lv_obj_set_event_bubble(overlay, false);
	lv_obj_set_flex_flow(overlay, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(overlay, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_hidden(overlay, true);

	overlay_label = lv_label_create(overlay);
	lv_label_set_text(overlay_label, "0");
	lv_obj_set_style_text_font(overlay_label, &font_ui_22_bold, 0);

	// The scale, vertical: a slider taller than wide runs bottom to top.
	overlay_slider = lv_slider_create(overlay);
	lv_obj_set_width(overlay_slider, SCALE_W);
	lv_obj_set_flex_grow(overlay_slider, 1);
	lv_slider_set_range(overlay_slider, 0, 100);
	lv_obj_set_style_radius(overlay_slider, SCALE_W / 2, LV_PART_MAIN);
	lv_obj_set_style_bg_color(overlay_slider, lv_color_white(), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(overlay_slider, LV_OPA_20, LV_PART_MAIN);
	lv_obj_set_style_radius(overlay_slider, SCALE_W / 2, LV_PART_INDICATOR);
	lv_obj_set_style_bg_opa(overlay_slider, LV_OPA_COVER, LV_PART_INDICATOR);
	// No knob: the fill's top is the level, as on the design.
	lv_obj_set_style_bg_opa(overlay_slider, LV_OPA_TRANSP, LV_PART_KNOB);
	lv_obj_set_style_pad_all(overlay_slider, 0, LV_PART_KNOB);
	lv_obj_set_style_shadow_width(overlay_slider, 0, LV_PART_KNOB);
	// A slim scale is a hard target: the whole bar's width takes the finger.
	lv_obj_set_ext_click_area(overlay_slider, (OVERLAY_WIDTH - SCALE_W) / 2);
	lv_obj_add_event_cb(overlay_slider, slider_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

	overlay_icon = lv_image_create(overlay);
	lv_image_set_src(overlay_icon, &icon_volume_high);
	lv_obj_set_style_image_recolor(overlay_icon, lv_color_white(), 0);
	lv_obj_set_style_image_recolor_opa(overlay_icon, LV_OPA_COVER, 0);

	hide_timer = lv_timer_create(hide_cb, OVERLAY_HIDE_MS, NULL);
	lv_timer_pause(hide_timer);

	apply_theme();
	theme_register_refresh(apply_theme);
}

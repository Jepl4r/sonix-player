#include "toast.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"

#define TOAST_MS 1400
// Not TOAST_W / TOAST_H: the second of those collides with toast.h's own
// include guard, which is defined by the time this line is read.
#define TOAST_WIDTH 260
// The height with a glyph above the text. Without one the card measures itself
// instead: 200 px of card around a single line reads as an empty box with a
// sentence lost in the middle of it.
#define TOAST_HEIGHT 200

// The Adwaita "success" green, the same one the active chevrons use, so a
// confirmation reads as one across the interface, and the Adwaita red beside
// it for the card that reports a failure.
#define TOAST_GREEN lv_color_make(0x2e, 0xc2, 0x7e)
#define TOAST_RED lv_color_make(0xe0, 0x1b, 0x24)

static lv_obj_t *veil;
static lv_obj_t *card;
static lv_obj_t *icon;
static lv_obj_t *label;
static lv_timer_t *hide_timer;

static void toast_dismiss(void) {
	if (veil) {
		lv_obj_add_flag(veil, LV_OBJ_FLAG_HIDDEN);
	}
	lv_timer_pause(hide_timer);
}

static void hide_cb(lv_timer_t *timer) {
	(void)timer;
	toast_dismiss();
}

// A tap off the card takes it away early. Taps on the card do not get here --
// it is clickable, so it swallows its own.
static void veil_cb(lv_event_t *e) {
	(void)e;
	toast_dismiss();
}

void toast_init(gui_config_t *cfg) {
	(void)cfg;

	// The same veil the notices and the confirmation dialog use: black at 60%,
	// and no drop shadow on the card, so a confirmation and a warning read as
	// the same kind of object.
	veil = lv_obj_create(lv_layer_top());
	lv_obj_add_flag(veil, LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_size(veil, lv_pct(100), lv_pct(100));
	lv_obj_set_pos(veil, 0, 0);
	lv_obj_set_style_bg_color(veil, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(veil, LV_OPA_60, 0);
	lv_obj_set_style_border_width(veil, 0, 0);
	lv_obj_set_style_radius(veil, 0, 0);
	lv_obj_set_style_shadow_width(veil, 0, 0);
	lv_obj_set_style_pad_all(veil, 0, 0);
	lv_obj_remove_flag(veil, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(veil, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(veil, veil_cb, LV_EVENT_CLICKED, NULL);

	card = lv_obj_create(veil);
	lv_obj_set_size(card, TOAST_WIDTH, TOAST_HEIGHT);
	lv_obj_center(card);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 20, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, 20, 0);
	lv_obj_set_style_pad_gap(card, 16, 0);
	lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE); // a tap on it is not a tap outside
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	icon = lv_image_create(card);
	lv_image_set_src(icon, &icon_circle_check);
	lv_obj_set_style_image_recolor(icon, TOAST_GREEN, 0);
	lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);

	label = lv_label_create(card);
	lv_label_set_text(label, "");
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(label, lv_pct(100));

	hide_timer = lv_timer_create(hide_cb, TOAST_MS, NULL);
	lv_timer_pause(hide_timer);
}

// The one that does the work. `glyph` NULL leaves the card with the text alone,
// which the flex layout centres by itself once the image is out of the flow --
// hence the hidden flag rather than a zero size.
static void show(const lv_image_dsc_t *glyph, lv_color_t colour, const char *text) {
	if (!card) {
		return;
	}

	if (glyph) {
		lv_image_set_src(icon, glyph);
		lv_obj_set_style_image_recolor(icon, colour, 0);
		lv_obj_remove_flag(icon, LV_OBJ_FLAG_HIDDEN);
		lv_obj_set_height(card, TOAST_HEIGHT);
	} else {
		lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
		lv_obj_set_height(card, LV_SIZE_CONTENT);
	}

	lv_label_set_text(label, text ? tr(text) : "");
	lv_obj_remove_flag(veil, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(veil);
	lv_timer_reset(hide_timer);
	lv_timer_resume(hide_timer);
}

void toast_success(const char *text) { show(&icon_circle_check, TOAST_GREEN, text); }

void toast_error(const char *text) { show(&icon_circle_alert, TOAST_RED, text); }

void toast_plain(const char *text) { show(NULL, TOAST_GREEN, text); }

void toast_glyph(const lv_image_dsc_t *glyph, const char *text) { show(glyph, TOAST_GREEN, text); }

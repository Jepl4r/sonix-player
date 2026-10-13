#include "streamerror.h"

#include <stdlib.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"

// See streamerror.h.

// The picture's size: the page's whole content now, so larger than a row's.
#define ICON_PX 64

typedef struct {
	lv_obj_t *icon;
	lv_obj_t *text;
	lv_obj_t *button;
	lv_obj_t *button_label;
	void (*action)(void);
} panel_t;

static void button_cb(lv_event_t *e) {
	// The back swipe starts anywhere on the page, this button included.
	if (switcher_back_drag_active()) {
		return;
	}
	panel_t *p = lv_event_get_user_data(e);
	if (p && p->action) {
		p->action();
	}
}

static void deleted_cb(lv_event_t *e) { free(lv_event_get_user_data(e)); }

lv_obj_t *streamerror_create(lv_obj_t *parent) {
	panel_t *p = calloc(1, sizeof(*p));
	if (!p) {
		return NULL;
	}

	lv_obj_t *panel = lv_obj_create(parent);
	lv_obj_remove_style_all(panel);
	lv_obj_set_size(panel, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_top(panel, 36, 0);
	lv_obj_set_style_pad_bottom(panel, 16, 0);
	lv_obj_set_style_pad_hor(panel, 12, 0);
	lv_obj_set_style_pad_row(panel, 18, 0);
	lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(panel, false);
	lv_obj_set_user_data(panel, p);
	lv_obj_add_event_cb(panel, deleted_cb, LV_EVENT_DELETE, p);

	p->icon = lv_image_create(panel);
	lv_obj_add_style(p->icon, &theme_style_icon, 0);
	lv_obj_set_size(p->icon, ICON_PX, ICON_PX);
	lv_image_set_inner_align(p->icon, LV_IMAGE_ALIGN_CENTER);

	p->text = lv_label_create(panel);
	lv_label_set_long_mode(p->text, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(p->text, lv_pct(100));
	lv_obj_set_style_text_align(p->text, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(p->text, &theme_style_text, 0);
	lv_obj_set_style_text_font(p->text, &font_ui_22, 0);
	lv_label_set_text(p->text, "");

	// The shape of the confirmation dialog's accent button, so it reads as the
	// thing to press.
	p->button = lv_btn_create(panel);
	lv_obj_set_size(p->button, 220, 64);
	lv_obj_set_style_radius(p->button, 12, 0);
	lv_obj_set_style_border_width(p->button, 0, 0);
	lv_obj_set_style_shadow_width(p->button, 0, 0);
	lv_obj_set_style_bg_color(p->button, theme()->accent, 0);
	lv_obj_set_style_bg_opa(p->button, LV_OPA_COVER, 0);
	lv_obj_set_style_margin_top(p->button, 6, 0);
	lv_obj_add_event_cb(p->button, button_cb, LV_EVENT_CLICKED, p);

	p->button_label = lv_label_create(p->button);
	lv_obj_set_style_text_font(p->button_label, &font_ui_24, 0);
	lv_obj_set_style_text_color(p->button_label, lv_color_white(), 0);
	lv_label_set_text(p->button_label, tr("stream_retry"));
	lv_obj_center(p->button_label);

	lv_obj_set_hidden(panel, true);
	return panel;
}

void streamerror_show(lv_obj_t *panel, stream_err_t cls, const char *text, void (*retry)(void),
					  void (*sign_in)(void)) {
	if (!panel) {
		return;
	}
	panel_t *p = lv_obj_get_user_data(panel);
	if (!p) {
		return;
	}

	// One picture for "the network", one for everything else: the sentence
	// says which; the picture only has to say at a glance that this is a
	// fault and not an empty list.
	const lv_image_dsc_t *icon = cls == STREAM_ERR_NO_WIFI ? &icon_wifi_off : &icon_circle_alert;
	lv_image_set_src(p->icon, icon);
	// The two are drawn at different sizes (30 and 64 px): shown the same.
	lv_image_set_scale(p->icon, icon->header.w ? (uint32_t)(LV_SCALE_NONE * ICON_PX / icon->header.w) : LV_SCALE_NONE);
	lv_label_set_text(p->text, text && text[0] ? text : tr(streamerr_key(cls)));

	bool offer_sign_in = cls == STREAM_ERR_SIGN_IN && sign_in;
	p->action = offer_sign_in ? sign_in : retry;
	lv_label_set_text(p->button_label, tr(offer_sign_in ? "sign_in" : "stream_retry"));
	// The accent may have changed with the theme since the panel was built.
	lv_obj_set_style_bg_color(p->button, theme()->accent, 0);
	lv_obj_set_hidden(p->button, p->action == NULL);

	lv_obj_set_hidden(panel, false);
	lv_obj_move_foreground(panel);
}

void streamerror_hide(lv_obj_t *panel) {
	if (panel) {
		lv_obj_set_hidden(panel, true);
	}
}

bool streamerror_visible(lv_obj_t *panel) { return panel && !lv_obj_is_hidden(panel); }

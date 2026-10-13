#include "streamerror.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/sx_styles.h"
#include "src/system/core/lang.h"

// See streamerror.h. The layout is the prototype's state block: 60 px down, a
// round 84 px plaque, the headline (30 bold), the reason (22, secondary), a
// 68 px accent pill and, under it, a 68 px quiet one.

#define PLAQUE 84
#define BUTTON_H 68
// A load is at most a few requests of STREAM_UI_TIMEOUT_SECS each.
#define RETRY_WINDOW_MS 40000

typedef struct {
	lv_obj_t *mark;	 // "!" on the plaque
	lv_obj_t *wifi;	 // or the Wi-Fi glyph, for "no network"
	lv_obj_t *title;
	lv_obj_t *text;
	lv_obj_t *button;
	lv_obj_t *button_label;
	lv_obj_t *offline;
	void (*action)(void);
	void (*open_offline)(void);
	char service[64];
	int attempts; // presses of "Retry" that failed again since it was shown
	// When "Retry" was pressed: a failure shown within the time a load can
	// take is the same trouble again. A time rather than a flag cleared on
	// success, because pages hide the panel both when a load starts and when
	// it succeeds, and cannot say which.
	bool retried;
	uint32_t retried_at;
} panel_t;

static void button_cb(lv_event_t *e) {
	// The back swipe starts anywhere on the page, this button included.
	if (switcher_back_drag_active()) {
		return;
	}
	panel_t *p = lv_event_get_user_data(e);
	if (p && p->action) {
		p->retried = true;
		p->retried_at = lv_tick_get();
		p->action();
	}
}

static void offline_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	panel_t *p = lv_event_get_user_data(e);
	if (p && p->open_offline) {
		p->open_offline();
	}
}

static void deleted_cb(lv_event_t *e) { free(lv_event_get_user_data(e)); }

static lv_obj_t *pill(lv_obj_t *parent, bool primary, lv_event_cb_t cb, panel_t *p, lv_obj_t **label_out) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, LV_SIZE_CONTENT, BUTTON_H);
	lv_obj_set_style_radius(btn, BUTTON_H / 2, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_pad_hor(btn, primary ? 56 : 40, 0);
	lv_obj_set_style_pad_ver(btn, 0, 0);
	lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
	lv_obj_add_style(btn, primary ? &sx_style_accent : &sx_style_press, 0);
	if (!primary) {
		lv_obj_add_style(btn, &sx_style_text, 0);
	}
	lv_obj_set_style_transform_width(btn, 0, LV_STATE_PRESSED);
	lv_obj_set_style_transform_height(btn, 0, LV_STATE_PRESSED);
	lv_obj_set_style_margin_top(btn, primary ? 26 : 12, 0);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, p);
	lv_obj_t *label = lv_label_create(btn);
	lv_obj_set_style_text_font(label, primary ? &font_ui_24_bold : &font_ui_24, 0);
	lv_obj_center(label);
	*label_out = label;
	return btn;
}

lv_obj_t *streamerror_create(lv_obj_t *parent) {
	panel_t *p = calloc(1, sizeof(*p));
	if (!p) {
		return NULL;
	}
	lv_obj_t *panel = lv_obj_create(parent);
	lv_obj_remove_style_all(panel);
	lv_obj_set_size(panel, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_top(panel, 60, 0);
	lv_obj_set_style_pad_bottom(panel, 16, 0);
	lv_obj_set_style_pad_hor(panel, 16, 0);
	lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(panel, false);
	lv_obj_set_user_data(panel, p);
	lv_obj_add_event_cb(panel, deleted_cb, LV_EVENT_DELETE, p);

	lv_obj_t *plaque = sx_box(panel);
	lv_obj_set_size(plaque, PLAQUE, PLAQUE);
	lv_obj_set_style_radius(plaque, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(plaque, LV_OPA_COVER, 0);
	lv_obj_add_style(plaque, &sx_style_surface, 0);
	p->mark = lv_label_create(plaque);
	lv_obj_set_style_text_font(p->mark, &font_ui_36_bold, 0);
	lv_obj_add_style(p->mark, &sx_style_text, 0);
	lv_label_set_text(p->mark, "!");
	lv_obj_center(p->mark);
	p->wifi = lv_image_create(plaque);
	lv_image_set_src(p->wifi, &icon_wifi_off);
	lv_obj_add_style(p->wifi, &sx_style_icon, 0);
	lv_obj_center(p->wifi);
	lv_obj_set_hidden(p->wifi, true);

	p->title = lv_label_create(panel);
	lv_label_set_long_mode(p->title, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(p->title, lv_pct(100));
	lv_obj_set_style_text_align(p->title, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(p->title, &sx_style_text, 0);
	lv_obj_set_style_text_font(p->title, &font_ui_30_bold, 0);
	lv_obj_set_style_margin_top(p->title, 20, 0);
	lv_label_set_text(p->title, "");

	p->text = lv_label_create(panel);
	lv_label_set_long_mode(p->text, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(p->text, lv_pct(100));
	lv_obj_set_style_text_align(p->text, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(p->text, &sx_style_text2, 0);
	lv_obj_set_style_text_font(p->text, &font_ui_22, 0);
	lv_obj_set_style_text_line_space(p->text, 8, 0);
	lv_obj_set_style_margin_top(p->text, 8, 0);
	lv_label_set_text(p->text, "");

	p->button = pill(panel, true, button_cb, p, &p->button_label);
	lv_label_set_text(p->button_label, tr("stream_retry"));
	lv_obj_t *offline_label;
	p->offline = pill(panel, false, offline_cb, p, &offline_label);
	lv_label_set_text(offline_label, tr("stream_saved_tracks"));
	lv_obj_set_hidden(p->offline, true);

	lv_obj_set_hidden(panel, true);
	return panel;
}

static panel_t *panel_of(lv_obj_t *panel) { return panel ? lv_obj_get_user_data(panel) : NULL; }

void streamerror_set_service(lv_obj_t *panel, const char *name) {
	panel_t *p = panel_of(panel);
	if (p) {
		snprintf(p->service, sizeof(p->service), "%s", name ? name : "");
	}
}

void streamerror_set_offline(lv_obj_t *panel, void (*open)(void)) {
	panel_t *p = panel_of(panel);
	if (p) {
		p->open_offline = open;
		lv_obj_set_hidden(p->offline, open == NULL);
	}
}

// The headline of each kind of trouble. In an array for the string extractor
// (see radiopage.c STORED_EMPTY): the keys reach tr() through a variable.
static const char *const HEADLINES[] = {
	"stream_head_other",	  "stream_head_no_wifi", "stream_head_no_response", "stream_head_sign_in",
	"stream_head_no_subscription", "stream_head_keys",	 "stream_head_server",		"stream_head_not_found",
};

static const char *headline(stream_err_t cls) {
	switch (cls) {
	case STREAM_ERR_NO_WIFI:
		return HEADLINES[1];
	case STREAM_ERR_NO_RESPONSE:
		return HEADLINES[2];
	case STREAM_ERR_SIGN_IN:
		return HEADLINES[3];
	case STREAM_ERR_NO_SUBSCRIPTION:
		return HEADLINES[4];
	case STREAM_ERR_KEYS:
		return HEADLINES[5];
	case STREAM_ERR_SERVER:
		return HEADLINES[6];
	case STREAM_ERR_NOT_FOUND:
		return HEADLINES[7];
	default:
		return HEADLINES[0];
	}
}

void streamerror_show(lv_obj_t *panel, stream_err_t cls, const char *text, void (*retry)(void),
					  void (*sign_in)(void)) {
	panel_t *p = panel_of(panel);
	if (!p) {
		return;
	}
	// A failure right after "Retry" is the same trouble again: counted.
	bool repeat = p->retried && lv_tick_elaps(p->retried_at) < RETRY_WINDOW_MS;
	p->attempts = repeat ? p->attempts + 1 : 0;
	p->retried = false;

	lv_obj_set_hidden(p->wifi, cls != STREAM_ERR_NO_WIFI);
	lv_obj_set_hidden(p->mark, cls == STREAM_ERR_NO_WIFI);
	lv_label_set_text(p->title, tr(headline(cls)));

	// The reason. "Did not answer" is said with who and how long when the
	// page said who it is, and with what that means when the Wi-Fi is up:
	// the trouble is at the other end, not in the player.
	char reason[400];
	if (cls == STREAM_ERR_NO_RESPONSE && p->service[0]) {
		lv_snprintf(reason, sizeof(reason), tr(streamerr_online() ? "stream_why_no_response_online" : "stream_why_no_response"),
					p->service, STREAM_UI_TIMEOUT_SECS);
	} else {
		snprintf(reason, sizeof(reason), "%s", text && text[0] ? text : tr(streamerr_key(cls)));
	}
	if (p->attempts > 0) {
		char line[480];
		lv_snprintf(line, sizeof(line), tr("stream_attempt_n"), p->attempts + 1);
		size_t used = strlen(line);
		snprintf(line + used, sizeof(line) - used, " %s", reason);
		lv_label_set_text(p->text, line);
	} else {
		lv_label_set_text(p->text, reason);
	}

	bool offer_sign_in = cls == STREAM_ERR_SIGN_IN && sign_in;
	p->action = offer_sign_in ? sign_in : retry;
	lv_label_set_text(p->button_label, tr(offer_sign_in ? "sign_in" : "stream_retry"));
	lv_obj_set_hidden(p->button, p->action == NULL);
	lv_obj_set_hidden(p->offline, p->open_offline == NULL);
	lv_obj_set_hidden(panel, false);
	lv_obj_move_foreground(panel);
}

void streamerror_hide(lv_obj_t *panel) {
	panel_t *p = panel_of(panel);
	if (p) {
		// A retry under way keeps its count: the answer decides whether it
		// was a repeat (shown again) or a success (hidden for good).
		if (!p->retried || lv_tick_elaps(p->retried_at) >= RETRY_WINDOW_MS) {
			p->attempts = 0;
		}
		lv_obj_set_hidden(panel, true);
	}
}

bool streamerror_visible(lv_obj_t *panel) { return panel && !lv_obj_is_hidden(panel); }

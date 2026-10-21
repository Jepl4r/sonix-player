#include "toast.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/spinner.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/uiscale.h"
#include "src/system/core/lang.h"

#include <string.h>

#define TOAST_MS 1400
// A longer sentence stays up longer, so it can be read to the end: this much
// more per character past TOAST_QUICK_CHARS, up to TOAST_MAX_MS.
#define TOAST_QUICK_CHARS 40
#define TOAST_MS_PER_CHAR 35
#define TOAST_MAX_MS 5000
// Not TOAST_W / TOAST_H: the second of those collides with toast.h's own
// include guard, which is defined by the time this line is read.
#define TOAST_WIDTH ui_px(260)
// Past this many characters the card widens to TOAST_WIDE_WIDTH (or the panel
// less TOAST_SIDE_MARGIN on each side): four lines at the narrow width would
// make a tall thin column of a sentence.
#define TOAST_WIDE_CHARS 60
#define TOAST_WIDE_WIDTH ui_px(380)
#define TOAST_SIDE_MARGIN ui_px(30)
// The least height with a glyph above the text, so a short message keeps the
// usual square card. The card grows past it for a longer one -- the AutoEq
// messages run to three sentences -- and never past the panel less
// TOAST_SIDE_MARGIN above and below. Without a glyph there is no least height:
// 200 px of card around a single line reads as an empty box with a sentence
// lost in the middle of it.
#define TOAST_HEIGHT ui_px(200)

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

// The busy card's spinner, alive only while it shows.
static lv_obj_t *busy_spinner;
// The busy card is about work that carries on without anybody watching it, so
// a tap off the card may put it away (toast_busy_dismissable).
static bool busy_dismissable;

static void toast_dismiss(void) {
	if (busy_spinner) {
		if (!busy_dismissable) {
			return; // only toast_busy_end() takes the busy card away
		}
		lv_obj_delete(busy_spinner);
		busy_spinner = NULL;
		busy_dismissable = false;
	}
	if (veil) {
		lv_obj_set_hidden(veil, true);
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
	lv_obj_set_hidden(veil, true);
	lv_obj_set_size(veil, lv_pct(100), lv_pct(100));
	lv_obj_set_pos(veil, 0, 0);
	lv_obj_set_style_bg_color(veil, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(veil, LV_OPA_60, 0);
	lv_obj_set_style_border_width(veil, 0, 0);
	lv_obj_set_style_radius(veil, 0, 0);
	lv_obj_set_style_shadow_width(veil, 0, 0);
	lv_obj_set_style_pad_all(veil, 0, 0);
	lv_obj_set_scrollable(veil, false);
	lv_obj_set_clickable(veil, true);
	lv_obj_add_event_cb(veil, veil_cb, LV_EVENT_CLICKED, NULL);

	card = lv_obj_create(veil);
	lv_obj_set_size(card, TOAST_WIDTH, LV_SIZE_CONTENT);
	lv_obj_set_style_max_height(card, lv_display_get_vertical_resolution(lv_display_get_default()) - 2 * TOAST_SIDE_MARGIN, 0);
	lv_obj_center(card);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, ui_px(20), 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, ui_px(20), 0);
	lv_obj_set_style_pad_gap(card, ui_px(16), 0);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_clickable(card, true); // a tap on it is not a tap outside
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

// The card's width and least height for `text`, with or without something
// above it.
static void fit_card(const char *text, bool above) {
	int32_t width = TOAST_WIDTH;
	if (text && strlen(text) > TOAST_WIDE_CHARS) {
		int32_t room = lv_display_get_horizontal_resolution(lv_display_get_default()) - 2 * TOAST_SIDE_MARGIN;
		width = room < TOAST_WIDE_WIDTH ? room : TOAST_WIDE_WIDTH;
		if (width < TOAST_WIDTH) {
			width = TOAST_WIDTH;
		}
	}
	lv_obj_set_width(card, width);
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_set_style_min_height(card, above ? TOAST_HEIGHT : 0, 0);
}

// How long a message stays: longer for a longer one.
static uint32_t shown_for(const char *text) {
	size_t length = text ? strlen(text) : 0;
	uint32_t ms = TOAST_MS;
	if (length > TOAST_QUICK_CHARS) {
		ms += (uint32_t)(length - TOAST_QUICK_CHARS) * TOAST_MS_PER_CHAR;
	}
	return ms < TOAST_MAX_MS ? ms : TOAST_MAX_MS;
}

// The one that does the work. `glyph` NULL leaves the card with the text alone,
// which the flex layout centres by itself once the image is out of the flow --
// hence the hidden flag rather than a zero size.
static void show(const lv_image_dsc_t *glyph, lv_color_t colour, const char *text) {
	if (!card) {
		return;
	}
	if (busy_spinner) {
		lv_obj_delete(busy_spinner);
		busy_spinner = NULL;
	}
	busy_dismissable = false;

	const char *shown = text ? tr(text) : "";
	if (glyph) {
		lv_image_set_src(icon, glyph);
		lv_obj_set_style_image_recolor(icon, colour, 0);
		lv_obj_set_hidden(icon, false);
	} else {
		lv_obj_set_hidden(icon, true);
	}
	fit_card(shown, glyph != NULL);

	lv_label_set_text(label, shown);
	lv_obj_set_hidden(veil, false);
	lv_obj_move_foreground(veil);
	lv_timer_set_period(hide_timer, shown_for(shown));
	lv_timer_reset(hide_timer);
	lv_timer_resume(hide_timer);
}

void toast_success(const char *text) { show(&icon_circle_check, TOAST_GREEN, text); }

void toast_error(const char *text) { show(&icon_circle_alert, TOAST_RED, text); }

void toast_plain(const char *text) { show(NULL, TOAST_GREEN, text); }

void toast_glyph(const lv_image_dsc_t *glyph, const char *text) { show(glyph, TOAST_GREEN, text); }

void toast_busy(const char *text) {
	if (!card) {
		return;
	}
	busy_dismissable = false;
	lv_timer_pause(hide_timer);
	lv_obj_set_hidden(icon, true);
	if (!busy_spinner) {
		busy_spinner = spinner_create(card, &icon_loader_big);
		lv_obj_move_to_index(busy_spinner, 0);
	}
	const char *shown = text ? tr(text) : "";
	fit_card(shown, true);
	lv_label_set_text(label, shown);
	lv_obj_set_hidden(veil, false);
	lv_obj_move_foreground(veil);
}

void toast_busy_end(void) {
	if (!busy_spinner) {
		return;
	}
	lv_obj_delete(busy_spinner);
	busy_spinner = NULL;
	busy_dismissable = false;
	lv_obj_set_hidden(veil, true);
}

void toast_busy_dismissable(const char *text) {
	toast_busy(text);
	busy_dismissable = true;
}

bool toast_busy_showing(void) { return busy_spinner != NULL; }

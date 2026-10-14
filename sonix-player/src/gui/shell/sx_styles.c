#include "sx_styles.h"

#include <math.h>

#include "src/gui/shell/sx_palette.h"

lv_style_t sx_style_page;
lv_style_t sx_style_surface;
lv_style_t sx_style_press;
lv_style_t sx_style_active;
lv_style_t sx_style_text;
lv_style_t sx_style_text2;
lv_style_t sx_style_text_accent;
lv_style_t sx_style_accent;
lv_style_t sx_style_icon;
lv_style_t sx_style_icon2;
lv_style_t sx_style_icon_accent;
lv_style_t sx_style_icon_on_accent;
lv_style_t sx_style_hairline;
lv_style_t sx_style_swipe_end;
lv_style_t sx_style_cover_tone;
lv_style_t sx_style_mini;
lv_style_t sx_style_mini_tone;
lv_style_t sx_style_np_accent;
lv_style_t sx_style_star_on;
lv_style_t sx_style_flat;

static void init_bg(lv_style_t *s) {
	lv_style_init(s);
	lv_style_set_bg_opa(s, LV_OPA_COVER);
}

static void init_recolour(lv_style_t *s) {
	lv_style_init(s);
	lv_style_set_image_recolor_opa(s, LV_OPA_COVER);
}

void sx_styles_init(void) {
	init_bg(&sx_style_page);
	init_bg(&sx_style_surface);
	init_bg(&sx_style_press);
	// The pressed fill is immediate and flat: no grow, no shadow (principle 2:
	// the fill changes at once, without animation).
	lv_style_set_transform_width(&sx_style_press, 0);
	lv_style_set_transform_height(&sx_style_press, 0);
	init_bg(&sx_style_active);
	lv_style_init(&sx_style_text);
	lv_style_init(&sx_style_text2);
	lv_style_init(&sx_style_text_accent);
	init_bg(&sx_style_accent);
	lv_style_set_image_recolor_opa(&sx_style_accent, LV_OPA_COVER);
	init_recolour(&sx_style_icon);
	init_recolour(&sx_style_icon2);
	init_recolour(&sx_style_icon_accent);
	init_recolour(&sx_style_icon_on_accent);
	lv_style_init(&sx_style_hairline);
	init_bg(&sx_style_swipe_end);
	init_bg(&sx_style_cover_tone);
	init_bg(&sx_style_mini);
	lv_style_set_text_color(&sx_style_mini, lv_color_white());
	init_bg(&sx_style_mini_tone);
	init_bg(&sx_style_np_accent);
	init_recolour(&sx_style_star_on);

	lv_style_init(&sx_style_flat);
	lv_style_set_bg_opa(&sx_style_flat, LV_OPA_TRANSP);
	lv_style_set_border_width(&sx_style_flat, 0);
	lv_style_set_outline_width(&sx_style_flat, 0);
	lv_style_set_shadow_width(&sx_style_flat, 0);
	lv_style_set_radius(&sx_style_flat, 0);
	lv_style_set_pad_all(&sx_style_flat, 0);
	lv_style_set_pad_gap(&sx_style_flat, 0);
}

// WCAG relative luminance of an sRGB colour.
static float luminance(lv_color_t c) {
	const uint8_t channel[3] = {c.red, c.green, c.blue};
	const float weight[3] = {0.2126f, 0.7152f, 0.0722f};
	float sum = 0.0f;
	for (int i = 0; i < 3; i++) {
		float v = channel[i] / 255.0f;
		v = v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
		sum += weight[i] * v;
	}
	return sum;
}

// The accent as writing. On the light theme the yellow preset (and the green
// and orange, a little less) is a fill colour that text on white cannot be
// read in -- the playing row's title, "24/96" -- so as text it is taken
// towards the theme's ink until it stands 3:1 against a card, the large-text
// minimum. Fills, pills and glyphs keep the accent itself; the dark theme and
// the cover's own accent (made for it) are untouched. Runs on a palette
// change only.
static lv_color_t text_accent(const sx_palette_t *p) {
	lv_color_t c = p->accent;
	if (p->dark) {
		return c;
	}
	float card = luminance(p->surface);
	for (int pct = 90; pct >= 40 && (card + 0.05f) / (luminance(c) + 0.05f) < 3.0f; pct -= 10) {
		c = sx_mix(p->accent, p->text, (uint8_t)pct);
	}
	return c;
}

void sx_styles_apply(void) {
	const sx_palette_t *p = sx_pal();

	lv_style_set_bg_color(&sx_style_page, p->bg);
	lv_style_set_bg_color(&sx_style_surface, p->surface);
	lv_style_set_text_color(&sx_style_surface, p->text);
	lv_style_set_bg_color(&sx_style_press, p->press);
	lv_style_set_bg_color(&sx_style_active, p->active);
	lv_style_set_text_color(&sx_style_text, p->text);
	lv_style_set_text_color(&sx_style_text2, p->text2);
	lv_style_set_text_color(&sx_style_text_accent, text_accent(p));
	lv_style_set_bg_color(&sx_style_accent, p->accent);
	lv_style_set_text_color(&sx_style_accent, p->on_accent);
	lv_style_set_image_recolor(&sx_style_accent, p->on_accent);
	lv_style_set_image_recolor(&sx_style_icon, p->text);
	lv_style_set_image_recolor(&sx_style_icon2, p->text2);
	lv_style_set_image_recolor(&sx_style_icon_accent, p->accent);
	lv_style_set_image_recolor(&sx_style_icon_on_accent, p->on_accent);
	lv_style_set_bg_color(&sx_style_hairline, p->hairline);
	lv_style_set_bg_opa(&sx_style_hairline, p->hairline_opa);
	lv_style_set_bg_color(&sx_style_swipe_end, p->swipe_end);
	lv_style_set_text_color(&sx_style_swipe_end, p->text);
	lv_style_set_image_recolor(&sx_style_swipe_end, p->text);
	lv_style_set_bg_color(&sx_style_cover_tone, p->cover_tone);
	lv_style_set_bg_color(&sx_style_mini, p->mini_bg);
	lv_style_set_bg_color(&sx_style_mini_tone, p->mini_cover_tone);
	lv_style_set_bg_color(&sx_style_np_accent, p->np_accent);
	lv_style_set_image_recolor(&sx_style_star_on, p->star_on);
}

lv_obj_t *sx_box(lv_obj_t *parent) {
	lv_obj_t *box = lv_obj_create(parent);
	lv_obj_remove_style_all(box);
	lv_obj_add_style(box, &sx_style_flat, 0);
	lv_obj_set_scrollable(box, false);
	lv_obj_set_clickable(box, false);
	lv_obj_set_click_focusable(box, false);
	lv_obj_set_event_bubble(box, true);
	return box;
}

lv_obj_t *sx_label(lv_obj_t *parent, const lv_font_t *font, lv_style_t *colour_style) {
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text(label, "");
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	if (font) {
		lv_obj_set_style_text_font(label, font, 0);
		// One line, and the height is what says so: LV_LABEL_LONG_DOT wraps
		// first and puts the dots in only where the wrapped text runs past
		// the height it is given -- left to its content, a long row title
		// took two lines and ran over the subtitle under it.
		lv_obj_set_height(label, lv_font_get_line_height(font));
	}
	if (colour_style) {
		lv_obj_add_style(label, colour_style, 0);
	}
	return label;
}

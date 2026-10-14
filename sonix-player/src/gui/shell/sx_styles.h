#ifndef GUI_SX_STYLES_H
#define GUI_SX_STYLES_H

#include "lvgl/lvgl.h"

// ---------------------------------------------------------------------------
// Sonix UI 2.0: the shared styles
//
// The colours of sx_palette.h as LVGL styles, for the 2.0 components and for
// any page built on them. As with theme_style_*: an object that adds one of
// these follows every later change of theme, accent and hue by itself, while
// a colour set by hand is a snapshot of the moment it was set.
//
// theme.c builds them (sx_styles_init from theme_init) and refills them on
// every palette change (sx_styles_apply). UI thread only.
// ---------------------------------------------------------------------------

extern lv_style_t sx_style_page;	   // bg: the page
extern lv_style_t sx_style_surface;	   // bg: a card, the sheet, the playing row
extern lv_style_t sx_style_press;	   // bg: under a finger (add with LV_STATE_PRESSED)
extern lv_style_t sx_style_active;	   // bg: 16 % accent in surface, "on now"
extern lv_style_t sx_style_text;	   // text: primary
extern lv_style_t sx_style_text2;	   // text: secondary
extern lv_style_t sx_style_text_accent; // text: the accent
extern lv_style_t sx_style_accent;	   // bg accent, text and glyphs in on_accent
extern lv_style_t sx_style_icon;	   // image recolour: primary text
extern lv_style_t sx_style_icon2;	   // image recolour: secondary text
extern lv_style_t sx_style_icon_accent; // image recolour: the accent
extern lv_style_t sx_style_hairline;   // bg: the 1 px line over a sheet or the mini player
extern lv_style_t sx_style_swipe_end;  // bg + text: the underlay of a row pulled left
extern lv_style_t sx_style_cover_tone; // bg: the plaque before a thumbnail, page hue
extern lv_style_t sx_style_mini;	   // bg mini_bg, text white: the mini player
extern lv_style_t sx_style_mini_tone;  // bg: the mini player's cover plaque
extern lv_style_t sx_style_np_accent;  // bg: the accent of the always-dark surfaces
extern lv_style_t sx_style_star_on;	   // image recolour: a set star

// No border, no shadow, no padding, no radius, transparent, no scrollbar: the
// starting point for every plain container a component builds.
extern lv_style_t sx_style_flat;

void sx_styles_init(void);
void sx_styles_apply(void);

// A plain container: flat, not scrollable, not clickable, presses bubbling to
// the parent. The building block of every component.
lv_obj_t *sx_box(lv_obj_t *parent);

// A one-line label in `font` and the colour of `colour_style` (one of the
// text styles above, or NULL to set the colour yourself), ending in an
// ellipsis when it does not fit its width.
lv_obj_t *sx_label(lv_obj_t *parent, const lv_font_t *font, lv_style_t *colour_style);

// Setters that leave the widget alone when it already shows the value. LVGL
// invalidates on every set, the same value included (and a label is laid out
// again), so a page refreshed from a poll -- the player's clock and buttons
// twice a second, the status bar -- repainted the same pixels over themselves.
// sx_set_text compares with what the label holds; a label that cuts its text
// with an ellipsis holds the cut text, and is simply set again.
void sx_set_text(lv_obj_t *label, const char *text);
void sx_set_src(lv_obj_t *img, const void *src);
// The image recoloured in `color` at full strength, as local style.
void sx_set_recolor(lv_obj_t *img, lv_color_t color);

#endif /* GUI_SX_STYLES_H */

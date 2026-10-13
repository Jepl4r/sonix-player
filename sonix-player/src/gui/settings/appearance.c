#include "appearance.h"

#include <stdint.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/corners.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/sx_card.h"
#include "src/gui/shell/sx_palette.h"
#include "src/gui/shell/sx_styles.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/topbar.h"
#include "src/gui/shell/touch.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"

lv_obj_t *appearance_screen;

static lv_obj_t *btn_dark;
static lv_obj_t *btn_light;
static lv_obj_t *btn_clock[4]; // left / centre / right / hidden
// The coloured circles: the cover's own first (ACCENT_SLOT_COVER), then the
// six presets, preset i in slot i + 1.
#define ACCENT_SLOT_COVER 0
#define ACCENT_SLOTS (THEME_ACCENT_COUNT + 1)
static lv_obj_t *btn_accent[ACCENT_SLOTS];
static lv_obj_t *cover_mark[2]; // the half moon on the cover's circle: disc, then the bite out of it
static lv_obj_t *tint_toggle;
static lv_obj_t *battery_percent_toggle;
static lv_obj_t *statusbar_always_toggle;
static lv_obj_t *rounded_corners_toggle;
static lv_obj_t *large_rows_toggle;
static lv_obj_t *text_size_pill[2]; // normal / large
// The status bar draws the battery either way; this setting is only about the
// number beside it, and is on by default.
static void battery_percent_cb(lv_event_t *e) {
	(void)e;
	bool shown = lv_obj_has_state(battery_percent_toggle, LV_STATE_CHECKED);
	topbar_set_battery_percent(shown);
	config_set_int("screen", "battery_percent", shown ? 1 : 0);
	config_save();
}

static void statusbar_always_cb(lv_event_t *e) {
	(void)e;
	bool on = lv_obj_has_state(statusbar_always_toggle, LV_STATE_CHECKED);
	player_set_statusbar_always(on);
	config_set_int("screen", "statusbar_always", on ? 1 : 0);
	config_save();
}

static void large_rows_cb(lv_event_t *e) {
	(void)e;
	touch_set_large_rows(lv_obj_has_state(large_rows_toggle, LV_STATE_CHECKED));
}

static void rounded_corners_cb(lv_event_t *e) {
	(void)e;
	corners_set_enabled(lv_obj_has_state(rounded_corners_toggle, LV_STATE_CHECKED));
}

// The accent's hue spread over the backgrounds and cards. theme_set_dynamic_tint
// repaints the interface itself, including this page.
static void dynamic_tint_cb(lv_event_t *e) {
	(void)e;
	theme_set_dynamic_tint(lv_obj_has_state(tint_toggle, LV_STATE_CHECKED));
}

static void refresh_text_size_pills(void) {
	int active = fonts_large_text() ? FONTS_TEXT_LARGE : FONTS_TEXT_NORMAL;
	for (int i = 0; i < 2; i++) {
		settingsrow_pill_active(text_size_pill[i], i == active);
	}
}

// Applied at once, on every page: see fonts_set_large_text().
static void text_size_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	int size = (int)(intptr_t)lv_event_get_user_data(e);
	if (!fonts_set_large_text(size == FONTS_TEXT_LARGE)) {
		return;
	}
	config_set_int("ui", "text_size", size);
	config_save();
	refresh_text_size_pills();
}

// The active choice is the accent pill, the rest the neutral ones; both
// follow the palette by themselves (sx_pill_set_active).
static void refresh_buttons(void) {
	bool dark = theme_is_dark();
	settingsrow_pill_active(btn_dark, dark);
	settingsrow_pill_active(btn_light, !dark);
}

static int slot_accent(int slot) { return slot == ACCENT_SLOT_COVER ? THEME_ACCENT_COVER : slot - 1; }

// The active circle wears a ring in the text colour; the rest sit flat. The
// cover's circle is in the colour the cover gives now -- it changes with the
// album -- and carries a half moon, the handoff's "from the cover" mark.
static void refresh_accent_buttons(void) {
	int active_accent = theme_get_accent();
	for (int i = 0; i < ACCENT_SLOTS; i++) {
		if (!btn_accent[i]) {
			continue;
		}
		int accent = slot_accent(i);
		lv_obj_set_style_bg_color(btn_accent[i], accent == THEME_ACCENT_COVER ? sx_palette_cover_accent()
																			  : theme_accent_preset(accent),
								  0);
		sx_swatch_set_active(btn_accent[i], accent == active_accent);
	}
	if (cover_mark[0]) {
		lv_obj_set_style_bg_color(cover_mark[0], sx_palette_cover_on_accent(), 0);
		lv_obj_set_style_bg_color(cover_mark[1], sx_palette_cover_accent(), 0);
	}
}

static void accent_pick_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	int slot = (int)(intptr_t)lv_event_get_user_data(e);
	theme_set_accent(slot_accent(slot));
	refresh_accent_buttons();
}

static lv_obj_t *make_mark_disc(lv_obj_t *parent, int size, int x) {
	lv_obj_t *disc = sx_box(parent);
	lv_obj_set_size(disc, size, size);
	lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
	lv_obj_align(disc, LV_ALIGN_CENTER, x, 0);
	return disc;
}

static lv_obj_t *make_accent_circle(lv_obj_t *parent, int slot) {
	lv_obj_t *btn = sx_swatch(parent, lv_color_black(), slot, accent_pick_cb);
	if (slot == ACCENT_SLOT_COVER) {
		// A disc with a bite taken out of its right half: a half moon in two
		// plain circles, the second in the swatch's own colour.
		cover_mark[0] = make_mark_disc(btn, 20, 0);
		cover_mark[1] = make_mark_disc(btn, 20, 9);
	}
	return btn;
}

// The clock-position pills the same way: accent on the active one.
static void refresh_clock_buttons(void) {
	int active = (int)config_get_int("screen", "clock_pos", TOPBAR_CLOCK_CENTER);
	for (int i = 0; i < 4; i++) {
		settingsrow_pill_active(btn_clock[i], i == active);
	}
}

static void clock_pos_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	int pos = (int)(intptr_t)lv_event_get_user_data(e);
	config_set_int("screen", "clock_pos", pos);
	config_save();
	topbar_set_clock_position(pos);
	refresh_clock_buttons();
}

static void pick_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	bool dark = (bool)(uintptr_t)lv_event_get_user_data(e);
	theme_set_dark(dark);
	refresh_buttons();
}

// The 2.0 page (the handoff's "Appearance"): one card per setting, choices on
// pills, the accent as a row of swatches, and every switch with what it does
// written inside its card instead of in a note under it.
void appearance_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(appearance_screen, cfg, "appearance");

	lv_obj_t *pills = NULL;
	settingsrow_pills(container, "appearance_theme", &pills);
	btn_dark = settingsrow_pill(pills, "appearance_dark", true, pick_cb);
	btn_light = settingsrow_pill(pills, "appearance_light", false, pick_cb);

	// Where the clock sits in the status bar. The pills wrap: four translated
	// words do not fit across 480 pixels in every language.
	settingsrow_pills(container, "appearance_clock_position", &pills);
	btn_clock[TOPBAR_CLOCK_LEFT] = settingsrow_pill(pills, "left", TOPBAR_CLOCK_LEFT, clock_pos_cb);
	btn_clock[TOPBAR_CLOCK_CENTER] = settingsrow_pill(pills, "centre", TOPBAR_CLOCK_CENTER, clock_pos_cb);
	btn_clock[TOPBAR_CLOCK_RIGHT] = settingsrow_pill(pills, "right", TOPBAR_CLOCK_RIGHT, clock_pos_cb);
	btn_clock[TOPBAR_CLOCK_HIDDEN] = settingsrow_pill(pills, "hide", TOPBAR_CLOCK_HIDDEN, clock_pos_cb);

	// The accent: the cover's own first, then the six presets, spread across
	// the card in one line (seven 52 px circles fit 410 px with room between).
	lv_obj_t *accent_card = sx_card_create(container);
	sx_card_title(accent_card, tr("appearance_accent_colour"));
	lv_obj_t *accent_row = sx_card_swatches(accent_card);
	lv_obj_set_flex_flow(accent_row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(accent_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(accent_row, 0, 0);
	lv_obj_set_style_pad_bottom(accent_row, 4, 0);
	for (int i = 0; i < ACCENT_SLOTS; i++) {
		btn_accent[i] = make_accent_circle(accent_row, i);
	}

	// Straight under the accent, because it is what the tint is made from: the
	// same hue carried across the backgrounds, the cards and the status bar.
	settingsrow_switch_card(container, "appearance_dynamic_tint", "appearance_dynamic_tint_note", &tint_toggle,
							dynamic_tint_cb);
	if (theme_dynamic_tint()) {
		lv_obj_add_state(tint_toggle, LV_STATE_CHECKED);
	}

	// Taller rows in the library and the folder browser (touch.h): "make it
	// easier to hit", with the text size under it as "easier to read".
	settingsrow_switch_card(container, "appearance_large_rows", "appearance_large_rows_sub", &large_rows_toggle,
							large_rows_cb);
	if (touch_large_rows()) {
		lv_obj_add_state(large_rows_toggle, LV_STATE_CHECKED);
	}

	// How big the small text on every page is: the secondary lines, notes,
	// clocks and counters. See fonts.c for which sizes grow and by how much.
	settingsrow_pills(container, "appearance_text_size", &pills);
	text_size_pill[FONTS_TEXT_NORMAL] =
		settingsrow_pill(pills, "appearance_text_normal", FONTS_TEXT_NORMAL, text_size_cb);
	text_size_pill[FONTS_TEXT_LARGE] = settingsrow_pill(pills, "appearance_text_large", FONTS_TEXT_LARGE, text_size_cb);

	// Whether the status bar prints the charge as a number as well as drawing
	// it.
	settingsrow_switch_card(container, "appearance_battery_percentage", NULL, &battery_percent_toggle,
							battery_percent_cb);
	if (config_get_int("screen", "battery_percent", 1) != 0) {
		lv_obj_add_state(battery_percent_toggle, LV_STATE_CHECKED);
	}

	// The bar up in the player too, where it otherwise steps aside for the
	// artwork.
	settingsrow_switch_card(container, "appearance_statusbar_always", "appearance_statusbar_always_sub",
							&statusbar_always_toggle, statusbar_always_cb);
	if (player_statusbar_always()) {
		lv_obj_add_state(statusbar_always_toggle, LV_STATE_CHECKED);
	}

	// Rounds off the four corners of the screen, over every page and overlay.
	settingsrow_switch_card(container, "appearance_rounded_corners", NULL, &rounded_corners_toggle,
							rounded_corners_cb);
	if (corners_enabled()) {
		lv_obj_add_state(rounded_corners_toggle, LV_STATE_CHECKED);
	}

	refresh_buttons();
	refresh_clock_buttons();
	refresh_accent_buttons();
	refresh_text_size_pills();
	theme_register_refresh(refresh_buttons);
	theme_register_refresh(refresh_text_size_pills);
	theme_register_refresh(refresh_clock_buttons);
	theme_register_refresh(refresh_accent_buttons);
}

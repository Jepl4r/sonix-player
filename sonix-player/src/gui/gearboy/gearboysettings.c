#include "gearboysettings.h"

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/gearboy/gearboy.h"
#include "src/system/core/lang.h"

#include <stdint.h>

lv_obj_t *gearboysettings_screen;

// The two pill rows: 5 palettes and 4 shaders. Same shape as the MSEB radius
// and keyboard-type pills, but wrapped over several lines: five names do not
// fit across 480 pixels.
static lv_obj_t *palette_pills[5];
static lv_obj_t *shader_pills[4];
static lv_obj_t *correction_switch;
static lv_obj_t *bootrom_switch;

static const char *const PALETTE_NAMES[5] = {
	"gearboy_palette_original", "gearboy_palette_sharp", "gearboy_palette_bw", "gearboy_palette_autumn", "gearboy_palette_soft",
};
static const char *const SHADER_NAMES[4] = {
	"Pixel perfect", "GBC", "GBC Dot matrix", "GB Dot matrix",
};

// Repaints the pills: accent on the selected one, quiet on the rest.
static void refresh_pills(void) {
	int pal = (int)config_get_int("gearboy", "palette", 0);
	int sh = (int)config_get_int("gearboy", "shader", 0);
	for (int i = 0; i < 5; i++) {
		if (!palette_pills[i]) {
			continue;
		}
		bool on = i == pal;
		lv_obj_set_style_bg_color(palette_pills[i], on ? theme()->accent : theme()->surface_pressed, 0);
		lv_obj_set_style_text_color(lv_obj_get_child(palette_pills[i], 0),
									on ? lv_color_white() : theme()->text_primary, 0);
	}
	for (int i = 0; i < 4; i++) {
		if (!shader_pills[i]) {
			continue;
		}
		bool on = i == sh;
		lv_obj_set_style_bg_color(shader_pills[i], on ? theme()->accent : theme()->surface_pressed, 0);
		lv_obj_set_style_text_color(lv_obj_get_child(shader_pills[i], 0),
									on ? lv_color_white() : theme()->text_primary, 0);
	}
}

static void palette_pick_cb(lv_event_t *e) {
	config_set_int("gearboy", "palette", (int)(intptr_t)lv_event_get_user_data(e));
	config_save();
	gearboy_refresh_video_settings();
	refresh_pills();
}

static void shader_pick_cb(lv_event_t *e) {
	config_set_int("gearboy", "shader", (int)(intptr_t)lv_event_get_user_data(e));
	config_save();
	gearboy_refresh_video_settings();
	refresh_pills();
}

static void correction_toggle_cb(lv_event_t *e) {
	(void)e;
	config_set_int("gearboy", "gbc_correction", lv_obj_has_state(correction_switch, LV_STATE_CHECKED) ? 1 : 0);
	config_save();
	gearboy_refresh_video_settings();
}

static void bootrom_toggle_cb(lv_event_t *e) {
	(void)e;
	config_set_int("gearboy", "bootrom", lv_obj_has_state(bootrom_switch, LV_STATE_CHECKED) ? 1 : 0);
	config_save();
	// No refresh: the boot ROMs are only consulted when a game starts.
}

static lv_obj_t *make_pill(lv_obj_t *parent, const char *text, lv_event_cb_t cb, int value) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, LV_SIZE_CONTENT, 60);
	lv_obj_set_style_pad_hor(btn, 18, 0);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0); // the Adwaita pill
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)(intptr_t)value);

	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, tr(text));
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_obj_center(label);
	return btn;
}

// A card with a title and a wrapping row of pills inside.
static lv_obj_t *make_pill_card(lv_obj_t *parent, const char *title_tag) {
	lv_obj_t *card = lv_obj_create(parent);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 12, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, 16, 0);
	lv_obj_set_style_pad_row(card, 12, 0);
	lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_obj_t *title = lv_label_create(card);
	lv_label_set_text(title, tr(title_tag));
	lv_obj_add_style(title, &theme_style_text, 0);
	lv_obj_set_style_text_font(title, &font_ui_24, 0);

	lv_obj_t *pills = lv_obj_create(card);
	lv_obj_set_width(pills, lv_pct(100));
	lv_obj_set_height(pills, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(pills, 0, 0);
	lv_obj_set_style_border_width(pills, 0, 0);
	lv_obj_set_style_pad_all(pills, 0, 0);
	lv_obj_set_style_pad_column(pills, 8, 0);
	lv_obj_set_style_pad_row(pills, 8, 0);
	lv_obj_remove_flag(pills, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(pills, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(pills, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	return pills;
}

void gearboysettings_init(gui_config_t *cfg) {
	gearboysettings_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(gearboysettings_screen, cfg, "gearboy_settings");

	// Palette: applies to original Game Boy games only, since GBC titles carry
	// their own colours.
	lv_obj_t *pal_pills = make_pill_card(container, "gearboy_palette");
	for (int i = 0; i < 5; i++) {
		palette_pills[i] = make_pill(pal_pills, PALETTE_NAMES[i], palette_pick_cb, i);
	}

	// Shaders: the real panels' grid, drawn inside the 3x upscale.
	lv_obj_t *sh_pills = make_pill_card(container, "gearboy_shaders");
	for (int i = 0; i < 4; i++) {
		shader_pills[i] = make_pill(sh_pills, SHADER_NAMES[i], shader_pick_cb, i);
	}

	settingsrow_toggle(container, "gearboy_gbc_color_correction", &correction_switch, correction_toggle_cb);
	if (config_get_int("gearboy", "gbc_correction", 1) != 0) {
		lv_obj_add_state(correction_switch, LV_STATE_CHECKED);
	}

	// Boot ROMs: the logo scrolling down at start-up, as on the real machine.
	// The toggle carries its instructions underneath, because without the two
	// files it does nothing and the user has to be told where to put them.
	settingsrow_toggle(container, "gearboy_show_boot_rom", &bootrom_switch, bootrom_toggle_cb);
	if (config_get_int("gearboy", "bootrom", 0) != 0) {
		lv_obj_add_state(bootrom_switch, LV_STATE_CHECKED);
	}
	lv_obj_t *note = lv_label_create(container);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_18, 0);
	lv_label_set_text(note, tr("gearboy_boot_rom_note"));

	refresh_pills();
	theme_register_refresh(refresh_pills);
	switcher_attach_back_gesture(gearboysettings_screen);
}

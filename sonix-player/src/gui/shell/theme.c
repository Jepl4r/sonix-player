#include "theme.h"

#include "src/gui/shell/sx_palette.h"
#include "src/gui/shell/sx_styles.h"
#include "src/system/device/bootlogo.h"
#include "src/system/core/config.h"

#include <stdio.h>
#include <stdlib.h>

lv_style_t theme_style_screen;
lv_style_t theme_style_panel;
lv_style_t theme_style_card;
lv_style_t theme_style_card_pressed;
lv_style_t theme_style_text;
lv_style_t theme_style_text_dim;
lv_style_t theme_style_icon;
lv_style_t theme_style_switch;		   // switch track, off state
lv_style_t theme_style_switch_checked; // switch indicator, on state
lv_style_t theme_style_accent_bg;	   // anything that must follow the accent
lv_style_t theme_style_slider_track;   // a slider's unfilled part

// The two palettes theme() hands out. Since 2.0 they are not presets any more
// but views of sx_palette.c's colours (see rebuild_active): the fields keep
// their names and meanings, so the hundred-odd places that read theme()->accent
// or theme()->surface carry on, now in the cover's colours.
static theme_palette_t palette_dark = {.dark = true};
static theme_palette_t palette_light = {.dark = false};

// Whether the dark theme is the chosen one.
static bool dark_selected = true;

// "Tint from cover" (Appearance; was "Dynamic tint" before 2.0): the
// backgrounds, cards and the status bar take the cover's hue. On unless the
// user turned it off -- the 2.0 look -- remembered in the config.
static bool dynamic_tint;

static theme_palette_t *active = &palette_dark;

// The accent presets, Adwaita's named accents, ordered as requested: blue
// (the default before 2.0), then warm to cool -- orange, yellow, green,
// purple, pink. The default since 2.0 is none of them but the cover's own
// (THEME_ACCENT_COVER).
static const lv_color_t ACCENT_COLORS[THEME_ACCENT_COUNT] = {
	LV_COLOR_MAKE(53, 132, 228), // blue (#3584e4)
	LV_COLOR_MAKE(255, 120, 0),  // orange (#ff7800)
	LV_COLOR_MAKE(229, 165, 10), // yellow (#e5a50a)
	LV_COLOR_MAKE(46, 194, 126), // green (#2ec27e)
	LV_COLOR_MAKE(145, 65, 172), // purple (#9141ac)
	LV_COLOR_MAKE(213, 97, 153), // pink (#d56199)
};
static int accent_index = THEME_ACCENT_COVER;

lv_color_t theme_accent_preset(int index) {
	if (index == THEME_ACCENT_COVER) {
		return sx_pal()->accent;
	}
	if (index < 0 || index >= THEME_ACCENT_COUNT) {
		index = 0;
	}
	return ACCENT_COLORS[index];
}

int theme_get_accent(void) { return accent_index; }

// Callers asking to be called back after a theme or accent change. The list
// grows on demand and has no fixed limit: a dropped registration shows up only
// as a page that keeps the colours it was built with, which is invisible until
// somebody changes the accent. A failed allocation is logged for the same
// reason.
static theme_refresh_cb_t *refresh_cbs;
static int refresh_cb_count;
static int refresh_cb_capacity;

// Fills `active` from sx_palette.c's colours. With `configure`, hands the
// Appearance choices over first, which recomputes them; without, takes the
// palette as it stands -- after a change of hue, which sx_palette.c has
// already worked out.
static void rebuild_active(bool configure) {
	if (configure) {
		lv_color_t preset = ACCENT_COLORS[accent_index >= 0 && accent_index < THEME_ACCENT_COUNT ? accent_index : 0];
		sx_palette_configure(dark_selected, accent_index, preset, dynamic_tint);
	}
	const sx_palette_t *p = sx_pal();

	theme_palette_t *t = dark_selected ? &palette_dark : &palette_light;
	t->screen_bg = p->bg;
	t->surface = p->surface;
	t->surface_pressed = p->press;
	// 2.0 draws the status bar in the page's own colour: one surface from the
	// top edge down, the bar telling itself apart only by what it carries.
	t->panel = p->bg;
	// Behind a missing artwork: a step below the page, as before 2.0.
	t->cover_bg = dark_selected ? sx_mix(lv_color_black(), p->bg, 20) : p->press;
	t->text_primary = p->text;
	t->text_secondary = p->text2;
	t->accent = p->accent;
	t->on_accent = p->on_accent;
	t->dark = dark_selected;
	active = t;
}

// Pushes the active palette into the shared styles.
static void apply_palette(void) {
	lv_style_set_bg_color(&theme_style_screen, active->screen_bg);

	lv_style_set_bg_color(&theme_style_panel, active->panel);

	lv_style_set_bg_color(&theme_style_card, active->surface);
	lv_style_set_text_color(&theme_style_card, active->text_primary);

	lv_style_set_bg_color(&theme_style_card_pressed, active->surface_pressed);

	lv_style_set_text_color(&theme_style_text, active->text_primary);
	lv_style_set_text_color(&theme_style_text_dim, active->text_secondary);

	lv_style_set_image_recolor(&theme_style_icon, active->text_primary);

	lv_style_set_bg_color(&theme_style_switch, active->surface_pressed);
	lv_style_set_bg_color(&theme_style_switch_checked, active->accent);

	lv_style_set_bg_color(&theme_style_accent_bg, active->accent);
	lv_style_set_bg_color(&theme_style_slider_track, active->text_secondary);

	// The 2.0 components' own shared styles (sx_*.c) follow the same palette.
	sx_styles_apply();
}

static void refresh_light(void);

// The hue moved under an unchanged theme: the playing album changed, or an
// album page put up its own colours. Only colours change, and a colour is read
// out of the shared styles at draw time, so invalidating what is on screen is
// the whole repaint (refresh_light) -- no walk of every object's styles, which
// is what a theme switch pays for.
static void hue_changed(void) {
	rebuild_active(false);
	apply_palette();
	refresh_light();
}

void theme_init(void) {
	sx_styles_init();

	lv_style_init(&theme_style_screen);
	lv_style_set_bg_opa(&theme_style_screen, LV_OPA_COVER);

	lv_style_init(&theme_style_panel);
	lv_style_set_bg_opa(&theme_style_panel, LV_OPA_COVER);

	lv_style_init(&theme_style_card);
	lv_style_set_bg_opa(&theme_style_card, LV_OPA_COVER);

	lv_style_init(&theme_style_card_pressed);
	lv_style_set_bg_opa(&theme_style_card_pressed, LV_OPA_COVER);
	// The default theme grows a pressed button by a few pixels; on a list of
	// cards that reads as the row jumping out of the list, so pin it flat.
	lv_style_set_transform_width(&theme_style_card_pressed, 0);
	lv_style_set_transform_height(&theme_style_card_pressed, 0);

	lv_style_init(&theme_style_text);
	lv_style_init(&theme_style_text_dim);

	lv_style_init(&theme_style_icon);
	lv_style_set_image_recolor_opa(&theme_style_icon, LV_OPA_COVER);

	lv_style_init(&theme_style_switch);
	lv_style_set_bg_opa(&theme_style_switch, LV_OPA_COVER);
	lv_style_init(&theme_style_switch_checked);
	lv_style_set_bg_opa(&theme_style_switch_checked, LV_OPA_COVER);

	lv_style_init(&theme_style_accent_bg);
	lv_style_set_bg_opa(&theme_style_accent_bg, LV_OPA_COVER);
	lv_style_init(&theme_style_slider_track);
	lv_style_set_bg_opa(&theme_style_slider_track, LV_OPA_40);
	lv_style_set_radius(&theme_style_slider_track, LV_RADIUS_CIRCLE);

	// Whatever was chosen last time. Dark is the default for a first boot.
	dark_selected = config_get_bool("ui", "dark_theme", true);

	// The accent as it was left. Absent -- nobody ever picked one -- means the
	// 2.0 default, the cover's own; a preset chosen before 2.0 is kept.
	accent_index = (int)config_get_int("ui", "accent", THEME_ACCENT_COVER);
	if (accent_index != THEME_ACCENT_COVER && (accent_index < 0 || accent_index >= THEME_ACCENT_COUNT)) {
		accent_index = THEME_ACCENT_COVER;
	}

	// On by default since 2.0; someone who switched the old "Dynamic tint" off
	// has it written down as off and keeps it so.
	dynamic_tint = config_get_bool("ui", "dynamic_tint", true);

	rebuild_active(true);
	apply_palette();

	// A new album, or an album page with its own colours: repaint in place.
	sx_palette_set_change_hook(hue_changed);
}

void theme_apply_slider_knob(lv_obj_t *slider) {
	if (!slider) {
		return;
	}
	bool dark = theme()->dark;
	lv_obj_set_style_border_color(slider, dark ? lv_color_black() : lv_color_make(120, 120, 120), LV_PART_KNOB);
	lv_obj_set_style_border_opa(slider, dark ? LV_OPA_10 : LV_OPA_COVER, LV_PART_KNOB);
	lv_obj_set_style_border_width(slider, dark ? 1 : 2, LV_PART_KNOB);
}

void theme_register_refresh(theme_refresh_cb_t cb) {
	if (!cb) {
		return;
	}

	if (refresh_cb_count == refresh_cb_capacity) {
		int wanted = refresh_cb_capacity ? refresh_cb_capacity * 2 : 16;
		theme_refresh_cb_t *grown = realloc(refresh_cbs, (size_t)wanted * sizeof(*grown));
		if (!grown) {
			// A device that cannot find a few dozen bytes has bigger problems,
			// but a registration must never be lost unnoticed.
			fprintf(stderr, "theme: out of memory for refresh callback number %d\n",
					refresh_cb_count + 1);
			return;
		}
		refresh_cbs = grown;
		refresh_cb_capacity = wanted;
	}

	refresh_cbs[refresh_cb_count++] = cb;
}

const theme_palette_t *theme(void) { return active; }

bool theme_is_dark(void) { return active->dark; }

void theme_set_dark(bool dark) {
	if (active->dark == dark) {
		return;
	}
	theme_toggle();
}

// Which palette the pages on screen were last walked for.
//
// lv_obj_report_style_change(NULL) walks every object of every screen and
// refreshes each one against its whole subtree. The pages here are built at
// startup and stay built, so that is the entire interface -- thousands of
// objects -- for a change that moves nothing but colours, and it takes far
// longer than a frame: the finger is still on the switch when it starts.
//
// A colour is read out of the style at draw time, so the page in front of the
// user is the only one that has to be walked at the moment of the change. The
// pages behind it are walked the first time they are shown again, which is
// where a redraw was going to happen anyway.
static unsigned palette_generation = 1;

static struct screen_generation {
	lv_obj_t *screen;
	unsigned generation;
} *screen_gens;
static int screen_gen_count;
static int screen_gen_capacity;

// True when `screen` still carries an older palette than the one in force, and
// records it as walked for this one. A screen this has never seen is taken as
// current: it was built from the styles as they stand.
//
// An allocation that fails costs a walk per page change rather than a page
// with the wrong colours, which is why the failure is silent here.
static bool screen_is_stale(lv_obj_t *screen) {
	for (int i = 0; i < screen_gen_count; i++) {
		if (screen_gens[i].screen != screen) {
			continue;
		}
		if (screen_gens[i].generation == palette_generation) {
			return false;
		}
		screen_gens[i].generation = palette_generation;
		return true;
	}

	if (screen_gen_count == screen_gen_capacity) {
		int wanted = screen_gen_capacity ? screen_gen_capacity * 2 : 32;
		struct screen_generation *grown = realloc(screen_gens, (size_t)wanted * sizeof(*grown));
		if (!grown) {
			return true;
		}
		screen_gens = grown;
		screen_gen_capacity = wanted;
	}

	screen_gens[screen_gen_count].screen = screen;
	screen_gens[screen_gen_count].generation = palette_generation;
	screen_gen_count++;
	return false;
}

// One tree, refreshed against the styles it carries. LV_STYLE_PROP_ANY takes
// the object's own properties and recurses into its children.
static void refresh_tree(lv_obj_t *obj) {
	if (obj) {
		lv_obj_refresh_style(obj, LV_PART_ANY, LV_STYLE_PROP_ANY);
	}
}

void theme_notify_screen_shown(lv_obj_t *screen) {
	if (screen && screen_is_stale(screen)) {
		refresh_tree(screen);
	}
}

// Repaints after a palette change: the page on screen and the three layers the
// status bar, the player sheet and the popups live on, then the callbacks,
// which catch what cannot be expressed as a shared style -- an icon recoloured
// at runtime, the blurred artwork behind the controls. Those run for every
// page, on screen or not, and cost about as much as one frame in total.
static void refresh_all(void) {
	palette_generation++;

	lv_obj_t *screen = lv_screen_active();
	screen_is_stale(screen);
	refresh_tree(screen);
	refresh_tree(lv_layer_top());
	refresh_tree(lv_layer_sys());
	refresh_tree(lv_layer_bottom());

	for (int i = 0; i < refresh_cb_count; i++) {
		refresh_cbs[i]();
	}
}

// The cheap repaint, for a change of hue: everything visible is redrawn from
// the styles as they now stand, and the callbacks recolour what was set by
// hand. Pages off screen need nothing -- they read the styles when drawn.
static void refresh_light(void) {
	lv_obj_invalidate(lv_screen_active());
	lv_obj_invalidate(lv_layer_top());
	lv_obj_invalidate(lv_layer_sys());
	lv_obj_invalidate(lv_layer_bottom());

	for (int i = 0; i < refresh_cb_count; i++) {
		refresh_cbs[i]();
	}
}

void theme_toggle(void) {
	dark_selected = !dark_selected;
	rebuild_active(true);
	apply_palette();

	config_set_bool("ui", "dark_theme", active->dark);
	config_save();

	// The picture shown while the player boots follows the theme too. It is
	// written into raw flash rather than into the config, because the script
	// that draws it runs long before any filesystem holding a config is
	// mounted -- see bootlogo.h.
	bootlogo_follow_theme(active->dark);

	refresh_all();
}

void theme_set_accent(int index) {
	if (index != THEME_ACCENT_COVER && (index < 0 || index >= THEME_ACCENT_COUNT)) {
		index = 0;
	}
	accent_index = index;
	rebuild_active(true);
	apply_palette();

	config_set_int("ui", "accent", index);
	config_save();

	refresh_all();
}

bool theme_dynamic_tint(void) { return dynamic_tint; }

void theme_set_dynamic_tint(bool on) {
	if (dynamic_tint == on) {
		return;
	}
	dynamic_tint = on;
	rebuild_active(true);
	apply_palette();

	config_set_bool("ui", "dynamic_tint", on);
	config_save();

	refresh_all();
}

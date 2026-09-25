#include "remap.h"

#include "src/system/core/respath.h"

// lv_image_cache_drop() moved here in LVGL 9.2; lvgl.h no longer pulls it in.
#include "lvgl/src/misc/cache/instance/lv_image_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/image/stb_image_decl.h"
#include "src/system/input/keymap.h"
#include "src/system/core/lang.h"
#include "src/system/device/sysinfo.h"

lv_obj_t *remap_screen;

// ---------------------------------------------------------------------------
// The photos
//
// The R3 Pro II has buttons on both flanks: the media rocker under the power
// button on the right, the volume rocker on the left. The page shows both at
// once, cut to the part that carries buttons: the right flank at the top left
// with its rows beside it, the left flank at the bottom right with its rows to
// its left, the way the device reads turned over in the hand.
//
// The R1 has every button on its right flank -- power, the volume rocker, then
// play/pause over next -- so there it is one photo, the media side's files, cut
// off at the bottom and sitting on the bottom edge, with four taller rows.
//
// The photos live in /usr/resource/sonix/gui rather than in the binary, and are
// loaded when the page opens and freed when it closes: a picture seen once a
// year does not stay resident on a 64 MB device.
//
// Two files per side, one per theme: the silver device in the dark theme, the
// black one in the light theme. A dark photo on a dark background has no
// outline, and the button profile is exactly what has to be recognised here.
//
// The PNG has transparent corners, so transparency is resolved once here by
// blending every pixel against the page background; from there on it is plain
// RGB565, the panel's format.
// ---------------------------------------------------------------------------

#define PHOTO_PLAY_DARK SONIX_RESOURCE_DIR "/gui/remap-play-dark.png"
#define PHOTO_PLAY_LIGHT SONIX_RESOURCE_DIR "/gui/remap-play-light.png"
#define PHOTO_VOL_DARK SONIX_RESOURCE_DIR "/gui/remap-vol-dark.png"
#define PHOTO_VOL_LIGHT SONIX_RESOURCE_DIR "/gui/remap-vol-light.png"
#define PHOTO_MAX_BYTES (2 * 1024 * 1024)

// Length of the leader line tying a row to its button, and the row heights.
#define LEADER_W 18
#define ROW_H 44
#define R1_ROW_H 56

// The buttons, as rectangles inside the photo. They are generous in width --
// the pictured button is thirty pixels wide, which is not a touch target -- but
// stay inside the device outline, so a finger that misses hits nothing rather
// than the wrong button.
typedef struct {
	keymap_button_t button;
	int y, h; // within the photo
} hit_t;

// The R3 Pro II's right flank, top to bottom. Power is absent: it does one
// thing and that is not negotiable.
static const hit_t PLAY_HITS[] = {
	{KEYMAP_BTN_PREV, 22, 50},
	{KEYMAP_BTN_PLAY, 85, 58},
	{KEYMAP_BTN_NEXT, 143, 59},
};

// And its left flank. Below the rocker is the microSD door, which is not a
// button.
static const hit_t VOL_HITS[] = {
	{KEYMAP_BTN_VOL_UP, 145, 57},
	{KEYMAP_BTN_VOL_DOWN, 202, 58},
};

// The R1's one flank, top to bottom under the power button: the volume rocker,
// then play/pause over next.
static const hit_t R1_HITS[] = {
	{KEYMAP_BTN_VOL_UP, 190, 64},
	{KEYMAP_BTN_VOL_DOWN, 254, 64},
	{KEYMAP_BTN_PLAY, 360, 66},
	{KEYMAP_BTN_NEXT, 426, 67},
};

// How wide the touch zone over a flank is: the buttons sit on its outer edge,
// on the right of a right flank and on the left of a left one.
#define HIT_W 86

typedef struct {
	const char *dark;  // file for the dark theme
	const char *light; // and for the light one
	int x, y, w, h;	   // where the photo sits on screen, and its size
	const hit_t *hits;
	int hit_count;
	bool rows_on_right; // a right flank: buttons on the photo's right, rows beyond

	lv_obj_t *image; // NULL for a side this player does not have
	lv_image_dsc_t dsc;
	uint8_t *pixels; // RGB565, owned here
} side_t;

static side_t play_side = {.dark = PHOTO_PLAY_DARK, .light = PHOTO_PLAY_LIGHT};
static side_t vol_side = {.dark = PHOTO_VOL_DARK, .light = PHOTO_VOL_LIGHT};

static lv_obj_t *value_labels[KEYMAP_BTN_COUNT];
static lv_obj_t *rows[KEYMAP_BTN_COUNT];

static int row_h;
static const lv_font_t *row_font;

// ---------------------------------------------------------------------------
// the photos
// ---------------------------------------------------------------------------

static uint16_t pack565(int r, int g, int b) {
	return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void side_free(side_t *side) {
	if (side->image) {
		lv_image_set_src(side->image, NULL);
		lv_obj_add_flag(side->image, LV_OBJ_FLAG_HIDDEN);
	}
	if (side->pixels) {
		// LVGL's cache is indexed by pointer: without this, reopening the page
		// gives the descriptor the same address as before and the stale entry is
		// reused over freed memory.
		lv_image_cache_drop(&side->dsc);
		free(side->pixels);
		side->pixels = NULL;
	}
	memset(&side->dsc, 0, sizeof(side->dsc));
}

static bool side_load(side_t *side, lv_color_t bg) {
	if (!side->image) {
		return false; // a side this player does not have
	}
	side_free(side);

	const char *path = theme_is_dark() ? side->dark : side->light;

	FILE *f = fopen(path, "rb");
	if (!f) {
		printf("remap: %s missing\n", path);
		return false;
	}

	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size <= 0 || size > PHOTO_MAX_BYTES) {
		fclose(f);
		return false;
	}

	uint8_t *file = malloc((size_t)size);
	if (!file || fread(file, 1, (size_t)size, f) != (size_t)size) {
		free(file);
		fclose(f);
		return false;
	}
	fclose(f);

	int w = 0, h = 0, channels = 0;
	uint8_t *rgba = stbi_load_from_memory(file, (int)size, &w, &h, &channels, 4);
	free(file);
	if (!rgba || w <= 0 || h <= 0) {
		stbi_image_free(rgba);
		printf("remap: %s cannot be decoded\n", path);
		return false;
	}

	uint16_t *out = malloc((size_t)w * (size_t)h * sizeof(uint16_t));
	if (!out) {
		stbi_image_free(rgba);
		return false;
	}

	int bg_r = bg.red, bg_g = bg.green, bg_b = bg.blue;
	for (int i = 0; i < w * h; i++) {
		const uint8_t *px = rgba + (size_t)i * 4;
		int a = px[3];
		int r = (px[0] * a + bg_r * (255 - a) + 127) / 255;
		int g = (px[1] * a + bg_g * (255 - a) + 127) / 255;
		int b = (px[2] * a + bg_b * (255 - a) + 127) / 255;
		out[i] = pack565(r, g, b);
	}
	stbi_image_free(rgba);

	side->pixels = (uint8_t *)out;
	side->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	side->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
	side->dsc.header.w = (uint32_t)w;
	side->dsc.header.h = (uint32_t)h;
	side->dsc.header.stride = (uint32_t)w * 2;
	side->dsc.data_size = (uint32_t)w * (uint32_t)h * 2;
	side->dsc.data = side->pixels;

	lv_image_set_src(side->image, &side->dsc);
	lv_obj_remove_flag(side->image, LV_OBJ_FLAG_HIDDEN);
	return true;
}

static void photos_show(void) {
	side_load(&play_side, theme()->screen_bg);
	side_load(&vol_side, theme()->screen_bg);
}

static void photos_free(void) {
	side_free(&play_side);
	side_free(&vol_side);
}

// ---------------------------------------------------------------------------
// choosing an action
// ---------------------------------------------------------------------------

// The button whose action is being chosen. The popover passes a single value to
// the callback and that carries the action, so the button has to be remembered
// here from the touch that opened it.
static keymap_button_t choosing = KEYMAP_BTN_COUNT;

// The rows' size: the largest of row_font and the two sizes under it at which
// every row's action fits across, all rows alike so the column reads as one
// list. The column is as narrow as the photo leaves it: "Abbassa il volume"
// fills an R1 row at 22 px, and with large text, or in a longer language, it
// would not fit at all. A name that fits at none of them is cut with dots.
static void refresh_values(void) {
	static const lv_font_t *const STEPS[] = {&font_ui_22, &font_ui_20, &font_ui_18};
	const size_t count = sizeof(STEPS) / sizeof(STEPS[0]);

	size_t step = 0;
	while (step + 1 < count && STEPS[step] != row_font) {
		step++;
	}
	for (int i = 0; i < KEYMAP_BTN_COUNT; i++) {
		if (!value_labels[i]) {
			continue;
		}
		const char *text = tr(keymap_action_name(keymap_get((keymap_button_t)i)));
		int32_t width = lv_obj_get_style_width(value_labels[i], LV_PART_MAIN);
		while (step + 1 < count) {
			lv_point_t size;
			lv_text_get_size(&size, text, STEPS[step], 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
			if (size.x <= width) {
				break;
			}
			step++;
		}
	}

	for (int i = 0; i < KEYMAP_BTN_COUNT; i++) {
		if (value_labels[i]) {
			lv_obj_set_style_text_font(value_labels[i], STEPS[step], 0);
			lv_obj_set_height(value_labels[i], lv_font_get_line_height(STEPS[step]));
			lv_label_set_text(value_labels[i], tr(keymap_action_name(keymap_get((keymap_button_t)i))));
		}
	}
}

static void picked(void *user) {
	keymap_action_t action = (keymap_action_t)(intptr_t)user;
	if (choosing >= KEYMAP_BTN_COUNT) {
		return;
	}
	keymap_set(choosing, action);
	choosing = KEYMAP_BTN_COUNT;
	refresh_values();
}

static void open_chooser(keymap_button_t button) {
	if (button >= KEYMAP_BTN_COUNT || !rows[button]) {
		return;
	}
	choosing = button;

	keymap_action_t current = keymap_get(button);

	// Six entries, exactly as many as the popover shows: one per action these
	// buttons can perform, plus "none", since a button pressed by accident in a
	// pocket is reason enough to disable it.
	static popover_item_t items[KEYMAP_ACTION_COUNT];
	for (int i = 0; i < KEYMAP_ACTION_COUNT; i++) {
		items[i].label = keymap_action_name((keymap_action_t)i);
		items[i].action = picked;
		items[i].user = (void *)(intptr_t)i;
		items[i].checked = ((keymap_action_t)i == current);
	}

	// The anchor is always the row, even when the photo was touched: the popover
	// opens next to its anchor, and next to an invisible rectangle on a screen
	// edge it would end up half off-screen.
	popover_show(rows[button], items, KEYMAP_ACTION_COUNT);
}

static void pick_clicked_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	open_chooser((keymap_button_t)(intptr_t)lv_event_get_user_data(e));
}

// ---------------------------------------------------------------------------
// building the page
// ---------------------------------------------------------------------------

// One side: the photo, and next to each button a row saying what it does, on
// the far side of the photo from the device's body.
static void build_side(side_t *side, gui_config_t *cfg) {
	side->image = lv_image_create(remap_screen);
	lv_obj_set_pos(side->image, side->x, side->y);
	lv_obj_set_size(side->image, side->w, side->h);
	lv_obj_add_flag(side->image, LV_OBJ_FLAG_HIDDEN); // until it is loaded

	// Where the touch zones start, where the row column starts and how wide it
	// is.
	int hit_x, col_x, col_w, leader_x;
	if (side->rows_on_right) {
		hit_x = side->x + side->w - HIT_W;
		col_x = side->x + side->w + LEADER_W;
		col_w = cfg->screen_width - col_x - cfg->padding;
		leader_x = side->x + side->w;
	} else {
		hit_x = side->x;
		col_x = cfg->padding;
		col_w = side->x - LEADER_W - col_x;
		leader_x = side->x - LEADER_W;
	}

	for (int i = 0; i < side->hit_count; i++) {
		const hit_t *hit = &side->hits[i];
		int centre = side->y + hit->y + hit->h / 2;

		// The touch rectangle over the button in the photo. Invisible: what is
		// seen is the photographed button itself.
		lv_obj_t *zone = lv_obj_create(remap_screen);
		lv_obj_remove_style_all(zone);
		lv_obj_set_pos(zone, hit_x, side->y + hit->y);
		lv_obj_set_size(zone, HIT_W, hit->h);
		lv_obj_add_flag(zone, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_remove_flag(zone, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_add_event_cb(zone, pick_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)hit->button);

		// The leader line tying a button to its row. Eighteen pixels of nothing,
		// but without it rows stacked next to closely spaced buttons have to be
		// matched up by eye.
		lv_obj_t *leader = lv_obj_create(remap_screen);
		lv_obj_remove_style_all(leader);
		lv_obj_set_pos(leader, leader_x, centre - 1);
		lv_obj_set_size(leader, LEADER_W, 2);
		lv_obj_set_style_bg_color(leader, theme()->text_secondary, 0);
		lv_obj_set_style_bg_opa(leader, LV_OPA_40, 0);

		// The row: what that button does. It is touchable too, because a
		// 260-pixel target is easier to hit than an 86-pixel one.
		lv_obj_t *row = lv_btn_create(remap_screen);
		rows[hit->button] = row;
		lv_obj_set_pos(row, col_x, centre - row_h / 2);
		lv_obj_set_size(row, col_w, row_h);
		lv_obj_add_style(row, &theme_style_card, 0);
		lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_set_style_radius(row, 10, 0);
		lv_obj_set_style_border_width(row, 0, 0);
		lv_obj_set_style_shadow_width(row, 0, 0);
		lv_obj_set_style_pad_hor(row, 14, 0);
		lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_add_event_cb(row, pick_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)hit->button);

		lv_obj_t *value = lv_label_create(row);
		value_labels[hit->button] = value;
		lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
		lv_obj_set_width(value, col_w - 28);
		// One line, cut with dots: the rows sit a few pixels apart, and a name
		// that wrapped would spill out of its row onto the next one. The size
		// is chosen with the text, in refresh_values().
		lv_obj_set_height(value, lv_font_get_line_height(row_font));
		lv_obj_add_style(value, &theme_style_text, 0);
		lv_obj_set_style_text_font(value, row_font, 0);
		lv_obj_align(value, LV_ALIGN_LEFT_MID, 0, 0);
	}
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	// The theme may have changed since the page was built, and with it both
	// which photo is needed and the colour it is blended against.
	photos_show();
	refresh_values();
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	photos_free();
}

// A theme change alters both the colour the photos are blended against and
// which version is needed. They are rebuilt only while this page is on screen;
// otherwise the next open loads them under the new theme anyway.
static void refresh_theme(void) {
	if (lv_screen_active() == remap_screen) {
		photos_show();
	}
}

void remap_init(gui_config_t *cfg) {
	remap_screen = lv_obj_create(NULL);
	lv_obj_add_style(remap_screen, &theme_style_screen, 0);
	lv_obj_remove_flag(remap_screen, LV_OBJ_FLAG_SCROLLABLE);

	const sysinfo_model_t *model = sysinfo_model();
	if (!model) {
		model = sysinfo_model_by_panel(cfg->screen_width, cfg->screen_height);
	}
	bool one_flank = model && model->one_flank;

	lv_obj_t *title = settingsrow_title(remap_screen, cfg, "remap_buttons");
	settingsrow_title_corner_slots(title, cfg, 0);

	row_h = one_flank ? R1_ROW_H : ROW_H;
	row_font = one_flank ? &font_ui_22 : &font_ui_20;
	// The size refresh_values() settles on depends on the text size.
	fonts_register_change(refresh_values);

	// Sizes are those of the files; positions put the photos where the design
	// has them. The R3 Pro II's right flank starts under the title and its left
	// flank runs four pixels past the bottom edge; the R1's photo stands on the
	// bottom edge.
	play_side.hits = one_flank ? R1_HITS : PLAY_HITS;
	play_side.hit_count = one_flank ? (int)(sizeof(R1_HITS) / sizeof(R1_HITS[0]))
									: (int)(sizeof(PLAY_HITS) / sizeof(PLAY_HITS[0]));
	play_side.rows_on_right = true;
	if (one_flank) {
		play_side.w = 229;
		play_side.h = 663;
		play_side.x = 0;
		play_side.y = cfg->screen_height - play_side.h;
	} else {
		play_side.w = 185;
		play_side.h = 411;
		play_side.x = -1; // the first column of the light photo is not opaque
		play_side.y = 126;
	}
	build_side(&play_side, cfg);

	if (!one_flank) {
		vol_side.hits = VOL_HITS;
		vol_side.hit_count = (int)(sizeof(VOL_HITS) / sizeof(VOL_HITS[0]));
		vol_side.rows_on_right = false;
		vol_side.w = 186;
		vol_side.h = 294;
		vol_side.x = cfg->screen_width - vol_side.w;
		vol_side.y = cfg->screen_height - vol_side.h + 4;
		build_side(&vol_side, cfg);
	}

	refresh_values();

	lv_obj_add_event_cb(remap_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(remap_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(remap_screen);
	theme_register_refresh(refresh_theme);
}

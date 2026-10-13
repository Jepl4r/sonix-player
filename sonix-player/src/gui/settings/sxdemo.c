#include "sxdemo.h"

#include <stdio.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/sx_card.h"
#include "src/gui/shell/sx_palette.h"
#include "src/gui/shell/sx_row.h"
#include "src/gui/shell/sx_sheet.h"
#include "src/gui/shell/sx_styles.h"
#include "src/gui/shell/sx_toast.h"
#include "src/gui/shell/sx_tokens.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/volume_overlay.h"
#include "src/system/core/lang.h"

lv_obj_t *sxdemo_screen;

// The sample items. Names of records and people are data, not interface text,
// and stay as they are in every language.
typedef struct {
	const char *title;
	const char *artist;
	const char *duration;
	const char *badge;
	float hue;
	bool starred;
} demo_item_t;

static demo_item_t items[] = {
	{"Africa", "Various Artists", "1:05", NULL, 200, false},
	{"Dark Track 1", "Pink Floyd", "0:20", NULL, 30, true},
	{"Dogs", "Pink Floyd", "0:20", "\xC2\xB7 24/96", 10, false},
	{"The Wall", "Pink Floyd", NULL, NULL, 170, false},
	{"Pink Floyd", NULL, NULL, NULL, 30, false},
	{"Comfortably Numb", "Pink Floyd", "6:23", NULL, 170, false},
	{"Hey You", "Pink Floyd", "4:40", NULL, 170, false},
};

static lv_obj_t *star_rows[8];
static lv_obj_t *codec_pills[4];
static lv_obj_t *swatches[4];
static lv_obj_t *gradient_box;
static lv_draw_buf_t *gradient_strip;

// ---------------------------------------------------------------------------
// Row handlers: everything says what it would do, in a toast
// ---------------------------------------------------------------------------

static void undo_star(void *user) {
	demo_item_t *it = user;
	it->starred = !it->starred;
	for (size_t i = 0; i < sizeof(star_rows) / sizeof(star_rows[0]); i++) {
		if (star_rows[i] && sx_row_get_user(star_rows[i]) == it) {
			sx_row_set_starred(star_rows[i], it->starred);
		}
	}
}

static void row_star(lv_obj_t *row, void *user) {
	demo_item_t *it = user;
	it->starred = !it->starred;
	sx_row_set_starred(row, it->starred);
	static char text[160];
	snprintf(text, sizeof(text), "%s: %s", tr(it->starred ? "added_to_favourites" : "remove_from_favourites"),
			 it->title);
	sx_toast_undo(text, undo_star, it);
}

static void nothing_undone(void *user) { (void)user; }

static void row_next(lv_obj_t *row, void *user) {
	(void)row;
	demo_item_t *it = user;
	static char text[160];
	snprintf(text, sizeof(text), "%s: %s", tr("sx_row_next"), it->title);
	sx_toast_undo(text, nothing_undone, NULL);
}

static void row_end(lv_obj_t *row, void *user) {
	(void)row;
	demo_item_t *it = user;
	static char text[160];
	snprintf(text, sizeof(text), "%s: %s", tr("sx_row_end"), it->title);
	sx_toast_undo(text, nothing_undone, NULL);
}

static void row_remove(lv_obj_t *row, void *user) {
	demo_item_t *it = user;
	static char text[160];
	snprintf(text, sizeof(text), "%s: %s", tr("sx_row_remove"), it->title);
	sx_toast_undo(text, nothing_undone, NULL);
	(void)row;
}

static void row_tap(lv_obj_t *row, void *user) {
	(void)row;
	demo_item_t *it = user;
	sx_toast(it->title);
}

static void row_sheet(lv_obj_t *row, void *user) {
	(void)row;
	demo_item_t *it = user;
	static sx_sheet_item_t menu[4];
	menu[0] = (sx_sheet_item_t){.label = tr("queue_play_next"), .icon = &icon_list_start};
	menu[1] = (sx_sheet_item_t){.label = tr("queue_add_to_end"), .icon = &icon_list_plus_small};
	menu[2] = (sx_sheet_item_t){.label = tr("playlist_add_to_playlist_2"), .icon = &icon_list_music};
	menu[3] = (sx_sheet_item_t){.label = tr("delete"), .icon = &icon_x_small, .destructive = true};
	sx_sheet_menu(it->title, it->artist, menu, 4);
}

// ---------------------------------------------------------------------------
// Card handlers
// ---------------------------------------------------------------------------

static void codec_pick(lv_event_t *e) {
	intptr_t v = (intptr_t)lv_event_get_user_data(e);
	for (int i = 0; i < 4; i++) {
		sx_pill_set_active(codec_pills[i], i == v);
	}
}

static void swatch_pick(lv_event_t *e) {
	intptr_t v = (intptr_t)lv_event_get_user_data(e);
	for (int i = 0; i < 4; i++) {
		sx_swatch_set_active(swatches[i], i == v);
	}
}

static void cleared(void *user) {
	(void)user;
	sx_toast(tr("downloads_cleared"));
}

static void clear_cache(lv_event_t *e) {
	(void)e;
	sx_sheet_t sheet = {
		.title = tr("downloads_clear_cache"),
		.subtitle = "Yandex Music",
		.cancel_label = tr("cancel"),
		.action_label = tr("downloads_clear"),
		.action_destructive = true,
		.on_action = cleared,
	};
	sx_sheet_open(&sheet);
}

static void show_sheet(lv_event_t *e) {
	(void)e;
	row_sheet(NULL, &items[1]);
}

static void show_toast(lv_event_t *e) {
	(void)e;
	row_next(NULL, &items[0]);
}

static void show_volume(lv_event_t *e) {
	(void)e;
	volume_overlay_show(40);
}

// The gradient of the album page, rendered for the hue in force.
static void paint_gradient(void) {
	if (!gradient_box) {
		return;
	}
	const sx_palette_t *p = sx_pal();
	int32_t h = lv_obj_get_style_height(gradient_box, LV_PART_MAIN);
	sx_grad_stop_t stops[3] = {
		{p->album_grad[0], 0},
		{p->album_grad[1], h * 380 / 900},
		{p->album_grad[2], h},
	};
	lv_draw_buf_t *strip = sx_gradient_create(4, h, stops, 3);
	lv_obj_set_style_bg_image_src(gradient_box, strip, 0);
	sx_gradient_destroy(gradient_strip);
	gradient_strip = strip;
}

static lv_obj_t *group(lv_obj_t *parent, const char *text) {
	lv_obj_t *label = lv_label_create(parent);
	lv_obj_set_width(label, lv_pct(100));
	lv_label_set_text(label, text);
	lv_obj_set_style_text_font(label, &font_ui_18, 0);
	lv_obj_add_style(label, &sx_style_text2, 0);
	lv_obj_set_style_pad_top(label, 10, 0);
	lv_obj_set_style_pad_left(label, 4, 0);
	return label;
}

static lv_obj_t *demo_row(lv_obj_t *list, demo_item_t *it, const sx_row_handlers_t *h, bool compact) {
	lv_obj_t *row = sx_row_create(list, h, compact);
	sx_row_set_user(row, it);
	sx_row_set_title(row, it->title);
	sx_row_set_subtitle(row, it->artist, it->badge, it->duration);
	sx_row_set_thumb(row, NULL, sx_tone(SX_T_COVER_TONE, it->hue));
	return row;
}

static void build(gui_config_t *cfg) {
	lv_obj_t *page = settingsrow_page(sxdemo_screen, cfg, "sx_demo_title");
	lv_obj_t *title = settingsrow_page_title(sxdemo_screen);
	settingsrow_title_corner_slots(title, cfg, 0);
	lv_obj_set_style_pad_gap(page, SX_ROW_GAP, 0);

	group(page, tr("sx_demo_rows"));
	sx_row_handlers_t track = {
		.on_tap = row_tap, .on_long_press = row_sheet, .on_swipe_next = row_next, .on_swipe_end = row_end,
		.on_star = row_star,
	};
	sx_row_handlers_t album = {.on_tap = row_tap, .on_long_press = row_sheet, .on_swipe_next = row_next,
							   .on_swipe_end = row_end};
	sx_row_handlers_t artist = {.on_tap = row_tap};
	sx_row_handlers_t queue = {.on_tap = row_tap, .on_long_press = row_sheet, .on_swipe_end = row_remove,
							   .end_is_remove = true};

	int n = 0;
	lv_obj_t *row = demo_row(page, &items[0], &track, false);
	sx_row_set_trail(row, SX_ROW_TRAIL_STAR);
	star_rows[n++] = row;

	row = demo_row(page, &items[1], &track, false);
	sx_row_set_trail(row, SX_ROW_TRAIL_STAR);
	sx_row_set_starred(row, items[1].starred);
	sx_row_set_playing(row, true);
	star_rows[n++] = row;

	row = demo_row(page, &items[2], &track, false);
	sx_row_set_trail(row, SX_ROW_TRAIL_STAR);
	star_rows[n++] = row;

	row = demo_row(page, &items[3], &album, false);
	sx_row_set_trail(row, SX_ROW_TRAIL_CHEVRON);

	row = demo_row(page, &items[4], &artist, false);
	sx_row_set_subtitle(row, "3", NULL, NULL);
	sx_row_set_letter(row, "P", sx_tone(SX_T_COVER_TONE, items[4].hue), true);
	sx_row_set_trail(row, SX_ROW_TRAIL_CHEVRON);

	row = demo_row(page, &items[5], &track, false);
	sx_row_set_number(row, 6);
	sx_row_set_subtitle(row, NULL, NULL, items[5].duration);
	sx_row_set_trail(row, SX_ROW_TRAIL_STAR);
	star_rows[n++] = row;

	row = demo_row(page, &items[6], &queue, true);
	(void)row;

	group(page, tr("sx_demo_cards"));
	lv_obj_t *card = sx_card_create(page);
	sx_card_title(card, tr("appearance"));
	sx_card_value(card, tr("appearance_dark"));
	sx_card_chevron(card);

	card = sx_card_create(page);
	sx_card_title(card, tr("appearance_dynamic_tint"));
	sx_card_subtitle(card, tr("appearance_dynamic_tint_note"), false);
	lv_obj_add_state(sx_card_switch(card, NULL), LV_STATE_CHECKED);

	card = sx_card_create(page);
	sx_card_title(card, "Space One Pro");
	sx_card_subtitle(card, "LDAC 990 \xC2\xB7 96 kHz \xC2\xB7 78%", true);
	sx_card_set_active(card, true);

	card = sx_card_create(page);
	sx_card_title(card, tr("btaudio_codecs"));
	lv_obj_t *pills = sx_card_pills(card);
	const char *codecs[4] = {tr("btaudio_codec_auto"), "LDAC", "AAC", "SBC"};
	for (int i = 0; i < 4; i++) {
		codec_pills[i] = sx_pill(pills, codecs[i], i, codec_pick);
		sx_pill_set_active(codec_pills[i], i == 1);
	}

	card = sx_card_create(page);
	sx_card_title(card, tr("appearance_accent_colour"));
	lv_obj_t *row_sw = sx_card_swatches(card);
	for (int i = 0; i < 4; i++) {
		swatches[i] = sx_swatch(row_sw, theme_accent_preset(i), i, swatch_pick);
		sx_swatch_set_active(swatches[i], i == 0);
	}

	card = sx_card_create(page);
	sx_card_title(card, "Yandex Music");
	sx_card_subtitle(card, "12 MB / 1.0 GB", false);
	sx_card_bar(card, 30);
	sx_card_action(card, tr("downloads_clear_cache"), clear_cache, NULL);

	card = sx_card_create(page);
	sx_card_title(card, "Tidal");
	sx_card_subtitle(card, "\xE2\x80\x94", false);
	sx_card_set_dim(card, true);

	group(page, tr("sx_demo_layers"));
	card = sx_card_create(page);
	lv_obj_t *actions = sx_card_pills(card);
	sx_pill(actions, tr("sx_demo_sheet"), 0, show_sheet);
	sx_pill(actions, tr("sx_demo_toast"), 0, show_toast);
	sx_pill(actions, tr("sx_demo_volume"), 0, show_volume);

	gradient_box = sx_box(page);
	lv_obj_set_size(gradient_box, lv_pct(100), 240);
	lv_obj_set_style_radius(gradient_box, SX_CARD_R, 0);
	lv_obj_set_style_clip_corner(gradient_box, true, 0);
	lv_obj_set_style_bg_image_tiled(gradient_box, true, 0);
	paint_gradient();
	theme_register_refresh(paint_gradient);
}

void sxdemo_init(gui_config_t *cfg) {
	(void)cfg;
	sxdemo_screen = lv_obj_create(NULL);
	gui_page_defer(sxdemo_screen, build);
}

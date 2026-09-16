#include "easteregg.h"

#include "src/system/core/respath.h"

#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"
#include "lvgl/src/misc/cache/instance/lv_image_cache.h"

#include "src/gui/nowplaying/cover.h"
#include "src/gui/shell/icons.h"
#include "src/system/core/lang.h"

// ---------------------------------------------------------------------------
// The two codes
//
// Ko-fi takes no cards issued in Russia, so the Russian interface gets a
// Boosty code instead; every other language gets Ko-fi. The language is the
// only thing there is to go on -- the device has no country and no account.
//
// The pictures live in /usr/resource/sonix/gui rather than in the binary, for
// the same reason the button photographs do (see remap.c): a 380x380 RGB565
// bitmap is not worth keeping resident for something seen once. As PNG on the
// card they cost nothing until they are asked for, and they are freed the
// moment the code is put away.
//
// 380 pixels for a 37-module code is about 9 pixels per module with the quiet
// zone the file already carries, which every phone camera reads at arm's
// length. The picture is drawn at its own size, so the decode is a straight
// unpack with no scaling.
// ---------------------------------------------------------------------------

#define QR_KOFI SONIX_RESOURCE_DIR "/gui/ko-fi-qr-code.png"
#define QR_BOOSTY SONIX_RESOURCE_DIR "/gui/boosty-qr-code.png"
#define QR_SIZE 380

// White border around the code. The file has its own quiet zone; this is the
// card's margin, so the rounded corners do not eat into it.
#define QR_PAD 14

// Taps before the press that opens it.
#define EASTEREGG_TAPS 5

// The heart. Grown from nothing to its own size, so the icon is generated at
// the size it is seen at: LVGL draws it 1:1 at the end of the animation and
// never above it.
#define HEART_GROW_MS 700
#define HEART_HOLD_MS 250
#define HEART_FADE_MS 500
#define HEART_COLOR lv_color_make(228, 55, 70)

static int taps;

// LVGL sends CLICKED on release even after LONG_PRESSED (lv_indev.c), so the
// press that opens the code would otherwise count as the first tap of the next
// gesture.
static bool swallow_next_click;

static lv_obj_t *veil;
static lv_obj_t *qr_image;
static cover_image_t qr;

static lv_obj_t *heart;

// ---------------------------------------------------------------------------

static const char *qr_path(void) {
	const char *language = lang_current();
	return (language && strcmp(language, "Russian") == 0) ? QR_BOOSTY : QR_KOFI;
}

static void qr_release(void) {
	if (qr_image) {
		lv_image_set_src(qr_image, NULL);
		qr_image = NULL;
	}

	// The cache is off, but it is indexed by pointer and this descriptor is
	// file-static: the next code loaded would land on the same address.
	lv_image_cache_drop(&qr.dsc);
	cover_free(&qr);
}

// ---------------------------------------------------------------------------
// The heart
// ---------------------------------------------------------------------------

static void heart_scale_cb(void *obj, int32_t value) { lv_image_set_scale((lv_obj_t *)obj, (uint32_t)value); }

static void heart_opa_cb(void *obj, int32_t value) {
	lv_obj_set_style_image_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void heart_faded_cb(lv_anim_t *a) {
	(void)a;
	if (heart) {
		lv_obj_delete(heart);
		heart = NULL;
	}
}

static void heart_grown_cb(lv_anim_t *a) {
	if (!heart) {
		return;
	}

	lv_anim_t fade;
	lv_anim_init(&fade);
	lv_anim_set_var(&fade, a->var);
	lv_anim_set_exec_cb(&fade, heart_opa_cb);
	lv_anim_set_values(&fade, LV_OPA_COVER, LV_OPA_TRANSP);
	lv_anim_set_delay(&fade, HEART_HOLD_MS);
	lv_anim_set_duration(&fade, HEART_FADE_MS);
	lv_anim_set_completed_cb(&fade, heart_faded_cb);
	lv_anim_start(&fade);
}

static void heart_pop(void) {
	// A second one on top of the first: the old heart goes, animations and all
	// (deleting an object cancels the animations running on it).
	if (heart) {
		lv_obj_delete(heart);
		heart = NULL;
	}

	heart = lv_image_create(lv_layer_top());
	lv_image_set_src(heart, &icon_heart);
	lv_obj_set_style_image_recolor(heart, HEART_COLOR, 0);
	lv_obj_set_style_image_recolor_opa(heart, LV_OPA_COVER, 0);
	lv_obj_center(heart);

	// From a single pixel rather than from zero: a scale of zero is a zero-wide
	// draw area, and nothing downstream has to be asked to divide by it.
	lv_image_set_scale(heart, 1);

	// bounce reaches full size at about two fifths of the duration, overshoots
	// back by a twentieth, and settles at 1:1 -- which is the growing-with-a-
	// bounce this wants, and never larger than the icon really is.
	lv_anim_t grow;
	lv_anim_init(&grow);
	lv_anim_set_var(&grow, heart);
	lv_anim_set_exec_cb(&grow, heart_scale_cb);
	lv_anim_set_values(&grow, 1, LV_SCALE_NONE);
	lv_anim_set_duration(&grow, HEART_GROW_MS);
	lv_anim_set_path_cb(&grow, lv_anim_path_bounce);
	lv_anim_set_completed_cb(&grow, heart_grown_cb);
	lv_anim_start(&grow);
}

// ---------------------------------------------------------------------------
// The code itself
// ---------------------------------------------------------------------------

static void qr_close(void) {
	if (!veil) {
		return;
	}

	qr_release();
	lv_obj_delete(veil);
	veil = NULL;

	heart_pop();
}

static void veil_clicked_cb(lv_event_t *e) {
	(void)e;
	qr_close();
}

static void qr_open(void) {
	if (veil) {
		return;
	}

	const char *path = qr_path();
	if (!cover_load_image_file(path, QR_SIZE, QR_SIZE, COVER_FIT_CONTAIN, &qr)) {
		fprintf(stderr, "easteregg: cannot read %s\n", path);
		return;
	}

	veil = lv_obj_create(lv_layer_top());
	lv_obj_set_size(veil, lv_pct(100), lv_pct(100));
	lv_obj_set_pos(veil, 0, 0);
	lv_obj_set_style_bg_color(veil, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(veil, LV_OPA_70, 0);
	lv_obj_set_style_border_width(veil, 0, 0);
	lv_obj_set_style_radius(veil, 0, 0);
	lv_obj_set_style_shadow_width(veil, 0, 0);
	lv_obj_set_style_pad_all(veil, 0, 0);
	lv_obj_remove_flag(veil, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(veil, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(veil, veil_clicked_cb, LV_EVENT_CLICKED, NULL);

	// White whatever the theme is: a code read by a camera is black on white,
	// and a dark card around it would only narrow the quiet zone. The card is
	// clickable, which is how a tap on the code is told apart from a tap beside
	// it -- the one closes, the other does not.
	lv_obj_t *card = lv_obj_create(veil);
	lv_obj_set_size(card, QR_SIZE + 2 * QR_PAD, QR_SIZE + 2 * QR_PAD);
	lv_obj_set_style_bg_color(card, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_radius(card, 18, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, QR_PAD, 0);
	lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_center(card);

	qr_image = lv_image_create(card);
	lv_image_set_src(qr_image, &qr.dsc);
	lv_obj_center(qr_image);
}

// ---------------------------------------------------------------------------
// The gesture
// ---------------------------------------------------------------------------

static void row_clicked_cb(lv_event_t *e) {
	(void)e;

	if (swallow_next_click) {
		swallow_next_click = false;
		return;
	}

	if (taps < EASTEREGG_TAPS) {
		taps++;
	}
}

static void row_long_pressed_cb(lv_event_t *e) {
	(void)e;

	swallow_next_click = true;

	bool armed = taps >= EASTEREGG_TAPS;
	taps = 0;

	if (armed) {
		qr_open();
	}
}

void easteregg_attach(lv_obj_t *row) {
	if (!row) {
		return;
	}

	lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(row, row_long_pressed_cb, LV_EVENT_LONG_PRESSED, NULL);
}

void easteregg_reset(void) {
	taps = 0;
	swallow_next_click = false;
}

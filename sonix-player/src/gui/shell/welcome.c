#include "welcome.h"

#include <stdlib.h>
#include <time.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/settings/language.h"
#include "src/gui/shell/sx_styles.h"
#include "src/gui/shell/sx_tokens.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"

// ---------------------------------------------------------------------------
// The words
//
// Literal rather than from tr(): these are the shipped languages greeting at
// once, not one string to translate into the chosen language -- which at this
// point has not been chosen yet.
//
// The language names beside them are for whoever reads this file; nothing is
// shown under the word on screen.
// ---------------------------------------------------------------------------

// English first: it is the language the player starts in on first boot (see
// LANG_FIRST_BOOT) and the first word shown. After that the cycle picks at
// random, never the same word twice running.
static const char *const WELCOME_WORDS[] = {
	"Welcome",	  // English
	"Benvenuto",  // Italian
	"Willkommen", // German
	"Bienvenue",  // French
	"Bienvenido", // Spanish
	"Welkom",	  // Dutch
	"Witamy",	  // Polish
	"Bem-vindo",  // Portuguese
	// The default face draws Cyrillic, kana and the CJK block, as the whole
	// interface does in Russian, Chinese and Japanese, so none of these needs
	// a font of its own.
	"Добро пожаловать",
	"欢迎",
	"ようこそ",
};

#define WELCOME_COUNT ((int)(sizeof(WELCOME_WORDS) / sizeof(WELCOME_WORDS[0])))

// Timing of one cycle step: the word rests for WELCOME_HOLD_MS, slides out over
// WELCOME_SLIDE_MS, and the next one slides in over the same time.
#define WELCOME_HOLD_MS 1500
#define WELCOME_SLIDE_MS 380

// 2.0 moves things and never fades them (the platform has no cheap way to
// blend): the word slides the whole width of its strip, out to the left and
// the next in from the right, and the strip clips it, so it leaves and enters
// at the edges of the page's margins.
static int slide_px;

static lv_obj_t *panel;
static lv_obj_t *word_strip; // clips the word as it slides
static lv_obj_t *word_label;
static lv_obj_t *start_btn;
static lv_obj_t *start_label;
static lv_timer_t *hold_timer;
static int current_word = -1;

// ---------------------------------------------------------------------------
// The word cycle
// ---------------------------------------------------------------------------

// Random, but never the same word twice in a row: with this few words a repeat
// looks like a stuck animation rather than a random pick.
static int next_word(void) {
	if (WELCOME_COUNT <= 1) {
		return 0;
	}
	int pick;
	do {
		pick = rand() % WELCOME_COUNT;
	} while (pick == current_word);
	return pick;
}

static void slide_out_done(lv_anim_t *a);
static void start_hold(void);

static void set_x(void *obj, int32_t value) { lv_obj_set_style_translate_x((lv_obj_t *)obj, value, 0); }

// Slides in from the right edge of the strip.
static void slide_in(void) {
	lv_anim_t move;
	lv_anim_init(&move);
	lv_anim_set_var(&move, word_label);
	lv_anim_set_exec_cb(&move, set_x);
	lv_anim_set_values(&move, slide_px, 0);
	lv_anim_set_duration(&move, WELCOME_SLIDE_MS);
	lv_anim_set_path_cb(&move, lv_anim_path_ease_out);
	lv_anim_start(&move);

	start_hold();
}

// Slides out past the left edge, then swaps the word -- out of sight, clipped
// by the strip -- and slides the next one in from the other side.
static void slide_out(void) {
	lv_anim_t move;
	lv_anim_init(&move);
	lv_anim_set_var(&move, word_label);
	lv_anim_set_exec_cb(&move, set_x);
	lv_anim_set_values(&move, 0, -slide_px);
	lv_anim_set_duration(&move, WELCOME_SLIDE_MS);
	lv_anim_set_path_cb(&move, lv_anim_path_ease_in);
	lv_anim_set_completed_cb(&move, slide_out_done);
	lv_anim_start(&move);
}

static void slide_out_done(lv_anim_t *a) {
	(void)a;
	if (!word_label) {
		return;
	}
	current_word = next_word();
	lv_label_set_text(word_label, WELCOME_WORDS[current_word]);
	slide_in();
}

static void hold_elapsed(lv_timer_t *t) {
	(void)t;
	hold_timer = NULL; // single shot: LVGL has already deleted it
	slide_out();
}

static void start_hold(void) {
	if (hold_timer) {
		lv_timer_delete(hold_timer);
	}
	hold_timer = lv_timer_create(hold_elapsed, WELCOME_HOLD_MS, NULL);
	lv_timer_set_repeat_count(hold_timer, 1);
}

static void stop_cycle(void) {
	if (hold_timer) {
		lv_timer_delete(hold_timer);
		hold_timer = NULL;
	}
	if (word_label) {
		lv_anim_delete(word_label, NULL);
	}
}

// ---------------------------------------------------------------------------

static void start_cb(lv_event_t *e) {
	(void)e;

	stop_cycle();
	lv_obj_set_hidden(panel, true);

	// The next screen holds the display on itself, so nothing is released here:
	// the panel must not blank between the two pages.
	language_show_first_boot();
}


void welcome_init(gui_config_t *cfg) {
	// The cycle is random and needs a seed: without one, rand() returns the
	// same sequence on every boot and the words always appear in one fixed
	// order.
	srand((unsigned int)time(NULL));

	panel = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(panel);
	lv_obj_set_size(panel, cfg->screen_width, cfg->screen_height);
	lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_add_style(panel, &sx_style_page, 0);
	lv_obj_set_style_pad_all(panel, cfg->padding, 0);
	lv_obj_set_scrollable(panel, false);
	lv_obj_set_clickable(panel, true);
	lv_obj_set_hidden(panel, true);

	// The word and the button below it are the only two elements, so they are
	// centred in the panel rather than stacked from the top as on the language
	// page, which holds more.
	lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(panel, 64, 0);

	// "Добро пожаловать" is the wide one: one centred line, the panel's width
	// less its padding, and the strip it slides in clips it there.
	int strip_w = cfg->screen_width - 2 * cfg->padding;
	slide_px = strip_w;
	word_strip = lv_obj_create(panel);
	lv_obj_remove_style_all(word_strip);
	lv_obj_set_size(word_strip, strip_w, lv_font_get_line_height(&font_ui_34_bold));
	lv_obj_set_scrollable(word_strip, false);

	word_label = lv_label_create(word_strip);
	lv_label_set_text(word_label, WELCOME_WORDS[0]);
	lv_obj_add_style(word_label, &sx_style_text, 0);
	lv_obj_set_style_text_font(word_label, &font_ui_34_bold, 0);
	lv_obj_set_width(word_label, strip_w);
	lv_obj_set_style_text_align(word_label, LV_TEXT_ALIGN_CENTER, 0);

	// The accent pill, 68 tall: the one thing to do here.
	start_btn = lv_btn_create(panel);
	lv_obj_remove_style_all(start_btn);
	lv_obj_set_size(start_btn, LV_SIZE_CONTENT, SX_SHEET_BTN_H);
	lv_obj_set_style_min_width(start_btn, 240, 0);
	lv_obj_set_style_pad_hor(start_btn, 40, 0);
	lv_obj_set_style_radius(start_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_add_style(start_btn, &sx_style_accent, 0);
	lv_obj_add_style(start_btn, &sx_style_press, LV_STATE_PRESSED);
	lv_obj_add_event_cb(start_btn, start_cb, LV_EVENT_CLICKED, NULL);

	start_label = lv_label_create(start_btn);
	lv_label_set_text(start_label, tr("welcome_start"));
	lv_obj_set_style_text_font(start_label, &font_ui_26_bold, 0);
	lv_obj_center(start_label);
}

void welcome_show_first_boot(void) {
	if (!panel) {
		return;
	}

	// Always starts on "Welcome", the language the player boots in, and it
	// stands still while the page appears; the cycle starts afterwards.
	current_word = 0;
	lv_label_set_text(word_label, WELCOME_WORDS[current_word]);
	lv_obj_set_style_translate_x(word_label, 0, 0);

	lv_obj_set_hidden(panel, false);
	lv_obj_move_foreground(panel);

	start_hold();

	// As on the other first-boot pages: reading the greetings takes as long as
	// it takes, and blanking while a top-layer panel is up also kills the
	// touchscreen.
	power_hold_screen_on(true);
}

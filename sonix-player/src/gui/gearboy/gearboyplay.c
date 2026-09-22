#include "gearboyplay.h"

// LVGL 9.2 made lv_timer_t opaque. There is a lv_timer_set_period() but no
// getter, and both places here have to read the period before changing it.
#include "lvgl/src/misc/lv_timer_private.h"

#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gb/gbcore.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/gearboy/gearboysettings.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/topbar.h"
#include "src/gui/shell/volume_overlay.h"
#include "src/system/gearboy/gbinput.h"
#include "src/system/gearboy/gearboy.h"
#include "src/system/core/lang.h"

lv_obj_t *gearboyplay_screen;

// ---------------------------------------------------------------------------
// the layout
// ---------------------------------------------------------------------------
//
// The game screen takes the first 432 rows at full width (exactly 3x, no
// interpolation) and the controls take whatever is left: 288 rows on a 720
// panel, 368 on an 800 one.
//
// D-pad on the left, A/B on the right and diagonal with A higher, as on the
// Game Boy itself. The measurements are in pixels rather than percentages on
// purpose: they are passed to gbinput, which works in screen coordinates
// because it reads the touchscreen without going through LVGL, and two
// definitions of the same geometry would be two to keep in step.

#define SCREEN_TOP 0
#define GAME_W GEARBOY_SCREEN_W // 480
#define GAME_H GEARBOY_SCREEN_H // 432

// The D-pad: a square split into nine cells. The corners give two directions
// at once, the centre none. The diagonals are not a luxury -- without them a
// finger sitting between up and right produces nothing instead of the obvious
// command, and a platformer cannot be played diagonally.
//
// 220 pixels a side, so the left thumb of a hand holding the device can find it
// without looking.
#define DPAD_X0 20
#define DPAD_Y0 466
#define DPAD_SIZE 220
#define DPAD_CELL (DPAD_SIZE / 3)
#define DPAD_CX (DPAD_X0 + DPAD_SIZE / 2)

// A and B, diagonal as on the real machine. The rectangle is both the drawing
// and the touch area: 110 pixels, already wider than a fingertip, and growing
// the area past the drawing would make the two buttons touch.
#define BTN_SIZE 110
#define A_X 352
#define A_Y 450
#define B_X 300
#define B_Y 592

// Select and Start: invisible, inside the game screen, in the bottom corners.
// Undrawn because a Game Boy game uses them twice a session, and two rectangles
// over the picture would be in the way the rest of the time.
#define CORNER_W 110
#define CORNER_H 60
#define CORNER_Y (GAME_H - CORNER_H)

// The rows above are measured on a 720-row panel. On a taller one every row
// past 720 is extra room under the picture -- which is always 480x432, three
// times the Game Boy's own 160x144, a fourth being wider than the panel -- and
// the block of controls is centred in it rather than left hanging under the
// picture. On 720 that is where it already sits: eighteen rows clear above the
// A button and eighteen below the B button.
//
// The shift is applied through the three names below and nowhere else, because
// the touch zones go to gbinput in screen coordinates without passing through
// LVGL: the drawing and the zones have to move together or the buttons stop
// being where they look.
#define PANEL_H_REF 720

static int panel_h = PANEL_H_REF;
static int controls_dy;

static void panel_set_height(int height) {
	panel_h = height > 0 ? height : PANEL_H_REF;
	controls_dy = (panel_h - PANEL_H_REF) / 2;
	if (controls_dy < 0) {
		controls_dy = 0; // a shorter panel than this layout was drawn for
	}
}

#define DPAD_Y (DPAD_Y0 + controls_dy)
#define DPAD_CENTRE_Y (DPAD_Y + DPAD_SIZE / 2)
#define A_ROW (A_Y + controls_dy)
#define B_ROW (B_Y + controls_dy)

// The in-game menu: in the MIDDLE of the picture, not in a corner.
//
// On this page the touchscreen belongs to the emulator and no drawn object
// receives touch, so a back arrow would be a picture and not a control. It is
// hidden (see back_btn_force_hidden) and this takes its place: press the middle
// of the screen and the game stops.
//
// The middle because it is the only part of the picture a playing hand never
// rests on: the controls are all below, Select and Start in the bottom corners.
#define MENU_W 240
#define MENU_H 200
#define MENU_X ((GAME_W - MENU_W) / 2)
#define MENU_Y (SCREEN_TOP + (GAME_H - MENU_H) / 2)

// How long the menu stays deaf after opening.
//
// It is opened by a finger still on the glass. An instant later the glass goes
// back to LVGL (gbinput_stop), and the release of THAT finger would land as a
// click on whatever is underneath -- an entry of the menu that just appeared.
// 350 ms is longer than a tap and shorter than a decision.
#define MENU_DEAF_MS 350

// The page's tick. Sixty times a second: the game runs at 59.73, and looking
// more often than needed costs one integer comparison.
#define POLL_MS 16

// LVGL's refresh period while a game is running.
//
// The game runs at 59.73 fps -- dictated by the sound card, since a 738-sample
// write at 44100 Hz is exactly 16.743 ms -- and the panel runs at 60. LVGL's
// default 33 ms period is 1.97 game frames, and that remainder accumulates into
// a beat: roughly once a second a refresh skips a frame or shows three.
//
// 16 ms puts LVGL in step with the game and the panel. ONLY on this page:
// elsewhere doubling the refresh rate would double the interface's idle work,
// and the standby logic rewrites this period for itself anyway.
#define GAME_REFRESH_MS 16

static uint32_t saved_refresh_ms;

static void refresh_period_set(uint32_t ms) {
	lv_display_t *disp = lv_display_get_default();
	lv_timer_t *timer = disp ? lv_display_get_refr_timer(disp) : NULL;
	if (!timer) {
		return;
	}
	if (!saved_refresh_ms) {
		saved_refresh_ms = timer->period;
	}
	lv_timer_set_period(timer, ms);
}

static void refresh_period_restore(void) {
	lv_display_t *disp = lv_display_get_default();
	lv_timer_t *timer = disp ? lv_display_get_refr_timer(disp) : NULL;
	if (!timer || !saved_refresh_ms) {
		return;
	}
	lv_timer_set_period(timer, saved_refresh_ms);
	saved_refresh_ms = 0;
}

static lv_obj_t *game_image;
static lv_obj_t *pause_veil;
static lv_obj_t *veil_label;
static lv_obj_t *controls_bg;
static lv_timer_t *poll_timer;
static lv_image_dsc_t game_dsc;
static uint32_t last_frame;

static lv_obj_t *menu_layer;
static uint32_t menu_opened_ms;

// The badge in the top right: an icon on a disc the colour of the page, shown
// when a state has been saved or loaded, and dismissed on a timer.
static lv_obj_t *badge;
static lv_obj_t *badge_icon;
static lv_timer_t *badge_timer;

// True between requesting a save/load state and the emulator thread serving it.
// The game stays paused in between: the thread only looks at requests while
// paused (see gearboy.h), so resuming before the result arrives would leave the
// request sitting there until the next pause.
//
// The wait carries no on-screen notice: with the state serialised in memory and
// written in one go (see gbcore.cpp) it is a few milliseconds, and a message
// appearing and vanishing in that time is just a flash.
static bool waiting_for_state;

// Button state when LVGL is doing the pressing rather than the touchscreen
// reader: on the simulator, and on the device if the evdev node did not open.
// One finger at a time, but the page is usable enough to try.
static uint16_t fallback_keys;

// The pending ROM: gearboyplay_open() sets it aside and the game starts once
// the screen is actually loaded. Starting it immediately would leave an
// emulator running behind a page still animating into view.
static char pending_rom[512];
static char pending_title[GEARBOY_TITLE_MAX];

static void menu_open(void);
static void menu_close_and_resume(void);

// ---------------------------------------------------------------------------
// the touch zones, which are the same geometry as the drawing
// ---------------------------------------------------------------------------

static int build_zones(gbinput_zone_t *out, int max) {
	int n = 0;

	// The nine D-pad cells, in reading order. The centre (1,1) is left out: a
	// thumb resting in the middle must produce nothing.
	static const uint16_t DPAD[3][3] = {
		{GB_KEY_UP | GB_KEY_LEFT, GB_KEY_UP, GB_KEY_UP | GB_KEY_RIGHT},
		{GB_KEY_LEFT, 0, GB_KEY_RIGHT},
		{GB_KEY_DOWN | GB_KEY_LEFT, GB_KEY_DOWN, GB_KEY_DOWN | GB_KEY_RIGHT},
	};

	for (int row = 0; row < 3; row++) {
		for (int col = 0; col < 3; col++) {
			if (!DPAD[row][col] || n >= max) {
				continue;
			}
			out[n].x = DPAD_X0 + col * DPAD_CELL;
			out[n].y = DPAD_Y + row * DPAD_CELL;
			out[n].w = DPAD_CELL;
			out[n].h = DPAD_CELL;
			out[n].keys = DPAD[row][col];
			n++;
		}
	}

	if (n < max) {
		out[n++] = (gbinput_zone_t){B_X, B_ROW, BTN_SIZE, BTN_SIZE, GB_KEY_B};
	}
	if (n < max) {
		out[n++] = (gbinput_zone_t){A_X, A_ROW, BTN_SIZE, BTN_SIZE, GB_KEY_A};
	}
	// The menu BEFORE Select and Start: zones are tested in order and the first
	// match wins, so whichever sits on top must be listed first. These do not
	// overlap, but the rule holds and is worth keeping visible.
	if (n < max) {
		out[n++] = (gbinput_zone_t){MENU_X, MENU_Y, MENU_W, MENU_H, GBINPUT_KEY_MENU};
	}
	if (n < max) {
		out[n++] = (gbinput_zone_t){0, CORNER_Y, CORNER_W, CORNER_H, GB_KEY_SELECT};
	}
	if (n < max) {
		out[n++] = (gbinput_zone_t){GAME_W - CORNER_W, CORNER_Y, CORNER_W, CORNER_H, GB_KEY_START};
	}

	return n;
}

// The menu request arrives on the touchscreen reader thread, and nothing in
// LVGL may be touched from there: gui_post() is the only bridge (see gui.h).
static void menu_on_gui_thread(void *unused) {
	(void)unused;
	menu_open();
}

static void menu_requested(void) { gui_post(menu_on_gui_thread, NULL); }

// Takes the touchscreen back after the menu handed it to LVGL.
static void grab_glass(void) {
	gbinput_zone_t zones[16];
	int n = build_zones(zones, 16);
	if (!gbinput_start(zones, n, menu_requested)) {
		fprintf(stderr, "gearboyplay: single-finger controls (multitouch not available)\n");
	}
}

// ---------------------------------------------------------------------------
// the one-finger fallback
// ---------------------------------------------------------------------------

static void fallback_apply(void) {
	if (!gbinput_active()) {
		gearboy_set_keys(fallback_keys);
	}
}

static void key_event_cb(lv_event_t *e) {
	uint16_t bit = (uint16_t)(uintptr_t)lv_event_get_user_data(e);
	lv_event_code_t code = lv_event_get_code(e);

	if (code == LV_EVENT_PRESSED) {
		fallback_keys |= bit;
	} else {
		fallback_keys &= (uint16_t)~bit;
	}
	fallback_apply();
}

// A pressable object that sets key bits while held. Only for the one-finger
// fallback: when the touchscreen reader is running, LVGL receives nothing at
// all because its indev is switched off.
static lv_obj_t *pressable(lv_obj_t *parent, int x, int y, int w, int h, uint16_t keys) {
	lv_obj_t *o = lv_obj_create(parent);
	lv_obj_remove_style_all(o);
	lv_obj_set_pos(o, x, y);
	lv_obj_set_size(o, w, h);
	lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_event_cb(o, key_event_cb, LV_EVENT_PRESSED, (void *)(uintptr_t)keys);
	lv_obj_add_event_cb(o, key_event_cb, LV_EVENT_RELEASED, (void *)(uintptr_t)keys);
	lv_obj_add_event_cb(o, key_event_cb, LV_EVENT_PRESS_LOST, (void *)(uintptr_t)keys);
	return o;
}

// ---------------------------------------------------------------------------
// the drawing
// ---------------------------------------------------------------------------

static void build_dpad(lv_obj_t *parent) {
	// Two overlapping rounded rectangles, which make a cross. The eight touch
	// cells go underneath and are wider than the arms of the cross, on purpose:
	// a finger does not aim at the edge.
	lv_color_t body = lv_color_make(58, 58, 62);

	lv_obj_t *v = lv_obj_create(parent);
	lv_obj_remove_style_all(v);
	lv_obj_set_size(v, DPAD_CELL, DPAD_SIZE);
	lv_obj_set_pos(v, DPAD_CX - DPAD_CELL / 2, DPAD_Y);
	lv_obj_set_style_bg_color(v, body, 0);
	lv_obj_set_style_bg_opa(v, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(v, 16, 0);

	lv_obj_t *h = lv_obj_create(parent);
	lv_obj_remove_style_all(h);
	lv_obj_set_size(h, DPAD_SIZE, DPAD_CELL);
	lv_obj_set_pos(h, DPAD_X0, DPAD_CENTRE_Y - DPAD_CELL / 2);
	lv_obj_set_style_bg_color(h, body, 0);
	lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(h, 16, 0);

	// The hub in the middle, as on the real D-pad.
	const int hub = 38;
	lv_obj_t *dot = lv_obj_create(parent);
	lv_obj_remove_style_all(dot);
	lv_obj_set_size(dot, hub, hub);
	lv_obj_set_pos(dot, DPAD_CX - hub / 2, DPAD_CENTRE_Y - hub / 2);
	lv_obj_set_style_bg_color(dot, lv_color_make(38, 38, 42), 0);
	lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
}

static void build_button(lv_obj_t *parent, int x, int y, const char *label, uint16_t key) {
	lv_obj_t *o = lv_obj_create(parent);
	lv_obj_remove_style_all(o);
	lv_obj_set_size(o, BTN_SIZE, BTN_SIZE);
	lv_obj_set_pos(o, x, y);
	lv_obj_set_style_bg_color(o, lv_color_make(150, 32, 74), 0); // the Game Boy button magenta
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);

	// Bold: on a 110-pixel circle a regular weight gets lost, and these are the
	// only two letters looked at while playing.
	lv_obj_t *text = lv_label_create(o);
	lv_label_set_text(text, label);
	lv_obj_set_style_text_font(text, &font_ui_36_bold, 0);
	lv_obj_set_style_text_color(text, lv_color_white(), 0);
	lv_obj_center(text);

	pressable(parent, x, y, BTN_SIZE, BTN_SIZE, key);
}

// ---------------------------------------------------------------------------
// the in-game menu
// ---------------------------------------------------------------------------
//
// Not one of the shared popovers: those dismiss themselves when the veil is
// touched, and here closing MUST resume emulation however it happened. A menu
// that closes without resuming leaves the device apparently alive and
// completely frozen.

// True while the touch that opened the menu may still be on the glass.
static bool menu_still_deaf(void) { return (lv_tick_get() - menu_opened_ms) < MENU_DEAF_MS; }

static void menu_open(void) {
	if (!menu_layer || !lv_obj_has_flag(menu_layer, LV_OBJ_FLAG_HIDDEN)) {
		return;
	}

	// Pause the game first, then hand back the glass. The other order leaves a
	// window in which the emulator runs with nobody feeding it keys, seen as a
	// character that keeps walking on its own.
	gearboy_set_paused(true);
	gbinput_stop();
	fallback_keys = 0;

	menu_opened_ms = lv_tick_get();
	lv_obj_remove_flag(menu_layer, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(menu_layer);
}

static void menu_close_and_resume(void) {
	if (!menu_layer) {
		return;
	}
	lv_obj_add_flag(menu_layer, LV_OBJ_FLAG_HIDDEN);
	grab_glass();
	gearboy_set_paused(false);
}


// How long the badge stays up.
//
// Top right rather than the middle of the screen: what was just done needs
// confirming, not looking at. A disc in the theme's background colour, because
// underneath it is the game picture, which can be any colour.
#define BADGE_SHOWN_MS 1400

static void badge_hide_cb(lv_timer_t *timer) {
	(void)timer;
	if (badge) {
		lv_obj_add_flag(badge, LV_OBJ_FLAG_HIDDEN);
	}
	lv_timer_pause(badge_timer);
}

static void badge_show(const lv_image_dsc_t *icon) {
	if (!badge) {
		return;
	}
	lv_obj_set_style_bg_color(badge, theme()->screen_bg, 0);
	lv_image_set_src(badge_icon, icon);
	lv_obj_set_style_image_recolor(badge_icon, theme()->text_primary, 0);
	lv_obj_set_style_image_recolor_opa(badge_icon, LV_OPA_COVER, 0);

	lv_obj_remove_flag(badge, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(badge);

	lv_timer_reset(badge_timer);
	lv_timer_resume(badge_timer);
}

static void menu_dismiss_cb(lv_event_t *e) {
	(void)e;
	if (menu_still_deaf()) {
		return;
	}
	menu_close_and_resume();
}

static void menu_savestate_cb(lv_event_t *e) {
	(void)e;
	if (menu_still_deaf()) {
		return;
	}
	// The game does NOT resume here: it stays paused until the thread is done,
	// because the thread only looks at requests while paused. poll_cb resumes
	// it when the result arrives.
	lv_obj_add_flag(menu_layer, LV_OBJ_FLAG_HIDDEN);
	waiting_for_state = true;
	gearboy_request_savestate();
}

static void menu_loadstate_cb(lv_event_t *e) {
	(void)e;
	if (menu_still_deaf()) {
		return;
	}
	lv_obj_add_flag(menu_layer, LV_OBJ_FLAG_HIDDEN);
	waiting_for_state = true;
	gearboy_request_loadstate();
}

static void menu_exit_cb(lv_event_t *e) {
	(void)e;
	if (menu_still_deaf()) {
		return;
	}
	lv_obj_add_flag(menu_layer, LV_OBJ_FLAG_HIDDEN);
	// No resume and no gbinput: this is the way out, and unloading the page
	// tears everything down properly (see screen_unloaded_cb).
	back_btn_cb(NULL);
}

// True while the settings page was entered FROM THE GAME: unloading this page
// must not stop the emulator (it stays paused with the ROM loaded), and coming
// back resumes it where it was.
static bool visiting_settings;

static void menu_settings_cb(lv_event_t *e) {
	(void)e;
	if (menu_still_deaf()) {
		return;
	}
	lv_obj_add_flag(menu_layer, LV_OBJ_FLAG_HIDDEN);
	// The game stays PAUSED: it resumes on return to this page
	// (screen_loaded_cb), which the settings page's back button or back swipe
	// reaches on their own.
	visiting_settings = true;
	switch_screen(gearboysettings_screen);
}

static lv_obj_t *menu_row(lv_obj_t *parent, const char *text, lv_event_cb_t cb) {
	lv_obj_t *row = lv_btn_create(parent);
	lv_obj_set_size(row, lv_pct(100), 76);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, 12, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, 20, 0);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text(label, tr(text));
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	lv_obj_center(label);

	return row;
}

static void build_menu(void) {
	menu_layer = lv_obj_create(gearboyplay_screen);
	lv_obj_remove_style_all(menu_layer);
	lv_obj_set_size(menu_layer, GAME_W, panel_h);
	lv_obj_set_pos(menu_layer, 0, 0);
	lv_obj_set_style_bg_color(menu_layer, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(menu_layer, LV_OPA_60, 0);
	lv_obj_add_flag(menu_layer, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(menu_layer, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(menu_layer, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_event_cb(menu_layer, menu_dismiss_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *card = lv_obj_create(menu_layer);
	lv_obj_set_size(card, 360, LV_SIZE_CONTENT);
	lv_obj_center(card);
	lv_obj_add_style(card, &theme_style_screen, 0);
	lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(card, 18, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, 16, 0);
	lv_obj_set_style_pad_gap(card, 10, 0);
	lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	menu_row(card, "gearboy_create_save_state", menu_savestate_cb);
	menu_row(card, "gearboy_load_save_state", menu_loadstate_cb);
	menu_row(card, "settings", menu_settings_cb);
	menu_row(card, "gearboy_quit_the_game", menu_exit_cb);
}

// The badge: a 56-pixel disc in the corner with the icon inside. It takes no
// touch -- it is a label, not a control.
#define BADGE_SIZE 56

static void build_badge(void) {
	badge = lv_obj_create(gearboyplay_screen);
	lv_obj_remove_style_all(badge);
	lv_obj_set_size(badge, BADGE_SIZE, BADGE_SIZE);
	lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -14, 14);
	lv_obj_set_style_bg_color(badge, theme()->screen_bg, 0);
	lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
	lv_obj_remove_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(badge, LV_OBJ_FLAG_HIDDEN);

	badge_icon = lv_image_create(badge);
	lv_image_set_src(badge_icon, &icon_save_state);
	lv_obj_center(badge_icon);

	badge_timer = lv_timer_create(badge_hide_cb, BADGE_SHOWN_MS, NULL);
	lv_timer_pause(badge_timer);
}

// On the simulator -- and on the device if the evdev node cannot be claimed --
// the glass stays with LVGL, so the menu zone must also exist as a clickable
// object. On the device with gbinput active this object receives nothing, so it
// never fires twice.
static void menu_zone_cb(lv_event_t *e) {
	(void)e;
	menu_open();
}

// ---------------------------------------------------------------------------
// the tick
// ---------------------------------------------------------------------------

// What the page is showing right now. Only there to avoid rewriting the same
// label sixty times a second: lv_label_set_text() copies the string every time,
// which would be one allocation per frame to change nothing.
typedef enum { VEIL_STARTING, VEIL_FAILED, VEIL_GONE } veil_state_t;
static veil_state_t veil_state;

static void set_veil(veil_state_t want) {
	if (veil_state == want) {
		return;
	}
	veil_state = want;

	if (want == VEIL_GONE) {
		lv_obj_add_flag(pause_veil, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(game_image, LV_OBJ_FLAG_HIDDEN);
		return;
	}

	lv_obj_add_flag(game_image, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag(pause_veil, LV_OBJ_FLAG_HIDDEN);
	lv_label_set_text(veil_label,
					  want == VEIL_FAILED
						  ? tr("gearboy_start_failed")
						  : tr("gearboy_starting_the_game"));
}

// Reports the outcome of a save or load once. The emulator thread leaves it
// behind and this consumes it.
static void report_state_result(void) {
	int result = gearboy_take_state_result();
	if (result == 0) {
		return;
	}

	// The result has arrived, so there is nothing left to wait for: resume.
	if (waiting_for_state) {
		waiting_for_state = false;
		grab_glass();
		gearboy_set_paused(false);
	}

	switch (result) {
	case 1:
		badge_show(&icon_save_state);
		break;
	case 2:
		badge_show(&icon_load_state);
		break;
	case -1:
		gui_notify_popup("gearboy_state_save_failed");
		break;
	case -2:
		gui_notify_popup("gearboy_no_saved_state");
		break;
	default:
		break;
	}
}

static void poll_cb(lv_timer_t *timer) {
	(void)timer;

	report_state_result();

	const uint16_t *pixels = gearboy_screen();

	// The signal is the frame COUNTER, not gearboy_running(): that is true
	// immediately, while the thread is still opening the ROM. A counted frame is
	// a drawn frame, so the veil stays up until there is something to see.
	uint32_t frames = gearboy_frame_count();
	if (!gearboy_running() || !pixels || frames == 0) {
		// If the emulator went away while a result was pending, that result
		// will never come, so do not wait for it forever.
		if (waiting_for_state && !gearboy_running()) {
			waiting_for_state = false;
		}
		set_veil(gearboy_failed() ? VEIL_FAILED : VEIL_STARTING);
		return;
	}

	if (game_dsc.data == NULL) {
		game_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
		game_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
		game_dsc.header.w = GAME_W;
		game_dsc.header.h = GAME_H;
		game_dsc.header.stride = GAME_W * 2;
		game_dsc.data_size = (uint32_t)GAME_W * GAME_H * 2;
		game_dsc.data = (const uint8_t *)pixels;
		lv_image_set_src(game_image, &game_dsc);
	}
	set_veil(VEIL_GONE);

	// Redraw only on a new frame. The counter is the only thing the interface
	// reads from the emulator thread, and without this comparison LVGL would
	// recompose 480x432 even when the pixels are identical -- exactly the
	// millisecond there is none to spare.
	//
	// The 3x scaling happens HERE, on the thread that then draws it: that is
	// what removes the horizontal tear. See the comment in gearboy.c.
	if (frames != last_frame) {
		last_frame = frames;
		if (gearboy_present()) {
			lv_obj_invalidate(game_image);
		}
	}
}

// ---------------------------------------------------------------------------
// entering and leaving
// ---------------------------------------------------------------------------

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;

	// The status bar goes: a Game Boy has no clock and battery above it, and
	// those 44 rows are exactly the ones the picture needs.
	topbar_set_hidden(true);

	// And the back arrow with it: the way out of this page is the menu.
	back_btn_force_hidden(true);

	// Without the status bar the volume would have nowhere to appear. The
	// player's volume pill is the answer -- same drawing, same place -- and
	// applies here for as long as the page is on screen.
	volume_overlay_allow_outside_player(true);

	if (pending_rom[0]) {
		gearboy_start(pending_rom, pending_title);
		pending_rom[0] = '\0';
	}

	// Coming back from settings: the game was left paused on purpose and now
	// resumes where it was, so "back" from settings returns to the game rather
	// than to a frozen frame.
	if (visiting_settings) {
		visiting_settings = false;
		if (gearboy_running() && gearboy_paused()) {
			gearboy_set_paused(false);
		}
	}

	// The glass goes to the emulator. If it cannot be claimed (no evdev node,
	// or the simulator) the LVGL buttons remain: one finger at a time, enough
	// to try the page and not enough to play.
	grab_glass();

	last_frame = 0;
	veil_state = VEIL_GONE;
	set_veil(VEIL_STARTING);
	lv_timer_resume(poll_timer);
	lv_timer_ready(poll_timer);
	refresh_period_set(GAME_REFRESH_MS);
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;

	lv_timer_pause(poll_timer);

	if (menu_layer) {
		lv_obj_add_flag(menu_layer, LV_OBJ_FLAG_HIDDEN);
	}
	waiting_for_state = false;
	if (badge) {
		lv_obj_add_flag(badge, LV_OBJ_FLAG_HIDDEN);
		lv_timer_pause(badge_timer);
	}

	// Release the glass first, then stop the game. The other order leaves the
	// reader writing keys into an emulator that is shutting down.
	gbinput_stop();
	fallback_keys = 0;

	// The image stops pointing at the frame buffer BEFORE gearboy_stop() frees
	// it. LVGL redraws whenever it likes, and a descriptor pointing at freed
	// memory is a SIGSEGV that lands half a second later somewhere else
	// entirely.
	lv_image_set_src(game_image, NULL);
	game_dsc.data = NULL;
	veil_state = VEIL_GONE; // force set_veil() to do the work again
	set_veil(VEIL_STARTING);

	// On the way to settings the game is NOT stopped: it stays paused with the
	// core alive and the ROM loaded, and returning to this page resumes it.
	// The image was detached from the buffer above regardless; it reattaches
	// itself on the first frame after the return.
	if (!visiting_settings) {
		gearboy_stop();
	}

	refresh_period_restore();

	volume_overlay_allow_outside_player(false);
	back_btn_force_hidden(false);
	topbar_set_hidden(false);
}

void gearboyplay_open(const char *rom_path, const char *title) {
	if (!rom_path || !rom_path[0]) {
		return;
	}
	snprintf(pending_rom, sizeof(pending_rom), "%s", rom_path);
	snprintf(pending_title, sizeof(pending_title), "%s", title ? title : "");
	switch_screen(gearboyplay_screen);
}

// The controls' background follows the theme, so it changes with it.
static void refresh_theme(void) {
	if (controls_bg) {
		lv_obj_set_style_bg_color(controls_bg, theme()->screen_bg, 0);
	}
}

void gearboyplay_init(gui_config_t *cfg) {
	// Before anything is placed: every row below the picture is measured from
	// this, and so are the touch zones handed to gbinput.
	panel_set_height((int)cfg->screen_height);
	gbinput_set_glass((int)cfg->screen_width, (int)cfg->screen_height);

	lv_obj_add_style(gearboyplay_screen, &theme_style_screen, 0);
	lv_obj_set_style_bg_color(gearboyplay_screen, lv_color_black(), 0);
	lv_obj_remove_flag(gearboyplay_screen, LV_OBJ_FLAG_SCROLLABLE);

	// Background for the controls half. This page's screen is black because the
	// game picture sits above it and any other colour would frame it; below the
	// picture there is nothing to frame, so the controls take the theme's own
	// background rather than leaving a black band on the light theme.
	controls_bg = lv_obj_create(gearboyplay_screen);
	lv_obj_remove_style_all(controls_bg);
	lv_obj_set_pos(controls_bg, 0, SCREEN_TOP + GAME_H);
	lv_obj_set_size(controls_bg, GAME_W, panel_h - (SCREEN_TOP + GAME_H));
	lv_obj_set_style_bg_color(controls_bg, theme()->screen_bg, 0);
	lv_obj_set_style_bg_opa(controls_bg, LV_OPA_COVER, 0);

	// What is shown until the first frame exists. Not a loading screen: it lasts
	// half a second, and is only there because an empty black rectangle looks
	// like a fault.
	pause_veil = lv_obj_create(gearboyplay_screen);
	lv_obj_remove_style_all(pause_veil);
	lv_obj_set_pos(pause_veil, 0, SCREEN_TOP);
	lv_obj_set_size(pause_veil, GAME_W, GAME_H);
	lv_obj_set_style_bg_color(pause_veil, lv_color_make(24, 24, 26), 0);
	lv_obj_set_style_bg_opa(pause_veil, LV_OPA_COVER, 0);

	veil_label = lv_label_create(pause_veil);
	lv_label_set_text(veil_label, tr("gearboy_starting_the_game"));
	lv_label_set_long_mode(veil_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(veil_label, GAME_W - 64);
	lv_obj_set_style_text_align(veil_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(veil_label, &font_ui_18, 0);
	lv_obj_set_style_text_color(veil_label, lv_color_make(150, 150, 155), 0);
	lv_obj_center(veil_label);

	game_image = lv_image_create(gearboyplay_screen);
	lv_obj_set_pos(game_image, 0, SCREEN_TOP);
	lv_obj_set_size(game_image, GAME_W, GAME_H);
	lv_obj_add_flag(game_image, LV_OBJ_FLAG_HIDDEN);

	// Select and Start: two colourless rectangles inside the picture. Invisible
	// by design -- they exist only so the simulator, which has no touchscreen
	// reader, can click on them.
	pressable(gearboyplay_screen, 0, SCREEN_TOP + CORNER_Y, CORNER_W, CORNER_H, GB_KEY_SELECT);
	pressable(gearboyplay_screen, GAME_W - CORNER_W, SCREEN_TOP + CORNER_Y, CORNER_W, CORNER_H, GB_KEY_START);

	// And the centre of the picture, which opens the menu. Invisible too.
	{
		lv_obj_t *zone = lv_obj_create(gearboyplay_screen);
		lv_obj_remove_style_all(zone);
		lv_obj_set_pos(zone, MENU_X, MENU_Y);
		lv_obj_set_size(zone, MENU_W, MENU_H);
		lv_obj_add_flag(zone, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_remove_flag(zone, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_add_event_cb(zone, menu_zone_cb, LV_EVENT_CLICKED, NULL);
	}

	build_dpad(gearboyplay_screen);

	// The eight D-pad touch cells, on top of the drawing.
	{
		gbinput_zone_t zones[16];
		int n = build_zones(zones, 16);
		for (int i = 0; i < n; i++) {
			// A, B, Select, Start and the menu already have theirs: D-pad
			// only here.
			if (zones[i].keys & (GB_KEY_A | GB_KEY_B | GB_KEY_SELECT | GB_KEY_START | GBINPUT_KEY_MENU)) {
				continue;
			}
			pressable(gearboyplay_screen, zones[i].x, zones[i].y, zones[i].w, zones[i].h, zones[i].keys);
		}
	}

	build_button(gearboyplay_screen, B_X, B_ROW, "B", GB_KEY_B);
	build_button(gearboyplay_screen, A_X, A_ROW, "A", GB_KEY_A);

	// The menu and the badge last: they are the only two things that must sit
	// above everything else on the page.
	build_menu();
	build_badge();

	poll_timer = lv_timer_create(poll_cb, POLL_MS, NULL);
	lv_timer_pause(poll_timer);

	lv_obj_add_event_cb(gearboyplay_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(gearboyplay_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	theme_register_refresh(refresh_theme);

	// No back swipe: while playing, a finger moving right means "run right",
	// not "leave". The way out is the menu, opened by pressing the middle of
	// the picture.
}

#include "sx_mini.h"

#include <stdio.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/sx_chain.h"
#include "src/gui/shell/sx_frame.h"
#include "src/gui/shell/sx_palette.h"
#include "src/gui/shell/sx_styles.h"
#include "src/gui/shell/sx_tokens.h"
#include "src/gui/shell/theme.h"

#define CHAIN_DOT 9
#define CHAIN_FONT font_ui_16
// How many progress updates go by between two looks at the chain: the link's
// rate settles a moment after a track starts, and the codec can change under a
// playing track, but neither needs the player's twice-a-second poll.
#define CHAIN_EVERY 4

static lv_obj_t *slot;	   // the 88 px band, transparent; takes the gestures
static lv_obj_t *card;	   // the 72 px dark card in it
static lv_obj_t *cover;	   // the tone plaque...
static lv_obj_t *cover_img; // ...and the thumbnail over it
static lv_obj_t *title;
static lv_obj_t *chain_dot;
static lv_obj_t *chain_label;
static lv_obj_t *play_area;
static lv_obj_t *play_icon;
static lv_obj_t *progress_fill;
static int progress_w; // the track's width, the fill's 100 %

static bool has_track;
static bool live;
static bool playing;
static bool player_open;
static bool shown;
static char chain_text[96];
static bool chain_warn;
static int chain_countdown;

#define HIDDEN_SCREENS_MAX 16
static lv_obj_t *hidden_screens[HIDDEN_SCREENS_MAX];
static int hidden_screen_count;

lv_obj_t *sx_mini_obj(void) { return slot; }

bool sx_mini_visible(void) { return shown; }

bool sx_mini_has_track(void) { return has_track; }

static bool screen_hides_mini(lv_obj_t *screen) {
	for (int i = 0; i < hidden_screen_count; i++) {
		if (hidden_screens[i] == screen) {
			return true;
		}
	}
	return false;
}

void sx_mini_hide_on_screen(lv_obj_t *screen) {
	if (!screen || screen_hides_mini(screen) || hidden_screen_count >= HIDDEN_SCREENS_MAX) {
		return;
	}
	hidden_screens[hidden_screen_count++] = screen;
	sx_mini_update_visibility();
}

void sx_mini_update_visibility(void) {
	if (!slot) {
		return;
	}
	lv_obj_t *screen = lv_screen_active();
	bool want = has_track && !player_open && !screen_hides_mini(screen) && !keyboard_shown_on(screen);
	if (want == shown) {
		return;
	}
	shown = want;
	lv_obj_set_hidden(slot, !want);
	sx_frame_slot_changed();
}

void sx_mini_player_shown(bool open) {
	player_open = open;
	sx_mini_update_visibility();
}

// ---------------------------------------------------------------------------
// What it shows
// ---------------------------------------------------------------------------

static void paint_chain(void) {
	lv_color_t c = lv_color_hex(chain_warn ? SX_CHAIN_WARN : SX_CHAIN_OK);
	lv_obj_set_style_bg_color(chain_dot, c, 0);
	lv_obj_set_style_text_color(chain_label, c, 0);
}

void sx_mini_refresh_chain(void) {
	if (!chain_label) {
		return;
	}
	char text[sizeof(chain_text)];
	bool warn = false;
	sx_chain_short(text, sizeof(text), &warn);
	if (strcmp(text, chain_text) == 0 && warn == chain_warn) {
		return;
	}
	snprintf(chain_text, sizeof(chain_text), "%s", text);
	chain_warn = warn;
	lv_label_set_text(chain_label, text);
	bool any = text[0] != '\0';
	lv_obj_set_hidden(chain_dot, !any);
	lv_obj_set_hidden(chain_label, !any);
	paint_chain();
}

void sx_mini_set_track(const char *track_title, const char *track_path, bool is_live) {
	if (!slot) {
		return;
	}
	has_track = (track_path && track_path[0]) || is_live || (track_title && track_title[0]);
	live = is_live;
	if (has_track) {
		lv_label_set_text(title, track_title ? track_title : "");
	}
	sx_mini_set_progress(0, 0);
	chain_countdown = 0;
	sx_mini_refresh_chain();
	sx_mini_update_visibility();
	sx_frame_slot_changed(); // the idle cards follow whether anything is loaded
}

void sx_mini_set_playing(bool now_playing) {
	if (!play_icon || now_playing == playing) {
		return;
	}
	playing = now_playing;
	// A stream has no pause to come back from; the player shows stop there,
	// and so does this.
	lv_image_set_src(play_icon, playing ? (live ? &icon_stop : &icon_pause_small) : &icon_play_small);
}

void sx_mini_set_progress(double position, double total) {
	if (!progress_fill) {
		return;
	}
	int w;
	if (live) {
		w = progress_w;
	} else if (total <= 0) {
		w = 0;
	} else {
		double f = position / total;
		f = f < 0 ? 0 : (f > 1 ? 1 : f);
		w = (int)(f * progress_w + 0.5);
	}
	// lv_obj_set_width() is a no-op for the same width: the line is redrawn
	// only when it grows by a pixel.
	lv_obj_set_width(progress_fill, w);
	lv_obj_set_hidden(progress_fill, w == 0);

	if (--chain_countdown <= 0) {
		chain_countdown = CHAIN_EVERY;
		sx_mini_refresh_chain();
	}
}

void sx_mini_set_cover(const lv_image_dsc_t *thumbnail) {
	if (!cover_img) {
		return;
	}
	if (thumbnail) {
		lv_image_set_src(cover_img, thumbnail);
		lv_obj_set_hidden(cover_img, false);
	} else {
		lv_obj_set_hidden(cover_img, true);
		lv_image_set_src(cover_img, NULL);
	}
}

// ---------------------------------------------------------------------------
// Gestures
// ---------------------------------------------------------------------------

static lv_point_t press_at;
static bool press_tracking;

static void open_player(void) {
	if (!player_sheet_is_open()) {
		switch_screen(player_screen);
	}
}

static void slot_event_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_indev_t *indev = lv_indev_active();
	if (!indev) {
		return;
	}
	lv_point_t p;
	lv_indev_get_point(indev, &p);

	if (code == LV_EVENT_PRESSED) {
		press_at = p;
		press_tracking = true;
		return;
	}
	if (code == LV_EVENT_PRESS_LOST) {
		press_tracking = false;
		return;
	}
	if (code != LV_EVENT_RELEASED || !press_tracking) {
		return;
	}
	press_tracking = false;

	int dx = p.x - press_at.x;
	int dy = p.y - press_at.y;
	if (dy < -SX_MINI_SWIPE_UP_PX && -dy > LV_ABS(dx)) {
		open_player();
	} else if (dx < -SX_MINI_SWIPE_PX) {
		player_screen_next();
	} else if (dx > SX_MINI_SWIPE_PX) {
		player_screen_prev();
	} else if (LV_ABS(dx) < SX_SWIPE_LOCK_PX && LV_ABS(dy) < SX_SWIPE_LOCK_PX) {
		open_player();
	}
}

static void play_clicked_cb(lv_event_t *e) {
	(void)e;
	player_key_play_pause();
}

// ---------------------------------------------------------------------------
// Building it
// ---------------------------------------------------------------------------

static void refresh_theme(void) {
	if (chain_label) {
		paint_chain();
	}
	if (play_icon) {
		lv_obj_set_style_image_recolor(play_icon, sx_pal()->np_ink, 0);
	}
}

void sx_mini_init(gui_config_t *cfg) {
	if (slot) {
		return;
	}
	int w = (int)cfg->screen_width;
	int h = (int)cfg->screen_height;

	slot = sx_box(lv_layer_top());
	lv_obj_set_size(slot, w, SX_MINI_SLOT_H);
	lv_obj_set_pos(slot, 0, h - SX_MINI_SLOT_H);
	// The margins around the card are the page's colour: a list scrolled
	// under the slot does not show through beside the card.
	lv_obj_add_style(slot, &sx_style_page, 0);
	lv_obj_set_style_pad_top(slot, SX_MINI_MARGIN_TOP, 0);
	lv_obj_set_style_pad_hor(slot, SX_MINI_MARGIN_SIDE, 0);
	lv_obj_set_style_pad_bottom(slot, SX_MINI_MARGIN_BOTTOM, 0);
	// The slot takes the presses: the margins around the card are part of the
	// target, and nothing under the slot is reachable while it is up anyway.
	lv_obj_set_clickable(slot, true);
	lv_obj_set_event_bubble(slot, false);
	lv_obj_add_event_cb(slot, slot_event_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(slot, slot_event_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(slot, slot_event_cb, LV_EVENT_PRESS_LOST, NULL);
	lv_obj_set_hidden(slot, true);

	card = sx_box(slot);
	lv_obj_set_size(card, lv_pct(100), SX_MINI_H);
	lv_obj_add_style(card, &sx_style_mini, 0);
	lv_obj_set_style_radius(card, SX_MINI_R, 0);
	lv_obj_set_style_clip_corner(card, false, 0);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_left(card, 10, 0);

	// The light line along the top: white at 16 %, whatever the theme -- the
	// card is always dark. Inset by the radius so it does not stick out past
	// the rounded corners.
	lv_obj_t *hairline = sx_box(card);
	lv_obj_set_ignore_layout(hairline, true);
	lv_obj_set_size(hairline, w - 2 * SX_MINI_MARGIN_SIDE - 2 * SX_MINI_R, 1);
	lv_obj_set_pos(hairline, SX_MINI_R - 10, 0);
	lv_obj_set_style_bg_color(hairline, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(hairline, SX_HAIRLINE_DARK_OPA, 0);

	cover = sx_box(card);
	lv_obj_set_size(cover, SX_MINI_COVER, SX_MINI_COVER);
	lv_obj_add_style(cover, &sx_style_mini_tone, 0);
	lv_obj_set_style_radius(cover, SX_MINI_COVER_R, 0);
	lv_obj_set_style_clip_corner(cover, true, 0);
	cover_img = lv_image_create(cover);
	lv_obj_center(cover_img);
	lv_obj_set_hidden(cover_img, true);

	lv_obj_t *text = sx_box(card);
	lv_obj_set_height(text, SX_MINI_H);
	lv_obj_set_flex_grow(text, 1);
	lv_obj_set_style_pad_hor(text, 12, 0);
	lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(text, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_row(text, 2, 0);

	title = sx_label(text, &font_ui_24_bold, NULL);
	lv_obj_set_width(title, lv_pct(100));
	lv_obj_set_style_text_color(title, lv_color_white(), 0);

	lv_obj_t *chain = sx_box(text);
	lv_obj_set_size(chain, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(chain, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(chain, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(chain, 7, 0);

	chain_dot = sx_box(chain);
	lv_obj_set_size(chain_dot, CHAIN_DOT, CHAIN_DOT);
	lv_obj_set_style_radius(chain_dot, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(chain_dot, LV_OPA_COVER, 0);

	chain_label = sx_label(chain, &CHAIN_FONT, NULL);
	lv_obj_set_flex_grow(chain_label, 1);
	lv_obj_set_hidden(chain_dot, true);
	lv_obj_set_hidden(chain_label, true);

	// The disc sits in a 68 px wide target that takes its own presses.
	play_area = lv_obj_create(card);
	lv_obj_remove_style_all(play_area);
	lv_obj_set_size(play_area, 68, SX_MINI_H);
	lv_obj_set_scrollable(play_area, false);
	lv_obj_set_event_bubble(play_area, false);
	lv_obj_add_event_cb(play_area, play_clicked_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *disc = sx_box(play_area);
	lv_obj_set_size(disc, SX_MINI_PLAY, SX_MINI_PLAY);
	lv_obj_center(disc);
	lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(disc, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
	lv_obj_set_event_bubble(disc, false);

	play_icon = lv_image_create(disc);
	lv_image_set_src(play_icon, &icon_play_small);
	lv_obj_set_style_image_recolor_opa(play_icon, LV_OPA_COVER, 0);
	lv_obj_center(play_icon);

	// The progress line: 3 px along the card's bottom edge, 18 px in from
	// each side, 4 px up.
	progress_w = w - 2 * SX_MINI_MARGIN_SIDE - 2 * 18;
	lv_obj_t *track = sx_box(card);
	lv_obj_set_ignore_layout(track, true);
	lv_obj_set_size(track, progress_w, SX_MINI_PROGRESS_H);
	lv_obj_align(track, LV_ALIGN_BOTTOM_LEFT, 18 - 10, -4);
	lv_obj_set_style_radius(track, 2, 0);
	lv_obj_set_style_bg_color(track, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(track, LV_OPA_20, 0);

	progress_fill = sx_box(track);
	lv_obj_set_size(progress_fill, 0, SX_MINI_PROGRESS_H);
	lv_obj_add_style(progress_fill, &sx_style_np_accent, 0);
	lv_obj_set_style_radius(progress_fill, 2, 0);
	lv_obj_set_hidden(progress_fill, true);

	refresh_theme();
	theme_register_refresh(refresh_theme);
}

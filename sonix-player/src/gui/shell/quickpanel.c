#include "quickpanel.h"

#include <stdio.h>
#include <string.h>

#include "src/gui/wireless/airplay.h"
#include "src/gui/audio/dacpage.h"
#include "src/gui/bluetooth/btsettings.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/settings/musicsettings.h"
#include "src/gui/audio/peqpage.h"
#include "src/gui/library/audiobooks.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/streaming/podcastpage.h"
#include "src/gui/wireless/dlna.h"
#include "src/gui/wireless/sonixlink.h"
#include "src/gui/wireless/wifitransfer.h"
#include "src/gui/shell/sx_palette.h"
#include "src/gui/shell/sx_sheet.h"
#include "src/gui/shell/sx_tokens.h"
#include "src/gui/shell/theme.h"
#include "src/system/bluetooth/btreceiver.h"
#include "src/system/audio/usbdac.h"
#include "src/system/net/wifitransfer.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/topbar.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/system/remote/airplay.h"
#include "src/system/audio/alsa-controls.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/audio/eq.h"
#include "src/system/playback/sleeptimer.h"
#include "src/system/device/power.h"
#include "src/system/remote/dlna.h"
#include "src/system/remote/sonixlink.h"
#include "src/system/net/wifi.h"

// ---------------------------------------------------------------------------
// Sonix UI 2.0: the control centre
//
// An overlay at the top of the screen, under the status bar: one card
// (SX_CC_BG, dark in both themes) with the round buttons four to a line,
// SX_CC_BTN across with their names under them, and the brightness slider.
// A tap on a button switches what it stands for (lit in the accent while on);
// a hold opens its page. It comes down from the status bar following the
// finger (topbar.c, player.c), and goes away with a swipe up of SX_CC_CLOSE_PX
// anywhere on it, or a tap anywhere off it.
//
// The page underneath stays as it is, undimmed: a translucent veil over the
// whole screen would have to be blended on every frame of the slide, the whole
// page under it redrawn first to blend against (see the note on the veil).
//
// The first eight buttons show; with more in the panel (Settings > More >
// Control centre arranges up to sixteen), a handle under the slider stretches
// the card down to show them, by a drag or a tap.
//
// The 1.2.5 panel's second card -- the track playing and its transport -- is
// gone: in 2.0 the mini player is under the card on every page, and the player
// has everything that card had.
//
// Costs nothing while shut: built once, its only timer (repainting the buttons
// that can change under it, line out with a jack pulled) runs while it is open.
// ---------------------------------------------------------------------------

#define PANEL_ANIM_MS SX_ANIM_SHEET_MS
#define PANEL_POLL_MS 700

#define CC_MARGIN 14	   // from the screen's sides
#define CC_TOP_GAP 12	   // under the status bar
#define CC_RADIUS 24
#define CC_PAD 20
#define CC_GAP 14		   // between the lines of buttons
#define CC_LABEL_GAP 6	   // between a button and its name
#define CC_BUTTONS_PER_LINE 4
#define BRIGHT_ROW_H 56
#define BRIGHT_ROW_GAP 12
#define HANDLE_ROOM 24 // added under the slider when the card has a handle

// The card's own colours: dark in both themes, as on the prototype.
#define CC_TEXT 0xFFFFFF
#define CC_LABEL_OPA 204   // the names, white at 80 %
#define CC_TRACK_OPA 51	   // the slider's track, white at 20 %

static lv_obj_t *veil;	// the full screen behind the card: a tap target, never drawn
static lv_obj_t *panel; // the card, parked above the top edge
static lv_obj_t *brightness_slider;
static lv_obj_t *mseb_btn, *eq_btn;
static lv_obj_t *wifi_btn, *bt_btn;
static lv_obj_t *fade_btn, *gain_btn;
static lv_obj_t *airplay_btn;
static lv_obj_t *lineout_btn;
static lv_obj_t *sonixlink_btn;
static lv_obj_t *peq_btn;
static lv_obj_t *dlna_btn;
static lv_obj_t *sleep_music_btn;
static lv_obj_t *sleep_audiobook_btn;
static lv_obj_t *sleep_podcast_btn;
static lv_obj_t *wifi_transfer_btn;
static lv_obj_t *gapless_btn;
static lv_obj_t *vlimit_btn;
static lv_obj_t *dac_btn;
static lv_obj_t *filter_btn;

// ---------------------------------------------------------------------------
// Which buttons the panel carries, and where
//
// Kept as names rather than as indices so that adding a button later joins a
// saved layout instead of shifting everything after it. The grid and the list
// of the ones left out together account for every button exactly once,
// whatever the config file says.
// ---------------------------------------------------------------------------

// Three tables and not one struct of tuples, because the columns are read by
// different things and only some of them are text. The keys are what the
// config file carries and must never be retranslated or renamed; the tags are
// what the settings page puts on screen, and the labels the short names under
// the circles. The keys stay out of the language files, where they never
// belonged.
static const char *const button_key[QP_BTN_COUNT] = {
	[QP_BTN_WIFI] = "wifi",
	[QP_BTN_BLUETOOTH] = "bluetooth",
	[QP_BTN_AIRPLAY] = "airplay",
	[QP_BTN_MSEB] = "mseb",
	[QP_BTN_EQ] = "eq",
	[QP_BTN_FADE] = "fade",
	[QP_BTN_GAIN] = "gain",
	[QP_BTN_LINEOUT] = "lineout",
	[QP_BTN_SONIXLINK] = "sonixlink",
	[QP_BTN_PEQ] = "peq",
	[QP_BTN_DLNA] = "dlna",
	[QP_BTN_SLEEP_MUSIC] = "sleep_music",
	[QP_BTN_SLEEP_AUDIOBOOK] = "sleep_audiobook",
	[QP_BTN_SLEEP_PODCAST] = "sleep_podcast",
	[QP_BTN_WIFI_TRANSFER] = "wifi_transfer",
	[QP_BTN_GAPLESS] = "gapless",
	[QP_BTN_VOLUME_LIMIT] = "volume_limit",
	[QP_BTN_DAC] = "dac",
	[QP_BTN_DAC_FILTER] = "dac_filter",
};

static const char *const button_tag[QP_BTN_COUNT] = {
	[QP_BTN_WIFI] = "wi_fi",
	[QP_BTN_BLUETOOTH] = "bluetooth",
	[QP_BTN_AIRPLAY] = "airplay",
	[QP_BTN_MSEB] = "mseb",
	[QP_BTN_EQ] = "equaliser",
	[QP_BTN_FADE] = "fade",
	[QP_BTN_GAIN] = "gain",
	[QP_BTN_LINEOUT] = "quickpanel_line_out",
	[QP_BTN_SONIXLINK] = "sonixlink",
	[QP_BTN_PEQ] = "peq",
	[QP_BTN_DLNA] = "dlna",
	[QP_BTN_SLEEP_MUSIC] = "quickpanel_sleep_music",
	[QP_BTN_SLEEP_AUDIOBOOK] = "quickpanel_sleep_audiobook",
	[QP_BTN_SLEEP_PODCAST] = "quickpanel_sleep_podcast",
	[QP_BTN_WIFI_TRANSFER] = "wifitransfer_title",
	[QP_BTN_GAPLESS] = "musicsettings_gapless_playback",
	[QP_BTN_VOLUME_LIMIT] = "musicsettings_volume_limit",
	[QP_BTN_DAC] = "quickpanel_dac_mode",
	[QP_BTN_DAC_FILTER] = "musicsettings_filters",
};

// Under the circles: a word or two, at most the width of a quarter of the card.
static const char *const button_label[QP_BTN_COUNT] = {
	[QP_BTN_WIFI] = "qp_label_wifi",
	[QP_BTN_BLUETOOTH] = "qp_label_bluetooth",
	[QP_BTN_AIRPLAY] = "qp_label_airplay",
	[QP_BTN_MSEB] = "qp_label_mseb",
	[QP_BTN_EQ] = "qp_label_eq",
	[QP_BTN_FADE] = "qp_label_fade",
	[QP_BTN_GAIN] = "qp_label_gain",
	[QP_BTN_LINEOUT] = "qp_label_lineout",
	[QP_BTN_SONIXLINK] = "qp_label_sonixlink",
	[QP_BTN_PEQ] = "qp_label_peq",
	[QP_BTN_DLNA] = "qp_label_dlna",
	[QP_BTN_SLEEP_MUSIC] = "qp_label_sleep_music",
	[QP_BTN_SLEEP_AUDIOBOOK] = "qp_label_sleep_audiobook",
	[QP_BTN_SLEEP_PODCAST] = "qp_label_sleep_podcast",
	[QP_BTN_WIFI_TRANSFER] = "qp_label_wifi_transfer",
	[QP_BTN_GAPLESS] = "qp_label_gapless",
	[QP_BTN_VOLUME_LIMIT] = "qp_label_volume_limit",
	[QP_BTN_DAC] = "qp_label_dac",
	[QP_BTN_DAC_FILTER] = "qp_label_dac_filter",
};

static uint8_t slots[QP_SLOT_COUNT];  // the grid, QP_BTN_NONE where it is empty
static uint8_t hidden[QP_BTN_COUNT]; // the buttons that are not in the grid
static uint8_t hidden_n;
static bool order_loaded;

// Set while a long press opens a page, so the click that ends the same press
// does not also toggle the effect.
static bool long_press_consumed;
static bool panel_open;
static int open_y; // the card's top while it is down, under the status bar

// Interactive drag state: where the card was when the finger went down.
static bool drag_active;
static int drag_from_y;
static uint32_t drag_begin_ms;

// A gesture this fast decides by its direction rather than by how far it got:
// a flick is over long before it has covered the distance a deliberate drag
// is measured against. The floor keeps the wobble of a tap out of it.
#define FLICK_MIN_PX 40
#define FLICK_SPEED_PX_S 500

// How far a drag down from the status bar has to bring the card before the
// release keeps it down rather than sending it back. Upwards, on the card,
// it is the handoff's SX_CC_CLOSE_PX.
#define DRAG_OPEN_PX 100

// ---------------------------------------------------------------------------
// The stretch of the card
//
// The card shows the first QP_SLOT_VISIBLE buttons. When the panel holds more,
// a handle sits under the brightness slider, and a drag down on the card (or a
// tap on the handle) stretches it downwards over the page: the slider moves
// down with the edge and the buttons past the eighth open up between the first
// two lines and the slider, as further lines of the same grid. A drag up folds
// it back before a second one carries the card away.
// ---------------------------------------------------------------------------

static lv_obj_t *quick_row;	   // the first QP_SLOT_VISIBLE buttons
static lv_obj_t *extra_clip;	   // the window the stretch opens, under the first lines
static lv_obj_t *extra_row;	   // the rest, inside that window
static lv_obj_t *bright_row;	   // moved down by the stretch
static lv_obj_t *expand_handle; // the line under the slider, and a tap target
static int card_h_rest;		   // the card's height with nothing past the eighth
static int card_h_base;		   // the card's height at rest, handle room included
static int expand_full;		   // how far the card stretches; 0 when nothing is past the eighth
static int expand_h;		   // how far it is stretched now
static int expand_from;		   // where a drag found it
static uint32_t expand_begin_ms;

// Set once a press has moved the card, so the click that ends the same press
// does not also fold the card from its handle.
static bool press_dragged;

static lv_timer_t *poll_timer;

bool quickpanel_is_open(void) { return panel_open; }

static void refresh_audio_buttons(void);

static void poll_cb(lv_timer_t *timer) {
	(void)timer;
	// The circles can be turned off by something other than a tap while the
	// panel is open -- line out ends when its jack is pulled or when Bluetooth
	// takes the sound.
	refresh_audio_buttons();
}

// Where the card waits while it is shut: its whole height above the top edge,
// whatever it is stretched to.
static int parked_y(void) { return -lv_obj_get_height(panel) - 4; }

// ---------------------------------------------------------------------------
// open / close
// ---------------------------------------------------------------------------

// The full-screen object behind the card has no background: it exists only to
// collect the tap that closes the card, and LVGL does not draw a background it
// cannot see.
//
// It must stay that way. A translucent veil over the whole 480x720 screen would
// have to be blended every frame of the slide, with everything under it redrawn
// first to blend against -- the whole page plus a full-screen alpha pass, at
// every step, on a CPU that is also feeding the DAC.
static void anim_y_cb(void *obj, int32_t v) { lv_obj_set_y((lv_obj_t *)obj, v); }

// True when the card raised the status bar itself and so has to put it away
// again. That happens exactly when the control centre is opened from the
// player, the one page that hides the bar.
static bool raised_topbar;

// Restores whatever the page underneath had. Called when the card has finished
// sliding away, not when it starts: the bar has to stay up for the whole slide.
//
// The bar goes back down only if the player is still the thing underneath. A
// long press on one of the icons closes the player and loads a page while this
// slide is still running, and a page keeps its status bar.
static void restore_topbar(void) {
	if (raised_topbar) {
		raised_topbar = false;
		if (player_sheet_is_open()) {
			topbar_set_hidden(true);
		}
	}
}

static void hide_when_parked_cb(lv_anim_t *a) {
	(void)a;
	if (!panel_open) {
		lv_obj_set_hidden(veil, true);
		restore_topbar();
	}
}

static void panel_slide_to(int y, bool animate) {
	lv_anim_delete(panel, anim_y_cb); // never race a previous slide
	if (!animate) {
		lv_obj_set_y(panel, y);
		if (!panel_open) {
			lv_obj_set_hidden(veil, true);
			restore_topbar();
		}
		return;
	}
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, panel);
	lv_anim_set_exec_cb(&a, anim_y_cb);
	lv_anim_set_values(&a, lv_obj_get_y(panel), y);
	lv_anim_set_duration(&a, PANEL_ANIM_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_completed_cb(&a, hide_when_parked_cb);
	lv_anim_start(&a);
}

// ---------------------------------------------------------------------------
// Stretching the card (see the note over quick_row)
// ---------------------------------------------------------------------------

static void expand_set(int h) {
	if (!panel) {
		return;
	}
	if (h > expand_full) {
		h = expand_full;
	}
	if (h < 0) {
		h = 0;
	}
	expand_h = h;
	lv_obj_set_height(panel, card_h_base + h);
	lv_obj_set_height(extra_clip, h);
	lv_obj_set_style_translate_y(bright_row, h, 0);
	lv_obj_set_hidden(extra_clip, h == 0);
}

static void expand_anim_cb(void *obj, int32_t v) {
	(void)obj;
	expand_set((int)v);
}

static void expand_to(int h, bool animate) {
	if (!panel) {
		return;
	}
	lv_anim_delete(panel, expand_anim_cb);
	if (!animate) {
		expand_set(h);
		return;
	}
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, panel);
	lv_anim_set_exec_cb(&a, expand_anim_cb);
	lv_anim_set_values(&a, expand_h, h);
	lv_anim_set_duration(&a, PANEL_ANIM_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_start(&a);
}

static void expand_drag_begin(void) {
	lv_anim_delete(panel, expand_anim_cb);
	expand_from = expand_h;
	expand_begin_ms = lv_tick_get();
}

static void expand_drag_update(int dy) { expand_set(expand_from + dy); }

// Past halfway it stays stretched, short of it it folds back; a flick goes the
// way it was flicked, as the card's own drag does.
static void expand_drag_end(void) {
	int travel = expand_h - expand_from;
	bool open = expand_h * 2 > expand_full;
	uint32_t elapsed = lv_tick_elaps(expand_begin_ms);
	int distance = travel > 0 ? travel : -travel;
	if (elapsed > 0 && distance >= FLICK_MIN_PX && (distance * 1000) / (int)elapsed >= FLICK_SPEED_PX_S) {
		open = travel > 0;
	}
	expand_to(open ? expand_full : 0, true);
}

static void expand_handle_clicked_cb(lv_event_t *e) {
	(void)e;
	if (press_dragged) {
		return;
	}
	expand_to(expand_h > 0 ? 0 : expand_full, true);
}

// Everything the card has to freshen before it becomes visible, whether it is
// being animated in or dragged in a pixel at a time.
static void panel_prepare(void) {
	long max = power_get_max_brightness();
	if (max <= 0) {
		max = 100;
	}
	long floor = max / 20 > 0 ? max / 20 : 1;
	lv_slider_set_range(brightness_slider, (int32_t)floor, (int32_t)max);
	long value = power_get_brightness();
	if (value < floor) {
		value = max;
	}
	lv_slider_set_value(brightness_slider, (int32_t)value, LV_ANIM_OFF);
	// The accent may have changed since the card was last down (a new cover).
	lv_obj_set_style_bg_color(brightness_slider, sx_pal()->accent, LV_PART_INDICATOR);

	refresh_audio_buttons();

	// The card always comes in folded.
	expand_to(0, false);

	lv_obj_set_hidden(veil, false);
	lv_obj_move_foreground(veil);

	// The player hides the status bar so artwork can run to the top edge. The
	// control centre needs it back: the clock, battery and volume belong with
	// these controls.
	if (topbar_is_hidden()) {
		raised_topbar = true;
		topbar_set_hidden(false);
	}

	// The bar is also restacked above the card, which slides in from under it.
	topbar_bring_to_front();
}

// While the device is acting as a USB sound card, a Wi-Fi file server or a
// Bluetooth receiver it is not playing anything, and those pages are meant to
// stay in front. The control centre refuses to open instead.
bool quickpanel_blocked(void) {
	// The transfer answers with the switch rather than with the server: killing
	// thttpd takes a moment and the answer about the process is cached on top of
	// that, so asking the process left the control centre refusing to open for
	// seconds after the page said the transfer was off.
	return usbdac_is_active() || wifitransfer_get_enabled() || btreceiver_is_active();
}

void quickpanel_open(void) {
	if (!panel || panel_open || quickpanel_blocked()) {
		return;
	}
	panel_open = true;
	panel_prepare();
	lv_obj_set_y(panel, parked_y());
	lv_timer_resume(poll_timer);
	panel_slide_to(open_y, true);
}

void quickpanel_close(void) {
	if (!panel || !panel_open) {
		return;
	}
	panel_open = false;
	lv_timer_pause(poll_timer);
	panel_slide_to(parked_y(), true);
}

// ---------------------------------------------------------------------------
// Following the finger: the gesture moves the card directly, and only the last
// stretch after the release is animated.
// ---------------------------------------------------------------------------

void quickpanel_drag_begin(void) {
	if (!panel || drag_active) {
		return;
	}
	lv_anim_delete(panel, anim_y_cb);
	drag_active = true;
	if (!panel_open) {
		panel_prepare();
		lv_obj_set_y(panel, parked_y());
		lv_timer_resume(poll_timer);
	}
	drag_from_y = lv_obj_get_y(panel);
	drag_begin_ms = lv_tick_get();
}

void quickpanel_drag_update(int dy) {
	if (!drag_active) {
		return;
	}
	int y = drag_from_y + dy;
	if (y > open_y) {
		y = open_y;
	}
	if (y < parked_y()) {
		y = parked_y();
	}
	lv_obj_set_y(panel, y);
}

void quickpanel_drag_end(void) {
	if (!drag_active) {
		return;
	}
	drag_active = false;

	// A fixed stretch of travel decides the outcome: down from the status bar
	// DRAG_OPEN_PX, up on the card the handoff's SX_CC_CLOSE_PX.
	int y = lv_obj_get_y(panel);
	int travel = y - drag_from_y;
	bool was_open = drag_from_y >= open_y;
	bool open = was_open ? travel > -SX_CC_CLOSE_PX : travel > DRAG_OPEN_PX;

	// Unless it was a flick, which is over long before it has covered that much.
	// Then only the direction counts.
	uint32_t elapsed = lv_tick_elaps(drag_begin_ms);
	int distance = travel > 0 ? travel : -travel;
	if (elapsed > 0 && distance >= FLICK_MIN_PX && (distance * 1000) / (int)elapsed >= FLICK_SPEED_PX_S) {
		open = travel > 0;
	}

	panel_open = open;
	if (open) {
		lv_timer_resume(poll_timer);
	} else {
		lv_timer_pause(poll_timer);
	}
	panel_slide_to(open ? open_y : parked_y(), true);
}

static void veil_clicked_cb(lv_event_t *e) {
	(void)e;
	quickpanel_close();
}

// The upward half of the gesture: a drag up anywhere on the card carries it
// back out, following the finger. With buttons past the eighth, a drag down
// stretches the card instead, and a drag up on a stretched card folds it before
// anything carries the card away.
static void panel_drag_cb(lv_event_t *e) {
	static lv_point_t start;
	static bool tracking;
	static enum { DRAG_NONE, DRAG_SHEET, DRAG_CARD } engaged;

	lv_indev_t *indev = lv_indev_active();
	if (!indev) {
		return;
	}
	lv_event_code_t code = lv_event_get_code(e);

	if (quickpanel_blocked()) {
		return;
	}

	if (code == LV_EVENT_PRESSED) {
		lv_indev_get_point(indev, &start);
		tracking = panel_open;
		engaged = DRAG_NONE;
		press_dragged = false;
		return;
	}
	if (!tracking) {
		return;
	}

	lv_point_t p;
	lv_indev_get_point(indev, &p);
	int dy = p.y - start.y;

	if (code == LV_EVENT_PRESSING) {
		if (engaged == DRAG_NONE) {
			if (dy <= -8) {
				engaged = expand_h > 0 ? DRAG_CARD : DRAG_SHEET;
			} else if (dy >= 8 && expand_full > 0 && expand_h < expand_full) {
				engaged = DRAG_CARD;
			} else {
				return; // not a drag yet, or a drag down with nothing to uncover
			}
			press_dragged = true;
			if (engaged == DRAG_CARD) {
				expand_drag_begin();
			} else {
				quickpanel_drag_begin();
			}
		}
		if (engaged == DRAG_CARD) {
			expand_drag_update(dy);
		} else {
			quickpanel_drag_update(dy);
		}
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		tracking = false;
		if (engaged == DRAG_CARD) {
			expand_drag_end();
		} else if (engaged == DRAG_SHEET) {
			quickpanel_drag_end();
		}
		engaged = DRAG_NONE;
	}
}

// ---------------------------------------------------------------------------
// brightness
// ---------------------------------------------------------------------------

static void brightness_changed_cb(lv_event_t *e) {
	(void)e;
	power_set_brightness(lv_slider_get_value(brightness_slider));
}

static void brightness_released_cb(lv_event_t *e) {
	(void)e;
	config_set_int("screen", "brightness", (int)lv_slider_get_value(brightness_slider));
	config_save();
}

// ---------------------------------------------------------------------------
// the order of the round buttons
// ---------------------------------------------------------------------------

// What an empty place in the grid is written as.
#define QP_SLOT_EMPTY_KEY "-"

// A list is written as two keys of at most eight names each -- `order` and
// `order_more`, `hidden` and `hidden_more` -- because a setting holds 127
// characters and sixteen names do not fit in them. Eight of the longest do,
// with room to spare.
#define QP_LIST_SPLIT 8

// Reads one comma-separated list of button names into `out`, keeping only names
// that are still buttons and dropping the ones already taken by an earlier
// list. A truncated or hand-mangled setting therefore costs the names it
// mangled and nothing else. "-" is kept as an empty place, which is how the
// grid's gaps survive a restart.
static uint8_t parse_list(const char *saved, uint8_t *out, uint8_t cap, bool *placed) {
	uint8_t n = 0;
	const char *p = saved ? saved : "";

	while (*p && n < cap) {
		while (*p == ',' || *p == ' ') {
			p++;
		}
		const char *start = p;
		while (*p && *p != ',') {
			p++;
		}
		size_t len = (size_t)(p - start);
		while (len > 0 && start[len - 1] == ' ') {
			len--;
		}
		if (len == strlen(QP_SLOT_EMPTY_KEY) && strncmp(QP_SLOT_EMPTY_KEY, start, len) == 0) {
			out[n++] = QP_BTN_NONE;
			continue;
		}
		for (int i = 0; i < QP_BTN_COUNT && len; i++) {
			if (placed[i] || strlen(button_key[i]) != len || strncmp(button_key[i], start, len) != 0) {
				continue;
			}
			placed[i] = true;
			out[n++] = (uint8_t)i;
			break;
		}
	}
	return n;
}

// The layout the config file is written in.
//
//   1  no grid and no list of the ones left out; every button was in the panel
//   2  a visible list and a hidden list, crossfade out and SonixLink in its place
//   3  a grid of eight places that can be empty
//   4  a grid of sixteen places, written without the empty ones at its end and
//      split over two keys each (see QP_LIST_SPLIT)
//
// A file older than the current version is brought forward on the way in: at 1
// crossfade leaves the panel and SonixLink takes the place it had, and below 4
// the list is poured into the first eight places, anything past the eighth
// going out with the rest. The other eight start empty: the panel a file of
// that age describes is the panel it keeps.
//
// Nothing is written here. The move is worked out again at every start until
// the page is used, and then saved with the rest -- a boot that changes nothing
// has no business writing to the card.
#define QP_LAYOUT_VERSION 4

static void order_load(void) {
	if (order_loaded) {
		return;
	}
	order_loaded = true;

	// A panel nobody has arranged -- nothing in the file at all, which is every
	// player until the settings page is used -- gets 2.0's: Wi-Fi, Bluetooth,
	// AirPlay, MSEB, EQ, the volume limit, the DAC and its filter in front,
	// and what 1.2.5 had in front besides (gain, line out, SonixLink) with the
	// parametric EQ behind the handle, so nothing that was a tap away goes
	// further. The rest wait among the unused ones.
	const char *saved_order = config_get("quickpanel", "order", "");
	const char *saved_hidden = config_get("quickpanel", "hidden", "");
	if ((!saved_order || !saved_order[0]) && (!saved_hidden || !saved_hidden[0])) {
		static const uint8_t FRESH[] = {
			QP_BTN_WIFI, QP_BTN_BLUETOOTH, QP_BTN_AIRPLAY, QP_BTN_MSEB, QP_BTN_EQ, QP_BTN_VOLUME_LIMIT,
			QP_BTN_DAC,	 QP_BTN_DAC_FILTER, QP_BTN_GAIN, QP_BTN_LINEOUT, QP_BTN_SONIXLINK, QP_BTN_PEQ,
		};
		bool used[QP_BTN_COUNT] = {false};
		int at = 0;
		for (size_t i = 0; i < sizeof(FRESH); i++) {
			slots[at++] = FRESH[i];
			used[FRESH[i]] = true;
		}
		while (at < QP_SLOT_COUNT) {
			slots[at++] = QP_BTN_NONE;
		}
		hidden_n = 0;
		for (int i = 0; i < QP_BTN_COUNT; i++) {
			if (!used[i]) {
				hidden[hidden_n++] = (uint8_t)i;
			}
		}
		return;
	}

	long saved_version = config_get_int("quickpanel", "layout", 1);
	int grid = saved_version < 4 ? QP_SLOT_VISIBLE : QP_SLOT_COUNT;

	bool placed[QP_BTN_COUNT] = {false};

	// Long enough for a saved grid of empty places plus every button the file
	// never mentioned, which is the worst a hand-edited file can produce.
	uint8_t line[QP_SLOT_COUNT + QP_BTN_COUNT];
	uint8_t line_n = parse_list(config_get("quickpanel", "order", ""), line, QP_LIST_SPLIT, placed);
	if (saved_version >= 4) {
		line_n += parse_list(config_get("quickpanel", "order_more", ""), line + line_n,
							 (uint8_t)(QP_SLOT_COUNT - line_n), placed);
	}
	hidden_n = parse_list(config_get("quickpanel", "hidden", ""), hidden, QP_BTN_COUNT, placed);
	hidden_n += parse_list(config_get("quickpanel", "hidden_more", ""), hidden + hidden_n,
						   (uint8_t)(QP_BTN_COUNT - hidden_n), placed);

	// Anything the file did not mention: a fresh install, or a button that did
	// not exist when it was written. It goes into the panel, because a control
	// nobody has decided about is better offered than hidden -- except the two
	// below, and the grid is already full without them.
	for (int i = 0; i < QP_BTN_COUNT; i++) {
		if (placed[i]) {
			continue;
		}
		// Crossfade, which the migration below is taking out on purpose, and the
		// buttons that arrive into a panel already holding eight: DLNA, the three
		// sleep timers, Wi-Fi transfer and gapless. Appending them would change a
		// panel nobody asked to change, so they wait among the unused ones, where
		// the settings page shows them.
		bool waits = i == QP_BTN_DLNA || i == QP_BTN_SLEEP_MUSIC || i == QP_BTN_SLEEP_AUDIOBOOK ||
					 i == QP_BTN_SLEEP_PODCAST || i == QP_BTN_WIFI_TRANSFER || i == QP_BTN_GAPLESS ||
					 i == QP_BTN_VOLUME_LIMIT || i == QP_BTN_DAC || i == QP_BTN_DAC_FILTER;
		if ((saved_version < 2 && i == QP_BTN_FADE) || waits) {
			hidden[hidden_n++] = (uint8_t)i;
		} else {
			line[line_n++] = (uint8_t)i;
		}
	}

	if (saved_version < 2) {
		// Crossfade out, SonixLink into the place it held. By name and not by
		// index, because the saved order is the user's and not the built-in one.
		for (int i = 0; i < line_n; i++) {
			if (line[i] != QP_BTN_FADE) {
				continue;
			}
			line[i] = QP_BTN_SONIXLINK;
			hidden[hidden_n++] = QP_BTN_FADE;

			// SonixLink was appended a moment ago as an unmentioned button; now
			// that it has a place of its own, that copy has to go.
			for (int j = i + 1; j < line_n; j++) {
				if (line[j] == QP_BTN_SONIXLINK) {
					memmove(&line[j], &line[j + 1], (size_t)(line_n - j - 1));
					line_n--;
					break;
				}
			}
			break;
		}
	}

	// Into the grid. From version 3 on the list already carries the empty
	// places as QP_BTN_NONE, so this is a copy; below it, it is the pour.
	int at = 0;
	for (int i = 0; i < line_n && at < grid; i++) {
		slots[at++] = line[i];
	}
	while (at < QP_SLOT_COUNT) {
		slots[at++] = QP_BTN_NONE;
	}

	// Whatever did not fit, which only the buttons appended above can be.
	for (int i = grid; i < line_n; i++) {
		if (line[i] < QP_BTN_COUNT) {
			hidden[hidden_n++] = line[i];
		}
	}
}

// Writes `count` names from `items` under `key`, "-" for an empty place.
static void write_list(const char *key, const uint8_t *items, int count) {
	char text[QP_LIST_SPLIT * 20];
	size_t used = 0;

	text[0] = '\0';
	for (int i = 0; i < count; i++) {
		const char *name = items[i] < QP_BTN_COUNT ? button_key[items[i]] : QP_SLOT_EMPTY_KEY;
		int wrote = snprintf(text + used, sizeof(text) - used, "%s%s", i ? "," : "", name);
		if (wrote <= 0 || (size_t)wrote >= sizeof(text) - used) {
			break;
		}
		used += (size_t)wrote;
	}
	config_set("quickpanel", key, text);
}

static void order_save(void) {
	// Up to the last button: the empty places after it are what a short list
	// reads back as anyway.
	int last = QP_SLOT_COUNT;
	while (last > 0 && slots[last - 1] >= QP_BTN_COUNT) {
		last--;
	}
	int head = last < QP_LIST_SPLIT ? last : QP_LIST_SPLIT;
	write_list("order", slots, head);
	write_list("order_more", slots + head, last - head);

	head = hidden_n < QP_LIST_SPLIT ? hidden_n : QP_LIST_SPLIT;
	write_list("hidden", hidden, head);
	write_list("hidden_more", hidden + head, hidden_n - head);

	config_set_int("quickpanel", "layout", QP_LAYOUT_VERSION);
	config_save();
}

static lv_obj_t *button_circle(quickpanel_button_t which) {
	switch (which) {
	case QP_BTN_WIFI:
		return wifi_btn;
	case QP_BTN_BLUETOOTH:
		return bt_btn;
	case QP_BTN_AIRPLAY:
		return airplay_btn;
	case QP_BTN_MSEB:
		return mseb_btn;
	case QP_BTN_EQ:
		return eq_btn;
	case QP_BTN_FADE:
		return fade_btn;
	case QP_BTN_GAIN:
		return gain_btn;
	case QP_BTN_LINEOUT:
		return lineout_btn;
	case QP_BTN_SONIXLINK:
		return sonixlink_btn;
	case QP_BTN_PEQ:
		return peq_btn;
	case QP_BTN_DLNA:
		return dlna_btn;
	case QP_BTN_SLEEP_MUSIC:
		return sleep_music_btn;
	case QP_BTN_SLEEP_AUDIOBOOK:
		return sleep_audiobook_btn;
	case QP_BTN_SLEEP_PODCAST:
		return sleep_podcast_btn;
	case QP_BTN_WIFI_TRANSFER:
		return wifi_transfer_btn;
	case QP_BTN_GAPLESS:
		return gapless_btn;
	case QP_BTN_VOLUME_LIMIT:
		return vlimit_btn;
	case QP_BTN_DAC:
		return dac_btn;
	case QP_BTN_DAC_FILTER:
		return filter_btn;
	default:
		return NULL;
	}
}

// What the grid places: the circle's column, with its name under it.
static lv_obj_t *button_widget(quickpanel_button_t which) {
	lv_obj_t *circle = button_circle(which);
	return circle ? lv_obj_get_parent(circle) : NULL;
}

// Puts the widgets in the grid's order: the first QP_SLOT_VISIBLE into the
// card's own rows, the rest into the block the stretch uncovers. Both are
// wrapping flexes, so a child's index is its position on screen and nothing
// has to be rebuilt; a hidden child is skipped by the flex layout, so an empty
// place in the grid is simply a button the panel does not draw, and the row
// closes up behind it.
static void order_apply(void) {
	int at = 0;
	for (int i = 0; i < QP_SLOT_COUNT; i++) {
		if (slots[i] >= QP_BTN_COUNT) {
			continue;
		}
		lv_obj_t *btn = button_widget((quickpanel_button_t)slots[i]);
		if (!btn) {
			continue;
		}
		bool first = at < QP_SLOT_VISIBLE;
		lv_obj_t *home = first ? quick_row : extra_row;
		if (lv_obj_get_parent(btn) != home) {
			lv_obj_set_parent(btn, home);
		}
		lv_obj_set_hidden(btn, false);
		lv_obj_move_to_index(btn, first ? at : at - QP_SLOT_VISIBLE);
		at++;
	}
	for (int i = 0; i < hidden_n; i++) {
		lv_obj_t *btn = button_widget((quickpanel_button_t)hidden[i]);
		if (btn) {
			lv_obj_set_hidden(btn, true);
		}
	}

	// How far the card stretches: a line and the gap above it for each line of
	// the second block, the same pitch as the lines above.
	int extra = at > QP_SLOT_VISIBLE ? at - QP_SLOT_VISIBLE : 0;
	int lines = (extra + CC_BUTTONS_PER_LINE - 1) / CC_BUTTONS_PER_LINE;
	expand_full = lines * (SX_CC_BTN + CC_LABEL_GAP + lv_font_get_line_height(&font_ui_16) + CC_GAP);

	// With a handle to show, the card is HANDLE_ROOM taller under the slider.
	if (panel) {
		card_h_base = card_h_rest + (expand_full ? HANDLE_ROOM : 0);
		lv_obj_set_hidden(expand_handle, expand_full == 0);
	}
	expand_to(0, false);
}

int quickpanel_hidden_count(void) {
	order_load();
	return hidden_n;
}

quickpanel_button_t quickpanel_hidden_at(int position) {
	order_load();
	if (position < 0 || position >= hidden_n) {
		return QP_BTN_NONE;
	}
	return (quickpanel_button_t)hidden[position];
}

// Takes `button` out of the list of the ones left out. A handful of bytes, so a
// memmove and not a data structure.
static void hidden_remove(quickpanel_button_t button) {
	for (int i = 0; i < hidden_n; i++) {
		if (hidden[i] != button) {
			continue;
		}
		memmove(&hidden[i], &hidden[i + 1], (size_t)(hidden_n - i - 1));
		hidden_n--;
		return;
	}
}

static void layout_changed(void) {
	order_save();
	order_apply();
}

int quickpanel_in_use_count(void) {
	order_load();
	int n = 0;
	for (int i = 0; i < QP_SLOT_COUNT; i++) {
		n += slots[i] < QP_BTN_COUNT;
	}
	return n;
}

quickpanel_button_t quickpanel_in_use_at(int position) {
	order_load();
	for (int i = 0; i < QP_SLOT_COUNT; i++) {
		if (slots[i] < QP_BTN_COUNT && position-- == 0) {
			return (quickpanel_button_t)slots[i];
		}
	}
	return QP_BTN_NONE;
}

// The buttons in the panel without `button`, in order and with no gaps.
static int in_use_without(quickpanel_button_t button, uint8_t *out) {
	int n = 0;
	for (int i = 0; i < QP_SLOT_COUNT; i++) {
		if (slots[i] < QP_BTN_COUNT && slots[i] != button) {
			out[n++] = slots[i];
		}
	}
	return n;
}

static void hidden_insert(quickpanel_button_t button, int position) {
	if (position < 0) {
		position = 0;
	}
	if (position > hidden_n) {
		position = hidden_n;
	}
	memmove(&hidden[position + 1], &hidden[position], (size_t)(hidden_n - position));
	hidden[position] = (uint8_t)button;
	hidden_n++;
}

void quickpanel_move_in_use(quickpanel_button_t button, int position) {
	order_load();
	if (button < 0 || button >= QP_BTN_COUNT) {
		return;
	}
	uint8_t list[QP_SLOT_COUNT + 1];
	int n = in_use_without(button, list);
	hidden_remove(button);

	if (position < 0) {
		position = 0;
	}
	if (position > n) {
		position = n;
	}
	memmove(&list[position + 1], &list[position], (size_t)(n - position));
	list[position] = (uint8_t)button;
	n++;

	// One too many: the last goes to the top of the ones left out.
	if (n > QP_SLOT_COUNT) {
		hidden_insert((quickpanel_button_t)list[--n], 0);
	}
	for (int i = 0; i < QP_SLOT_COUNT; i++) {
		slots[i] = i < n ? list[i] : QP_BTN_NONE;
	}
	layout_changed();
}

void quickpanel_move_hidden(quickpanel_button_t button, int position) {
	order_load();
	if (button < 0 || button >= QP_BTN_COUNT) {
		return;
	}
	uint8_t list[QP_SLOT_COUNT];
	int n = in_use_without(button, list);
	for (int i = 0; i < QP_SLOT_COUNT; i++) {
		slots[i] = i < n ? list[i] : QP_BTN_NONE;
	}
	hidden_remove(button);
	hidden_insert(button, position);
	layout_changed();
}

const char *quickpanel_button_tag(quickpanel_button_t button) {
	if (button < 0 || button >= QP_BTN_COUNT) {
		return "";
	}
	return button_tag[button];
}

const lv_image_dsc_t *quickpanel_button_icon(quickpanel_button_t button) {
	switch (button) {
	case QP_BTN_WIFI:
		return &icon_wifi;
	case QP_BTN_BLUETOOTH:
		return &icon_bluetooth;
	case QP_BTN_AIRPLAY:
		return &icon_airplay_quick;
	case QP_BTN_MSEB:
		return &icon_mseb;
	case QP_BTN_EQ:
		return &icon_equalizer;
	case QP_BTN_FADE:
		return &icon_fade_track;
	case QP_BTN_GAIN:
		return &icon_low_gain;
	case QP_BTN_LINEOUT:
		return &icon_lineout;
	case QP_BTN_SONIXLINK:
		return &icon_sonixlink_quick;
	case QP_BTN_PEQ:
		return &icon_peq_quick;
	case QP_BTN_DLNA:
		return &icon_dlna_quick;
	case QP_BTN_SLEEP_MUSIC:
		return &icon_sleep_music_quick;
	case QP_BTN_SLEEP_AUDIOBOOK:
		return &icon_sleep_audiobook_quick;
	case QP_BTN_SLEEP_PODCAST:
		return &icon_sleep_podcast_quick;
	case QP_BTN_WIFI_TRANSFER:
		return &icon_wifi_transfer_quick;
	case QP_BTN_GAPLESS:
		return &icon_gapless_on_quick;
	case QP_BTN_VOLUME_LIMIT:
		return &icon_volume_limit_quick;
	case QP_BTN_DAC:
		return &icon_dac_quick;
	case QP_BTN_DAC_FILTER:
		return &icon_dac_filter_quick;
	default:
		return &icon_wifi;
	}
}


// ---------------------------------------------------------------------------
// construction
// ---------------------------------------------------------------------------

// First in line for a circle's tap and hold: a press that has become a drag of
// the card ends there, and the circle's own handlers never hear of it. The
// finger stays on a circle while the card carries both, and a circle keeps its
// press while the finger leaves it, so either would otherwise end the drag by
// toggling or by opening a page.
static void circle_guard_cb(lv_event_t *e) {
	if (press_dragged) {
		lv_event_stop_processing(e);
	}
}

// The width of one place in the grid: a quarter of the card inside its padding.
static int cell_w;

// One round quick control and its name under it, in a column the grid places:
// a circle SX_CC_BTN across in SX_CC_BTN_OFF with the glyph white on it.
// Returns the circle, which callers attach their handlers to; the column is
// its parent. Presses bubble on to the card, so a drag that starts on a circle
// moves the card like a drag anywhere else.
static lv_obj_t *make_circle_button(lv_obj_t *parent, const lv_image_dsc_t *glyph, quickpanel_button_t which) {
	lv_obj_t *cell = lv_obj_create(parent);
	lv_obj_remove_style_all(cell);
	lv_obj_set_size(cell, cell_w, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(cell, CC_LABEL_GAP, 0);
	lv_obj_set_scrollable(cell, false);
	lv_obj_set_event_bubble(cell, true);

	lv_obj_t *btn = lv_btn_create(cell);
	lv_obj_remove_style_all(btn);
	lv_obj_set_event_bubble(btn, true);
	lv_obj_add_event_cb(btn, circle_guard_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(btn, circle_guard_cb, LV_EVENT_LONG_PRESSED, NULL);
	lv_obj_set_size(btn, SX_CC_BTN, SX_CC_BTN);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(btn, lv_color_hex(SX_CC_BTN_OFF), 0);
	// The pressed fill at once (principle 2): a step lighter.
	lv_obj_set_style_bg_color(btn, lv_color_hex(0x5A5A5A), LV_STATE_PRESSED);
	// The name under it answers a tap too.
	lv_obj_set_ext_click_area(btn, CC_LABEL_GAP + 10);

	lv_obj_t *icon = lv_image_create(btn);
	lv_image_set_src(icon, glyph);
	lv_obj_set_style_image_recolor(icon, lv_color_hex(CC_TEXT), 0);
	lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
	// The 46 px glyphs drawn at 40, the prototype's weight in an 84 circle.
	lv_image_set_scale(icon, 256 * 40 / 46);
	lv_obj_center(icon);

	lv_obj_t *label = lv_label_create(cell);
	lv_obj_set_width(label, cell_w);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(label, &font_ui_16, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(CC_TEXT), 0);
	lv_obj_set_style_text_opa(label, CC_LABEL_OPA, 0);
	lv_label_set_text(label, tr(button_label[which]));

	return btn;
}

// Paints a quick button for its state: the accent with its own ink while on,
// the neutral circle with a white glyph while off.
static void circle_button_set_on(lv_obj_t *btn, bool on) {
	if (!btn) {
		return;
	}
	lv_obj_t *icon = lv_obj_get_child(btn, 0);
	const sx_palette_t *p = sx_pal();
	lv_obj_set_style_bg_color(btn, on ? p->accent : lv_color_hex(SX_CC_BTN_OFF), 0);
	if (icon) {
		lv_obj_set_style_image_recolor(icon, on ? p->on_accent : lv_color_hex(CC_TEXT), 0);
	}
}

// Whether that timer will actually stop the music: the switch on AND a length
// chosen. See the note over the handlers.
static bool sleep_timer_armed(sleeptimer_kind_t kind);

// Repaints every round control from the state it switches, so the panel shows
// the truth even when the setting was changed somewhere else.
static void refresh_audio_buttons(void) {
	circle_button_set_on(eq_btn, eq_get_enabled());
	circle_button_set_on(mseb_btn, mseb_get_enabled());
	circle_button_set_on(wifi_btn, wifi_get_enabled());
	circle_button_set_on(bt_btn, bluetooth_get_enabled());
	circle_button_set_on(fade_btn, musicsettings_fade_enabled());
	circle_button_set_on(airplay_btn, airplay_get_enabled());
	circle_button_set_on(sonixlink_btn, sonixlink_get_enabled());
	circle_button_set_on(dlna_btn, dlna_get_enabled());
	circle_button_set_on(peq_btn, peq_get_enabled());
	circle_button_set_on(sleep_music_btn, sleep_timer_armed(SLEEPTIMER_MUSIC));
	circle_button_set_on(sleep_audiobook_btn, sleep_timer_armed(SLEEPTIMER_AUDIOBOOK));
	circle_button_set_on(sleep_podcast_btn, sleep_timer_armed(SLEEPTIMER_PODCAST));
	circle_button_set_on(wifi_transfer_btn, wifitransfer_get_enabled());
	circle_button_set_on(vlimit_btn, volume_limit_enabled());
	// The DAC and the filter are ways to a page and a choice, never "on".
	circle_button_set_on(dac_btn, false);
	circle_button_set_on(filter_btn, false);

	bool high = musicsettings_high_gain();
	if (gain_btn) {
		lv_obj_t *icon = lv_obj_get_child(gain_btn, 0);
		if (icon) {
			lv_image_set_src(icon, high ? &icon_high_gain : &icon_low_gain);
		}
	}
	circle_button_set_on(gain_btn, high);
	circle_button_set_on(lineout_btn, lineout_is_active());

	// The drawing changes with the state as well as the fill: the two halves
	// closed up while on, apart while off.
	bool gapless = musicsettings_gapless_enabled();
	if (gapless_btn) {
		lv_obj_t *icon = lv_obj_get_child(gapless_btn, 0);
		if (icon) {
			lv_image_set_src(icon, gapless ? &icon_gapless_on_quick : &icon_gapless_off_quick);
		}
	}
	circle_button_set_on(gapless_btn, gapless);
}

// After a button has switched something: the circles repaint, and the page
// under the sheet is told, since it may be the very page that setting lives on
// and must not go on showing the old state. Pages that carry one of these
// settings answer LV_EVENT_REFRESH by reading it again; the rest ignore it.
static void toggled(void) {
	refresh_audio_buttons();
	lv_obj_t *page = lv_screen_active();
	if (page) {
		lv_obj_send_event(page, LV_EVENT_REFRESH, NULL);
	}
}

// Line out: the jack driven for an amplifier, at the fixed level the stock
// player uses. Switching it on is worth a word first -- the output goes to full
// scale, and a pair of headphones still in the socket would say so loudly --
// while switching it off needs none, since it only ever lowers the level back
// to the user's own.
// The level moves the instant the mode is switched, so the number in the status
// bar has to move with it. Left to the bar's own poll, the figure would lag the
// sound by up to a second.
static void lineout_applied(void) {
	refresh_audio_buttons();
	topbar_refresh_volume(get_volume_percent());
}

static void lineout_confirmed(void *user) {
	(void)user;
	lineout_set(true);
	lineout_applied();
}

static void lineout_clicked_cb(lv_event_t *e) {
	(void)e;
	if (long_press_consumed) {
		long_press_consumed = false;
		return;
	}

	if (lineout_is_active()) {
		lineout_set(false);
		lineout_applied();
		return;
	}

	// Nothing to drive: an empty socket, or sound going out over the air, where
	// the fixed level would reach the headphones as an absolute volume.
	if (bluetooth_audio_active() || headphone_jack_state() == JACK_NONE) {
		gui_notify_popup("quickpanel_line_out_needs_the_jack");
		return;
	}

	quickpanel_close(); // the dialog belongs to the page, not under the sheet
	confirm_show("quickpanel_line_out", tr("quickpanel_the_volume_will_go_to_100"), "quickpanel_activate", lineout_confirmed, NULL);
}

static void wifi_clicked_cb(lv_event_t *e) {
	(void)e;
	if (long_press_consumed) {
		long_press_consumed = false;
		return;
	}
	if (!wifi_available()) {
		return; // no radio to switch; the settings page explains why
	}
	wifi_set_enabled(!wifi_get_enabled());
	toggled();
	topbar_refresh_radios();
}

static void bt_clicked_cb(lv_event_t *e) {
	(void)e;
	if (long_press_consumed) {
		long_press_consumed = false;
		return;
	}
	if (!bluetooth_available()) {
		return;
	}
	bluetooth_set_enabled(!bluetooth_get_enabled());
	toggled();
	topbar_refresh_radios();
}

// Whether there is a network to be reachable on. The same question the pages of
// these three services ask, so the circle and the page never disagree.
static bool wifi_is_connected(void) {
	wifi_status_t status;
	wifi_get_status(&status);
	return status.state == WIFI_STATE_CONNECTED && status.ip[0] != '\0';
}

// Switching one of the network services on with no network. Says so and
// refuses, rather than lighting a circle for something nothing can reach.
//
// Switching OFF is never gated: if it is on it goes off, whatever has happened
// to the network meanwhile.
static bool network_needed(const char *why) {
	if (wifi_is_connected()) {
		return false;
	}
	gui_notify_popup(why);
	return true;
}

// The AirPlay circle switches the receiver on: from then on the player is
// merely discoverable by phones. It does not stop playback, prompt, or open
// anything. Local music stops only when a phone actually starts sending audio,
// and it is the receiving thread that stops it (play_worker in
// system/airplay.c), not this tap. Reachable is not the same as in use, and
// this matches the Wi-Fi and Bluetooth circles beside it, which also switch on
// without switching anything off.
static void airplay_clicked_cb(lv_event_t *e) {
	(void)e;
	if (long_press_consumed) {
		long_press_consumed = false;
		return;
	}

	bool want = !airplay_get_enabled();

	if (want) {
		if (!airplay_available()) {
			gui_notify_popup("airplay_unavailable");
			return;
		}
		if (network_needed("quickpanel_airplay_only_works_over_wi_fi")) {
			return;
		}
	}

	airplay_set_enabled(want);
	toggled();
}

static void mseb_clicked_cb(lv_event_t *e) {
	(void)e;
	if (long_press_consumed) {
		long_press_consumed = false;
		return; // the long press that opened the page must not also toggle
	}
	musicsettings_set_mseb_enabled(!mseb_get_enabled());
	toggled();
}

static void eq_clicked_cb(lv_event_t *e) {
	(void)e;
	if (long_press_consumed) {
		long_press_consumed = false;
		return;
	}
	musicsettings_set_eq_enabled(!eq_get_enabled());
	toggled();
}

static void fade_clicked_cb(lv_event_t *e) {
	(void)e;
	if (long_press_consumed) {
		long_press_consumed = false;
		return;
	}
	musicsettings_set_fade_enabled(!musicsettings_fade_enabled());
	toggled();
}

// SonixLink, same contract as the radios: a tap switches the server on or off,
// a hold opens its page, which is where the address and the port are. And the
// same gate as AirPlay beside it -- a server with no network to listen on is a
// lit circle that means nothing.
static void sonixlink_clicked_cb(lv_event_t *e) {
	(void)e;
	if (long_press_consumed) {
		long_press_consumed = false;
		return;
	}

	bool want = !sonixlink_get_enabled();
	// Bluetooth carries it too, for a paired phone: Wi-Fi is needed only when
	// Bluetooth is off.
	if (want && !bluetooth_get_enabled() && network_needed("sonixlink_needs_wifi_or_bluetooth")) {
		return;
	}

	sonixlink_set_enabled(want);
	toggled();
}

// ---------------------------------------------------------------------------
// The sleep timers
//
// Three switches for three timers, and what they are for is the switching OFF:
// "it is later than I thought, do not stop" is a thing somebody thinks with the
// player already in their hand, without wanting to walk into the settings page
// of whichever of the three is counting.
//
// Lit means it will actually stop the music, which is the switch AND a length:
// the switch on with the wheels at 00:00 arms nothing (see sleeptimer.h), and a
// button lit for that would be a button that lies. Pressing one with no length
// chosen says so rather than lighting up and doing nothing.
// ---------------------------------------------------------------------------

static bool sleep_timer_armed(sleeptimer_kind_t kind) {
	return sleeptimer_enabled(kind) && sleeptimer_minutes(kind) > 0;
}

static void sleep_timer_clicked(sleeptimer_kind_t kind) {
	if (long_press_consumed) {
		long_press_consumed = false;
		return;
	}

	if (sleeptimer_enabled(kind)) {
		sleeptimer_set_enabled(kind, false);
	} else if (sleeptimer_minutes(kind) > 0) {
		sleeptimer_set_enabled(kind, true);
	} else {
		gui_notify_popup("quickpanel_sleep_no_length");
		return;
	}
	toggled();
}

static void sleep_music_clicked_cb(lv_event_t *e) {
	(void)e;
	sleep_timer_clicked(SLEEPTIMER_MUSIC);
}

static void sleep_audiobook_clicked_cb(lv_event_t *e) {
	(void)e;
	sleep_timer_clicked(SLEEPTIMER_AUDIOBOOK);
}

static void sleep_podcast_clicked_cb(lv_event_t *e) {
	(void)e;
	sleep_timer_clicked(SLEEPTIMER_PODCAST);
}

static void open_page(lv_obj_t *screen);

// A hold opens the page that sets the length: the playback options for music,
// the audiobook options, the podcast settings. Looked up at the press rather
// than when the buttons are made, since not every one of those pages exists
// by then.
static void sleep_page_cb(lv_event_t *e) {
	sleeptimer_kind_t kind = (sleeptimer_kind_t)(intptr_t)lv_event_get_user_data(e);
	lv_obj_t *screen = kind == SLEEPTIMER_MUSIC		 ? musicsettings_playback_screen()
					   : kind == SLEEPTIMER_AUDIOBOOK ? audiobooksettings_screen
													  : podcastpage_settings_screen();
	open_page(screen);
}

// DLNA, the third of the same family: a phone pushes music at the player over
// the network, so without one there is nothing to push it from.
static void dlna_clicked_cb(lv_event_t *e) {
	(void)e;
	if (long_press_consumed) {
		long_press_consumed = false;
		return;
	}

	bool want = !dlna_get_enabled();

	if (want) {
		if (!dlna_available()) {
			gui_notify_popup("dlna_unavailable");
			return;
		}
		if (network_needed("quickpanel_dlna_only_works_over_wi_fi")) {
			return;
		}
	}

	dlna_set_enabled(want);
	toggled();
}

// Wi-Fi transfer. The server lives only as long as its page is open (see
// gui/wireless/wifitransfer.h), so switching it on opens the page with the
// server starting; switching it off stops it where it is. Same network gate as
// the three above.
static void wifi_transfer_clicked_cb(lv_event_t *e) {
	(void)e;
	if (long_press_consumed) {
		long_press_consumed = false;
		return;
	}

	if (wifitransfer_get_enabled()) {
		wifitransfer_page_stop();
		toggled();
		return;
	}
	if (!wifitransfer_available()) {
		gui_notify_popup("wifitransfer_unavailable");
		return;
	}
	if (network_needed("quickpanel_wifitransfer_only_works_over_wi_fi")) {
		return;
	}

	quickpanel_close();
	bool from_player = player_sheet_is_open();
	if (from_player) {
		player_sheet_close(false);
	}
	wifitransfer_page_open_started();
	if (from_player) {
		switcher_set_player_return(wifitransfer_screen);
	}
}

// The parametric equaliser, same contract as the graphic one beside it: a tap
// switches it, a hold opens its page.
static void peq_clicked_cb(lv_event_t *e) {
	(void)e;
	if (long_press_consumed) {
		long_press_consumed = false;
		return;
	}
	peq_set_enabled(!peq_get_enabled());
	toggled();
}

// Gapless, the switch on the playback options page: a tap flips it, a hold
// opens that page. Looked up at the press, like the sleep timer's page.
static void gapless_clicked_cb(lv_event_t *e) {
	(void)e;
	if (long_press_consumed) {
		long_press_consumed = false;
		return;
	}
	musicsettings_set_gapless_enabled(!musicsettings_gapless_enabled());
	toggled();
}

static void gapless_page_cb(lv_event_t *e) {
	(void)e;
	open_page(musicsettings_playback_screen());
}

static void gain_clicked_cb(lv_event_t *e) {
	(void)e;
	if (long_press_consumed) {
		long_press_consumed = false;
		return;
	}
	musicsettings_set_high_gain(!musicsettings_high_gain());
	toggled();
}

static void open_page(lv_obj_t *screen) {
	long_press_consumed = true;
	quickpanel_close();
	if (!screen) {
		return;
	}

	// The player is a sheet on the top layer, not a page: with it up, loading a
	// screen leaves the page behind the sheet, and closing the sheet puts its
	// own opener back, taking the page away again. So the sheet goes down
	// first, and leaving the page comes back into it -- the same route the
	// player's own menu takes to the queue and the track details.
	bool from_player = player_sheet_is_open();
	if (from_player) {
		player_sheet_close(false);
	}
	if (screen == lv_screen_active()) {
		return; // already there: switching would record a step nobody took
	}
	switch_screen(screen);
	if (from_player) {
		switcher_set_player_return(screen);
	}
}

static void open_page_cb(lv_event_t *e) { open_page(lv_event_get_user_data(e)); }

// The volume limit: a tap switches the ceilings on or off, a hold opens their
// page. The level may come down under a ceiling at once, and the status bar's
// figure is the one place that says so.
static void vlimit_clicked_cb(lv_event_t *e) {
	(void)e;
	if (long_press_consumed) {
		long_press_consumed = false;
		return;
	}
	volume_limit_set_enabled(!volume_limit_enabled());
	config_save();
	topbar_refresh_volume(get_volume_percent());
	toggled();
}

static void vlimit_page_cb(lv_event_t *e) {
	(void)e;
	open_page(musicsettings_volume_limit_screen());
}

// USB DAC mode is a page that takes the whole player over (and shuts the
// control centre while it runs), so the circle is the way to it rather than a
// switch: a tap or a hold opens it.
static void dac_clicked_cb(lv_event_t *e) {
	(void)e;
	if (long_press_consumed) {
		long_press_consumed = false;
		return;
	}
	open_page(dacpage_screen);
}

// The DAC's filter is one of four, not on or off: a tap offers the four in a
// sheet, the one in force ticked; a hold opens their page, which explains them.
static void filter_picked(void *user) { musicsettings_set_dac_filter((int)(intptr_t)user); }

static void filter_clicked_cb(lv_event_t *e) {
	(void)e;
	if (long_press_consumed) {
		long_press_consumed = false;
		return;
	}
	quickpanel_close(); // the sheet belongs to the page, not under the card
	int current = musicsettings_dac_filter();
	sx_sheet_item_t items[4];
	for (int i = 0; i < 4; i++) {
		items[i] = (sx_sheet_item_t){tr(musicsettings_dac_filter_name(i)), NULL, i == current, false, filter_picked,
									 (void *)(intptr_t)i};
	}
	sx_sheet_menu(tr("musicsettings_filters"), NULL, items, 4);
}

static void filter_page_cb(lv_event_t *e) {
	(void)e;
	open_page(musicsettings_filter_screen());
}

void quickpanel_init(gui_config_t *cfg) {
	int card_w = cfg->screen_width - 2 * CC_MARGIN;
	cell_w = (card_w - 2 * CC_PAD) / CC_BUTTONS_PER_LINE;
	open_y = SX_STATUSBAR_H + CC_TOP_GAP;

	veil = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(veil);
	lv_obj_set_size(veil, lv_pct(100), lv_pct(100));
	lv_obj_set_pos(veil, 0, 0); // see anim_y_cb: it is a tap target, not a dimmer
	lv_obj_set_scrollable(veil, false);
	lv_obj_set_clickable(veil, true);
	lv_obj_set_hidden(veil, true);
	lv_obj_add_event_cb(veil, veil_clicked_cb, LV_EVENT_CLICKED, NULL);

	// One line of buttons with their names, and the card around two of them,
	// the slider and the padding.
	int line_h = SX_CC_BTN + CC_LABEL_GAP + lv_font_get_line_height(&font_ui_16);
	int lines = QP_SLOT_VISIBLE / CC_BUTTONS_PER_LINE;
	int grid_h = lines * line_h + (lines - 1) * CC_GAP;
	card_h_rest = 2 * CC_PAD + grid_h + BRIGHT_ROW_GAP + BRIGHT_ROW_H;
	card_h_base = card_h_rest;

	panel = lv_obj_create(veil);
	lv_obj_remove_style_all(panel);
	lv_obj_set_size(panel, card_w, card_h_rest);
	lv_obj_set_pos(panel, CC_MARGIN, -card_h_rest - 4);
	lv_obj_set_style_bg_color(panel, lv_color_hex(SX_CC_BG), 0);
	lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(panel, CC_RADIUS, 0);
	lv_obj_set_style_clip_corner(panel, false, 0);
	lv_obj_set_style_pad_all(panel, CC_PAD, 0);
	lv_obj_set_style_pad_row(panel, BRIGHT_ROW_GAP, 0);
	lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scrollable(panel, false);
	lv_obj_set_event_bubble(panel, false);
	lv_obj_set_clickable(panel, true);
	lv_obj_add_event_cb(panel, panel_drag_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(panel, panel_drag_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(panel, panel_drag_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(panel, panel_drag_cb, LV_EVENT_PRESS_LOST, NULL);

	quick_row = lv_obj_create(panel);
	lv_obj_remove_style_all(quick_row);
	lv_obj_set_size(quick_row, lv_pct(100), grid_h);
	lv_obj_set_style_pad_row(quick_row, CC_GAP, 0);
	lv_obj_set_scrollable(quick_row, false);
	lv_obj_set_event_bubble(quick_row, true);
	lv_obj_set_flex_flow(quick_row, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(quick_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_obj_t *row = quick_row;

	// The radios: a tap switches them, a hold opens their page, the same contract
	// MSEB and the equaliser use.
	wifi_btn = make_circle_button(row, &icon_wifi, QP_BTN_WIFI);
	lv_obj_add_event_cb(wifi_btn, wifi_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(wifi_btn, open_page_cb, LV_EVENT_LONG_PRESSED, wifisettings_screen);

	bt_btn = make_circle_button(row, &icon_bluetooth, QP_BTN_BLUETOOTH);
	lv_obj_add_event_cb(bt_btn, bt_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(bt_btn, open_page_cb, LV_EVENT_LONG_PRESSED, btsettings_screen);

	// AirPlay sits beside the other two radios because it is a switch to reach
	// while doing something else, not a page to enter. Same contract as Wi-Fi and
	// Bluetooth: tap switches it, long press opens the page, which remains where
	// the incoming stream is shown.
	airplay_btn = make_circle_button(row, &icon_airplay_quick, QP_BTN_AIRPLAY);
	lv_obj_add_event_cb(airplay_btn, airplay_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(airplay_btn, open_page_cb, LV_EVENT_LONG_PRESSED, airplay_screen);

	// MSEB and the equaliser: tap to switch on or off, hold to open the page.
	mseb_btn = make_circle_button(row, &icon_mseb, QP_BTN_MSEB);
	lv_obj_add_event_cb(mseb_btn, mseb_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(mseb_btn, open_page_cb, LV_EVENT_LONG_PRESSED, musicsettings_mseb_screen());

	eq_btn = make_circle_button(row, &icon_equalizer, QP_BTN_EQ);
	lv_obj_add_event_cb(eq_btn, eq_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(eq_btn, open_page_cb, LV_EVENT_LONG_PRESSED, musicsettings_eq_screen());

	// Crossfade, same contract: tap switches it, hold opens its page.
	fade_btn = make_circle_button(row, &icon_fade_track, QP_BTN_FADE);
	lv_obj_add_event_cb(fade_btn, fade_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(fade_btn, open_page_cb, LV_EVENT_LONG_PRESSED, musicsettings_fade_screen());

	// Gain is the odd control: two states rather than on and off, so the glyph
	// itself changes and the accent marks only the loud one. Low gain is not
	// "off", it is the normal setting. No long press: gain is a plain switch on
	// the music settings page rather than a section of its own.
	gain_btn = make_circle_button(row, &icon_low_gain, QP_BTN_GAIN);
	lv_obj_add_event_cb(gain_btn, gain_clicked_cb, LV_EVENT_CLICKED, NULL);

	// Line out: a tap switches the jack between headphones and an amplifier. No
	// long press either -- there is no page behind it.
	lineout_btn = make_circle_button(row, &icon_lineout, QP_BTN_LINEOUT);
	lv_obj_add_event_cb(lineout_btn, lineout_clicked_cb, LV_EVENT_CLICKED, NULL);

	sonixlink_btn = make_circle_button(row, &icon_sonixlink_quick, QP_BTN_SONIXLINK);
	lv_obj_add_event_cb(sonixlink_btn, sonixlink_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(sonixlink_btn, open_page_cb, LV_EVENT_LONG_PRESSED, sonixlink_screen);

	peq_btn = make_circle_button(row, &icon_peq_quick, QP_BTN_PEQ);
	lv_obj_add_event_cb(peq_btn, peq_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(peq_btn, open_page_cb, LV_EVENT_LONG_PRESSED, peqpage_screen());

	dlna_btn = make_circle_button(row, &icon_dlna_quick, QP_BTN_DLNA);
	lv_obj_add_event_cb(dlna_btn, dlna_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(dlna_btn, open_page_cb, LV_EVENT_LONG_PRESSED, dlna_screen);

	// Each of the three opens, on a long press, the page of its own kind of
	// listening, where its length is chosen.
	sleep_music_btn = make_circle_button(row, &icon_sleep_music_quick, QP_BTN_SLEEP_MUSIC);
	lv_obj_add_event_cb(sleep_music_btn, sleep_music_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(sleep_music_btn, sleep_page_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)SLEEPTIMER_MUSIC);

	sleep_audiobook_btn = make_circle_button(row, &icon_sleep_audiobook_quick, QP_BTN_SLEEP_AUDIOBOOK);
	lv_obj_add_event_cb(sleep_audiobook_btn, sleep_audiobook_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(sleep_audiobook_btn, sleep_page_cb, LV_EVENT_LONG_PRESSED,
						(void *)(intptr_t)SLEEPTIMER_AUDIOBOOK);

	sleep_podcast_btn = make_circle_button(row, &icon_sleep_podcast_quick, QP_BTN_SLEEP_PODCAST);
	lv_obj_add_event_cb(sleep_podcast_btn, sleep_podcast_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(sleep_podcast_btn, sleep_page_cb, LV_EVENT_LONG_PRESSED,
						(void *)(intptr_t)SLEEPTIMER_PODCAST);

	// A tap opens the page with the server starting; a hold opens it as it is.
	wifi_transfer_btn = make_circle_button(row, &icon_wifi_transfer_quick, QP_BTN_WIFI_TRANSFER);
	lv_obj_add_event_cb(wifi_transfer_btn, wifi_transfer_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(wifi_transfer_btn, open_page_cb, LV_EVENT_LONG_PRESSED, wifitransfer_screen);

	gapless_btn = make_circle_button(row, &icon_gapless_off_quick, QP_BTN_GAPLESS);
	lv_obj_add_event_cb(gapless_btn, gapless_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(gapless_btn, gapless_page_cb, LV_EVENT_LONG_PRESSED, NULL);

	vlimit_btn = make_circle_button(row, &icon_volume_limit_quick, QP_BTN_VOLUME_LIMIT);
	lv_obj_add_event_cb(vlimit_btn, vlimit_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(vlimit_btn, vlimit_page_cb, LV_EVENT_LONG_PRESSED, NULL);

	dac_btn = make_circle_button(row, &icon_dac_quick, QP_BTN_DAC);
	lv_obj_add_event_cb(dac_btn, dac_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(dac_btn, open_page_cb, LV_EVENT_LONG_PRESSED, dacpage_screen);

	filter_btn = make_circle_button(row, &icon_dac_filter_quick, QP_BTN_DAC_FILTER);
	lv_obj_add_event_cb(filter_btn, filter_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(filter_btn, filter_page_cb, LV_EVENT_LONG_PRESSED, NULL);

	// Brightness: the sun, then a 10 px track (white at 20 %) filled with the
	// accent, in a row tall enough to be a target. No knob, as on the
	// prototype: the fill's end is where the finger is.
	bright_row = lv_obj_create(panel);
	lv_obj_remove_style_all(bright_row);
	lv_obj_set_size(bright_row, lv_pct(100), BRIGHT_ROW_H);
	lv_obj_set_style_pad_column(bright_row, 16, 0);
	lv_obj_set_scrollable(bright_row, false);
	lv_obj_set_event_bubble(bright_row, true);
	lv_obj_set_flex_flow(bright_row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(bright_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *sun = lv_image_create(bright_row);
	lv_image_set_src(sun, &icon_sun);
	lv_obj_set_style_image_recolor(sun, lv_color_hex(CC_TEXT), 0);
	lv_obj_set_style_image_recolor_opa(sun, LV_OPA_COVER, 0);

	brightness_slider = lv_slider_create(bright_row);
	lv_obj_set_height(brightness_slider, 10);
	lv_obj_set_flex_grow(brightness_slider, 1);
	lv_slider_set_range(brightness_slider, 5, 100);
	lv_obj_set_style_radius(brightness_slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
	lv_obj_set_style_bg_color(brightness_slider, lv_color_hex(CC_TEXT), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(brightness_slider, CC_TRACK_OPA, LV_PART_MAIN);
	lv_obj_set_style_radius(brightness_slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(brightness_slider, sx_pal()->accent, LV_PART_INDICATOR);
	lv_obj_set_style_bg_opa(brightness_slider, LV_OPA_TRANSP, LV_PART_KNOB);
	lv_obj_set_style_shadow_width(brightness_slider, 0, LV_PART_KNOB);
	lv_obj_set_style_outline_width(brightness_slider, 0, LV_PART_KNOB);
	lv_obj_set_style_border_width(brightness_slider, 0, LV_PART_KNOB);
	lv_obj_set_style_pad_all(brightness_slider, 0, LV_PART_KNOB);
	// The whole row high, so a finger anywhere on the line takes it.
	lv_obj_set_ext_click_area(brightness_slider, (BRIGHT_ROW_H - 10) / 2);
	lv_obj_add_event_cb(brightness_slider, brightness_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(brightness_slider, brightness_released_cb, LV_EVENT_RELEASED, NULL);

	// The buttons past the eighth. A window outside the flex, starting where the
	// first two lines end, opens as tall as the stretch while the slider moves
	// down by as much; the buttons sit in it one gap down, so they read as
	// further lines of the grid above. The window clips them, so the stretch is
	// what uncovers them.
	extra_clip = lv_obj_create(panel);
	lv_obj_remove_style_all(extra_clip);
	lv_obj_set_ignore_layout(extra_clip, true);
	lv_obj_set_size(extra_clip, lv_pct(100), 0);
	lv_obj_set_pos(extra_clip, 0, grid_h);
	lv_obj_set_scrollable(extra_clip, false);
	lv_obj_set_event_bubble(extra_clip, true);
	lv_obj_set_hidden(extra_clip, true);

	extra_row = lv_obj_create(extra_clip);
	lv_obj_remove_style_all(extra_row);
	lv_obj_set_size(extra_row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_pos(extra_row, 0, CC_GAP);
	lv_obj_set_style_pad_row(extra_row, CC_GAP, 0);
	lv_obj_set_scrollable(extra_row, false);
	lv_obj_set_event_bubble(extra_row, true);
	lv_obj_set_flex_flow(extra_row, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(extra_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	// The handle: a short line in the band under the slider -- the room the
	// card gains for it and the card's bottom padding -- which travels down
	// with the edge as the card stretches. The line is what is seen; the strip
	// round it is what a tap finds, and a tap folds or stretches the card.
	expand_handle = lv_obj_create(panel);
	lv_obj_remove_style_all(expand_handle);
	lv_obj_set_ignore_layout(expand_handle, true);
	lv_obj_set_size(expand_handle, 160, HANDLE_ROOM + CC_PAD);
	lv_obj_align(expand_handle, LV_ALIGN_BOTTOM_MID, 0, CC_PAD);
	lv_obj_set_scrollable(expand_handle, false);
	lv_obj_set_event_bubble(expand_handle, true);
	lv_obj_set_clickable(expand_handle, true);
	lv_obj_add_event_cb(expand_handle, expand_handle_clicked_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *pill = lv_obj_create(expand_handle);
	lv_obj_remove_style_all(pill);
	lv_obj_set_size(pill, SX_SHEET_HANDLE_W, SX_SHEET_HANDLE_H);
	lv_obj_center(pill);
	lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(pill, lv_color_hex(CC_TEXT), 0);
	lv_obj_set_style_bg_opa(pill, LV_OPA_40, 0);
	lv_obj_set_clickable(pill, false);

	// Built in the order above, then arranged into the user's -- see
	// Settings > More > Control centre. Last, once both rows and the handle
	// exist to arrange.
	order_load();
	order_apply();

	poll_timer = lv_timer_create(poll_cb, PANEL_POLL_MS, NULL);
	lv_timer_pause(poll_timer);
}

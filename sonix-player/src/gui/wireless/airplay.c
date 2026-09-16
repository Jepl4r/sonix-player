#include "airplay.h"

#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/scrolltext.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/system/remote/airplay.h"
#include "src/system/core/lang.h"
#include "src/system/net/wifi.h"

lv_obj_t *airplay_screen;

#define AP_POLL_MS 400

// The waiting text is HiBy's own, taken from /usr/resource/str/italy/wifi.ini:
// their wording for this exact screen beats inventing a new one.
#define AP_WAIT "airplay_waiting"
#define AP_CONNECTED "connected"
#define AP_UNAVAILABLE "airplay_unavailable"
#define AP_OFF_HELP "airplay_off_note"
#define AP_STARTING "turning_on"

static lv_obj_t *toggle;
static lv_obj_t *glyph;
static lv_obj_t *title_label;
static lv_obj_t *artist_label;
static lv_obj_t *album_label;
static lv_obj_t *status_label;
static lv_obj_t *wifi_row;
static lv_timer_t *poll_timer;

// ---------------------------------------------------------------------------

static void hide(lv_obj_t *obj) { lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN); }
static void show(lv_obj_t *obj) { lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN); }

static bool switch_is_on(void) { return lv_obj_has_state(toggle, LV_STATE_CHECKED); }

static void set_receiver(bool on);

static bool wifi_is_connected(void) {
	wifi_status_t status;
	wifi_get_status(&status);
	return status.state == WIFI_STATE_CONNECTED && status.ip[0] != '\0';
}

// A metadata line: shown with its text, or taken out of the layout entirely.
static void set_line(lv_obj_t *label, const char *text) {
	if (text[0] == '\0') {
		hide(label);
		return;
	}
	scrolltext_set(label, text);
	show(label);
}

// Everything under the switch belongs to a running receiver; with the switch
// off there is nothing for it to say.
static void hide_receiver(void) {
	hide(glyph);
	hide(title_label);
	hide(artist_label);
	hide(album_label);
}

// ---------------------------------------------------------------------------

static void refresh(void) {
	if (!airplay_available()) {
		lv_label_set_text(status_label, tr(AP_UNAVAILABLE));
		lv_obj_add_state(toggle, LV_STATE_DISABLED);
		hide_receiver();
		hide(wifi_row);
		return;
	}

	if (!wifi_is_connected()) {
		// The same wording as every other network-dependent page, in the normal
		// text color: this is an instruction, not a footnote.
		lv_label_set_text(status_label, tr("enable_wi_fi_first"));
		lv_obj_set_style_text_color(status_label, theme()->text_primary, 0);
		lv_obj_add_state(toggle, LV_STATE_DISABLED);
		hide_receiver();
		show(wifi_row);
		return;
	}
	lv_obj_remove_local_style_prop(status_label, LV_STYLE_TEXT_COLOR, 0);

	lv_obj_remove_state(toggle, LV_STATE_DISABLED);
	hide(wifi_row);

	if (!switch_is_on()) {
		lv_label_set_text(status_label, tr(AP_OFF_HELP));
		hide_receiver();
		return;
	}

	// On, but shairport takes a moment: sys_server queues the request and only
	// looks at its queue once a second. Until the receiver is really up there
	// is nothing to go looking for on the phone.
	if (!airplay_running()) {
		lv_label_set_text(status_label, tr(AP_STARTING));
		hide_receiver();
		return;
	}

	airplay_state_t s;
	airplay_get_state(&s);

	show(glyph);
	set_line(title_label, s.title);
	set_line(artist_label, s.artist);
	set_line(album_label, s.album);

	if (s.connected) {
		lv_label_set_text(status_label, tr(AP_CONNECTED));
		return;
	}

	// Nothing has claimed the receiver yet. Say what to look for on the phone
	// -- the stock screen just says "wait", which is not much help the first
	// time.
	lv_label_set_text_fmt(status_label, "%s\n\xC2\xAB%s\xC2\xBB", tr(AP_WAIT), airplay_name());
}

static void poll_cb(lv_timer_t *timer) {
	(void)timer;

	refresh();

	if (!switch_is_on()) {
		return;
	}

	// The network went away under a running receiver: there is nothing left
	// for a sender to reach the player on.
	if (!wifi_is_connected()) {
		lv_obj_remove_state(toggle, LV_STATE_CHECKED);
		set_receiver(false);
	}
}

// ---------------------------------------------------------------------------
// switching it on and off
// ---------------------------------------------------------------------------

// Switching the receiver on does not stop the music. On means "discoverable",
// not "playing": leaving AirPlay on and silent for an hour is the normal case.
//
// The pause lives in the audio receive thread instead (play_worker in
// system/airplay.c), which stops local playback the moment the first bytes from
// a phone arrive and before the DAC is opened. Only one thing can come out of
// the DAC, so something has to give way, but only once there is really
// something to play.
static void set_receiver(bool on) {
	airplay_set_enabled(on);
	refresh();
}

static void toggle_changed_cb(lv_event_t *e) {
	(void)e;

	bool on = switch_is_on();

	if (on && !airplay_available()) {
		lv_obj_remove_state(toggle, LV_STATE_CHECKED);
		gui_notify_popup(AP_UNAVAILABLE);
		return;
	}
	if (on && !wifi_is_connected()) {
		lv_obj_remove_state(toggle, LV_STATE_CHECKED);
		gui_notify_popup("enable_wi_fi_first");
		return;
	}

	set_receiver(on);
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;

	set_line(title_label, "");
	set_line(artist_label, "");
	set_line(album_label, "");

	// The switch says what the receiver is doing, not what it was doing last
	// time this page was open.
	if (airplay_get_enabled() && airplay_available()) {
		lv_obj_add_state(toggle, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(toggle, LV_STATE_CHECKED);
	}

	refresh();
	lv_timer_resume(poll_timer);
	lv_timer_ready(poll_timer);
}

// Deliberately does NOT stop the receiver. The stock firmware's AirPlay keeps
// running when its screen is left, and so does this: the switch is the switch,
// the page is only where it lives. All that stops here is the polling.
static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	lv_timer_pause(poll_timer);
}

static void wifi_row_cb(lv_event_t *e) {
	(void)e;
	switch_screen(wifisettings_screen);
}

// ---------------------------------------------------------------------------

void airplay_page_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(airplay_screen, cfg, "airplay");

	settingsrow_toggle(container, "airplay", &toggle, toggle_changed_cb);

	// No artwork: the receiver has one job and a picture of the last track is
	// not part of it. What is left is the section's own glyph, quiet enough to
	// read as a mark rather than a control.
	glyph = lv_image_create(container);
	lv_image_set_src(glyph, &icon_airplay_page);
	lv_obj_add_style(glyph, &theme_style_icon, 0);
	lv_obj_set_style_image_opa(glyph, LV_OPA_40, 0);
	lv_obj_set_style_margin_top(glyph, 40, 0);
	lv_obj_set_style_margin_bottom(glyph, 28, 0);
	lv_obj_set_style_align(glyph, LV_ALIGN_CENTER, 0);

	title_label = lv_label_create(container);
	lv_obj_set_width(title_label, lv_pct(100));
	lv_obj_add_style(title_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(title_label, &font_ui_26, 0);
	lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
	scrolltext_apply(title_label);

	artist_label = lv_label_create(container);
	lv_obj_set_width(artist_label, lv_pct(100));
	lv_obj_add_style(artist_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(artist_label, &font_ui_22, 0);
	lv_obj_set_style_text_align(artist_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_pad_top(artist_label, 6, 0);
	scrolltext_apply(artist_label);

	album_label = lv_label_create(container);
	lv_obj_set_width(album_label, lv_pct(100));
	lv_obj_add_style(album_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(album_label, &font_ui_20, 0);
	lv_obj_set_style_text_align(album_label, LV_TEXT_ALIGN_CENTER, 0);
	scrolltext_apply(album_label);

	status_label = lv_label_create(container);
	lv_obj_set_width(status_label, lv_pct(100));
	lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
	lv_obj_add_style(status_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(status_label, &font_ui_20, 0);
	lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_pad_hor(status_label, 4, 0);
	lv_obj_set_style_pad_top(status_label, 14, 0);

	// The same row as the Wi-Fi transfer page: the notice says what is missing,
	// the Wi-Fi settings row leads to where it gets fixed.
	wifi_row = settingsrow_add(container, "wi_fi_settings", NULL, wifi_row_cb, NULL);
	lv_obj_set_style_margin_top(wifi_row, 16, 0);
	hide(wifi_row);

	poll_timer = lv_timer_create(poll_cb, AP_POLL_MS, NULL);
	lv_timer_pause(poll_timer);

	lv_obj_add_event_cb(airplay_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(airplay_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

	refresh();
}

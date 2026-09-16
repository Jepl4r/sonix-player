#include "dlna.h"

#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/scrolltext.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/system/audio/alsa-controls.h"
#include "src/system/audio/audio.h"
#include "src/system/playback/device_state.h"
#include "src/system/remote/dlna.h"
#include "src/system/core/lang.h"
#include "src/system/device/system.h"
#include "src/system/net/wifi.h"

lv_obj_t *dlna_screen;

// The command pump does not tick at the page's rate: commands arrive while the
// page is closed too, and that is exactly when they have to work.
#define PUMP_MS 120
#define PAGE_POLL_MS 400

#define DL_UNAVAILABLE "dlna_unavailable"
#define DL_OFF_HELP "dlna_off_note"
#define DL_STARTING "turning_on"
#define DL_WAIT "dlna_waiting_for_a_device"
#define DL_RECEIVING "dlna_playing_from_dlna"
#define DL_FETCHING "dlna_downloading_the_track"
#define DL_NO_CARD "dlna_no_card_note"

static lv_obj_t *toggle;
static lv_obj_t *glyph;
static lv_obj_t *title_label;
static lv_obj_t *artist_label;
static lv_obj_t *album_label;
static lv_obj_t *status_label;
static lv_obj_t *wifi_row;
static lv_timer_t *page_timer;

// ---------------------------------------------------------------------------

static void hide(lv_obj_t *obj) { lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN); }
static void show(lv_obj_t *obj) { lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN); }

static bool switch_is_on(void) { return lv_obj_has_state(toggle, LV_STATE_CHECKED); }

static bool wifi_is_connected(void) {
	wifi_status_t status;
	wifi_get_status(&status);
	return status.state == WIFI_STATE_CONNECTED && status.ip[0] != '\0';
}

static void set_line(lv_obj_t *label, const char *text) {
	if (!text || text[0] == '\0') {
		hide(label);
		return;
	}
	scrolltext_set(label, text);
	show(label);
}

static void hide_receiver(void) {
	hide(glyph);
	hide(title_label);
	hide(artist_label);
	hide(album_label);
}

// ---------------------------------------------------------------------------
// the pump: the phone's commands, carried out here
// ---------------------------------------------------------------------------
//
// system/dlna.c collects them on a network thread and leaves them in a queue;
// this is the only place playback is touched from.

static void apply_command(const dlna_command_t *cmd) {
	switch (cmd->kind) {
	case DLNA_CMD_PLAY: {
		// A queue of exactly one track: the real queue lives on the phone,
		// which sends the next track when this one ends. With one entry,
		// auto-advance stops on its own instead of picking something at random
		// from the user's library.
		const char *one[1] = {cmd->path};
		device_state_play_list(one, 1, 0);
		if (cmd->position > 0.5) {
			device_state_seek(cmd->position);
		}
		dlna_report_playing(cmd->path);
		player_refresh_now_playing();
		// The track comes from outside and was not asked for here, so showing
		// the player is the only way to signal that something is happening.
		if (!player_sheet_is_open()) {
			player_sheet_open(true);
		}
		break;
	}
	case DLNA_CMD_PAUSE:
		if (audio_get_status() == AUDIO_STATUS_PLAYING) {
			player_key_play_pause();
		}
		break;
	case DLNA_CMD_RESUME:
		if (audio_get_status() != AUDIO_STATUS_PLAYING) {
			player_key_play_pause();
		}
		break;
	case DLNA_CMD_STOP:
		device_state_stop();
		dlna_report_stopped();
		player_refresh_now_playing();
		break;
	case DLNA_CMD_SEEK:
		device_state_seek(cmd->position);
		break;
	case DLNA_CMD_VOLUME:
		set_volume_percent(cmd->volume);
		gui_notify_volume(get_volume_percent());
		break;
	case DLNA_CMD_NONE:
	default:
		break;
	}
}

// How slowly the pump ticks while DLNA is off. It cannot stop entirely, since
// it is also what notices DLNA being switched on, but with the service down no
// command can arrive, and two seconds is the worst-case delay between the
// switch being touched and the first useful tick. (Touching the switch also
// restores the fast period directly, below.)
#define PUMP_IDLE_MS 2000

static lv_timer_t *pump_timer;

// Puts the pump back at the rate the current state calls for.
static void pump_pace(void) {
	if (pump_timer) {
		lv_timer_set_period(pump_timer, dlna_get_enabled() ? PUMP_MS : PUMP_IDLE_MS);
	}
}

static void pump_cb(lv_timer_t *timer) {
	// Eight wakeups a second for a service that is off is a cost paid all night
	// in standby. While it is on the rate is unchanged.
	lv_timer_set_period(timer, dlna_get_enabled() ? PUMP_MS : PUMP_IDLE_MS);
	if (!dlna_get_enabled()) {
		return;
	}

	dlna_command_t cmd;
	while (dlna_take_command(&cmd)) {
		apply_command(&cmd);
	}

	if (!dlna_owns_playback()) {
		return;
	}

	// Something else has taken the player: a local track, the radio, an album
	// opened from the library. From here DLNA has nothing left to report, and
	// the phone learns it from its next request.
	char current[512];
	audio_get_current_file(current, sizeof(current));
	if (!dlna_owns_path(current)) {
		dlna_report_stopped();
		return;
	}

	double position = 0, duration = 0;
	audio_get_progress(&position, &duration);
	dlna_report_progress((int)position, (int)duration, get_volume_percent());
}

// ---------------------------------------------------------------------------
// the page
// ---------------------------------------------------------------------------

static void refresh(void) {
	if (!dlna_available()) {
		lv_label_set_text(status_label, tr(DL_UNAVAILABLE));
		lv_obj_add_state(toggle, LV_STATE_DISABLED);
		hide_receiver();
		hide(wifi_row);
		return;
	}

	if (!wifi_is_connected()) {
		// The same line every network-backed page uses, in the normal text
		// colour: it is an instruction, not a footnote.
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
		const char *root = storage_sd_root();
		lv_label_set_text(status_label, root && root[0] ? tr(DL_OFF_HELP) : tr(DL_NO_CARD));
		hide_receiver();
		return;
	}

	dlna_state_t s;
	dlna_get_state(&s);

	// The error is checked before the "starting" state: when startup fails,
	// dlna_running() stays false forever, and in the other order the page would
	// say "starting" indefinitely instead of what went wrong.
	if (s.error[0]) {
		lv_label_set_text(status_label, tr(s.error));
		hide_receiver();
		return;
	}

	// On, but dmrd takes a moment: sys_server queues the request and waits for
	// the renderer to report that it has started.
	if (!dlna_running()) {
		lv_label_set_text(status_label, tr(DL_STARTING));
		hide_receiver();
		return;
	}

	show(glyph);
	set_line(title_label, s.title);
	set_line(artist_label, s.artist);
	set_line(album_label, s.album);

	if (s.fetching) {
		lv_label_set_text(status_label, tr(DL_FETCHING));
		return;
	}
	if (dlna_owns_playback()) {
		lv_label_set_text(status_label, tr(DL_RECEIVING));
		return;
	}

	// Nothing has claimed the renderer yet, so show the name to look for on the
	// phone: a music app's device list never says which of the names belongs to
	// this player.
	lv_label_set_text_fmt(status_label, "%s\n\xC2\xAB%s\xC2\xBB", tr(DL_WAIT), dlna_name());
}

static void page_poll_cb(lv_timer_t *timer) {
	(void)timer;

	refresh();

	if (!switch_is_on()) {
		return;
	}

	// The network went away under a running receiver: there is nothing left to
	// be discovered on.
	if (!wifi_is_connected()) {
		lv_obj_remove_state(toggle, LV_STATE_CHECKED);
		dlna_set_enabled(false);
		refresh();
	}
}

static void toggle_changed_cb(lv_event_t *e) {
	(void)e;

	bool on = switch_is_on();

	if (on && !dlna_available()) {
		lv_obj_remove_state(toggle, LV_STATE_CHECKED);
		gui_notify_popup(DL_UNAVAILABLE);
		return;
	}
	if (on && !wifi_is_connected()) {
		lv_obj_remove_state(toggle, LV_STATE_CHECKED);
		gui_notify_popup("enable_wi_fi_first");
		return;
	}

	if (on) {
		// Re-check the scratch directory now: the card may have been inserted
		// after boot. Only when one is really there, though -- main.c has
		// already set a fallback root, and overwriting it with NULL would leave
		// DLNA nowhere to write.
		const char *root = storage_sd_root();
		if (root && root[0]) {
			dlna_set_root(root);
		} else {
			gui_notify_popup(DL_NO_CARD);
		}
	}

	dlna_set_enabled(on);
	pump_pace(); // no wait between the switch and the first command served
	refresh();
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;

	set_line(title_label, "");
	set_line(artist_label, "");
	set_line(album_label, "");

	// The switch reflects what the receiver is doing now, not what it was doing
	// the last time this page was open.
	if (dlna_get_enabled() && dlna_available()) {
		lv_obj_add_state(toggle, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(toggle, LV_STATE_CHECKED);
	}

	refresh();
	lv_timer_resume(page_timer);
	lv_timer_ready(page_timer);
}

// Like AirPlay, leaving the page does not stop the receiver: the switch owns
// that state, the page is only where it lives. All that stops here is the
// redraw.
static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	lv_timer_pause(page_timer);
}

static void wifi_row_cb(lv_event_t *e) {
	(void)e;
	switch_screen(wifisettings_screen);
}

// ---------------------------------------------------------------------------

void dlna_page_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(dlna_screen, cfg, "dlna");

	settingsrow_toggle(container, "dlna", &toggle, toggle_changed_cb);

	glyph = lv_image_create(container);
	lv_image_set_src(glyph, &icon_dlna_page);
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
	// the Wi-Fi settings row leads to where it is fixed.
	wifi_row = settingsrow_add(container, "wi_fi_settings", NULL, wifi_row_cb, NULL);
	lv_obj_set_style_margin_top(wifi_row, 16, 0);
	hide(wifi_row);

	page_timer = lv_timer_create(page_poll_cb, PAGE_POLL_MS, NULL);
	lv_timer_pause(page_timer);

	// This timer never stops, not even with the screen off: it is the bridge
	// between the network thread and playback, and a phone pushing a track
	// while another page is open still has to be served. It only slows down
	// while DLNA is off, which is nearly always.
	pump_timer = lv_timer_create(pump_cb, PUMP_IDLE_MS, NULL);
	pump_pace();

	lv_obj_add_event_cb(dlna_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(dlna_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

	// No dlna_set_root() here: main.c has already called it with the real card
	// or the fallback root, and this runs later, so repeating it with whatever
	// storage_sd_root() answers now (NULL without a card) would undo it.
	refresh();
}

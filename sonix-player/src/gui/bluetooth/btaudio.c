#include "btaudio.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/bluetooth/airpodspage.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/sx_mini.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/nowplaying/btline.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/sx_styles.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/sx_tokens.h"
#include "src/system/device/power.h"
#include "src/system/bluetooth/airpods.h"
#include "src/system/audio/alsa-controls.h"
#include "src/system/audio/audio.h"
#include "src/system/audio/eqprofile.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/core/config.h"
#include "src/system/device/ota.h"
#include "src/system/net/wifitransfer.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/provider.h"
#include "src/system/core/lang.h"

lv_obj_t *btaudio_screen;

#define BTAUDIO_POLL_MS 700

static lv_obj_t *volume_switch;
static lv_obj_t *airpods_row;
static lv_obj_t *airpods_icon;
static lv_obj_t *battery_row;  // the headset's own level; hidden while unknown
static lv_obj_t *battery_value;
static lv_obj_t *battery_switch;
static lv_timer_t *poll_timer;


// ---------------------------------------------------------------------------
// switching the output over
// ---------------------------------------------------------------------------

// The ALSA device is only read when a stream is opened, so a change made
// while something is playing would not be heard until the next track. Rather
// than reach into the playback thread, the track is reloaded where it stands:
// the same path a normal track change takes, and one it is known to survive.
static void restart_playback_if_running(void) {
	char file[512];
	audio_get_current_file(file, sizeof(file));
	if (!file[0]) {
		return;
	}

	audio_status_t status = audio_get_status();
	if (status == AUDIO_STATUS_STOPPED) {
		return;
	}

	double position = 0;
	double total = 0;
	audio_get_progress(&position, &total);

	audio_play(file);
	audio_seek(position);
	if (status == AUDIO_STATUS_PAUSED) {
		audio_pause();
	}
	player_refresh_now_playing();
}

// ---------------------------------------------------------------------------
// the page itself
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// The AirPods row
//
// It exists only while a pair is actually connected, so the page is one row
// shorter with anything else on the other end. The picture at its right is the
// model that is in the room, worked out from what the headphones themselves
// say (see airpods.h); it sits to the left of the chevron, in the space a
// value would use on any other row.
// ---------------------------------------------------------------------------

#define AIRPODS_ICON_INSET 48 // clears the chevron at the row's right edge

static void airpods_row_cb(lv_event_t *e) {
	(void)e;
	switch_screen(airpodspage_screen);
}

static bool refresh_airpods_row(void) {
	airpods_state_t state;
	bool present = airpods_get(&state);

	if (present) {
		lv_obj_set_hidden(airpods_row, false);
		lv_image_set_src(airpods_icon, airpodspage_model_icon(state.model));
		// The product name, not the tag: "AirPods Pro 2" rather than "AirPods",
		// so the row says which pair is on the other end.
		lv_label_set_text(settingsrow_name_label(airpods_row), state.name[0] ? state.name : tr("airpods"));
	} else {
		lv_obj_set_hidden(airpods_row, true);
	}
	return present;
}

// ---------------------------------------------------------------------------
// The headset battery
//
// Any headset's own level, read over the hands-free control channel when the
// switch below lets bluealsa run as a hands-free gateway (bluetooth.h says
// what that costs). The row exists only while there is a number: off, nothing
// connected, or a headset that has not said leave the page as it was. AirPods
// have their own page with a level per bud, so the row stays out of their way.
// ---------------------------------------------------------------------------

static void refresh_battery_row(bool airpods_present) {
	int percent = bluetooth_headset_battery(NULL);
	bool show = percent >= 0 && !airpods_present;
	if (show) {
		char text[16];
		snprintf(text, sizeof(text), "%d %%", percent);
		if (strcmp(lv_label_get_text(battery_value), text) != 0) {
			lv_label_set_text(battery_value, text);
		}
	}
	lv_obj_set_hidden(battery_row, !show);
}

static void battery_toggle_cb(lv_event_t *e) {
	(void)e;
	bluetooth_set_headset_battery_enabled(lv_obj_has_state(battery_switch, LV_STATE_CHECKED));
}

static void volume_toggle_cb(lv_event_t *e) {
	(void)e;
	bluetooth_set_volume_sync(lv_obj_has_state(volume_switch, LV_STATE_CHECKED));
}

static lv_obj_t *details_switch;

static void details_toggle_cb(lv_event_t *e) {
	(void)e;
	btline_set_enabled(lv_obj_has_state(details_switch, LV_STATE_CHECKED));
}

// ---------------------------------------------------------------------------
// The encoder's own options
//
// Read by bluealsa when it starts and never again, so each change restarts it
// (see bluetooth.h): the headphones drop out for a few seconds and come back on
// their own. Kept as pills rather than switches because each is a choice
// between two named things, not something that is on or off.
// ---------------------------------------------------------------------------

// One pill per bt_rate_mode_t, in its order: the pill's value is the mode.
static lv_obj_t *rate_pills[4];
static lv_obj_t *aac_vbr_pill;
static lv_obj_t *aac_cbr_pill;

static void encoder_pills_refresh(void) {
	if (rate_pills[0]) {
		bt_rate_mode_t mode = bluetooth_rate_mode();
		for (int i = 0; i < (int)(sizeof(rate_pills) / sizeof(rate_pills[0])); i++) {
			settingsrow_pill_active(rate_pills[i], (int)mode == i);
		}
	}
	if (aac_vbr_pill) {
		bool vbr = bluetooth_aac_vbr();
		settingsrow_pill_active(aac_vbr_pill, vbr);
		settingsrow_pill_active(aac_cbr_pill, !vbr);
	}
}

static void aac_pick_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	bluetooth_set_aac_vbr((intptr_t)lv_event_get_user_data(e) != 0);
	encoder_pills_refresh();
}

static void rate_pick_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	bluetooth_set_rate_mode((bt_rate_mode_t)(intptr_t)lv_event_get_user_data(e));
	encoder_pills_refresh();
}

// What a choice does, inside its card under the pills: the 2.0 card carries
// its explanation rather than leaving it to a note under it.
static lv_obj_t *option_note(lv_obj_t *card, const char *text) {
	lv_obj_t *note = lv_label_create(card);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &sx_style_text2, 0);
	lv_obj_set_style_text_font(note, &font_ui_21, 0);
	lv_label_set_text(note, tr(text));
	return note;
}

static void build_encoder_options(lv_obj_t *container) {
	lv_obj_t *pills = NULL;
	lv_obj_t *card = settingsrow_pills(container, "btaudio_sample_rate", &pills);
	// Auto and 44.1 kHz are what bluealsa is started with; "Follow the music"
	// and 96 kHz move the link between tracks without restarting it (see
	// bluetooth.c, "the link's rate"). 96 kHz is an LDAC choice: on any other
	// codec it is Auto, which the note says.
	rate_pills[BT_RATE_AUTO] = settingsrow_pill(pills, "btaudio_rate_auto", BT_RATE_AUTO, rate_pick_cb);
	rate_pills[BT_RATE_44K] = settingsrow_pill_text(pills, "44.1 kHz", BT_RATE_44K, rate_pick_cb);
	rate_pills[BT_RATE_FOLLOW] = settingsrow_pill(pills, "btaudio_rate_follow", BT_RATE_FOLLOW, rate_pick_cb);
	rate_pills[BT_RATE_96K] = settingsrow_pill_text(pills, "96 kHz", BT_RATE_96K, rate_pick_cb);
	option_note(card, "btaudio_sample_rate_note");

	pills = NULL;
	card = settingsrow_pills(container, "btaudio_aac_mode", &pills);
	aac_vbr_pill = settingsrow_pill_text(pills, "VBR", 1, aac_pick_cb);
	aac_cbr_pill = settingsrow_pill_text(pills, "CBR 220", 0, aac_pick_cb);
	option_note(card, "btaudio_aac_mode_note");

	encoder_pills_refresh();
	theme_register_refresh(encoder_pills_refresh);
}

// ---------------------------------------------------------------------------
// Keep Bluetooth on
//
// How long the radio stays up, idle, after the screen goes dark before it is
// parked (power.c). Longer means headphones put back on after a pause find the
// player still there and reconnect by themselves; shorter saves the few mA of an
// idle radio. A wait past the standby delay keeps the player out of standby
// until it runs out, which the note says.
// ---------------------------------------------------------------------------

static const int KEEP_ON_SECONDS[] = {15, 60, 5 * 60, 15 * 60};
#define KEEP_ON_COUNT ((int)(sizeof(KEEP_ON_SECONDS) / sizeof(KEEP_ON_SECONDS[0])))
static lv_obj_t *keep_on_pills[KEEP_ON_COUNT];

static void keep_on_refresh(void) {
	uint32_t current = power_bt_park_seconds();
	// The nearest choice lights up, so a hand-edited config still shows one.
	int best = 0;
	for (int i = 1; i < KEEP_ON_COUNT; i++) {
		long d_best = labs((long)KEEP_ON_SECONDS[best] - (long)current);
		long d_this = labs((long)KEEP_ON_SECONDS[i] - (long)current);
		if (d_this < d_best) {
			best = i;
		}
	}
	for (int i = 0; i < KEEP_ON_COUNT; i++) {
		settingsrow_pill_active(keep_on_pills[i], i == best);
	}
}

static void keep_on_pick_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	int seconds = (int)(intptr_t)lv_event_get_user_data(e);
	power_set_bt_park_seconds((uint32_t)seconds);
	config_set_int("power", "bt_park_seconds", seconds);
	config_save();
	keep_on_refresh();
}

static void build_keep_on_options(lv_obj_t *container) {
	lv_obj_t *pills = NULL;
	lv_obj_t *card = settingsrow_pills(container, "btaudio_keep_on", &pills);
	for (int i = 0; i < KEEP_ON_COUNT; i++) {
		char text[16];
		if (KEEP_ON_SECONDS[i] < 60) {
			snprintf(text, sizeof(text), "%d s", KEEP_ON_SECONDS[i]);
		} else {
			snprintf(text, sizeof(text), "%d min", KEEP_ON_SECONDS[i] / 60);
		}
		keep_on_pills[i] = settingsrow_pill_text(pills, text, KEEP_ON_SECONDS[i], keep_on_pick_cb);
	}
	option_note(card, "btaudio_keep_on_note");
	keep_on_refresh();
	theme_register_refresh(keep_on_refresh);
}

// Whether the Wi-Fi radio is moving data in bulk right now: a track being
// cached for Qobuz, Tidal or a podcast, or a firmware update downloading. The
// transfer server is the other case and is handled by bluetooth.c itself.
static bool wifi_downloading(void) {
	return stream_provider_any_downloading() || podcastcache_downloading_id() > 0 ||
		   ota_busy();
}

static void refresh_name_row(void);

static void refresh_page(void) {
	// The name is read from the firmware's file at startup and can be changed
	// from this page, so it is re-read here rather than written once.
	refresh_name_row();

	refresh_battery_row(refresh_airpods_row());

	bool sync = bluetooth_volume_sync();
	if (lv_obj_has_state(volume_switch, LV_STATE_CHECKED) != sync) {
		if (sync) {
			lv_obj_add_state(volume_switch, LV_STATE_CHECKED);
		} else {
			lv_obj_remove_state(volume_switch, LV_STATE_CHECKED);
		}
	}
}

static void poll_cb(lv_timer_t *timer) {
	(void)timer;
	refresh_page();
}

// Watches where the sound is going and reloads the track when that changes --
// headphones connecting mid-song, or walking out of range. Runs whether or not
// this page is open, because that is when it matters.
//
// Once a second while the device is in use, and once every five with the panel
// dark and nothing playing: there is no stream for a routing change to spoil
// then, and a slower tick leaves the main loop asleep in standby.
#define ROUTING_WATCH_MS 1000
#define ROUTING_WATCH_IDLE_MS 5000

static void routing_watch_cb(lv_timer_t *timer) {
	bool matters = power_screen_is_on() || audio_get_status() == AUDIO_STATUS_PLAYING;
	lv_timer_set_period(timer, matters ? ROUTING_WATCH_MS : ROUTING_WATCH_IDLE_MS);

	// LDAC 990 and the Wi-Fi transfer server (bluetooth.c). Nothing happens
	// unless the switch moved.
	bluetooth_set_wifi_busy(wifitransfer_get_enabled());

	// Short downloads under LDAC 990 are only said, in the log here and on the
	// page above: changing the quality under them would cost more than it saves.
	static bool was_downloading;
	bool downloading = wifi_downloading();
	if (downloading != was_downloading) {
		was_downloading = downloading;
		if (bluetooth_audio_active() && strcmp(bluetooth_ldac_quality(), "high") == 0) {
			printf("btaudio: Wi-Fi %s under LDAC 990%s\n", downloading ? "downloading" : "quiet again",
				   downloading ? "; dropouts are possible meanwhile" : "");
		}
	}

	char now[160];
	audio_get_output_device(now, sizeof(now));
	bool now_bt = strncmp(now, "bluealsa", 8) == 0;

	// The volume profile first, and driven from where the sound is actually
	// going rather than from the change detection below, so a player started
	// with the headphones already connected also gets the right profile. Safe
	// on every tick: alsa-controls holds every level and knows which one is
	// current, and a call that changes nothing returns at once.
	//
	// Ordering matters on connection: btvolume waits for this swap and then
	// reads the level the headphones are actually at, so the player adopts
	// theirs rather than pushing its own.
	//
	// The PCM name answers for Bluetooth and USB-C; for the two holes in the
	// side of the player only the jack detect can, because they are the same
	// ALSA device and differ in the route.
	volume_output_t out;
	if (now_bt) {
		// Which pair, first: the Bluetooth level is kept per device, and the
		// swap below applies whichever level that makes current. The address
		// is read out of the PCM name -- "bluealsa:DEV=<mac>,PROFILE=a2dp" --
		// so it is always the pair the sound is actually going to.
		const char *dev = strstr(now, "DEV=");
		char mac[BT_MAC_MAX] = "";
		if (dev) {
			dev += 4;
			size_t length = strcspn(dev, ",");
			if (length < sizeof(mac)) {
				memcpy(mac, dev, length);
				mac[length] = '\0';
			}
		}
		volume_profile_set_bluetooth_device(mac);
		// And the EQ, from the same place and for the same reasons as the
		// volume (eqprofile.h); a no-op with "EQ per output" off.
		eqprofile_output_changed(EQPROFILE_BLUETOOTH, mac);
		out = VOLUME_OUTPUT_BLUETOOTH;
	} else if (strncmp(now, "plughw:", 7) == 0) {
		out = VOLUME_OUTPUT_USB;
		eqprofile_output_changed(EQPROFILE_USB, NULL);
	} else {
		out = headphone_jack_state() == JACK_BALANCED ? VOLUME_OUTPUT_BALANCED : VOLUME_OUTPUT_PHONES;
		eqprofile_output_changed(out == VOLUME_OUTPUT_BALANCED ? EQPROFILE_BALANCED : EQPROFILE_PHONES, NULL);
	}
	volume_profile_set_output(out);

	static char known[160];
	static unsigned known_generation;
	if (!known[0]) {
		snprintf(known, sizeof(known), "%s", now); // first look: nothing to reload
		known_generation = bluetooth_output_generation();
		return;
	}

	// The same headphones behind a bluealsa that was restarted: same name, dead
	// PCM. Reloading opens it again on the new daemon.
	unsigned generation = bluetooth_output_generation();
	if (generation != known_generation) {
		known_generation = generation;
		if (now_bt && strcmp(known, now) == 0) {
			printf("btaudio: '%s' is a new bluealsa's now; reloading the track\n", now);
			restart_playback_if_running();
			return;
		}
	}

	if (strcmp(known, now) == 0) {
		return;
	}

	bool was_bt = strncmp(known, "bluealsa", 8) == 0;
	snprintf(known, sizeof(known), "%s", now);
	printf("btaudio: output moved to '%s'; reloading the track\n", now);

	// Leaving the headphones is a disconnection, whichever end caused it: they
	// walked out of range, their battery went, or the radio was switched off.
	// Either way nobody is listening any more, and the reload below is what
	// would otherwise carry the album on out of the jack. Pause first: the
	// reload keeps whatever state it finds.
	if (was_bt && !now_bt) {
		player_output_unplugged("the Bluetooth headphones");
	}

	restart_playback_if_running();
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	refresh_page();
	lv_timer_resume(poll_timer);
}

static void name_layer_hide(void);

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	// Leaving the page puts the rename sheet away: coming back to a keyboard
	// nobody asked for is a page that looks stuck.
	name_layer_hide();
	lv_timer_pause(poll_timer);
}

// The player's own Bluetooth name, and the sheet that changes it: a field and a
// keyboard over the page, the same shape the preset and playlist names use.
#define NAME_CANCEL_SIZE 56

static lv_obj_t *name_row_value;
static lv_obj_t *name_layer;
static lv_obj_t *name_field;
static keyboard_t *name_keyboard;

// ---------------------------------------------------------------------------
// the name
// ---------------------------------------------------------------------------

static void refresh_name_row(void) {
	if (name_row_value) {
		lv_label_set_text(name_row_value, bluetooth_local_name());
	}
}

static void name_layer_hide(void) {
	if (name_layer) {
		lv_obj_set_hidden(name_layer, true);
		sx_mini_update_visibility(); // its keyboard comes or goes with it
	}
	back_btn_force_hidden(false);
}

static void name_cancel_cb(lv_event_t *e) {
	(void)e;
	name_layer_hide();
}

static void name_accept_cb(lv_event_t *e) {
	(void)e;

	const char *typed = lv_textarea_get_text(name_field);
	if (!bluetooth_set_local_name(typed ? typed : "")) {
		gui_notify_popup("name_required");
		return;
	}

	name_layer_hide();
	refresh_name_row();
	toast_success("bt_name_changed");
}

static void name_row_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active() || !name_layer) {
		return;
	}
	lv_textarea_set_text(name_field, bluetooth_local_name());
	keyboard_reset(name_keyboard);
	lv_obj_set_hidden(name_layer, false);
	sx_mini_update_visibility(); // its keyboard comes or goes with it
	lv_obj_move_foreground(name_layer);
	// The sheet has its own close in the corner, and the chevron under it would
	// leave the page without putting the keyboard away.
	back_btn_force_hidden(true);
}

static void build_name_layer(gui_config_t *cfg) {
	name_layer = lv_obj_create(btaudio_screen);
	lv_obj_set_size(name_layer, lv_pct(100), lv_pct(100));
	lv_obj_align(name_layer, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_add_style(name_layer, &theme_style_screen, 0);
	lv_obj_set_style_bg_opa(name_layer, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(name_layer, 0, 0);
	lv_obj_set_style_radius(name_layer, 0, 0);
	lv_obj_set_style_pad_all(name_layer, 0, 0);
	lv_obj_set_scrollable(name_layer, false);
	lv_obj_set_hidden(name_layer, true);

	lv_obj_t *heading = lv_label_create(name_layer);
	lv_label_set_text(heading, tr("bt_rename"));
	lv_obj_add_style(heading, &theme_style_text, 0);
	lv_obj_set_style_text_font(heading, &font_ui_24, 0);
	lv_obj_align(heading, LV_ALIGN_TOP_LEFT, cfg->padding, cfg->padding + cfg->top_bar_height + 10);

	lv_obj_t *cancel = lv_btn_create(name_layer);
	lv_obj_set_size(cancel, NAME_CANCEL_SIZE, NAME_CANCEL_SIZE);
	lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, -cfg->padding, cfg->padding + cfg->top_bar_height);
	lv_obj_set_style_bg_opa(cancel, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(cancel, 0, 0);
	lv_obj_set_style_shadow_width(cancel, 0, 0);
	lv_obj_set_style_pad_all(cancel, 0, 0);
	lv_obj_add_event_cb(cancel, name_cancel_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *cancel_icon = lv_image_create(cancel);
	lv_image_set_src(cancel_icon, &icon_close);
	lv_obj_add_style(cancel_icon, &theme_style_icon, 0);
	lv_obj_center(cancel_icon);

	name_field = lv_textarea_create(name_layer);
	lv_textarea_set_one_line(name_field, true);
	// One under the adapter's own limit, so a name that fits the field is a
	// name the adapter will take whole.
	lv_textarea_set_max_length(name_field, BT_NAME_MAX - 1);
	lv_textarea_set_placeholder_text(name_field, tr("name"));
	lv_obj_set_size(name_field, cfg->screen_width - 2 * cfg->padding, 62);
	lv_obj_set_scrollbar_mode(name_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(name_field, LV_ALIGN_TOP_LEFT, cfg->padding, cfg->padding + cfg->top_bar_height + 60);
	lv_obj_add_style(name_field, &theme_style_card, 0);
	// The 2.0 input: a pill on the surface.
	lv_obj_add_style(name_field, &sx_style_surface, 0);
	lv_obj_set_style_radius(name_field, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_border_width(name_field, 0, 0);
	lv_obj_set_style_shadow_width(name_field, 0, 0);
	lv_obj_set_style_pad_all(name_field, 14, 0);
	lv_obj_set_style_pad_hor(name_field, 22, 0);
	lv_obj_set_style_text_font(name_field, &font_ui_24, 0);
	keyboard_style_caret(name_field);

	name_keyboard = keyboard_create(name_layer, cfg->screen_width, 316, name_field, NULL, "ok", name_accept_cb, NULL);
}

void btaudio_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(btaudio_screen, cfg, "btaudio_title");

	// No "audio via Bluetooth" switch, no paragraph explaining the absence, and
	// no "output" row: the stock player has none of the three. Connected
	// headphones are the output, and which ones they are is written on the
	// Bluetooth page itself.

	// The player's own name, first: it is about this device rather than about
	// whatever is connected to it.
	settingsrow_add(container, "bt_rename", &name_row_value, name_row_cb, NULL);

	// The AirPods row, near the top: it is about the headphones themselves
	// rather than about the link, and it is the one row here that is not
	// always there.
	airpods_row = settingsrow_add(container, "airpods", NULL, airpods_row_cb, NULL);
	airpods_icon = lv_image_create(airpods_row);
	lv_obj_add_style(airpods_icon, &theme_style_icon, 0);
	lv_image_set_src(airpods_icon, airpodspage_model_icon(AIRPODS_MODEL_UNKNOWN));
	lv_obj_align(airpods_icon, LV_ALIGN_RIGHT_MID, -AIRPODS_ICON_INSET, 0);
	lv_obj_set_hidden(airpods_row, true);

	// Any other headset's battery, in the same place and on the same terms:
	// there only while there is something to show. Not a button.
	battery_row = settingsrow_add(container, "btaudio_battery", &battery_value, NULL, NULL);
	lv_obj_set_clickable(battery_row, false);
	lv_obj_set_hidden(battery_row, true);

	// The codec and LDAC's rate are on the Bluetooth page itself since 2.0,
	// as pills under the connected headphones.

	// One level or two, on by default. On, the keys here move the headphones
	// over AVRCP absolute volume and their own buttons move the number on the
	// screen. Off, bluealsa attenuates the stream on this side and the
	// headphones keep their own level, so the two are independent.
	settingsrow_switch_card(container, "btaudio_synchronised_volume", NULL, &volume_switch, volume_toggle_cb);
	if (bluetooth_volume_sync()) {
		lv_obj_add_state(volume_switch, LV_STATE_CHECKED);
	}

	// The headset battery, opt-in: see refresh_battery_row() above.
	settingsrow_switch_card(container, "btaudio_headset_battery", "btaudio_headset_battery_note", &battery_switch,
							battery_toggle_cb);
	if (bluetooth_headset_battery_enabled()) {
		lv_obj_add_state(battery_switch, LV_STATE_CHECKED);
	}

	// The codec and any rate conversion, under the artist on Now Playing
	// (btline.h). On by default.
	settingsrow_switch_card(container, "btaudio_nowplaying_details", "btaudio_nowplaying_details_note",
							&details_switch, details_toggle_cb);
	if (btline_enabled()) {
		lv_obj_add_state(details_switch, LV_STATE_CHECKED);
	}

	build_encoder_options(container);
	build_keep_on_options(container);

	poll_timer = lv_timer_create(poll_cb, BTAUDIO_POLL_MS, NULL);
	lv_timer_pause(poll_timer);

	// This one is never paused: the output can change while the user is on the
	// player, which is exactly when they would like to keep hearing the music.
	lv_timer_create(routing_watch_cb, ROUTING_WATCH_MS, NULL);

	lv_obj_add_event_cb(btaudio_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(btaudio_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(btaudio_screen);

	build_name_layer(cfg);

	refresh_page();
}

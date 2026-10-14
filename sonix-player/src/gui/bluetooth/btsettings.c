#include "btsettings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "lvgl/lvgl.h"

#include "src/gui/bluetooth/airpodspage.h"
#include "src/gui/bluetooth/btaudio.h"
#include "src/gui/bluetooth/btreceiverpage.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/sx_mini.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/spinner.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/sx_card.h"
#include "src/gui/shell/sx_sheet.h"
#include "src/gui/shell/sx_styles.h"
#include "src/gui/shell/sx_tokens.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/topbar.h"
#include "src/gui/shell/touch.h"
#include "src/system/bluetooth/airpods.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/core/lang.h"
#include "src/system/device/ota.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/provider.h"

lv_obj_t *btsettings_screen;

#define BT_PAGE_POLL_MS 500
#define CORNER_BTN_SIZE 56

// ---------------------------------------------------------------------------
// The emulator's radio
//
// The host has no Bluetooth, so the page could only be seen saying so. With
// SONIX_BT_FAKE set (host build only) the calls below answer for a radio with
// a pair of LDAC headphones connected, another pair paired, and a speaker
// that a search finds, and the page's actions move them about. On the device
// none of this exists: the names are the real ones.
// ---------------------------------------------------------------------------

#ifdef HOST_BUILD
static bool fake_on(void) {
	static int on = -1;
	if (on < 0) {
		const char *env = getenv("SONIX_BT_FAKE");
		on = env && env[0] ? 1 : 0;
	}
	return on == 1;
}

static bool fake_enabled = true;
static bool fake_scanning;
static int fake_connected = 0; // index into FAKE_DEVICES, -1 for none
static uint32_t fake_serial = 1;
static bt_op_t fake_op;
static char fake_codec[BT_CODEC_MAX] = "LDAC";
static bool fake_codec_by_hand;
static char fake_ldac[16] = "high";

static const struct {
	const char *mac;
	const char *name;
	bool paired;
} FAKE_DEVICES[] = {
	{"00:1B:66:A1:B2:C3", "Space One Pro", true},
	{"F4:4E:FD:11:22:33", "WH-1000XM4", true},
	{"74:5C:4B:44:55:66", "JBL Flip 6", false},
};
#define FAKE_DEVICE_COUNT 3

static bool fake_bluetooth_available(void) { return fake_on() || bluetooth_available(); }
static bool fake_bluetooth_get_enabled(void) { return fake_on() ? fake_enabled : bluetooth_get_enabled(); }
static void fake_bluetooth_set_enabled(bool on) {
	if (!fake_on()) {
		bluetooth_set_enabled(on);
		return;
	}
	fake_enabled = on;
	fake_serial++;
}
static bool fake_bluetooth_busy(void) { return fake_on() ? false : bluetooth_busy(); }
static const char *fake_bluetooth_local_name(void) { return fake_on() ? "HiBy R1" : bluetooth_local_name(); }
static uint32_t fake_bluetooth_devices_serial(void) { return fake_on() ? fake_serial : bluetooth_devices_serial(); }
static void fake_fill(bt_device_t *out, int i) {
	memset(out, 0, sizeof(*out));
	snprintf(out->mac, sizeof(out->mac), "%s", FAKE_DEVICES[i].mac);
	snprintf(out->name, sizeof(out->name), "%s", FAKE_DEVICES[i].name);
	out->paired = FAKE_DEVICES[i].paired;
	out->connected = i == fake_connected;
}
static int fake_bluetooth_get_paired(bt_device_t *out, int max) {
	if (!fake_on()) {
		return bluetooth_get_paired(out, max);
	}
	int n = 0;
	for (int i = 0; i < FAKE_DEVICE_COUNT && n < max && fake_enabled; i++) {
		if (FAKE_DEVICES[i].paired) {
			fake_fill(&out[n++], i);
		}
	}
	return n;
}
static int fake_bluetooth_get_found(bt_device_t *out, int max) {
	if (!fake_on()) {
		return bluetooth_get_found(out, max);
	}
	int n = 0;
	for (int i = 0; i < FAKE_DEVICE_COUNT && n < max && fake_scanning; i++) {
		if (!FAKE_DEVICES[i].paired) {
			fake_fill(&out[n++], i);
		}
	}
	return n;
}
static bool fake_bluetooth_scan_running(void) { return fake_on() ? fake_scanning : bluetooth_scan_running(); }
static void fake_bluetooth_scan_start(void) {
	if (!fake_on()) {
		bluetooth_scan_start();
		return;
	}
	fake_scanning = true;
	fake_serial++;
}
static void fake_bluetooth_scan_stop(void) {
	if (!fake_on()) {
		bluetooth_scan_stop();
		return;
	}
	fake_scanning = false;
	fake_serial++;
}
static void fake_bluetooth_refresh(void) {
	if (!fake_on()) {
		bluetooth_refresh();
	}
}
static void fake_bluetooth_set_discoverable(bool on) {
	if (!fake_on()) {
		bluetooth_set_discoverable(on);
	}
}
static int fake_index(const char *mac) {
	for (int i = 0; i < FAKE_DEVICE_COUNT; i++) {
		if (strcmp(FAKE_DEVICES[i].mac, mac) == 0) {
			return i;
		}
	}
	return -1;
}
static void fake_bluetooth_connect(const char *mac) {
	if (!fake_on()) {
		bluetooth_connect(mac);
		return;
	}
	fake_connected = fake_index(mac);
	fake_op = BT_OP_OK;
	fake_serial++;
}
static void fake_bluetooth_pair(const char *mac) {
	if (!fake_on()) {
		bluetooth_pair(mac);
		return;
	}
	fake_connected = fake_index(mac);
	fake_serial++;
}
static void fake_bluetooth_disconnect(const char *mac) {
	if (!fake_on()) {
		bluetooth_disconnect(mac);
		return;
	}
	fake_connected = -1;
	fake_op = BT_OP_OK;
	fake_serial++;
}
static void fake_bluetooth_forget(const char *mac) {
	if (!fake_on()) {
		bluetooth_forget(mac);
		return;
	}
	if (fake_connected == fake_index(mac)) {
		fake_connected = -1;
	}
	fake_op = BT_OP_OK;
	fake_serial++;
}
static bool fake_bluetooth_rename(const char *mac, const char *name) {
	return fake_on() ? true : bluetooth_rename(mac, name);
}
static bt_op_t fake_bluetooth_take_op_result(char *name_out, int name_size) {
	if (!fake_on()) {
		return bluetooth_take_op_result(name_out, name_size);
	}
	bt_op_t op = fake_op;
	fake_op = BT_OP_IDLE;
	if (name_out && name_size > 0) {
		name_out[0] = '\0';
	}
	return op;
}
static bool fake_bluetooth_audio_active(void) {
	return fake_on() ? fake_enabled && fake_connected >= 0 : bluetooth_audio_active();
}
static int fake_bluetooth_get_codecs(char out[][BT_CODEC_MAX], int max, char *selected, int selected_size) {
	if (!fake_on()) {
		return bluetooth_get_codecs(out, max, selected, selected_size);
	}
	static const char *const CODECS[] = {"SBC", "AAC", "LDAC"};
	int n = 0;
	for (int i = 0; i < 3 && n < max; i++) {
		snprintf(out[n++], BT_CODEC_MAX, "%s", CODECS[i]);
	}
	snprintf(selected, (size_t)selected_size, "%s", fake_codec);
	return n;
}
static void fake_bluetooth_set_codec(const char *codec) {
	if (!fake_on()) {
		bluetooth_set_codec(codec);
		return;
	}
	snprintf(fake_codec, sizeof(fake_codec), "%s", codec);
	fake_codec_by_hand = true;
}
static void fake_bluetooth_set_codec_auto(void) {
	if (!fake_on()) {
		bluetooth_set_codec_auto();
		return;
	}
	snprintf(fake_codec, sizeof(fake_codec), "LDAC");
	fake_codec_by_hand = false;
}
static bool fake_bluetooth_codec_chosen(char *out, int size) {
	if (!fake_on()) {
		return bluetooth_codec_chosen(out, size);
	}
	snprintf(out, (size_t)size, "%s", fake_codec_by_hand ? fake_codec : "");
	return fake_codec_by_hand;
}
static const char *fake_bluetooth_ldac_quality(void) { return fake_on() ? fake_ldac : bluetooth_ldac_quality(); }
static void fake_bluetooth_set_ldac_quality(const char *mode) {
	if (!fake_on()) {
		bluetooth_set_ldac_quality(mode);
		return;
	}
	snprintf(fake_ldac, sizeof(fake_ldac), "%s", mode);
}
static bool fake_bluetooth_codec_summary(const char *mac, char *out, int size) {
	if (!fake_on()) {
		return bluetooth_codec_summary(mac, out, size);
	}
	if (fake_connected < 0) {
		return false;
	}
	if (strcmp(fake_codec, "LDAC") == 0) {
		const char *rate = strcmp(fake_ldac, "high") == 0 ? " 990" : strcmp(fake_ldac, "standard") == 0 ? " 660"
														   : strcmp(fake_ldac, "mobile") == 0	   ? " 330"
																								   : "";
		snprintf(out, (size_t)size, "LDAC%s \xC2\xB7 96 kHz", rate);
	} else {
		snprintf(out, (size_t)size, "%s \xC2\xB7 44.1 kHz", fake_codec);
	}
	return true;
}
static int fake_bluetooth_headset_battery(unsigned *serial_out) {
	return fake_on() ? (fake_connected >= 0 ? 78 : -1) : bluetooth_headset_battery(serial_out);
}
static int fake_bluetooth_local_codecs(char out[][BT_CODEC_MAX], int max, bool receiving) {
	if (!fake_on()) {
		return bluetooth_local_codecs(out, max, receiving);
	}
	snprintf(out[0], BT_CODEC_MAX, "LDAC");
	return max > 0 ? 1 : 0;
}

#define bluetooth_available fake_bluetooth_available
#define bluetooth_get_enabled fake_bluetooth_get_enabled
#define bluetooth_set_enabled fake_bluetooth_set_enabled
#define bluetooth_busy fake_bluetooth_busy
#define bluetooth_local_name fake_bluetooth_local_name
#define bluetooth_devices_serial fake_bluetooth_devices_serial
#define bluetooth_get_paired fake_bluetooth_get_paired
#define bluetooth_get_found fake_bluetooth_get_found
#define bluetooth_scan_running fake_bluetooth_scan_running
#define bluetooth_scan_start fake_bluetooth_scan_start
#define bluetooth_scan_stop fake_bluetooth_scan_stop
#define bluetooth_refresh fake_bluetooth_refresh
#define bluetooth_set_discoverable fake_bluetooth_set_discoverable
#define bluetooth_connect fake_bluetooth_connect
#define bluetooth_pair fake_bluetooth_pair
#define bluetooth_disconnect fake_bluetooth_disconnect
#define bluetooth_forget fake_bluetooth_forget
#define bluetooth_rename fake_bluetooth_rename
#define bluetooth_take_op_result fake_bluetooth_take_op_result
#define bluetooth_audio_active fake_bluetooth_audio_active
#define bluetooth_get_codecs fake_bluetooth_get_codecs
#define bluetooth_set_codec fake_bluetooth_set_codec
#define bluetooth_set_codec_auto fake_bluetooth_set_codec_auto
#define bluetooth_codec_chosen fake_bluetooth_codec_chosen
#define bluetooth_ldac_quality fake_bluetooth_ldac_quality
#define bluetooth_set_ldac_quality fake_bluetooth_set_ldac_quality
#define bluetooth_codec_summary fake_bluetooth_codec_summary
#define bluetooth_headset_battery fake_bluetooth_headset_battery
#define bluetooth_local_codecs fake_bluetooth_local_codecs
#endif

// ---------------------------------------------------------------------------
// The page's parts
// ---------------------------------------------------------------------------

static lv_obj_t *bt_switch_card;
static lv_obj_t *bt_switch;
static lv_obj_t *status_label;
static lv_obj_t *connected_list; // the plaques: what is connected now
static lv_obj_t *codec_card;
static lv_obj_t *codec_pills;
static lv_obj_t *ldac_card;
static lv_obj_t *ldac_pill[4];
static lv_obj_t *coexist_note; // LDAC 990 and a busy Wi-Fi radio; hidden otherwise
static lv_obj_t *more_card;
static lv_obj_t *receiver_card;
static lv_obj_t *paired_label;
static lv_obj_t *paired_list;
static lv_obj_t *found_label;
static lv_obj_t *found_list;
static lv_obj_t *scan_btn;
static lv_obj_t *scan_icon;
static lv_timer_t *poll_timer;

static uint32_t drawn_serial = 0xFFFFFFFFu;
static bool drawn_enabled;

// The device a sheet is about. Copied, so the list can be rebuilt under it.
static char menu_mac[BT_MAC_MAX];
static char menu_name[BT_NAME_MAX];

// The last action requested. The radio reports the same "it worked" for
// connect, disconnect and forget alike, so this is what picks the wording of
// the confirmation. The confirmation names the action, not the device, which
// the list already shows two lines below.
typedef enum {
	ACTION_NONE = 0,
	ACTION_CONNECT,
	ACTION_DISCONNECT,
	ACTION_FORGET,
} last_action_t;

static last_action_t last_action;

static void rebuild_lists(void);

// ---------------------------------------------------------------------------
// Pairing watch
//
// Pairing is the slowest action on this page: bluez has to rediscover the
// device, pair it, and only then bring the A2DP link up -- half a minute on a
// good day, two discovery sweeps on a bad one. A timed toast would expire long
// before the answer, leaving no way to tell a slow success from a silent
// failure.
//
// The notice therefore stays up until the device is actually connected -- not
// paired, not "the command returned" -- watched by this timer. It is separate
// from the page poll timer because it must survive leaving the page: the modal
// lives on the top layer and would otherwise sit over whatever screen follows.
// ---------------------------------------------------------------------------

#define PAIRING_WATCH_MS 300
#define PAIRING_TIMEOUT_MS 90000

static lv_timer_t *pairing_timer;
static char pairing_mac[BT_MAC_MAX];
static char pairing_name[BT_NAME_MAX];
static uint32_t pairing_started_ms;

static bool pairing_watch_active(void) { return pairing_timer != NULL; }

static void pairing_watch_stop(void) {
	if (pairing_timer) {
		lv_timer_delete(pairing_timer);
		pairing_timer = NULL;
	}
	gui_modal_hide();
	pairing_mac[0] = '\0';
}

// True only once the link is really up: bluetooth_get_paired() reports the
// Connected flag bluez publishes on D-Bus, the same source the list is drawn
// from.
static bool pairing_device_connected(void) {
	bt_device_t devices[BT_MAX_DEVICES];
	int count = bluetooth_get_paired(devices, BT_MAX_DEVICES);
	for (int i = 0; i < count; i++) {
		if (devices[i].connected && strcmp(devices[i].mac, pairing_mac) == 0) {
			return true;
		}
	}
	return false;
}

static void pairing_watch_cb(lv_timer_t *timer) {
	(void)timer;

	if (!pairing_mac[0]) {
		pairing_watch_stop();
		return;
	}

	if (pairing_device_connected()) {
		bluetooth_take_op_result(NULL, 0); // consume it here so the page poll does not toast it too
		pairing_watch_stop();
		toast_success("bt_device_connected");
		topbar_refresh_radios();
		return;
	}

	// The worker gave up before the link came up.
	char reported[BT_NAME_MAX];
	if (bluetooth_take_op_result(reported, sizeof(reported)) == BT_OP_FAILED) {
		pairing_watch_stop();
		gui_notify_popup("bt_pairing_failed");
		return;
	}

	// The radio was switched off, or nothing ever came of the request.
	if (!bluetooth_get_enabled() || (lv_tick_get() - pairing_started_ms) > PAIRING_TIMEOUT_MS) {
		pairing_watch_stop();
		gui_notify_popup("bt_pairing_failed");
	}
}

static void pairing_watch_start(const char *mac, const char *name) {
	if (pairing_timer) {
		lv_timer_delete(pairing_timer);
		pairing_timer = NULL;
	}

	snprintf(pairing_mac, sizeof(pairing_mac), "%s", mac);
	snprintf(pairing_name, sizeof(pairing_name), "%s", name ? name : "");
	pairing_started_ms = lv_tick_get();

	// The Bluetooth glyph in the default accent blue, regardless of the accent
	// the user picked elsewhere: here the colour identifies the radio rather
	// than decorating the screen.
	gui_modal_show(&icon_bluetooth_connecting, theme_accent_preset(0), "pairing",
				   pairing_name[0] ? pairing_name : NULL);

	pairing_timer = lv_timer_create(pairing_watch_cb, PAIRING_WATCH_MS, NULL);
}

// ---------------------------------------------------------------------------
// The note under the switch
// ---------------------------------------------------------------------------

// What the radio is doing, said once: no radio here, coming up, going down, or
// off -- and then what to do about it. Up, the switch card's own subtitle says
// the name this player is seen as, and this line stands down.
static void refresh_status_label(void) {
	if (!status_label) {
		return;
	}

	const char *note = NULL;
	if (!bluetooth_available()) {
		note = tr("bt_no_hardware");
	} else if (bluetooth_busy()) {
		note = bluetooth_get_enabled() ? tr("turning_on") : tr("bt_turning_off");
	} else if (!bluetooth_get_enabled()) {
		note = tr("bt_off_note");
	}
	if (note) {
		lv_label_set_text(status_label, note);
	}
	lv_obj_set_hidden(status_label, note == NULL);

	char visible[BT_NAME_MAX + 32] = "";
	if (bluetooth_available() && bluetooth_get_enabled() && !bluetooth_busy()) {
		snprintf(visible, sizeof(visible), tr("bt_visible_as"), bluetooth_local_name());
	}
	sx_card_subtitle(bt_switch_card, visible, false);
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

static void connect_action(void *user) {
	(void)user;
	last_action = ACTION_CONNECT;
	bluetooth_connect(menu_mac);
}

static void disconnect_action(void *user) {
	(void)user;
	last_action = ACTION_DISCONNECT;
	bluetooth_disconnect(menu_mac);
}

static void forget_confirmed(void *user) {
	(void)user;
	last_action = ACTION_FORGET;
	bluetooth_forget(menu_mac);
}

// Forgetting a pair means putting it into pairing mode again to get it back:
// it asks first, in the red of the irreversible.
static void forget_action(void *user) {
	(void)user;
	sx_sheet_t sheet = {
		.title = menu_name[0] ? menu_name : menu_mac,
		.subtitle = tr("bt_forget_note"),
		.cancel_label = tr("cancel"),
		.action_label = tr("bt_forget"),
		.action_destructive = true,
		.on_action = forget_confirmed,
	};
	sx_sheet_open(&sheet);
}

static void airpods_action(void *user) {
	(void)user;
	switch_screen(airpodspage_screen);
}

// ---------------------------------------------------------------------------
// Renaming a device
//
// The name a paired device is shown under on this player, kept by bluez with
// the pairing. A sheet over the page with the field and the keyboard, as for
// the player's own name. Emptied, the field gives the device back the name it
// announces itself.
// ---------------------------------------------------------------------------

#define RENAME_CANCEL_SIZE 56

static lv_obj_t *rename_layer;
static lv_obj_t *rename_field;
static keyboard_t *rename_keyboard;
// The device being renamed. Its own copy: the sheet's can change under the
// layer if another row's sheet is opened from behind it.
static char rename_mac[BT_MAC_MAX];

static void rename_layer_hide(void) {
	if (rename_layer) {
		lv_obj_set_hidden(rename_layer, true);
		sx_mini_update_visibility(); // its keyboard comes or goes with it
	}
	back_btn_force_hidden(false);
}

static void rename_cancel_cb(lv_event_t *e) {
	(void)e;
	rename_layer_hide();
}

static void rename_accept_cb(lv_event_t *e) {
	(void)e;
	const char *typed = lv_textarea_get_text(rename_field);
	rename_layer_hide();
	if (bluetooth_rename(rename_mac, typed ? typed : "")) {
		toast_success("bt_name_changed");
	} else {
		toast_error("bt_bluetooth_off");
	}
}

static void rename_action(void *user) {
	(void)user;
	if (!rename_layer) {
		return;
	}
	snprintf(rename_mac, sizeof(rename_mac), "%s", menu_mac);

	// The field starts with the name the list shows.
	const char *current = "";
	bt_device_t devices[BT_MAX_DEVICES];
	int count = bluetooth_get_paired(devices, BT_MAX_DEVICES);
	for (int i = 0; i < count; i++) {
		if (strcmp(devices[i].mac, rename_mac) == 0) {
			current = devices[i].name;
			break;
		}
	}
	lv_textarea_set_text(rename_field, current);
	keyboard_reset(rename_keyboard);
	lv_obj_set_hidden(rename_layer, false);
	sx_mini_update_visibility(); // its keyboard comes or goes with it
	lv_obj_move_foreground(rename_layer);
	// The sheet has its own close in the corner, and the chevron under it would
	// leave the page without putting the keyboard away.
	back_btn_force_hidden(true);
}

static void build_rename_layer(gui_config_t *cfg) {
	rename_layer = lv_obj_create(btsettings_screen);
	lv_obj_set_size(rename_layer, lv_pct(100), lv_pct(100));
	lv_obj_align(rename_layer, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_add_style(rename_layer, &theme_style_screen, 0);
	lv_obj_set_style_bg_opa(rename_layer, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(rename_layer, 0, 0);
	lv_obj_set_style_radius(rename_layer, 0, 0);
	lv_obj_set_style_pad_all(rename_layer, 0, 0);
	lv_obj_set_scrollable(rename_layer, false);
	lv_obj_set_hidden(rename_layer, true);

	lv_obj_t *heading = lv_label_create(rename_layer);
	lv_label_set_text(heading, tr("rename"));
	lv_obj_add_style(heading, &theme_style_text, 0);
	lv_obj_set_style_text_font(heading, &font_ui_24, 0);
	lv_obj_align(heading, LV_ALIGN_TOP_LEFT, cfg->padding, cfg->padding + cfg->top_bar_height + 10);

	lv_obj_t *cancel = lv_btn_create(rename_layer);
	lv_obj_set_size(cancel, RENAME_CANCEL_SIZE, RENAME_CANCEL_SIZE);
	lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, -cfg->padding, cfg->padding + cfg->top_bar_height);
	lv_obj_set_style_bg_opa(cancel, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(cancel, 0, 0);
	lv_obj_set_style_shadow_width(cancel, 0, 0);
	lv_obj_set_style_pad_all(cancel, 0, 0);
	lv_obj_add_event_cb(cancel, rename_cancel_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *cancel_icon = lv_image_create(cancel);
	lv_image_set_src(cancel_icon, &icon_close);
	lv_obj_add_style(cancel_icon, &theme_style_icon, 0);
	lv_obj_center(cancel_icon);

	rename_field = lv_textarea_create(rename_layer);
	lv_textarea_set_one_line(rename_field, true);
	// One under bluez's own limit, so a name that fits the field is a name the
	// device keeps whole.
	lv_textarea_set_max_length(rename_field, BT_NAME_MAX - 1);
	lv_textarea_set_placeholder_text(rename_field, tr("name"));
	// The 2.0 input: the search field's 60 px pill on the surface colour.
	lv_obj_set_size(rename_field, cfg->screen_width - 2 * cfg->padding, 60);
	lv_obj_set_scrollbar_mode(rename_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(rename_field, LV_ALIGN_TOP_LEFT, cfg->padding, cfg->padding + cfg->top_bar_height + 60);
	lv_obj_add_style(rename_field, &theme_style_card, 0);
	lv_obj_add_style(rename_field, &sx_style_surface, 0);
	lv_obj_set_style_radius(rename_field, 30, 0);
	lv_obj_set_style_border_width(rename_field, 0, 0);
	lv_obj_set_style_shadow_width(rename_field, 0, 0);
	lv_obj_set_style_pad_ver(rename_field, 13, 0);
	lv_obj_set_style_pad_hor(rename_field, 22, 0);
	lv_obj_set_style_text_font(rename_field, &font_ui_24, 0);
	keyboard_style_caret(rename_field);

	rename_keyboard =
		keyboard_create(rename_layer, cfg->screen_width, 316, rename_field, NULL, "ok", rename_accept_cb, NULL);
}

static void row_free_cb(lv_event_t *e) { free(lv_event_get_user_data(e)); }

// What a device card hands its callbacks (the card's own user data belongs to
// sx_card).
typedef struct {
	char mac[BT_MAC_MAX];
	char name[BT_NAME_MAX];
	bool connected;
} dev_ref_t;

static void remember(const dev_ref_t *ref) {
	snprintf(menu_mac, sizeof(menu_mac), "%s", ref->mac);
	snprintf(menu_name, sizeof(menu_name), "%s", ref->name);
}

// The sheet for a known device, titled with its name: the plaque's on a tap,
// any paired one's on a long press. AirPods get the way to their own page.
static void show_device_menu(bool connected) {
	airpods_state_t pods;
	bool airpods = connected && airpods_get(&pods);
	sx_sheet_item_t items[4];
	int n = 0;
	if (connected) {
		items[n++] = (sx_sheet_item_t){.label = tr("disconnect"), .action = disconnect_action};
		if (airpods) {
			items[n++] = (sx_sheet_item_t){.label = tr("airpods"), .action = airpods_action};
		}
	} else {
		items[n++] = (sx_sheet_item_t){.label = tr("connect"), .action = connect_action};
	}
	items[n++] = (sx_sheet_item_t){.label = tr("rename"), .action = rename_action};
	items[n++] = (sx_sheet_item_t){.label = tr("bt_forget"), .destructive = true, .action = forget_action};
	sx_sheet_menu(menu_name[0] ? menu_name : menu_mac, NULL, items, n);
}

// A tap on a paired device connects it; on the connected one it opens the
// sheet instead, since connecting again would be a no-op.
static void paired_clicked_cb(lv_event_t *e) {
	const dev_ref_t *ref = lv_event_get_user_data(e);
	if (!ref || !ref->mac[0]) {
		return;
	}
	remember(ref);
	if (ref->connected) {
		show_device_menu(true);
	} else {
		last_action = ACTION_CONNECT;
		bluetooth_connect(ref->mac);
	}
}

static void paired_long_pressed_cb(lv_event_t *e) {
	const dev_ref_t *ref = lv_event_get_user_data(e);
	if (!ref || !ref->mac[0]) {
		return;
	}
	// The lift that ends this hold would otherwise arrive as a tap as well and
	// connect the headphones under the sheet just opened about them.
	lv_indev_t *indev = lv_indev_active();
	if (indev) {
		lv_indev_wait_release(indev);
	}
	remember(ref);
	show_device_menu(ref->connected);
}

static void found_clicked_cb(lv_event_t *e) {
	const dev_ref_t *ref = lv_event_get_user_data(e);
	if (!ref || !ref->mac[0] || pairing_watch_active()) {
		return;
	}
	// The pairing notice names the headphones, not their address.
	bluetooth_pair(ref->mac);
	pairing_watch_start(ref->mac, ref->name);
}

// ---------------------------------------------------------------------------
// The codec and LDAC's bit rate
//
// 1.2.5 had them on a page of their own, a row per codec and four for LDAC;
// 2.0 puts them on this page as two rows of pills, on the prototype's terms:
// the codec -- "Auto" and what the headphones offer, best first -- only while
// something is connected, since the list is the headphones' answer; LDAC's
// rate whenever this player can send LDAC, faded while the link is on
// another codec (it is kept per pair and as the default for the next pair, so
// it can be set with nothing connected).
// ---------------------------------------------------------------------------

#define CODEC_PILLS_MAX (BT_MAX_CODECS + 1)

static char codec_names[BT_MAX_CODECS][BT_CODEC_MAX];
static int codec_count;
static lv_obj_t *codec_pill[CODEC_PILLS_MAX]; // 0 is "Auto"
static char drawn_codecs[BT_MAX_CODECS * BT_CODEC_MAX];

static const char *const LDAC_MODES[4] = {"high", "standard", "mobile", "abr"};
static const char *const LDAC_LABELS[4] = {"990", "660", "330", "ABR"};

// Best first, which is the order the automatic choice walks.
static int codec_order(const char *name) {
	if (strcasecmp(name, "LDAC") == 0) {
		return 50;
	}
	if (strcasecmp(name, "aptX-HD") == 0) {
		return 40;
	}
	if (strcasecmp(name, "aptX") == 0) {
		return 30;
	}
	if (strcasecmp(name, "AAC") == 0) {
		return 20;
	}
	if (strcasecmp(name, "SBC") == 0) {
		return 10;
	}
	return 5;
}

static void codec_pick_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index == 0) {
		bluetooth_set_codec_auto();
	} else if (index - 1 < codec_count) {
		bluetooth_set_codec(codec_names[index - 1]);
	}
	if (poll_timer) {
		lv_timer_ready(poll_timer);
	}
}

static bool link_is_ldac(void) {
	char offered[BT_MAX_CODECS][BT_CODEC_MAX];
	char selected[BT_CODEC_MAX] = "";
	bluetooth_get_codecs(offered, BT_MAX_CODECS, selected, sizeof(selected));
	return strcasecmp(selected, "LDAC") == 0;
}

static void ldac_pick_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	int mode = (int)(intptr_t)lv_event_get_user_data(e);
	// Faded with the link on another codec: say why rather than do nothing.
	if (bluetooth_audio_active() && !link_is_ldac()) {
		toast_plain("bt_ldac_needs_ldac");
		return;
	}
	if (mode >= 0 && mode < 4) {
		bluetooth_set_ldac_quality(LDAC_MODES[mode]);
	}
	if (poll_timer) {
		lv_timer_ready(poll_timer);
	}
}

// Whether this player can send LDAC at all: a property of the firmware's
// bluealsa, known once the stack is up.
static bool can_send_ldac(void) {
	char mine[BT_MAX_CODECS][BT_CODEC_MAX];
	int count = bluetooth_local_codecs(mine, BT_MAX_CODECS, false);
	for (int i = 0; i < count; i++) {
		if (strcasecmp(mine[i], "LDAC") == 0) {
			return true;
		}
	}
	return false;
}

// Whether the Wi-Fi radio is moving data in bulk right now: a track being
// cached for a service or a podcast, or a firmware update downloading.
static bool wifi_downloading(void) {
	return stream_provider_any_downloading() || podcastcache_downloading_id() > 0 || ota_busy();
}

static void refresh_codec_cards(void) {
	bool on = bluetooth_get_enabled() && bluetooth_available();
	bool active = on && bluetooth_audio_active();

	char selected[BT_CODEC_MAX] = "";
	char offered[BT_MAX_CODECS][BT_CODEC_MAX];
	int count = active ? bluetooth_get_codecs(offered, BT_MAX_CODECS, selected, sizeof(selected)) : 0;
	if (count < 0) {
		count = 0;
	}

	// The pills are rebuilt only when the offer changes.
	char key[sizeof(drawn_codecs)] = "";
	size_t used = 0;
	for (int i = 0; i < count && used < sizeof(key); i++) {
		used += (size_t)snprintf(key + used, sizeof(key) - used, "%s,", offered[i]);
	}
	if (strcmp(key, drawn_codecs) != 0) {
		snprintf(drawn_codecs, sizeof(drawn_codecs), "%s", key);
		lv_obj_clean(codec_pills);
		memset(codec_pill, 0, sizeof(codec_pill));
		codec_count = 0;
		for (int i = 0; i < count && codec_count < BT_MAX_CODECS; i++) {
			snprintf(codec_names[codec_count++], BT_CODEC_MAX, "%.*s", BT_CODEC_MAX - 1, offered[i]);
		}
		for (int i = 0; i < codec_count; i++) {
			for (int j = i + 1; j < codec_count; j++) {
				if (codec_order(codec_names[j]) > codec_order(codec_names[i])) {
					char swap[BT_CODEC_MAX];
					memcpy(swap, codec_names[i], BT_CODEC_MAX);
					memcpy(codec_names[i], codec_names[j], BT_CODEC_MAX);
					memcpy(codec_names[j], swap, BT_CODEC_MAX);
				}
			}
		}
		if (codec_count > 0) {
			codec_pill[0] = settingsrow_pill(codec_pills, "bt_codec_auto_short", 0, codec_pick_cb);
			for (int i = 0; i < codec_count; i++) {
				codec_pill[i + 1] = settingsrow_pill_text(codec_pills, codec_names[i], i + 1, codec_pick_cb);
			}
		}
	}
	lv_obj_set_hidden(codec_card, codec_count == 0);

	// "Auto" while nothing is remembered for these headphones, else the one
	// asked for.
	char chosen[BT_CODEC_MAX] = "";
	bool by_hand = active && bluetooth_codec_chosen(chosen, sizeof(chosen));
	settingsrow_pill_active(codec_pill[0], !by_hand);
	for (int i = 0; i < codec_count; i++) {
		settingsrow_pill_active(codec_pill[i + 1], by_hand && strcasecmp(chosen, codec_names[i]) == 0);
	}

	// LDAC's rate.
	bool ldac_here = on && can_send_ldac();
	lv_obj_set_hidden(ldac_card, !ldac_here);
	const char *quality = bluetooth_ldac_quality();
	for (int i = 0; i < 4; i++) {
		settingsrow_pill_active(ldac_pill[i], quality && strcmp(quality, LDAC_MODES[i]) == 0);
	}
	bool ldac_now = active && strcasecmp(selected, "LDAC") == 0;
	sx_card_set_dim(ldac_card, active && !ldac_now);

	// LDAC 990 with the Wi-Fi radio busy: the one rate that leaves it no room.
	bool ldac_990 = quality && strcmp(quality, "high") == 0;
	bool held = bluetooth_ldac_held_back();
	bool show = ldac_here && active && ldac_990 && (held || wifi_downloading());
	if (show) {
		const char *text = tr(held ? "btaudio_ldac_wifi_transfer" : "btaudio_ldac_wifi_busy");
		if (strcmp(lv_label_get_text(coexist_note), text) != 0) {
			lv_label_set_text(coexist_note, text);
		}
	}
	lv_obj_set_hidden(coexist_note, !show);
}

// ---------------------------------------------------------------------------
// The lists
// ---------------------------------------------------------------------------

static dev_ref_t *make_ref(const bt_device_t *device) {
	dev_ref_t *ref = malloc(sizeof(*ref));
	if (ref) {
		snprintf(ref->mac, sizeof(ref->mac), "%.*s", BT_MAC_MAX - 1, device->mac);
		snprintf(ref->name, sizeof(ref->name), "%.*s", BT_NAME_MAX - 1, device->name);
		ref->connected = device->connected;
	}
	return ref;
}

static lv_obj_t *device_card(lv_obj_t *parent, const bt_device_t *device, dev_ref_t *ref) {
	lv_obj_t *card = sx_card_create(parent);
	lv_obj_t *title = sx_card_title(card, device->name);
	lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
	lv_obj_set_height(title, lv_font_get_line_height(&font_ui_24));
	lv_obj_add_event_cb(card, row_free_cb, LV_EVENT_DELETE, ref);
	return card;
}

// The headphones the sound is going to: the plaque, with what the link carries
// in the mono line -- "LDAC 990 · 96 kHz", the headset's own charge after it
// when it says ("· 78 %").
static void add_connected(const bt_device_t *device) {
	dev_ref_t *ref = make_ref(device);
	if (!ref) {
		return;
	}
	lv_obj_t *card = device_card(connected_list, device, ref);
	sx_card_set_active(card, true);

	char summary[80];
	if (bluetooth_codec_summary(device->mac, summary, sizeof(summary))) {
		int percent = bluetooth_headset_battery(NULL);
		if (percent >= 0) {
			size_t used = strlen(summary);
			snprintf(summary + used, sizeof(summary) - used, " \xC2\xB7 %d %%", percent);
		}
		sx_card_subtitle(card, summary, true);
	} else {
		sx_card_subtitle(card, tr("connected"), true);
	}
	sx_card_on_click(card, paired_clicked_cb, ref);
	lv_obj_add_event_cb(card, paired_long_pressed_cb, LV_EVENT_LONG_PRESSED, ref);
}

static void add_paired(const bt_device_t *device) {
	dev_ref_t *ref = make_ref(device);
	if (!ref) {
		return;
	}
	lv_obj_t *card = device_card(paired_list, device, ref);
	sx_card_value(card, tr("connect"));
	sx_card_on_click(card, paired_clicked_cb, ref);
	lv_obj_add_event_cb(card, paired_long_pressed_cb, LV_EVENT_LONG_PRESSED, ref);
}

static void add_found(const bt_device_t *device) {
	dev_ref_t *ref = make_ref(device);
	if (!ref) {
		return;
	}
	lv_obj_t *card = device_card(found_list, device, ref);
	sx_card_on_click(card, found_clicked_cb, ref);
}

// `spin` puts a spinner left of the text: without something moving, "nothing
// here" and "still searching" read the same.
static void add_placeholder(lv_obj_t *parent, const char *text, bool spin) {
	lv_obj_t *row = lv_obj_create(parent);
	lv_obj_remove_style_all(row);
	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_height(row, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_top(row, 4, 0);
	lv_obj_set_style_pad_hor(row, 6, 0);
	lv_obj_set_style_pad_column(row, 10, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	if (spin) {
		spinner_create(row, &icon_loader_small);
	}

	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text(label, tr(text));
	lv_obj_add_style(label, &sx_style_text2, 0);
	lv_obj_set_style_text_font(label, &font_ui_21, 0);
}

static void rebuild_lists(void) {
	lv_obj_clean(connected_list);
	lv_obj_clean(paired_list);
	lv_obj_clean(found_list);

	bool on = bluetooth_get_enabled() && bluetooth_available();
	lv_obj_set_hidden(more_card, !on);
	lv_obj_set_hidden(receiver_card, !on);
	refresh_codec_cards();
	if (!on) {
		lv_obj_set_hidden(connected_list, true);
		lv_obj_set_hidden(paired_label, true);
		lv_obj_set_hidden(paired_list, true);
		lv_obj_set_hidden(found_label, true);
		lv_obj_set_hidden(found_list, true);
		return;
	}

	bt_device_t devices[BT_MAX_DEVICES];
	int count = bluetooth_get_paired(devices, BT_MAX_DEVICES);
	int connected = 0;
	for (int i = 0; i < count; i++) {
		if (devices[i].connected) {
			add_connected(&devices[i]);
			connected++;
		}
	}
	lv_obj_set_hidden(connected_list, connected == 0);

	int others = 0;
	for (int i = 0; i < count; i++) {
		if (!devices[i].connected) {
			add_paired(&devices[i]);
			others++;
		}
	}
	// "None" only when there is nothing known at all: a heading over the
	// word "None" under a plaque would be furniture.
	if (others == 0 && connected == 0) {
		add_placeholder(paired_list, "bt_none", false);
	}
	lv_obj_set_hidden(paired_label, others == 0 && connected > 0);
	lv_obj_set_hidden(paired_list, others == 0 && connected > 0);

	// The "other devices" heading appears only once a scan is running or has
	// found something.
	count = bluetooth_get_found(devices, BT_MAX_DEVICES);
	if (count == 0) {
		bool searching = bluetooth_scan_running();
		lv_obj_set_hidden(found_label, !searching);
		lv_obj_set_hidden(found_list, !searching);
		if (searching) {
			add_placeholder(found_list, "searching", true);
		}
		return;
	}
	lv_obj_set_hidden(found_label, false);
	lv_obj_set_hidden(found_list, false);
	for (int i = 0; i < count; i++) {
		add_found(&devices[i]);
	}
}

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------

static void switch_changed_cb(lv_event_t *e) {
	(void)e;
	bool on = lv_obj_has_state(bt_switch, LV_STATE_CHECKED);

	if (on && !bluetooth_available()) {
		lv_obj_remove_state(bt_switch, LV_STATE_CHECKED);
		gui_notify_popup("bt_bluetooth_not_available");
		return;
	}

	bluetooth_set_enabled(on);
	refresh_status_label();
	rebuild_lists();
	topbar_refresh_radios();
}

// The radio is up and answering, which is not the same as the switch being on:
// that is true from the moment it is tapped and stays true for the ten seconds
// the stack needs to load. A scan started in that window finds nothing and
// looks broken, so the scan button stays disabled until this holds.
static bool radio_ready(void) {
	return bluetooth_available() && bluetooth_get_enabled() && !bluetooth_busy();
}

// The receiver page is a trap once it is receiving, so it is not somewhere to
// arrive by accident with the radio off.
static void receiver_clicked_cb(lv_event_t *e) {
	(void)e;
	if (!radio_ready()) {
		return;
	}
	// With headphones on the other end this device is the source, and receiver
	// mode is the opposite arrangement: the output is already taken and there is
	// nothing for a phone to stream into, so the page is not opened at all.
	if (bluetooth_audio_active()) {
		gui_notify_popup("bt_disconnect_headphones_first");
		return;
	}
	switch_screen(btreceiverpage_screen);
}

static void scan_clicked_cb(lv_event_t *e) {
	(void)e;
	if (!radio_ready()) {
		return;
	}
	if (bluetooth_scan_running()) {
		bluetooth_scan_stop();
	} else {
		bluetooth_scan_start();
	}
	refresh_status_label();
	rebuild_lists();
}

static void poll_cb(lv_timer_t *timer) {
	(void)timer;

	bool on = bluetooth_get_enabled();
	if (lv_obj_has_state(bt_switch, LV_STATE_CHECKED) != on) {
		if (on) {
			lv_obj_add_state(bt_switch, LV_STATE_CHECKED);
		} else {
			lv_obj_remove_state(bt_switch, LV_STATE_CHECKED);
		}
	}

	refresh_status_label();

	// The headset's battery and the link's codec move on their own, without the
	// device list changing: they are watched apart and cost a rebuild only when
	// the plaque would say something different.
	static int drawn_battery = -1;
	static char drawn_summary[80];
	int battery = bluetooth_headset_battery(NULL);
	char summary[80] = "";
	bluetooth_codec_summary(NULL, summary, sizeof(summary));
	uint32_t serial = bluetooth_devices_serial();
	if (serial != drawn_serial || on != drawn_enabled || battery != drawn_battery ||
		strcmp(summary, drawn_summary) != 0) {
		drawn_serial = serial;
		drawn_enabled = on;
		drawn_battery = battery;
		snprintf(drawn_summary, sizeof(drawn_summary), "%s", summary);
		rebuild_lists();
	} else {
		refresh_codec_cards();
	}

	// Scan button: disabled while the stack is still loading, faded while a
	// sweep is running (tapping it then stops the sweep), solid otherwise.
	bool ready = radio_ready();
	if (scan_btn) {
		if (ready) {
			lv_obj_remove_state(scan_btn, LV_STATE_DISABLED);
		} else {
			lv_obj_add_state(scan_btn, LV_STATE_DISABLED);
		}
	}
	if (scan_icon) {
		lv_obj_set_style_image_opa(scan_icon,
								   !ready ? LV_OPA_20 : (bluetooth_scan_running() ? LV_OPA_50 : LV_OPA_COVER), 0);
	}
	sx_card_set_dim(receiver_card, !ready);

	// While a pairing is in flight its own watcher owns the result: consuming
	// it here too would mean whichever timer ran first swallowed it.
	if (pairing_watch_active()) {
		return;
	}

	char name[BT_NAME_MAX];
	switch (bluetooth_take_op_result(name, sizeof(name))) {
	case BT_OP_OK:
		switch (last_action) {
		case ACTION_CONNECT:
			toast_success("bt_device_connected");
			break;
		case ACTION_DISCONNECT:
			toast_success("bt_device_disconnected");
			break;
		case ACTION_FORGET:
			toast_success("bt_device_removed");
			break;
		default:
			toast_success("done");
			break;
		}
		last_action = ACTION_NONE;
		topbar_refresh_radios();
		break;
	case BT_OP_FAILED:
		last_action = ACTION_NONE;
		gui_notify_popup("bt_connect_failed");
		break;
	default:
		break;
	}
}

// The control centre switched the radio with the page open under it. The poll
// already follows the radio; this only saves waiting for its next round.
static void switched_elsewhere_cb(lv_event_t *e) {
	(void)e;
	if (poll_timer) {
		lv_timer_ready(poll_timer);
	}
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	drawn_serial = 0xFFFFFFFFu;
	lv_timer_resume(poll_timer);
	lv_timer_ready(poll_timer);
	bluetooth_refresh();
	bluetooth_refresh_codecs();
	// Visible to other devices only from here. Searching and being searched for
	// are the two halves of the same page: without this a phone looking for the
	// player finds nothing, whatever it does.
	bluetooth_set_discoverable(true);
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	// A layer left open would greet the next visit.
	if (rename_layer && !lv_obj_is_hidden(rename_layer)) {
		rename_layer_hide();
	}
	lv_timer_pause(poll_timer);
	bluetooth_set_discoverable(false);
	if (bluetooth_scan_running()) {
		bluetooth_scan_stop(); // a scan nobody is watching only costs battery
	}
}

// Corner buttons, laid out from the right edge as on the Music page.
static lv_obj_t *corner_button(lv_obj_t *screen, gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph,
							   lv_obj_t **icon_out) {
	lv_obj_t *button = lv_btn_create(screen);
	lv_obj_set_size(button, CORNER_BTN_SIZE, CORNER_BTN_SIZE);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -cfg->padding - slot * (CORNER_BTN_SIZE + 6),
				 cfg->padding + cfg->top_bar_height);

	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, glyph);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_center(icon);
	if (icon_out) {
		*icon_out = icon;
	}

	return button;
}

static lv_obj_t *make_box(lv_obj_t *container) {
	lv_obj_t *box = lv_obj_create(container);
	lv_obj_set_width(box, lv_pct(100));
	lv_obj_set_height(box, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(box, 0, 0);
	lv_obj_set_style_border_width(box, 0, 0);
	lv_obj_set_style_pad_all(box, 0, 0);
	lv_obj_set_style_pad_gap(box, SX_CARD_GAP, 0);
	lv_obj_set_scrollable(box, false);
	lv_obj_set_event_bubble(box, true);
	lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
	return box;
}

static void more_clicked_cb(lv_event_t *e) {
	(void)e;
	switch_screen(btaudio_screen);
}

// The 2.0 page (the handoff's "Bluetooth"): the switch; the headphones
// connected now as the plaque; their codec and LDAC's rate on pills; the
// encoder's and the headset's other options one tap away under "More"; then
// the other paired devices and whatever a search finds.
void btsettings_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(btsettings_screen, cfg, "bluetooth");

	bt_switch_card = settingsrow_switch_card(container, "bluetooth", NULL, &bt_switch, switch_changed_cb);
	if (bluetooth_get_enabled()) {
		lv_obj_add_state(bt_switch, LV_STATE_CHECKED);
	}

	status_label = lv_label_create(container);
	lv_obj_set_width(status_label, lv_pct(100));
	lv_obj_add_style(status_label, &sx_style_text2, 0);
	lv_obj_set_style_text_font(status_label, &font_ui_20, 0);
	lv_obj_set_style_pad_hor(status_label, 6, 0);
	lv_obj_set_style_pad_top(status_label, 4, 0);
	lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);

	connected_list = make_box(container);

	codec_card = settingsrow_pills(container, "bt_codec", &codec_pills);
	lv_obj_set_hidden(codec_card, true);

	lv_obj_t *ldac_pills = NULL;
	ldac_card = settingsrow_pills(container, "bt_ldac_quality", &ldac_pills);
	for (int i = 0; i < 4; i++) {
		ldac_pill[i] = settingsrow_pill_text(ldac_pills, LDAC_LABELS[i], i, ldac_pick_cb);
	}
	coexist_note = lv_label_create(ldac_card);
	lv_label_set_long_mode(coexist_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(coexist_note, lv_pct(100));
	lv_obj_add_style(coexist_note, &sx_style_text2, 0);
	lv_obj_set_style_text_font(coexist_note, &font_ui_20, 0);
	lv_label_set_text(coexist_note, "");
	lv_obj_set_hidden(coexist_note, true);

	more_card = sx_card_create(container);
	sx_card_title(more_card, tr("bt_more"));
	sx_card_subtitle(more_card, tr("bt_more_note"), false);
	sx_card_chevron(more_card);
	sx_card_on_click(more_card, more_clicked_cb, NULL);

	receiver_card = sx_card_create(container);
	sx_card_title(receiver_card, tr("btreceiver_title"));
	sx_card_subtitle(receiver_card, tr("bt_receiver_note"), false);
	sx_card_chevron(receiver_card);
	sx_card_on_click(receiver_card, receiver_clicked_cb, NULL);

	paired_label = settingsrow_group(container, "bt_paired_devices", -1);
	paired_list = make_box(container);
	found_label = settingsrow_group(container, "bt_other_devices", -1);
	found_list = make_box(container);
	lv_obj_set_style_pad_bottom(found_list, 12, 0);

	// One button in the header, the search: the gear and the receiver of 1.2.5
	// are cards on the page now ("More", "Bluetooth receiver").
	scan_btn = corner_button(btsettings_screen, cfg, 0, &icon_bluetooth_search, &scan_icon);
	lv_obj_add_event_cb(scan_btn, scan_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_state(scan_btn, LV_STATE_DISABLED); // enabled by the poll once the radio answers
	lv_obj_set_style_image_opa(scan_icon, LV_OPA_20, 0);

	poll_timer = lv_timer_create(poll_cb, BT_PAGE_POLL_MS, NULL);
	lv_timer_pause(poll_timer);

	lv_obj_add_event_cb(btsettings_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(btsettings_screen, switched_elsewhere_cb, LV_EVENT_REFRESH, NULL);
	lv_obj_add_event_cb(btsettings_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(btsettings_screen);

	build_rename_layer(cfg);

	refresh_status_label();
	rebuild_lists();
}

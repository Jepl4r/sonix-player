#include "wifisettings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/spinner.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/sx_card.h"
#include "src/gui/shell/sx_sheet.h"
#include "src/gui/shell/sx_styles.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/topbar.h"
#include "src/gui/shell/touch.h"
#include "src/system/core/lang.h"
#include "src/system/net/wifi.h"

lv_obj_t *wifisettings_screen;

// ---------------------------------------------------------------------------
// The emulator's radio
//
// The host has no Wi-Fi, so the page could only ever be looked at switched
// off. With SONIX_WIFI_FAKE set (host build only) the calls below answer for a
// radio with four networks around, one of them joined, and joining, leaving
// and forgetting move that around -- enough to see every state of the page.
// On the device none of this exists: the names are the real ones.
// ---------------------------------------------------------------------------

#ifdef HOST_BUILD
static bool fake_on(void) {
	static int on = -1;
	if (on < 0) {
		const char *env = getenv("SONIX_WIFI_FAKE");
		on = env && env[0] ? 1 : 0;
	}
	return on == 1;
}

static bool fake_enabled = true;
static char fake_current[WIFI_SSID_MAX] = "Home_5G";
static uint32_t fake_serial = 1;
static wifi_op_t fake_op;
static wifi_opkind_t fake_kind;
static char fake_op_ssid[WIFI_SSID_MAX];

static bool fake_wifi_available(void) { return fake_on() || wifi_available(); }
static bool fake_wifi_get_enabled(void) { return fake_on() ? fake_enabled : wifi_get_enabled(); }
static void fake_wifi_set_enabled(bool on) {
	if (!fake_on()) {
		wifi_set_enabled(on);
		return;
	}
	fake_enabled = on;
	fake_serial++;
}
static bool fake_wifi_busy(void) { return fake_on() ? false : wifi_busy(); }
static bool fake_wifi_scan_running(void) { return fake_on() ? false : wifi_scan_running(); }
static void fake_wifi_scan_start(void) {
	if (!fake_on()) {
		wifi_scan_start();
	}
}
static uint32_t fake_wifi_networks_serial(void) { return fake_on() ? fake_serial : wifi_networks_serial(); }
static void fake_wifi_get_status(wifi_status_t *out) {
	if (!fake_on()) {
		wifi_get_status(out);
		return;
	}
	memset(out, 0, sizeof(*out));
	out->state = !fake_enabled ? WIFI_STATE_OFF : fake_current[0] ? WIFI_STATE_CONNECTED : WIFI_STATE_ON;
	if (out->state == WIFI_STATE_CONNECTED) {
		snprintf(out->ssid, sizeof(out->ssid), "%s", fake_current);
		snprintf(out->ip, sizeof(out->ip), "192.168.1.42");
		snprintf(out->mac, sizeof(out->mac), "c0:84:7d:12:34:56");
		bool five = strstr(fake_current, "5G") != NULL;
		out->signal = five ? -52 : -61;
		out->bars = 3;
		out->freq_mhz = five ? 5180 : 2437;
		out->link_mbps = five ? 433 : 72;
	}
}
static int fake_wifi_get_networks(wifi_network_t *out, int max) {
	if (!fake_on()) {
		return wifi_get_networks(out, max);
	}
	static const struct {
		const char *ssid;
		int signal;
		bool secured;
		bool saved;
	} NETS[] = {{"Home_5G", -52, true, true},
				{"Home_2G", -58, true, true},
				{"Sonix-Guest", -67, false, false},
				{"MTS_Router_7B", -79, true, false}};
	int n = 0;
	for (size_t i = 0; i < sizeof(NETS) / sizeof(NETS[0]) && n < max; i++) {
		wifi_network_t *net = &out[n++];
		memset(net, 0, sizeof(*net));
		snprintf(net->ssid, sizeof(net->ssid), "%s", NETS[i].ssid);
		net->signal = NETS[i].signal;
		net->bars = NETS[i].signal > -55 ? 3 : NETS[i].signal > -67 ? 2 : 1;
		net->secured = NETS[i].secured;
		net->saved = NETS[i].saved;
		net->current = strcmp(fake_current, NETS[i].ssid) == 0;
		net->id = net->saved ? (int)i : -1;
	}
	return n;
}
static void fake_result(wifi_opkind_t kind, const char *ssid) {
	fake_op = WIFI_OP_OK;
	fake_kind = kind;
	snprintf(fake_op_ssid, sizeof(fake_op_ssid), "%s", ssid ? ssid : "");
	fake_serial++;
}
static void fake_wifi_connect(const char *ssid, const char *psk) {
	if (!fake_on()) {
		wifi_connect(ssid, psk);
		return;
	}
	snprintf(fake_current, sizeof(fake_current), "%s", ssid);
	fake_result(WIFI_OPKIND_CONNECT, ssid);
}
static void fake_wifi_disconnect(void) {
	if (!fake_on()) {
		wifi_disconnect();
		return;
	}
	fake_current[0] = '\0';
	fake_result(WIFI_OPKIND_DISCONNECT, NULL);
}
static void fake_wifi_forget(const char *ssid) {
	if (!fake_on()) {
		wifi_forget(ssid);
		return;
	}
	if (strcmp(fake_current, ssid) == 0) {
		fake_current[0] = '\0';
	}
	fake_result(WIFI_OPKIND_FORGET, NULL);
}
static wifi_op_t fake_wifi_take_op_result(char *ssid_out, int ssid_size) {
	if (!fake_on()) {
		return wifi_take_op_result(ssid_out, ssid_size);
	}
	wifi_op_t op = fake_op;
	fake_op = WIFI_OP_IDLE;
	snprintf(ssid_out, (size_t)ssid_size, "%s", fake_op_ssid);
	return op;
}
static wifi_opkind_t fake_wifi_last_op_kind(void) { return fake_on() ? fake_kind : wifi_last_op_kind(); }

#define wifi_available fake_wifi_available
#define wifi_get_enabled fake_wifi_get_enabled
#define wifi_set_enabled fake_wifi_set_enabled
#define wifi_busy fake_wifi_busy
#define wifi_scan_running fake_wifi_scan_running
#define wifi_scan_start fake_wifi_scan_start
#define wifi_networks_serial fake_wifi_networks_serial
#define wifi_get_status fake_wifi_get_status
#define wifi_get_networks fake_wifi_get_networks
#define wifi_connect fake_wifi_connect
#define wifi_disconnect fake_wifi_disconnect
#define wifi_forget fake_wifi_forget
#define wifi_take_op_result fake_wifi_take_op_result
#define wifi_last_op_kind fake_wifi_last_op_kind
#endif

// How often the page looks at what the radio module has found out. The work
// itself happens on the wifi worker; this only redraws.
#define WIFI_PAGE_POLL_MS 500

#define WIFI_ROW_H 82
#define CORNER_BTN_SIZE 56

static lv_obj_t *wifi_switch;
static lv_obj_t *status_label;
// The network in use: the plaque (sx_card_set_active) with its figures in the
// mono line, built once and filled on every redraw, hidden with nothing joined.
static lv_obj_t *current_card;
static char current_ssid[WIFI_SSID_MAX];
static lv_obj_t *section_label;
static lv_obj_t *list;
static lv_obj_t *scan_btn;
static lv_obj_t *scan_icon;
static lv_timer_t *poll_timer;

static uint32_t drawn_serial = 0xFFFFFFFFu;
static bool drawn_enabled;
// Whether a scan was running the last time the list was drawn.
//
// The list is otherwise rebuilt only when the networks found change, and with
// an empty list nothing changes: switching Wi-Fi on would leave "no networks
// found" on screen for the whole sweep, which is exactly when the right answer
// is "searching".
static bool drawn_scanning;

// Whether a sweep has been asked for since the radio came up.
//
// Turning Wi-Fi on does not start one -- the page does that when it is opened,
// and the switch is on the page -- so between the radio answering and the first
// results nothing is running and nothing is found, which is not the same as
// "no networks found". The flag makes the page start that first sweep itself,
// and until it has, the list says it is searching.
static bool scanned_since_on;

// The passphrase page, built once and pointed at whichever network asked.
static lv_obj_t *password_screen;
static lv_obj_t *password_title;
static lv_obj_t *password_field;
static keyboard_t *password_keyboard;
static char pending_ssid[WIFI_SSID_MAX];

// How long the last typed character stays readable before becoming a bullet.
// Zero: the character never appears, not even for a moment. Showing the last
// letter typed is LVGL's default (1.5 s) and it is the wrong trade here -- the
// eye button is right there for anyone who wants to read what they typed, and
// a password that flashes a letter at a time in a room is a password read over
// a shoulder.
#define PASSWORD_SHOW_MS 0

static lv_obj_t *password_eye_btn;
static lv_obj_t *password_eye_icon;

// The icon shows what the next tap WILL DO: with the password hidden it is the
// open eye ("show it"), with the password in clear the crossed-out eye ("hide
// it again").
static void password_eye_refresh(void) {
	if (!password_eye_icon || !password_field) {
		return;
	}
	bool hidden = lv_textarea_get_password_mode(password_field);
	lv_image_set_src(password_eye_icon, hidden ? &icon_eye : &icon_eye_off);
	// The tight kerning only applies while the bullets are showing.
	keyboard_refresh_password(password_field);
}

static void password_eye_cb(lv_event_t *e) {
	(void)e;
	bool hidden = lv_textarea_get_password_mode(password_field);
	lv_textarea_set_password_mode(password_field, !hidden);
	password_eye_refresh();
}

// The details page: everything about the link that does not belong on the
// list. Built once, filled in when it is opened.
static lv_obj_t *details_wifi_screen;
static lv_obj_t *details_ssid_value;
static lv_obj_t *details_ip_value;
static lv_obj_t *details_mac_value;
static lv_obj_t *details_signal_value;
static lv_obj_t *details_security_value;

// The network a sheet is about. The row that owns the string outlives the
// menu, but copying it keeps the two independent.
static char menu_ssid[WIFI_SSID_MAX];

static void rebuild_list(void);

// ---------------------------------------------------------------------------
// bits and pieces
// ---------------------------------------------------------------------------

static const lv_image_dsc_t *glyph_for_bars(int bars) {
	switch (bars) {
	case 3:
		return &icon_wifi_max;
	case 2:
		return &icon_wifi_high;
	case 1:
		return &icon_wifi_low;
	default:
		return &icon_wifi_zero;
	}
}

// Whether the page should be showing a spinner where the list of other
// networks goes. Not just "a sweep is running": the seconds while the radio is
// still coming up, and the gap before the first sweep is asked for, look
// exactly the same to the user and must read the same way.
static bool searching_now(void) {
	if (!wifi_get_enabled() || !wifi_available()) {
		return false;
	}
	return wifi_scan_running() || wifi_busy() || !scanned_since_on;
}

static void refresh_status_label(void) {
	if (!status_label) {
		return;
	}

	if (!wifi_available()) {
		lv_label_set_text(status_label, tr("wifi_no_hardware"));
		lv_obj_set_hidden(status_label, false);
		return;
	}

	if (wifi_busy()) {
		lv_label_set_text(status_label, wifi_get_enabled() ? tr("turning_on") : tr("wifi_turning_off_2"));
		lv_obj_set_hidden(status_label, false);
		return;
	}

	wifi_status_t status;
	wifi_get_status(&status);

	// With the radio up this line stands down: the plaque says which network
	// is in use and the list what else is around. Connecting is said on the
	// plaque itself.
	if (status.state == WIFI_STATE_OFF) {
		// What being off costs, which is the reason to look at this page.
		lv_label_set_text(status_label, tr("wifi_off_note"));
		lv_obj_set_hidden(status_label, false);
		return;
	}
	lv_obj_set_hidden(status_label, true);
}

// ---------------------------------------------------------------------------
// what a tap on a network does
// ---------------------------------------------------------------------------

static void details_action(void *user) {
	(void)user;

	wifi_status_t status;
	wifi_get_status(&status);

	lv_label_set_text(details_ssid_value, status.ssid[0] ? status.ssid : menu_ssid);
	lv_label_set_text(details_ip_value, status.ip[0] ? status.ip : "--");
	lv_label_set_text(details_mac_value, status.mac[0] ? status.mac : "--");

	if (status.signal) {
		lv_label_set_text_fmt(details_signal_value, tr("wifi_dbm"), status.signal);
	} else {
		lv_label_set_text(details_signal_value, "--");
	}

	// Security comes from the scan, not from the link: wpa_cli's status does
	// not spell it out in a form worth showing.
	const char *security = "--";
	wifi_network_t networks[WIFI_MAX_NETWORKS];
	int count = wifi_get_networks(networks, WIFI_MAX_NETWORKS);
	for (int i = 0; i < count; i++) {
		if (strcmp(networks[i].ssid, menu_ssid) == 0) {
			security = networks[i].secured ? tr("wifi_secured") : tr("wifi_open");
			break;
		}
	}
	lv_label_set_text(details_security_value, security);

	switch_screen(details_wifi_screen);
}

static void connect_action(void *user) {
	(void)user;
	wifi_connect(menu_ssid, NULL);
}

static void disconnect_action(void *user) {
	(void)user;
	wifi_disconnect();
}

static void forget_confirmed(void *user) {
	(void)user;
	wifi_forget(menu_ssid);
}

// Forgetting throws the password away, the one thing here that cannot be
// taken back with a tap: it asks first, in the red of the irreversible.
static void forget_action(void *user) {
	(void)user;
	sx_sheet_t sheet = {
		.title = menu_ssid,
		.subtitle = tr("wifi_forget_note"),
		.cancel_label = tr("cancel"),
		.action_label = tr("wifi_forget_network"),
		.action_destructive = true,
		.on_action = forget_confirmed,
	};
	sx_sheet_open(&sheet);
}

static void password_accept_cb(lv_event_t *e) {
	(void)e;
	const char *psk = lv_textarea_get_text(password_field);
	wifi_connect(pending_ssid, psk);
	lv_textarea_set_text(password_field, "");
	switch_screen(wifisettings_screen);
}

static void ask_for_password(const char *ssid) {
	snprintf(pending_ssid, sizeof(pending_ssid), "%s", ssid);
	lv_label_set_text(password_title, ssid);
	lv_textarea_set_text(password_field, "");
	// Every network starts hidden, even if the eye was tapped on the previous
	// one: the "in clear" state belongs to that one entry, not forever.
	lv_textarea_set_password_mode(password_field, true);
	password_eye_refresh();
	keyboard_reset(password_keyboard);
	keyboard_set_visible(password_keyboard, true);
	switch_screen(password_screen);
}

// The sheet for a network that has something to say: the one in use (a tap on
// its plaque), or a saved one (a long press), so "forget" stays reachable
// without giving every row a button. Titled with the network's name.
static void show_row_menu(lv_obj_t *anchor, bool current) {
	(void)anchor;
	if (current) {
		const sx_sheet_item_t items[] = {
			{.label = tr("details"), .action = details_action},
			{.label = tr("disconnect"), .action = disconnect_action},
			{.label = tr("wifi_forget_network"), .destructive = true, .action = forget_action},
		};
		sx_sheet_menu(menu_ssid, NULL, items, 3);
	} else {
		const sx_sheet_item_t items[] = {
			{.label = tr("connect"), .action = connect_action},
			{.label = tr("wifi_forget_network"), .destructive = true, .action = forget_action},
		};
		sx_sheet_menu(menu_ssid, NULL, items, 2);
	}
}

// What a row knows about its network, handed to its callbacks (the card's own
// user data belongs to sx_card).
#define ROW_CURRENT 1u
#define ROW_SAVED 2u
#define ROW_SECURED 4u

typedef struct {
	unsigned flags;
	char ssid[WIFI_SSID_MAX];
} net_ref_t;

// Which network the menu that is about to open is about. Copied rather than
// pointed at, so the list can be rebuilt under it without any drama.
static void remember_ssid(const char *ssid) { snprintf(menu_ssid, sizeof(menu_ssid), "%s", ssid); }

static void row_clicked_cb(lv_event_t *e) {
	const net_ref_t *ref = lv_event_get_user_data(e);
	if (!ref || !ref->ssid[0]) {
		return;
	}
	remember_ssid(ref->ssid);

	// A tap does the obvious thing: join, or ask for the password first.
	if (ref->flags & ROW_CURRENT) {
		show_row_menu(NULL, true);
		return;
	}
	if ((ref->flags & ROW_SAVED) || !(ref->flags & ROW_SECURED)) {
		wifi_connect(ref->ssid, NULL);
		return;
	}
	ask_for_password(ref->ssid);
}

// A long press opens the sheet on any row that has one to open -- the way to
// forget a saved network that is not the one currently joined.
static void row_long_pressed_cb(lv_event_t *e) {
	const net_ref_t *ref = lv_event_get_user_data(e);
	if (!ref || !ref->ssid[0] || !(ref->flags & (ROW_CURRENT | ROW_SAVED))) {
		return; // nothing to say about a network never joined
	}
	remember_ssid(ref->ssid);
	show_row_menu(NULL, (ref->flags & ROW_CURRENT) != 0);
}

static void row_free_cb(lv_event_t *e) { free(lv_event_get_user_data(e)); }

// ---------------------------------------------------------------------------
// the list
// ---------------------------------------------------------------------------

// One network nearby: a card with its name, what joining it takes on the
// right ("Secured", "Open", "Saved"), and its signal bars after that, in the
// place a chevron takes on other cards. A tap joins it (or asks for the
// password); a long press on a saved one opens its sheet.
static void add_row(lv_obj_t *parent, const wifi_network_t *net) {
	net_ref_t *ref = malloc(sizeof(*ref));
	if (!ref) {
		return;
	}
	ref->flags = (net->current ? ROW_CURRENT : 0u) | (net->saved ? ROW_SAVED : 0u) | (net->secured ? ROW_SECURED : 0u);
	snprintf(ref->ssid, sizeof(ref->ssid), "%.*s", WIFI_SSID_MAX - 1, net->ssid);

	lv_obj_t *card = sx_card_create(parent);
	lv_obj_t *title = sx_card_title(card, net->ssid);
	// One line: an SSID is data, and a long one ends in an ellipsis rather
	// than pushing the card to two lines.
	lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
	lv_obj_set_height(title, lv_font_get_line_height(&font_ui_24));

	const char *state;
	if (net->saved) {
		state = net->signal ? tr("wifi_saved") : tr("wifi_saved_out_of_range");
	} else {
		state = net->secured ? tr("wifi_secured") : tr("wifi_open");
	}
	sx_card_value(card, state);

	lv_obj_t *bars = sx_card_chevron(card);
	lv_image_set_src(bars, glyph_for_bars(net->bars));
	// Saved but not on the air: "no bars" already, and fainter for "not here".
	lv_obj_set_style_image_opa(bars, net->signal ? LV_OPA_60 : LV_OPA_20, 0);

	sx_card_on_click(card, row_clicked_cb, ref);
	lv_obj_add_event_cb(card, row_long_pressed_cb, LV_EVENT_LONG_PRESSED, ref);
	lv_obj_add_event_cb(card, row_free_cb, LV_EVENT_DELETE, ref);
}

// "5 GHz · -52 dBm · 433 Mbit/s": what the link is, for the plaque. Each part
// only when known.
static void link_line(const wifi_status_t *status, char *out, size_t size) {
	out[0] = '\0';
	size_t used = 0;
	const char *band = NULL;
	if (status->freq_mhz >= 5925) {
		band = tr("wifi_band_6");
	} else if (status->freq_mhz >= 4900) {
		band = tr("wifi_band_5");
	} else if (status->freq_mhz > 0) {
		band = tr("wifi_band_24");
	}
	if (band) {
		used += (size_t)snprintf(out + used, size - used, "%s", band);
	}
	if (status->signal && used < size) {
		used += (size_t)snprintf(out + used, size - used, "%s", used ? " \xC2\xB7 " : "");
		if (used < size) {
			used += (size_t)snprintf(out + used, size - used, tr("wifi_dbm"), status->signal);
		}
	}
	if (status->link_mbps > 0 && used < size) {
		used += (size_t)snprintf(out + used, size - used, "%s", used ? " \xC2\xB7 " : "");
		if (used < size) {
			snprintf(out + used, size - used, tr("wifi_mbps"), status->link_mbps);
		}
	}
}

// The plaque: the network joined, or being joined, with its figures.
static void refresh_current_card(void) {
	wifi_status_t status;
	wifi_get_status(&status);
	bool on = wifi_get_enabled() && wifi_available();
	bool show = on && status.ssid[0] &&
				(status.state == WIFI_STATE_CONNECTED || status.state == WIFI_STATE_CONNECTING);
	lv_obj_set_hidden(current_card, !show);
	if (!show) {
		current_ssid[0] = '\0';
		return;
	}
	snprintf(current_ssid, sizeof(current_ssid), "%s", status.ssid);
	sx_card_title(current_card, status.ssid);
	if (status.state == WIFI_STATE_CONNECTED) {
		char line[96];
		link_line(&status, line, sizeof(line));
		sx_card_subtitle(current_card, line, true);
		sx_card_value(current_card, tr("wifi_network_connected"));
	} else {
		sx_card_subtitle(current_card, "", true);
		sx_card_value(current_card, tr("connecting"));
	}
}

static void current_card_clicked_cb(lv_event_t *e) {
	(void)e;
	if (!current_ssid[0]) {
		return;
	}
	remember_ssid(current_ssid);
	show_row_menu(current_card, true);
}

// `spin` puts the spinner left of the text: without something moving,
// "nothing here" and "still searching" read the same.
static void add_placeholder(const char *text, bool spin) {
	lv_obj_t *row = lv_obj_create(list);
	lv_obj_remove_style_all(row);
	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_height(row, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_top(row, 6, 0);
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

static void rebuild_list(void) {
	lv_obj_clean(list);
	refresh_current_card();

	bool on = wifi_get_enabled() && wifi_available();
	lv_obj_set_hidden(section_label, !on);
	lv_obj_set_hidden(list, !on);
	if (!on) {
		return;
	}

	// A sweep replaces the list rather than sitting under it: the rows are the
	// previous sweep's, and leaving them up while a new one runs invites a tap
	// on a network that is no longer there.
	if (searching_now()) {
		add_placeholder("wifi_searching_again", true);
		return;
	}

	wifi_network_t networks[WIFI_MAX_NETWORKS];
	int count = wifi_get_networks(networks, WIFI_MAX_NETWORKS);
	int shown = 0;
	for (int i = 0; i < count; i++) {
		// The network in use is the plaque above; so is one being joined.
		if (networks[i].current || (current_ssid[0] && strcmp(networks[i].ssid, current_ssid) == 0)) {
			continue;
		}
		add_row(list, &networks[i]);
		shown++;
	}
	if (shown == 0) {
		add_placeholder("wifi_no_networks_found", false);
	}
}

// ---------------------------------------------------------------------------
// the page's own controls
// ---------------------------------------------------------------------------

static void switch_changed_cb(lv_event_t *e) {
	(void)e;
	bool on = lv_obj_has_state(wifi_switch, LV_STATE_CHECKED);

	if (on && !wifi_available()) {
		lv_obj_remove_state(wifi_switch, LV_STATE_CHECKED);
		toast_error("wifi_wi_fi_not_available");
		return;
	}

	wifi_set_enabled(on);
	refresh_status_label();
	rebuild_list();
	topbar_refresh_radios();
}

// The radio is up and answering -- not "the switch is on", which is true from
// the moment it is tapped and stays true while wpa_supplicant is still being
// brought up. A sweep started in that window finds nothing and reads as a
// broken button, so the button is dead until this is true.
static bool radio_ready(void) { return wifi_get_enabled() && !wifi_busy(); }

static void scan_clicked_cb(lv_event_t *e) {
	(void)e;
	if (!radio_ready()) {
		return;
	}
	wifi_scan_start();
	refresh_status_label();
	// And the list, otherwise the scan starts and the page never says so: with
	// an empty list the serial does not move, so the poll below would not
	// rebuild it.
	rebuild_list();
}

// Everything the page shows is polled: the worker has no way to call into
// LVGL, and at two redraws a second nobody can tell the difference.
static void poll_cb(lv_timer_t *timer) {
	(void)timer;

	bool on = wifi_get_enabled();
	if (lv_obj_has_state(wifi_switch, LV_STATE_CHECKED) != on) {
		if (on) {
			lv_obj_add_state(wifi_switch, LV_STATE_CHECKED);
		} else {
			lv_obj_remove_state(wifi_switch, LV_STATE_CHECKED);
		}
	}

	// The first sweep after the radio comes up. Nothing else asks for it when
	// Wi-Fi is switched on from this very page, and a page that has not looked
	// yet must not say there is nothing there.
	if (!on) {
		scanned_since_on = false;
	} else if (!scanned_since_on && radio_ready()) {
		wifi_scan_start();
		scanned_since_on = true;
	}

	refresh_status_label();

	uint32_t serial = wifi_networks_serial();
	bool scanning = searching_now();
	if (serial != drawn_serial || on != drawn_enabled || scanning != drawn_scanning) {
		drawn_serial = serial;
		drawn_enabled = on;
		drawn_scanning = scanning;
		rebuild_list();
	}

	// The scan button: dead while the radio is still coming up, faded while a
	// sweep is running, solid otherwise.
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
								   !ready ? LV_OPA_20 : (wifi_scan_running() ? LV_OPA_50 : LV_OPA_COVER), 0);
	}

	char ssid[WIFI_SSID_MAX];
	switch (wifi_take_op_result(ssid, sizeof(ssid))) {
	case WIFI_OP_OK:
		// Only joining a network names it. Leaving one or forgetting it are
		// things that happened rather than things achieved, so they get the
		// plain card and a sentence that says what happened: the network's name
		// over a green tick reads as "connected", the opposite of what was
		// asked for.
		switch (wifi_last_op_kind()) {
		case WIFI_OPKIND_FORGET:
			toast_plain("wifi_network_forgotten");
			break;
		case WIFI_OPKIND_DISCONNECT:
			toast_plain("wifi_network_disconnected");
			break;
		case WIFI_OPKIND_CONNECT:
		default:
			toast_success(ssid[0] ? ssid : "done");
			break;
		}
		topbar_refresh_radios();
		break;
	case WIFI_OP_FAILED:
		// A passphrase the access point turned down is the one failure the user
		// can do something about, so it says so and hands the keyboard back
		// with that network's name on it. Without this a saved network with a
		// mistyped password is a dead end: tapping it re-uses the stored one
		// for ever, and the only way out is "Forget network", which nobody
		// reads as an offer to type it again.
		if (wifi_last_failure() == WIFI_FAIL_WRONG_KEY) {
			toast_error("wifi_incorrect_password");
			if (ssid[0]) {
				ask_for_password(ssid);
			}
		} else {
			toast_error("wifi_connection_failed");
		}
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

	// Arriving on the page is reason enough to start a sweep.
	if (wifi_get_enabled()) {
		wifi_scan_start();
		scanned_since_on = true;
	}
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	lv_timer_pause(poll_timer);
}

static lv_obj_t *corner_button(lv_obj_t *screen, gui_config_t *cfg, const lv_image_dsc_t *glyph, lv_obj_t **icon_out) {
	lv_obj_t *button = lv_btn_create(screen);
	lv_obj_set_size(button, CORNER_BTN_SIZE, CORNER_BTN_SIZE);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -cfg->padding, cfg->padding + cfg->top_bar_height);

	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, glyph);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_center(icon);
	if (icon_out) {
		*icon_out = icon;
	}

	return button;
}

// ---------------------------------------------------------------------------
// the passphrase page
// ---------------------------------------------------------------------------

static void build_password_page(gui_config_t *cfg) {
	password_screen = lv_obj_create(NULL);
	lv_obj_add_style(password_screen, &theme_style_screen, 0);

	// The heading is the network's name, so it is a label the page rewrites
	// rather than a fixed title.
	password_title = settingsrow_title(password_screen, cfg, "");

	int top = settingsrow_content_top(cfg);
	int keyboard_h = 316; // the same keyboard the search page uses

	lv_obj_t *hint = lv_label_create(password_screen);
	lv_label_set_text(hint, tr("wifi_network_password"));
	lv_obj_add_style(hint, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(hint, &font_ui_22, 0);
	lv_obj_align(hint, LV_ALIGN_TOP_LEFT, cfg->padding, top);

	password_field = lv_textarea_create(password_screen);
	lv_textarea_set_one_line(password_field, true);
	lv_textarea_set_placeholder_text(password_field, tr("password"));
	// The 2.0 input: the search field's 60 px pill on the surface colour.
	lv_obj_set_size(password_field, cfg->screen_width - 2 * cfg->padding, 60);
	lv_obj_set_scrollbar_mode(password_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(password_field, LV_ALIGN_TOP_LEFT, cfg->padding, top + 36);
	lv_obj_add_style(password_field, &theme_style_card, 0);
	lv_obj_add_style(password_field, &sx_style_surface, 0);
	lv_obj_set_style_radius(password_field, 30, 0);
	lv_obj_set_style_border_width(password_field, 0, 0);
	lv_obj_set_style_shadow_width(password_field, 0, 0);
	lv_obj_set_style_pad_ver(password_field, 13, 0);
	lv_obj_set_style_pad_left(password_field, 22, 0);
	lv_obj_set_style_text_font(password_field, &font_ui_24, 0);
	keyboard_style_caret(password_field);

	// Bullets from the keystroke itself: PASSWORD_SHOW_MS is zero, so no
	// character is ever drawn in clear, not even for a moment. The eye below is
	// what rereads the whole thing.
	//
	// The bullet is LVGL's default U+2022: the fonts here are real OTF faces
	// read through FreeType rather than trimmed glyph sets, so it is present.
	keyboard_style_password(password_field, PASSWORD_SHOW_MS);

	// Room for the eye, otherwise a long password runs underneath it.
	lv_obj_set_style_pad_right(password_field, 60, 0);

	// The eye, over the field's right edge: same geometry and weight as the x
	// that clears the search box.
	password_eye_btn = lv_btn_create(password_screen);
	lv_obj_set_size(password_eye_btn, 56, 56);
	lv_obj_align(password_eye_btn, LV_ALIGN_TOP_RIGHT, -cfg->padding - 4, top + 36 + (60 - 56) / 2);
	lv_obj_set_style_bg_opa(password_eye_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(password_eye_btn, 0, 0);
	lv_obj_set_style_border_width(password_eye_btn, 0, 0);
	lv_obj_set_style_pad_all(password_eye_btn, 0, 0);
	lv_obj_add_event_cb(password_eye_btn, password_eye_cb, LV_EVENT_CLICKED, NULL);

	password_eye_icon = lv_image_create(password_eye_btn);
	lv_obj_add_style(password_eye_icon, &theme_style_icon, 0);
	lv_obj_set_style_image_opa(password_eye_icon, LV_OPA_70, 0);
	lv_obj_center(password_eye_icon);
	password_eye_refresh();

	password_keyboard = keyboard_create(password_screen, cfg->screen_width, keyboard_h, password_field, NULL, "ok",
										password_accept_cb, NULL);

	switcher_attach_back_gesture(password_screen);
}

// ---------------------------------------------------------------------------
// the details page
// ---------------------------------------------------------------------------

static void build_details_page(gui_config_t *cfg) {
	details_wifi_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(details_wifi_screen, cfg, "wifi_network_details");

	settingsrow_add(container, "wifi_network", &details_ssid_value, NULL, NULL);
	settingsrow_add(container, "wifi_ip_address", &details_ip_value, NULL, NULL);
	settingsrow_add(container, "wifi_mac_address", &details_mac_value, NULL, NULL);
	settingsrow_add(container, "wifi_signal", &details_signal_value, NULL, NULL);
	settingsrow_add(container, "wifi_security", &details_security_value, NULL, NULL);

	switcher_attach_back_gesture(details_wifi_screen);
}

// ---------------------------------------------------------------------------

static lv_obj_t *make_rows(lv_obj_t *parent) {
	lv_obj_t *box = lv_obj_create(parent);
	lv_obj_set_width(box, lv_pct(100));
	lv_obj_set_height(box, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(box, 0, 0);
	lv_obj_set_style_border_width(box, 0, 0);
	lv_obj_set_style_pad_all(box, 0, 0);
	lv_obj_set_style_pad_bottom(box, 12, 0);
	lv_obj_set_style_pad_gap(box, 8, 0);
	lv_obj_set_scrollable(box, false);
	lv_obj_set_event_bubble(box, true);
	lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
	return box;
}

void wifisettings_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(wifisettings_screen, cfg, "wi_fi");

	settingsrow_switch_card(container, "wi_fi", NULL, &wifi_switch, switch_changed_cb);
	if (wifi_get_enabled()) {
		lv_obj_add_state(wifi_switch, LV_STATE_CHECKED);
	}

	// The note under the switch: no radio, coming up, or off and what that
	// costs. Hidden while the radio is up.
	status_label = lv_label_create(container);
	lv_obj_set_width(status_label, lv_pct(100));
	lv_obj_add_style(status_label, &sx_style_text2, 0);
	lv_obj_set_style_text_font(status_label, &font_ui_20, 0);
	lv_obj_set_style_pad_hor(status_label, 6, 0);
	lv_obj_set_style_pad_top(status_label, 4, 0);
	lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);

	// The network in use, as the plaque: the handoff's "active now".
	current_card = sx_card_create(container);
	sx_card_set_active(current_card, true);
	lv_obj_t *current_title = sx_card_title(current_card, "");
	lv_label_set_long_mode(current_title, LV_LABEL_LONG_DOT);
	lv_obj_set_height(current_title, lv_font_get_line_height(&font_ui_24));
	sx_card_on_click(current_card, current_card_clicked_cb, NULL);
	lv_obj_add_event_cb(current_card, current_card_clicked_cb, LV_EVENT_LONG_PRESSED, NULL);
	lv_obj_set_hidden(current_card, true);

	// And everything else around, under a heading. The box exists from the
	// start and is hidden with the radio off, so a refresh is one
	// lv_obj_clean() and not a hunt through the page's children.
	section_label = settingsrow_group(container, "wifi_available_networks", -1);
	list = make_rows(container);

	scan_btn = corner_button(wifisettings_screen, cfg, &icon_wifi_search, &scan_icon);
	lv_obj_add_event_cb(scan_btn, scan_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_state(scan_btn, LV_STATE_DISABLED); // until the radio answers
	lv_obj_set_style_image_opa(scan_icon, LV_OPA_20, 0);

	build_password_page(cfg);
	build_details_page(cfg);

	poll_timer = lv_timer_create(poll_cb, WIFI_PAGE_POLL_MS, NULL);
	lv_timer_pause(poll_timer);

	lv_obj_add_event_cb(wifisettings_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(wifisettings_screen, switched_elsewhere_cb, LV_EVENT_REFRESH, NULL);
	lv_obj_add_event_cb(wifisettings_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(wifisettings_screen);

	refresh_status_label();
	rebuild_list();
}

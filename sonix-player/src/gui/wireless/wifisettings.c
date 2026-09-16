#include "wifisettings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/spinner.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/topbar.h"
#include "src/system/core/lang.h"
#include "src/system/net/wifi.h"

lv_obj_t *wifisettings_screen;

// How often the page looks at what the radio module has found out. The work
// itself happens on the wifi worker; this only redraws.
#define WIFI_PAGE_POLL_MS 500

#define WIFI_ROW_H 82
#define CORNER_BTN_SIZE 56

static lv_obj_t *wifi_switch;
static lv_obj_t *status_label;
static lv_obj_t *current_label;
static lv_obj_t *current_list;
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

// The network a popover is about. The row that owns the string outlives the
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
		lv_obj_remove_flag(status_label, LV_OBJ_FLAG_HIDDEN);
		return;
	}

	if (wifi_busy()) {
		lv_label_set_text(status_label, wifi_get_enabled() ? tr("turning_on") : tr("wifi_turning_off_2"));
		lv_obj_remove_flag(status_label, LV_OBJ_FLAG_HIDDEN);
		return;
	}

	wifi_status_t status;
	wifi_get_status(&status);

	// With the radio up this line stands down: the two headings below already
	// say which network is in use and what else is around, and "Connected to X"
	// over a section called "Current network" holding X is the same sentence
	// twice.
	switch (status.state) {
	case WIFI_STATE_CONNECTED:
	case WIFI_STATE_ON:
		lv_obj_add_flag(status_label, LV_OBJ_FLAG_HIDDEN);
		return;
	case WIFI_STATE_CONNECTING:
		lv_label_set_text(status_label, tr("connecting"));
		break;
	case WIFI_STATE_OFF:
	default:
		lv_label_set_text(status_label, tr("wifi_wi_fi_off"));
		break;
	}
	lv_obj_remove_flag(status_label, LV_OBJ_FLAG_HIDDEN);
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

static void forget_action(void *user) {
	(void)user;
	wifi_forget(menu_ssid);
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

// The menu behind the three dots, and behind a long press on any saved network,
// so "forget" stays reachable without giving every row a button.
static void show_row_menu(lv_obj_t *anchor, bool current) {
	if (current) {
		static const popover_item_t items[] = {
			{"details", details_action, NULL},
			{"disconnect", disconnect_action, NULL},
			{"wifi_forget_network", forget_action, NULL},
		};
		popover_show(anchor, items, 3);
	} else {
		static const popover_item_t items[] = {
			{"connect", connect_action, NULL},
			{"wifi_forget_network", forget_action, NULL},
		};
		popover_show(anchor, items, 2);
	}
}

// What the row knows about itself, packed into its user data.
#define ROW_CURRENT 1u
#define ROW_SAVED 2u
#define ROW_SECURED 4u

// Which network the menu that is about to open is about. Copied rather than
// pointed at, so the list can be rebuilt under it without any drama.
static void remember_ssid(const char *ssid) { snprintf(menu_ssid, sizeof(menu_ssid), "%s", ssid); }

static void row_clicked_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return; // a swipe across the row, not a tap on it
	}

	lv_obj_t *row = lv_event_get_current_target(e);
	const char *ssid = lv_event_get_user_data(e);
	if (!ssid || !ssid[0]) {
		return;
	}
	remember_ssid(ssid);

	uintptr_t flags = (uintptr_t)lv_obj_get_user_data(row);
	bool current = (flags & ROW_CURRENT) != 0;
	bool saved = (flags & ROW_SAVED) != 0;
	bool secured = (flags & ROW_SECURED) != 0;

	// A tap does the obvious thing; the menu lives behind the three dots.
	if (current) {
		show_row_menu(row, true);
		return;
	}
	if (saved || !secured) {
		wifi_connect(ssid, NULL);
		return;
	}
	ask_for_password(ssid);
}

// A long press opens the menu on any row that has one to open -- the way to
// forget a saved network that is not the one currently joined.
static void row_long_pressed_cb(lv_event_t *e) {
	lv_obj_t *row = lv_event_get_current_target(e);
	const char *ssid = lv_event_get_user_data(e);
	if (!ssid || !ssid[0]) {
		return;
	}

	uintptr_t flags = (uintptr_t)lv_obj_get_user_data(row);
	if (!(flags & (ROW_CURRENT | ROW_SAVED))) {
		return; // nothing to say about a network never joined
	}
	remember_ssid(ssid);
	show_row_menu(row, (flags & ROW_CURRENT) != 0);
}

// The three dots themselves. The button carries the same flags as its row, so
// the menu is the right one whether the network is the connected one or merely
// a saved one.
static void row_menu_clicked_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	const char *ssid = lv_event_get_user_data(e);
	lv_obj_t *button = lv_event_get_current_target(e);
	if (!ssid || !ssid[0]) {
		return;
	}
	remember_ssid(ssid);

	uintptr_t flags = (uintptr_t)lv_obj_get_user_data(button);
	show_row_menu(button, (flags & ROW_CURRENT) != 0);
}

static void row_free_cb(lv_event_t *e) { free(lv_event_get_user_data(e)); }

// ---------------------------------------------------------------------------
// the list
// ---------------------------------------------------------------------------

// `parent` is the current-network box or the list of the others, and `with_menu`
// says whether this row carries the three dots. Only the network being used
// does: for the rest the menu would offer "connect", which is what tapping the
// row already does, and "forget", which a long press still reaches.
static void add_row(lv_obj_t *parent, const wifi_network_t *net, bool with_menu) {
	char *ssid_copy = strdup(net->ssid);
	if (!ssid_copy) {
		return;
	}

	lv_obj_t *row = lv_btn_create(parent);
	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_height(row, WIFI_ROW_H);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, 12, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, 18, 0);
	// The button default carries vertical padding of its own, which cuts the
	// second line of text off along the bottom edge of the row.
	lv_obj_set_style_pad_ver(row, 0, 0);
	lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 14, 0);

	uintptr_t flags = (net->current ? ROW_CURRENT : 0u) | (net->saved ? ROW_SAVED : 0u) |
					  (net->secured ? ROW_SECURED : 0u);
	lv_obj_set_user_data(row, (void *)flags);

	lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, ssid_copy);
	lv_obj_add_event_cb(row, row_long_pressed_cb, LV_EVENT_LONG_PRESSED, ssid_copy);
	lv_obj_add_event_cb(row, row_free_cb, LV_EVENT_DELETE, ssid_copy);

	lv_obj_t *icon = lv_image_create(row);
	lv_image_set_src(icon, glyph_for_bars(net->bars));
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
	if (net->current) {
		lv_obj_set_style_image_recolor(icon, theme()->accent, 0);
	} else if (!net->signal) {
		// Saved, but not on the air right now: the glyph says "no bars"
		// already, the dimming says "and not here at all".
		lv_obj_set_style_image_opa(icon, LV_OPA_40, 0);
	}

	// Name over state, left-aligned, taking whatever width is left.
	lv_obj_t *text = lv_obj_create(row);
	lv_obj_set_flex_grow(text, 1);
	lv_obj_set_height(text, lv_pct(100));
	lv_obj_set_style_bg_opa(text, 0, 0);
	lv_obj_set_style_border_width(text, 0, 0);
	lv_obj_set_style_pad_all(text, 0, 0);
	lv_obj_remove_flag(text, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(text, LV_OBJ_FLAG_EVENT_BUBBLE);

	lv_obj_t *name = lv_label_create(text);
	lv_label_set_text(name, net->ssid);
	lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
	lv_obj_set_width(name, lv_pct(100));
	// Pinned to one line's worth of height. LV_LABEL_LONG_DOT wraps first and
	// only puts the dots at the end of the last line that fits, so an unpinned
	// long SSID takes two lines and pushes the row's second line out of shape.
	lv_obj_set_height(name, lv_font_get_line_height(&font_ui_24));
	lv_obj_add_style(name, &theme_style_text, 0);
	lv_obj_set_style_text_font(name, &font_ui_24, 0);

	const char *state = NULL;
	if (net->current) {
		state = tr("wifi_network_connected");
	} else if (net->saved) {
		state = net->signal ? tr("wifi_saved") : tr("wifi_saved_out_of_range");
	} else if (net->secured) {
		state = tr("wifi_secured");
	}

	if (state) {
		lv_obj_t *sub = lv_label_create(text);
		lv_label_set_text(sub, state);
		lv_obj_add_style(sub, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(sub, &font_ui_20, 0);
		if (net->current) {
			lv_obj_set_style_text_color(sub, theme()->accent, 0);
		}
		// Measured from the middle rather than the top: the pair then sits
		// centred in the row whatever padding the button style brings along.
		lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, -15);
		lv_obj_align(sub, LV_ALIGN_LEFT_MID, 0, 16);
	} else {
		lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);
	}

	if (with_menu) {
		lv_obj_t *menu_btn = lv_btn_create(row);
		lv_obj_set_size(menu_btn, 44, 56);
		lv_obj_set_style_bg_opa(menu_btn, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(menu_btn, 0, 0);
		lv_obj_set_style_shadow_width(menu_btn, 0, 0);
		lv_obj_set_style_pad_all(menu_btn, 0, 0);
		lv_obj_set_user_data(menu_btn, (void *)flags);
		lv_obj_add_event_cb(menu_btn, row_menu_clicked_cb, LV_EVENT_CLICKED, ssid_copy);

		lv_obj_t *dots = lv_image_create(menu_btn);
		lv_image_set_src(dots, &icon_ellipsis_vertical);
		lv_obj_add_style(dots, &theme_style_icon, 0);
		lv_obj_set_style_image_opa(dots, LV_OPA_70, 0);
		lv_obj_center(dots);
	}
}

static void rebuild_list(void) {
	lv_obj_clean(list);
	lv_obj_clean(current_list);

	bool on = wifi_get_enabled() && wifi_available();
	if (!on) {
		lv_obj_add_flag(current_label, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(current_list, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(section_label, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(list, LV_OBJ_FLAG_HIDDEN);
		return;
	}
	lv_obj_remove_flag(section_label, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag(list, LV_OBJ_FLAG_HIDDEN);

	wifi_network_t networks[WIFI_MAX_NETWORKS];
	int count = wifi_get_networks(networks, WIFI_MAX_NETWORKS);

	// The network in use gets a heading of its own, above the rest, rather than
	// being left to be found among thirty others.
	int current = -1;
	for (int i = 0; i < count; i++) {
		if (networks[i].current) {
			current = i;
			break;
		}
	}

	if (current >= 0) {
		lv_obj_remove_flag(current_label, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(current_list, LV_OBJ_FLAG_HIDDEN);
		add_row(current_list, &networks[current], true);
	} else {
		lv_obj_add_flag(current_label, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(current_list, LV_OBJ_FLAG_HIDDEN);
	}

	// A sweep replaces the list rather than sitting under it: the rows are the
	// previous sweep's, and leaving them up while a new one runs invites a tap
	// on a network that is no longer there.
	if (searching_now()) {
		lv_obj_t *row = lv_obj_create(list);
		lv_obj_remove_style_all(row);
		lv_obj_set_width(row, lv_pct(100));
		lv_obj_set_height(row, LV_SIZE_CONTENT);
		lv_obj_set_style_pad_top(row, 10, 0);
		lv_obj_set_style_pad_column(row, 10, 0);
		lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

		spinner_create(row, &icon_loader_small);

		lv_obj_t *searching = lv_label_create(row);
		lv_label_set_text(searching, tr("wifi_searching_again"));
		lv_obj_add_style(searching, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(searching, &font_ui_22, 0);
		return;
	}

	int shown = 0;
	for (int i = 0; i < count; i++) {
		if (i == current) {
			continue; // it has its own place above
		}
		add_row(list, &networks[i], false);
		shown++;
	}

	if (shown == 0) {
		lv_obj_t *none = lv_label_create(list);
		lv_label_set_text(none, tr("wifi_no_networks_found"));
		lv_obj_add_style(none, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(none, &font_ui_22, 0);
		lv_obj_set_style_pad_top(none, 10, 0);
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
	lv_obj_set_size(password_field, cfg->screen_width - 2 * cfg->padding, 62);
	lv_obj_set_scrollbar_mode(password_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(password_field, LV_ALIGN_TOP_LEFT, cfg->padding, top + 36);
	lv_obj_add_style(password_field, &theme_style_card, 0);
	lv_obj_set_style_radius(password_field, 12, 0);
	lv_obj_set_style_border_width(password_field, 0, 0);
	lv_obj_set_style_shadow_width(password_field, 0, 0);
	lv_obj_set_style_pad_all(password_field, 14, 0);
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
	lv_obj_align(password_eye_btn, LV_ALIGN_TOP_RIGHT, -cfg->padding - 4, top + 36 + (62 - 56) / 2);
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

// A section heading. Full width, so the page's centring flex leaves the text on
// the left where every other heading in the interface sits.
static lv_obj_t *make_heading(lv_obj_t *parent, const char *tag) {
	lv_obj_t *heading = lv_label_create(parent);
	lv_label_set_text(heading, tr(tag));
	lv_obj_set_width(heading, lv_pct(100));
	lv_obj_add_style(heading, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(heading, &font_ui_22, 0);
	lv_obj_set_style_pad_top(heading, 6, 0);
	lv_obj_set_style_pad_hor(heading, 4, 0);
	return heading;
}

static lv_obj_t *make_rows(lv_obj_t *parent) {
	lv_obj_t *box = lv_obj_create(parent);
	lv_obj_set_width(box, lv_pct(100));
	lv_obj_set_height(box, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(box, 0, 0);
	lv_obj_set_style_border_width(box, 0, 0);
	lv_obj_set_style_pad_all(box, 0, 0);
	lv_obj_set_style_pad_bottom(box, 12, 0);
	lv_obj_set_style_pad_gap(box, 8, 0);
	lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(box, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
	return box;
}

void wifisettings_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(wifisettings_screen, cfg, "wi_fi");

	settingsrow_toggle(container, "wi_fi", &wifi_switch, switch_changed_cb);
	if (wifi_get_enabled()) {
		lv_obj_add_state(wifi_switch, LV_STATE_CHECKED);
	}

	status_label = lv_label_create(container);
	lv_obj_set_width(status_label, lv_pct(100));
	lv_obj_add_style(status_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(status_label, &font_ui_20, 0);
	lv_obj_set_style_pad_hor(status_label, 4, 0);
	lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);

	// Two sections, each a heading over a box of rows: the network being used,
	// and everything else. Both boxes exist from the start and are hidden when
	// they have nothing in them, so a refresh is one lv_obj_clean() and not a
	// hunt through the page's children.
	current_label = make_heading(container, "wifi_current_network");
	current_list = make_rows(container);

	section_label = make_heading(container, "wifi_available_networks");
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
	lv_obj_add_event_cb(wifisettings_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(wifisettings_screen);

	refresh_status_label();
	rebuild_list();
}

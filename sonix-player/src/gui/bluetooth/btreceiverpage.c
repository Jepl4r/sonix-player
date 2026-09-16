#include "btreceiverpage.h"

#include <stdio.h>

#include "src/gui/shell/confirm.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/bluetooth/btreceiver.h"
#include "src/system/core/lang.h"

lv_obj_t *btreceiverpage_screen;

#define POLL_MS 500

static lv_obj_t *big_icon;
static lv_obj_t *format_label;
static lv_obj_t *device_label;
static lv_obj_t *status_label;
static lv_timer_t *poll_timer;
static unsigned last_serial = (unsigned)-1;
static lv_color_t status_normal_color;

// ---------------------------------------------------------------------------

// "44100" -> "44,1 kHz". The rates that divide evenly lose the decimal: 48 kHz
// written as 48,0 reads like a measurement rather than a number everyone knows.
static void format_rate(unsigned rate, char *out, size_t size) {
	if (rate == 0) {
		snprintf(out, size, "--");
	} else if (rate % 1000 == 0) {
		snprintf(out, size, "%u kHz", rate / 1000);
	} else {
		snprintf(out, size, "%.1f kHz", rate / 1000.0);
	}
}

static void refresh(void) {
	btreceiver_state_t st;
	btreceiver_get_state(&st);

	lv_obj_set_style_text_color(status_label, status_normal_color, 0);

	if (st.error[0]) {
		lv_label_set_text(device_label, st.device);
		lv_label_set_text(format_label, "");
		lv_label_set_text(status_label, st.error);
		lv_obj_set_style_text_color(status_label, lv_color_make(224, 27, 36), 0);
		return;
	}

	if (!st.active) {
		// Asked now, not remembered. The name the mode last ran with belongs to
		// the mode: with nothing streaming here it is the name of a phone that
		// has gone, and showing it would read as one that is arriving.
		char name[BT_NAME_MAX];
		bool source = bluetooth_receiver_device(NULL, 0, name, sizeof(name));
		lv_label_set_text(device_label, source ? name : "");
		lv_label_set_text(format_label, "");
		lv_label_set_text(status_label,
						  source ? tr("btreceiver_connecting") : tr("btreceiver_no_source_note"));
		return;
	}

	lv_label_set_text(device_label, st.device);

	// The codec and the rate, which is the whole of what this page is for: the
	// sender chose both and this is the only place the choice is visible.
	char rate[24];
	format_rate(st.sample_rate, rate, sizeof(rate));
	char text[64];
	if (st.codec[0]) {
		snprintf(text, sizeof(text), "%s  %s", st.codec, rate);
	} else {
		snprintf(text, sizeof(text), "%s", rate);
	}
	lv_label_set_text(format_label, text);
	lv_label_set_text(status_label, st.streaming ? tr("btreceiver_playing") : tr("btreceiver_waiting"));
}

static void poll_cb(lv_timer_t *timer) {
	(void)timer;
	if (lv_screen_active() != btreceiverpage_screen) {
		return;
	}
	// The codec can be renegotiated mid-stream and the Bluetooth serial moves
	// when it is, so both serials count. The stream falling silent counts too
	// and has no serial of its own: nothing runs when frames stop arriving, so
	// there is nobody to bump one. It is read here instead.
	btreceiver_state_t st;
	btreceiver_get_state(&st);
	unsigned now = btreceiver_serial() + bluetooth_devices_serial() + (st.streaming ? 1u : 0u);
	if (now == last_serial) {
		return;
	}
	last_serial = now;
	refresh();
}

// ---------------------------------------------------------------------------

static bool leaving;

static void leave_async(void *user) {
	(void)user;
	back_btn_cb(NULL);
	leaving = false;
}

// Deferred by one turn of the event loop, as on the DAC page: this runs from
// inside the confirmation's own handler, and navigating out from under a dialog
// that is still closing leaves the screen change undone.
static void really_leave(void *user) {
	(void)user;
	btreceiver_stop();
	last_serial = (unsigned)-1;
	refresh();
	leaving = true;
	lv_async_call(leave_async, NULL);
}

// The back chevron and the swipe both come through here. True means "handled,
// stay put": leaving stops the audio, and that is worth one question.
static bool back_guard(void) {
	if (leaving || !btreceiver_is_active()) {
		return false;
	}
	confirm_show("btreceiver_leave_confirm", "btreceiver_leave_confirm_note", "leave", really_leave, NULL);
	return true;
}

// ---------------------------------------------------------------------------

static void loaded_cb(lv_event_t *e) {
	(void)e;
	last_serial = (unsigned)-1;
	// Arriving is the switch: there is nothing else this page does, so a toggle
	// on it would only repeat what opening it already said.
	btreceiver_start();
	refresh();
}

static void unloaded_cb(lv_event_t *e) {
	(void)e;
	// Every other way out -- the power menu, a notification, a screen change
	// this page did not ask for -- ends the mode too. The guard above catches
	// the deliberate ones; this catches the rest, so the device is never left
	// holding a stream nobody can see.
	btreceiver_stop();
}

void btreceiverpage_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(btreceiverpage_screen, cfg, "btreceiver_title");

	// Straight into the flex flow with no alignment of its own: the container
	// settingsrow_page() builds already centres its children, and an object
	// positioned by alignment leaves the flow and is measured as taking no
	// space.
	big_icon = lv_image_create(container);
	lv_image_set_src(big_icon, &icon_bluetooth_receiver_page);
	lv_obj_set_style_margin_top(big_icon, 40, 0);

	format_label = lv_label_create(container);
	lv_obj_set_width(format_label, lv_pct(100));
	lv_label_set_long_mode(format_label, LV_LABEL_LONG_WRAP);
	lv_obj_add_style(format_label, &theme_style_text, 0);
	lv_obj_set_style_text_align(format_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(format_label, &font_ui_24_bold, 0);
	lv_obj_set_style_margin_top(format_label, 20, 0);
	lv_label_set_text(format_label, "");

	device_label = lv_label_create(container);
	lv_obj_set_width(device_label, lv_pct(100));
	lv_label_set_long_mode(device_label, LV_LABEL_LONG_DOT);
	lv_obj_set_style_text_align(device_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(device_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(device_label, &font_ui_22, 0);
	lv_obj_set_style_margin_top(device_label, 10, 0);
	lv_label_set_text(device_label, "");

	status_label = lv_label_create(container);
	lv_obj_set_width(status_label, lv_pct(100));
	lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(status_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(status_label, &font_ui_22, 0);
	lv_obj_set_style_margin_top(status_label, 10, 0);
	lv_label_set_text(status_label, "");
	status_normal_color = lv_obj_get_style_text_color(status_label, LV_PART_MAIN);

	lv_obj_add_event_cb(btreceiverpage_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(btreceiverpage_screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_set_back_guard(btreceiverpage_screen, back_guard);

	poll_timer = lv_timer_create(poll_cb, POLL_MS, NULL);
	(void)poll_timer;
}

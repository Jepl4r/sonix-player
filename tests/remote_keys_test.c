// Compile the production handlers selected by run-remote-keys-test.sh: hardware
// setup is excluded, while every command branch runs against observable GUI output.
#include <assert.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include "src/gui/shell/gui.h"
#include "src/system/audio/audio.h"
#include "src/system/device/sysinfo.h"
#include "src/system/input/keymap.h"
#include "src/system/input/remotekeys.h"
#include "src/system/input/hookclicks.c"

static bool loaded;
static bool duplicate;
static audio_status_t status;
static gui_key_t emitted;
static int notifications;
static keymap_button_t mapped;
static sysinfo_model_t model;
bool audiobook_is_playing(void) { return loaded; }
void gui_notify_key(gui_key_t key) { emitted = key; notifications++; }
static void expect(gui_key_t key) { assert(notifications == 1); assert(emitted == key); notifications = 0; }
audio_status_t audio_get_status(void) { return status; }
bool btplayer_command_is_duplicate(void) { return duplicate; }
const sysinfo_model_t *sysinfo_model(void) { return &model; }
static void run_mapped_action(keymap_button_t button) { mapped = button; }
static void apply_volume_step(int code) { (void)code; }
static int volume_step_delta(int code) { (void)code; return 0; }
void device_state_change_volume_percent(int delta) { (void)delta; }
int device_state_get_volume_percent(void) { return 50; }
void gui_notify_volume(int volume) { (void)volume; }
void power_notify_activity(void) {}
bool power_screen_is_on(void) { return true; }
bool power_double_tap_wake_enabled(void) { return false; }
void power_notify_power_button(void) {}
static uint32_t now_ms(void) { return 1000; }
static bool audiobook_mode;
static int units, chapter_step, track_step;
bool radio_is_active(void) { return false; }
bool dlna_owns_playback(void) { return false; }
int audiobook_unit_count(void) { return units; }
static void player_step_chapter(int direction) { chapter_step = direction; }
void player_key_next(void) { track_step = 1; }
void player_key_prev(void) { track_step = -1; }
#include "remote-handlers.h"

int main(void) {
	// Losing any caller's chapter mapping fails this matrix, including a paused book.
	for (int mode = 0; mode < 3; mode++) {
		loaded = mode != 0;
		status = mode == 2 ? AUDIO_STATUS_PAUSED : AUDIO_STATUS_PLAYING;
		gui_key_t next = loaded ? GUI_KEY_NEXT_CHAPTER : GUI_KEY_NEXT;
		gui_key_t prev = loaded ? GUI_KEY_PREV_CHAPTER : GUI_KEY_PREV;
		for (int direction = 0; direction < 2; direction++) {
			int code = direction ? KEY_NEXTSONG : KEY_PREVIOUSSONG;
			gui_key_t want = direction ? next : prev;
			handle_key_press("test", code, true); expect(want);
			hook_clicks_t clicks = {.clicks = direction ? 2 : 3};
			run_hook_gesture(&clicks); expect(want);
			for (int usb = 0; usb < 2; usb++) {
				handle_bt_remote_key("test", code, usb); expect(want);
				clicks.clicks = direction ? 2 : 3;
				run_remote_clicks("test", &clicks, usb); expect(want);
			}
			run_command(direction ? "Next" : "Previous"); expect(want);
		}
		handle_bt_remote_key("test", KEY_FASTFORWARD, true); expect(GUI_KEY_NEXT);
		handle_bt_remote_key("test", KEY_REWIND, true); expect(GUI_KEY_PREV);
		for (int swap = 0; swap < 2; swap++) {
			model.media_keys_swapped = swap;
			handle_key_press("test", KEY_NEXTSONG, false);
			assert(mapped == (swap ? KEYMAP_BTN_PREV : KEYMAP_BTN_NEXT));
			handle_key_press("test", KEY_PREVIOUSSONG, false);
			assert(mapped == (swap ? KEYMAP_BTN_NEXT : KEYMAP_BTN_PREV));
			assert(notifications == 0);
		}
	}
	duplicate = true;
	handle_bt_remote_key("test", KEY_NEXTSONG, false);
	run_command("Next"); assert(notifications == 0);
	handle_bt_remote_key("test", KEY_NEXTSONG, true); expect(GUI_KEY_NEXT_CHAPTER);
	// The production chapter receivers retain the no-chapter book fallback.
	audiobook_mode = true;
	units = 0;
	player_key_next_chapter(); assert(track_step == 1 && chapter_step == 0);
	player_key_prev_chapter(); assert(track_step == -1 && chapter_step == 0);
	units = 3;
	track_step = 0;
	player_key_next_chapter(); assert(chapter_step == 1 && track_step == 0);
	player_key_prev_chapter(); assert(chapter_step == -1 && track_step == 0);
	puts("PASS remote-keys");
}

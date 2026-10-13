#ifndef GUI_H
#define GUI_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

typedef struct {
	const uint32_t screen_width;
	const uint32_t screen_height;
	const int8_t padding;
	const int8_t top_bar_height;
	const char *sd_root_path;
} gui_config_t;

// Above this the volume readout turns red -- the status bar's number and the
// pop-up's alike. Not a limit, just the point where the scale stops being
// polite: this DAC is at -21 dB there, and every step above is loud.
#define VOLUME_WARN_PERCENT 55
#define VOLUME_WARN_COLOR lv_color_make(224, 60, 50)

// Builds the interface. UI thread, once.
//
// With `split`, only what the first frame shows is built -- the theme, the
// status bar, the home screen -- and true is returned: the caller paints that
// frame and then calls gui_init_rest() for the hundred-odd other pages, before
// entering the main loop. False means everything was built here, which is what
// happens without `split` and on a first boot (whose first frame is the
// greeting, built last).
bool gui_init(gui_config_t *cfg, bool split);

// Builds whatever gui_init() left for after the first frame; nothing when that
// is done. Also called, as a safety net, by navigation and by the gui_post
// drain, so no page is ever shown or updated before it exists.
void gui_init_rest(void);

// Pages built on first use. UI thread only, like everything that touches LVGL.
//
// A page that is rarely opened -- a settings sub-page, the process list, the
// radio directory -- does not have to hold its few hundred objects from boot
// on. Its module's *_init() then only creates (or is handed) its empty screen
// and registers `build` here against it; the screen pointer is valid from the
// start, so every row, tile and back-stack entry that points at it keeps
// working, and the switcher calls gui_page_ensure() on every screen it is about
// to load, which runs `build` the first time and is a short scan after that.
//
// What a deferred page must NOT have: anything outside it that reaches into
// its widgets (a refresh called from another module, a gui_post callback, a
// timer or a theme_register_refresh() callback set up before the build) --
// those would find NULL; the page sets them up in `build` instead. Code of the page's
// own that has to touch a widget outside a navigation (an *_open() that fills
// a field and then switches) calls gui_page_ensure() itself first.
void gui_page_defer(lv_obj_t *screen, void (*build)(gui_config_t *cfg));

// Builds `screen`'s page now if it was deferred and is not built yet; nothing
// otherwise (NULL included).
void gui_page_ensure(lv_obj_t *screen);

// The bridge from any thread to the UI thread: `cb(user)` runs on the UI
// thread within a few tens of milliseconds.
//
// Do not use lv_async_call() from another thread. Without LV_USE_OS (not
// enabled here) LVGL has no locks, and lv_async_call appends a timer to the
// list the UI is walking at that moment -- a real SIGSEGV on the device. This
// bridge touches nothing in LVGL on the posting side.
// False when the queue was full and the message dropped -- a caller that
// passed heap memory owns it again and has to free it.
bool gui_post(void (*cb)(void *), void *user);

// The descriptor the main loop waits on instead of sleeping on a timer: it
// becomes readable as soon as something is posted. -1 when it could not be
// opened, in which case the loop falls back to usleep().
//
// Only the UI thread may read it (post_drain_cb drains it); it exists to be
// passed to poll().
int gui_post_wake_fd(void);

// Called from the main loop when poll() reports the descriptor is ready: marks
// the queue-draining timer ready so posted work runs on the next pass instead
// of waiting for its period to elapse.
void gui_post_service(void);

void gui_notify_popup(const char *text);

// Whether the card is there and ours. False while it is exported to a computer
// and false when there is none in the slot.
bool gui_card_available(void);

// Says which of the two it is. The library cannot tell them apart -- its index
// is on the card either way -- and the wrong one of the two sends the user
// looking for a cable that is not plugged in, or for a card that is.
void gui_notify_no_card(void);

// The same three-second notice with a glyph over the sentence, recoloured to
// `color`. For the messages that are about a thing rather than about a
// failure -- the headphones, when the volume belongs to them and not to the
// player.
void gui_notify_popup_icon(const char *text, const lv_image_dsc_t *icon, lv_color_t color);

// ---------------------------------------------------------------------------
// The modal notice
//
// A card over a dimmed screen that stays up until gui_modal_hide() takes it
// down -- for the things that take longer than a toast can wait for, such as
// pairing headphones, which takes the better part of half a minute.
//
// The backdrop swallows taps on purpose: there is nothing useful to press
// while it is up, and a second pairing started on top of the first confuses
// the stack.
//
// Interface thread only.
// ---------------------------------------------------------------------------

void gui_modal_show(const lv_image_dsc_t *icon, lv_color_t color, const char *title, const char *subtitle);
void gui_modal_hide(void);
bool gui_modal_visible(void);

// A bar under the title, for the jobs whose length is known in advance.
//
// A modal with no bar says "something is happening" and nothing else, which is
// the right answer for a pairing. Copying a folder is the other kind: the
// bytes to move are counted in advance, so a real share can be shown.
//
// Call with a percentage to show the bar and move it; -1 hides it again.
// gui_modal_show() hides it by itself, so a modal that wants one asks after.
void gui_modal_progress(int percent);

// ---------------------------------------------------------------------------
// Notifications from other threads
//
// The physical buttons and a Bluetooth remote's keys are read on their own
// threads, and LVGL is not safe to touch from there. Each of these hands the
// work to the UI thread through gui_post() and returns immediately.
// ---------------------------------------------------------------------------

typedef enum {
	GUI_KEY_PLAY_PAUSE,
	GUI_KEY_NEXT,
	GUI_KEY_PREV,
	// What a remapped side button can also do (keymap.h). Never sent by a
	// remote: they act on the local player only.
	GUI_KEY_FAVOURITE,
	GUI_KEY_NEXT_ALBUM,
	GUI_KEY_CYCLE_MODE,
} gui_key_t;

void gui_notify_key(gui_key_t key);

// New volume level, already applied to the hardware: this only updates what is
// on screen.
void gui_notify_volume(int percent);

// The power key was held down.
void gui_notify_power_menu(void);

// Whether obj is still one of the objects on a display. It compares the pointer
// against the live widget tree and never reads through it, so a pointer to an
// object already deleted is safe to pass. LVGL's lv_obj_is_in_widget_tree(),
// which lv_obj_is_valid() maps to, follows obj's parent links and is not.
bool gui_obj_alive(const lv_obj_t *obj);

#endif /* GUI_H */

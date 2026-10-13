#include "switcher.h"

#include <stdio.h>

#include "lvgl/lvgl.h"
// The page slide moves both pages at once and keeps the one leaving drawn
// under the one arriving; LVGL keeps that page in the display's prev_scr,
// which has no public setter. And the edge swipe takes a press away from the
// object it landed on, which it finds in the pointer's state.
#include "lvgl/src/display/lv_display_private.h"
#include "lvgl/src/indev/lv_indev_private.h"

#include "src/gui/library/browser.h"
#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/library/audiobooks.h"
#include "src/gui/library/libraryscan.h"
#include "src/gui/shell/main_menu.h"
#include "src/gui/library/music.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/topbar.h"
#include "src/gui/shell/touch.h"
#include "src/gui/nowplaying/trackmenu.h"
#include "src/gui/shell/quickpanel.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/sx_frame.h"
#include "src/gui/shell/sx_mini.h"
#include "src/gui/shell/sx_sheet.h"
#include "src/gui/shell/sx_styles.h"
#include "src/gui/shell/sx_tokens.h"

// The round back target of the 2.0 header (SX_HEADER_BTN): flush with the
// left edge, centred in the 76 px header under the status bar, the title
// starting where it ends.
#define BACK_BTN_SIZE SX_HEADER_BTN
#define BACK_BTN_X 0
#define BACK_BTN_Y (SX_STATUSBAR_H + (SX_HEADER_H - SX_HEADER_BTN) / 2)
// On the player the chevron keeps the place the 56 px button gave it before
// 2.0 -- its centre 43 px in and 43 px down from the player's top -- because
// the player's own layout (back_btn_centre_y) is drawn around that point.
#define BACK_BTN_PLAYER_INSET 7

static lv_obj_t *back_btn;
static lv_obj_t *back_btn_icon;

// Chevron colour state: white only when it sits over the player's dark
// artwork backdrop (see back_btn_over_cover / back_btn_player_mode).
static bool chevron_in_player;
static bool chevron_over_cover;

// A page painted in a colour of its own -- an album in the colours of its
// sleeve -- and the colour the chevron takes on it so it stays readable.
// `chevron_page` is the page being shown, set as it is loaded: the active
// screen is still the previous one at that moment.
static lv_obj_t *chevron_tint_screen;
static lv_color_t chevron_tint;
static lv_obj_t *chevron_page;

// Where the back button sits depends on whether the page below it shows the
// status bar: normally it tucks in underneath, on the player (status bar
// hidden, artwork at the top) it moves up to the top edge.
static int8_t back_btn_top_bar_height;
static gui_config_t *switcher_cfg;

// On the player the status bar is hidden, so there is no row of its own for the
// chevron to line up with and it would sit against the top edge. This is the
// air it keeps instead, and what the player's own header row is aligned to.
#define BACK_BTN_PLAYER_TOP 14

// Whether the status bar stays up over the player too (back_btn_player_below_bar).
static bool player_bar_shown;

// How far down the chevron sits on the player.
static int player_top(void) { return player_bar_shown ? back_btn_top_bar_height : BACK_BTN_PLAYER_TOP; }

static void place_back_btn(bool below_top_bar) {
	// Pages: the header's back target. The player: the corner the player's
	// own header row is aligned to, under the status bar when that stays up
	// there (back_btn_player_below_bar).
	if (below_top_bar) {
		lv_obj_set_pos(back_btn, BACK_BTN_X, BACK_BTN_Y);
		return;
	}
	lv_obj_set_pos(back_btn, BACK_BTN_PLAYER_INSET, player_top() + BACK_BTN_PLAYER_INSET);
}

// Where the back button goes. A page can be reached by more than one route --
// the player from the Musica page or from a track in the browser -- so the way
// back is whatever was on screen before, not a fixed parent.
#define SCREEN_HISTORY_DEPTH 8

static lv_obj_t *screen_history[SCREEN_HISTORY_DEPTH];
static int screen_history_len;

static void history_push(lv_obj_t *screen) {
	if (!screen) {
		return;
	}

	if (screen_history_len == SCREEN_HISTORY_DEPTH) {
		// Drop the oldest rather than refusing to record: a very deep path
		// still ends up walking back to the menu, just not through every step.
		for (int i = 1; i < SCREEN_HISTORY_DEPTH; i++) {
			screen_history[i - 1] = screen_history[i];
		}
		screen_history_len--;
	}

	screen_history[screen_history_len++] = screen;
}

static lv_obj_t *history_pop(void) {
	if (screen_history_len == 0) {
		return NULL;
	}
	return screen_history[--screen_history_len];
}

// How a page change looks: a plain swap (restoring a page that was never
// really left, under the player), or a slide forward or back.
typedef enum { NAV_NONE, NAV_FORWARD, NAV_BACK } nav_dir_t;

static void load_screen_dir(lv_obj_t *target_screen, nav_dir_t dir);
static void load_screen(lv_obj_t *target_screen) { load_screen_dir(target_screen, NAV_NONE); }
static void chevron_apply_color(void);

// Forgets the whole back-stack: the next chevron press lands on the main
// menu. For pages that end a one-way flow (a finished scan) where walking
// back through the flow's screens would make no sense.
void screen_history_reset(void) {
	screen_history_len = 0;
	switcher_set_player_return(NULL);
}

// Loads a screen without touching the back-stack. For restoring a page that
// was never really left -- the player sheet putting its opener back.
void switch_screen_no_history(lv_obj_t *target_screen) { load_screen(target_screen); }

// Return to a visited screen, dropping it and newer pages from history so its
// next back action reaches the page that originally opened it.
void switch_screen_return_to(lv_obj_t *target_screen) {
	for (int i = screen_history_len - 1; i >= 0; i--) {
		if (screen_history[i] == target_screen) {
			screen_history_len = i;
			break;
		}
	}
	load_screen_dir(target_screen, NAV_BACK);
}

// The page (queue, details, the music settings...) that was opened from the
// player's own menu: leaving it goes back INTO the player, not to the page
// the sheet originally slid over. Set by the opener, consumed by the back
// navigation, dropped by anything that makes it stale.
static lv_obj_t *player_return_screen;

void switcher_set_player_return(lv_obj_t *screen) { player_return_screen = screen; }

static bool back_goes_to_player(void) {
	return player_return_screen && lv_screen_active() == player_return_screen;
}

// ---------------------------------------------------------------------------
// The swipe back, from the left edge only
//
// 2.0 splits the screen's gestures by where they start (principle 1: the edge
// is navigation, the content is actions): a list row takes a sideways drag for
// itself -- next in the queue, or the end of it -- so the page only goes back
// for a drag that starts in the SX_EDGE_BACK_W strip along the left edge. The
// page follows the finger to the right over a snapshot of the page it goes
// back to, and goes back when let go past SX_EDGE_BACK_PX; short of that it
// snaps back.
//
// It is watched on the pointer device itself (switcher_edge_back_attach)
// rather than hung on the pages: the strip is the same on every page, and no
// page has to be told. LVGL tells the pointer of every press; one that lands
// in the strip starts a short timer that follows it to its release. A press in the strip that
// does not turn into a decisive rightward drag is left alone -- a tap there
// is a tap on whatever is under it. Once the drag is taken, the object under
// the finger is told the press is lost and LVGL is kept off the gesture until
// the finger lifts, so nothing scrolls, swipes or clicks underneath.
// ---------------------------------------------------------------------------

#define BACK_ANIM_MS SX_ANIM_ROW_SNAP_MS

static bool back_drag_click_guard;	// suppresses the tap the drag ends with
static bool back_anim_running;
static lv_obj_t *back_underlay;			// full-screen container on the bottom layer
static lv_draw_buf_t *back_snapshot;	// the previous page, rendered once

bool switcher_back_drag_active(void) { return back_drag_click_guard; }

static bool back_stays_in_page(void);
static bool page_has_no_chevron(lv_obj_t *screen);
static bool chevron_forced_hidden;

static void back_drag_click_guard_clear_cb(void *unused) {
	(void)unused;
	back_drag_click_guard = false;
}

// The chevron belongs to the page, so it rides along with the drag (and the
// animation that finishes it) instead of hanging in the air.
static void back_btn_set_drag_offset(int32_t x) {
	if (back_btn) {
		lv_obj_set_style_translate_x(back_btn, x, 0);
	}
}

// Public flavour of the same, for the player sheet: its chevron rides the
// sheet while it is dragged or animated in and out.
void back_btn_translate(int32_t x) { back_btn_set_drag_offset(x); }

void back_btn_translate_y(int32_t y) {
	if (back_btn) {
		lv_obj_set_style_translate_y(back_btn, y, 0);
	}
}

int back_btn_centre_y(void) { return player_top() + BACK_BTN_PLAYER_INSET + BACK_BTN_SIZE / 2; }

void back_btn_player_below_bar(bool below) {
	player_bar_shown = below;
	if (back_btn && chevron_in_player) {
		place_back_btn(false);
	}
}

static void back_underlay_destroy(void) {
	if (back_underlay) {
		lv_obj_delete(back_underlay);
		back_underlay = NULL;
	}
	if (back_snapshot) {
		lv_draw_buf_destroy(back_snapshot);
		back_snapshot = NULL;
	}
}

// Builds what shows under the sliding page: a snapshot of the page the drag
// goes back to. Inside a browser subfolder the "previous page" is the same
// screen showing the parent folder -- that can't be rendered ahead of time,
// so a plain page-coloured panel stands in.
//
// No dimming veil over it, as iOS has: a translucent layer the size of the
// screen is exactly what the platform cannot afford to blend on every frame
// of the drag (handoff, platform limits).
static void back_underlay_build(void) {
	back_underlay_destroy();

	back_underlay = lv_obj_create(lv_layer_bottom());
	lv_obj_set_size(back_underlay, lv_pct(100), lv_pct(100));
	lv_obj_set_pos(back_underlay, 0, 0);
	lv_obj_set_style_bg_color(back_underlay, theme()->screen_bg, 0);
	lv_obj_set_style_bg_opa(back_underlay, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(back_underlay, 0, 0);
	lv_obj_set_style_radius(back_underlay, 0, 0);
	lv_obj_set_style_pad_all(back_underlay, 0, 0);
	lv_obj_set_scrollable(back_underlay, false);
	lv_obj_set_clickable(back_underlay, false);

	lv_obj_t *target = NULL;
	bool going_to_player = false;
	if (lv_screen_active() == browser_screen && browser_can_go_up()) {
		target = NULL; // parent folder: no ready-made page to show
	} else if (back_stays_in_page()) {
		// The step back stays on this same page (a list's internal stack:
		// episodes returning to results, results returning to the chips). What
		// will be there is not drawn anywhere yet, so the neutral panel goes
		// underneath, as for a browser folder. Using the top of the global
		// history here would show one page under the finger and land on a
		// different one at release.
		target = NULL;
	} else if (back_goes_to_player()) {
		// This page goes back INTO the player, so the player is what shows
		// underneath. It is parked hidden while closed; unhide it just for
		// the offscreen render.
		target = player_screen;
		going_to_player = true;
	} else if (screen_history_len > 0) {
		target = screen_history[screen_history_len - 1];
	} else {
		target = main_menu_screen;
	}

	if (target) {
		bool was_hidden = lv_obj_is_hidden(target);
		if (was_hidden) {
			lv_obj_set_hidden(target, false);
		}
		// The player is parked below the screen when closed; drawn where it
		// will be once open.
		int32_t parked_y = going_to_player ? lv_obj_get_y(target) : 0;
		if (going_to_player) {
			lv_obj_set_y(target, 0);
		}
		back_snapshot = lv_snapshot_take(target, LV_COLOR_FORMAT_RGB565);
		if (going_to_player) {
			lv_obj_set_y(target, parked_y);
		}
		if (was_hidden) {
			lv_obj_set_hidden(target, true);
		}
		if (back_snapshot) {
			lv_obj_t *img = lv_image_create(back_underlay);
			lv_image_set_src(img, back_snapshot);
			lv_obj_set_pos(img, 0, 0);
		}
	}

	// The page underneath shows its own chevron, exactly as it will once the
	// drag lands on it -- every page except the main menu has one. The global
	// floating button is busy riding the page being dragged, so the underlay
	// carries a plain copy at the same spot.
	if (target != main_menu_screen) {
		lv_obj_t *chevron = lv_image_create(back_underlay);
		lv_image_set_src(chevron, going_to_player ? &icon_chevron_down : &icon_chevron_left);
		// The same two placements as place_back_btn(), the 36 px glyph centred
		// in the 72 px target.
		int inset = (BACK_BTN_SIZE - 36) / 2;
		if (going_to_player) {
			lv_obj_set_pos(chevron, BACK_BTN_PLAYER_INSET + inset, player_top() + BACK_BTN_PLAYER_INSET + inset);
		} else {
			lv_obj_set_pos(chevron, BACK_BTN_X + inset, BACK_BTN_Y + inset);
		}
		if (going_to_player && chevron_over_cover) {
			lv_obj_set_style_image_recolor(chevron, lv_color_white(), 0);
		} else {
			lv_obj_set_style_image_recolor(chevron, theme()->text_primary, 0);
		}
		lv_obj_set_style_image_recolor_opa(chevron, LV_OPA_COVER, 0);
	}
}

// Set while a page slide (switch_screen) is under way: the edge swipe waits
// for it, as LVGL itself does -- no input is read while two pages are drawn.
static bool nav_anim_running;

static void back_anim_exec_cb(void *var, int32_t v) {
	lv_obj_set_x((lv_obj_t *)var, v);
	back_btn_set_drag_offset(v);
}

// Set while the back button's handler runs at the end of a committed swipe:
// the page has already slid away under the finger, so the load must not slide
// it again.
static bool back_load_immediate;

static void back_anim_commit_done_cb(lv_anim_t *a) {
	lv_obj_t *screen = (lv_obj_t *)a->var;
	back_load_immediate = true;
	back_btn_cb(NULL); // pops history / walks the browser up, loads the page below
	back_load_immediate = false;
	lv_obj_set_x(screen, 0);
	back_btn_set_drag_offset(0);
	back_underlay_destroy();
	back_anim_running = false;
	back_btn_sync_visibility();
}

static void back_anim_cancel_done_cb(lv_anim_t *a) {
	(void)a;
	back_btn_set_drag_offset(0);
	back_underlay_destroy();
	back_anim_running = false;
	back_btn_sync_visibility();
}

static bool back_gesture_off;

void back_gesture_blocked(bool blocked) { back_gesture_off = blocked; }

// Whether a press in the strip may become a swipe back at all: not while
// another gesture owns the screen, and not where the chevron would not be
// either.
static bool edge_back_allowed(void) {
	if (back_gesture_off || back_anim_running || nav_anim_running || chevron_forced_hidden) {
		return false;
	}
	if (player_sheet_is_open() || sx_sheet_is_open() || quickpanel_is_open()) {
		return false;
	}
	return !page_has_no_chevron(lv_screen_active());
}

// The press the strip has taken: the object under the finger is told it is
// lost (RELEASED would make it a click), LVGL's own state for it is reset so
// it does not scroll or long-press, and LVGL is kept off the rest of the
// gesture.
static void edge_take_press(lv_indev_t *indev) {
	lv_obj_t *pressed = indev->pointer.act_obj;
	if (pressed) {
		lv_obj_send_event(pressed, LV_EVENT_PRESS_LOST, indev);
	}
	lv_indev_reset(indev, NULL);
	lv_indev_wait_release(indev);
}

static void edge_release(int dx) {
	lv_obj_t *screen = lv_screen_active();
	int width = lv_obj_get_width(screen);
	int from = dx > 0 ? dx : 0;
	bool go = dx > SX_EDGE_BACK_PX;

	back_anim_running = true;
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, screen);
	lv_anim_set_exec_cb(&a, back_anim_exec_cb);
	lv_anim_set_values(&a, from, go ? width : 0);
	lv_anim_set_duration(&a, go ? SX_ANIM_NAV_MS : BACK_ANIM_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_completed_cb(&a, go ? back_anim_commit_done_cb : back_anim_cancel_done_cb);
	lv_anim_start(&a);

	// Deferred, not cleared here: the CLICKED event of this same gesture
	// still has to see the guard (same dance as the player sheet's flag).
	lv_async_call(back_drag_click_guard_clear_cb, NULL);
}

// The pointer the strip watches, and the timer that follows a press that
// began in it. The timer runs only from such a press to its release: nothing
// at all while the finger is elsewhere or off the panel.
static lv_indev_t *edge_indev;
static lv_timer_t *edge_timer;
static bool edge_candidate; // the press began in the strip and may yet become a swipe
static bool edge_engaged;	// it has: the page follows the finger
static lv_point_t edge_start;
static int edge_last_dx;

#define EDGE_POLL_MS 10

static void edge_timer_cb(lv_timer_t *t) {
	lv_indev_t *indev = edge_indev;
	if (!indev || !edge_candidate) {
		lv_timer_pause(t);
		return;
	}

	if (lv_indev_get_state(indev) != LV_INDEV_STATE_PRESSED) {
		if (edge_engaged) {
			edge_release(edge_last_dx);
		}
		edge_candidate = false;
		edge_engaged = false;
		lv_timer_pause(t);
		return;
	}

	lv_point_t p;
	lv_indev_get_point(indev, &p);
	int dx = p.x - edge_start.x;
	int dy = p.y - edge_start.y;
	if (!edge_engaged) {
		if (LV_ABS(dy) >= SX_DRAG_LOCK_PX && LV_ABS(dy) > dx) {
			edge_candidate = false; // a scroll that began at the edge
			lv_timer_pause(t);
			return;
		}
		if (dx < SX_SWIPE_LOCK_PX || dx <= 2 * LV_ABS(dy)) {
			return;
		}
		if (lv_indev_get_scroll_obj(indev) || !edge_back_allowed()) {
			edge_candidate = false;
			lv_timer_pause(t);
			return;
		}
		edge_engaged = true;
		back_drag_click_guard = true;
		edge_take_press(indev);
		back_underlay_build();
	}
	int x = dx > 0 ? dx : 0;
	lv_obj_set_x(lv_screen_active(), x);
	back_btn_set_drag_offset(x);
	edge_last_dx = dx;
}

// LVGL tells the pointer itself about every press, whatever object it lands
// on: that is where a press in the strip is noticed.
static void edge_pressed_cb(lv_event_t *e) {
	(void)e;
	if (!edge_indev || edge_candidate) {
		return;
	}
	lv_point_t p;
	lv_indev_get_point(edge_indev, &p);
	if (p.x >= SX_EDGE_BACK_W || !edge_back_allowed()) {
		return;
	}
	edge_candidate = true;
	edge_engaged = false;
	edge_start = p;
	edge_last_dx = 0;
	lv_timer_reset(edge_timer);
	lv_timer_resume(edge_timer);
}

void switcher_edge_back_attach(lv_indev_t *indev) {
	if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER || edge_indev) {
		return;
	}
	// One pointer: the device has the touch panel, the emulator its mouse.
	edge_indev = indev;
	lv_indev_add_event_cb(indev, edge_pressed_cb, LV_EVENT_PRESSED, NULL);
	edge_timer = lv_timer_create(edge_timer_cb, EDGE_POLL_MS, NULL);
	lv_timer_pause(edge_timer);
}

// Before 2.0 a page surface was given the swipe back here; the edge strip now
// serves every page by itself. Kept so that the pages need not change: the
// surface still becomes clickable, which some of them count on to stop presses
// from reaching what lies under them.
void switcher_attach_back_gesture(lv_obj_t *obj) {
	if (obj) {
		lv_obj_set_clickable(obj, true);
	}
}

// ---------------------------------------------------------------------------
// The page slide
//
// Forward, the new page comes in from the right and pushes the old one out to
// the left; back, the other way round: SX_ANIM_NAV_MS, ease-out, a translate
// and nothing else (principle 9; no opacity, which the panel cannot blend
// cheaply). LVGL's lv_screen_load_anim() makes the new page the active one
// only on the animation's first frame, while every caller here expects
// lv_screen_active() to be the new page the moment switch_screen() returns,
// and it tells the old page it was unloaded before the slide even starts --
// several pages empty themselves on that, which would show as a page wiped
// while it slides out. So the slide is done here: the new page is loaded at
// once (LOAD_START and LOADED as lv_screen_load() sends them), the old one is
// kept drawn underneath as the display's prev_scr, and it hears UNLOADED only
// once it is off screen.
// ---------------------------------------------------------------------------

static lv_obj_t *nav_old;
static lv_obj_t *nav_new;

static void nav_exec_cb(void *var, int32_t v) {
	(void)var;
	if (!nav_new || !nav_old) {
		return;
	}
	int32_t w = lv_obj_get_width(nav_new);
	// v runs from +-w to 0: the new page's offset; the old one is a screen's
	// width away on the other side.
	lv_obj_set_x(nav_new, v);
	lv_obj_set_x(nav_old, v > 0 ? v - w : v + w);
}

// Ends a slide: both pages back in place, the old one forgotten and told it is
// gone. Also called to cut a slide short when another page is asked for.
static void nav_finish(void) {
	if (!nav_anim_running) {
		return;
	}
	nav_anim_running = false;
	lv_anim_delete(&nav_new, nav_exec_cb);

	lv_obj_t *old = nav_old;
	lv_obj_t *now = nav_new;
	nav_old = NULL;
	nav_new = NULL;

	lv_display_t *d = lv_display_get_default();
	if (d && d->prev_scr == old) {
		d->prev_scr = NULL;
	}
	if (now) {
		lv_obj_set_x(now, 0);
		lv_obj_invalidate(now);
	}
	if (old) {
		lv_obj_set_x(old, 0);
		if (d) {
			lv_display_send_event(d, LV_EVENT_SCREEN_UNLOADED, old);
		}
		lv_obj_send_event(old, LV_EVENT_SCREEN_UNLOADED, NULL);
	}
	back_btn_sync_visibility();
}

static void nav_done_cb(lv_anim_t *a) {
	(void)a;
	nav_finish();
}

// Loads `target` with a slide: `forward` from the right, otherwise from the
// left. Falls back to a plain load when there is nothing to slide from.
static void nav_slide_load(lv_obj_t *target, bool forward) {
	lv_display_t *d = lv_obj_get_display(target);
	lv_obj_t *old = d ? d->act_scr : NULL;
	if (!d || !old || old == target) {
		lv_screen_load(target);
		return;
	}

	lv_display_send_event(d, LV_EVENT_SCREEN_UNLOAD_START, old);
	lv_obj_send_event(old, LV_EVENT_SCREEN_UNLOAD_START, NULL);
	lv_display_send_event(d, LV_EVENT_SCREEN_LOAD_START, target);
	lv_obj_send_event(target, LV_EVENT_SCREEN_LOAD_START, NULL);

	d->act_scr = target;
	d->scr_to_load = NULL;
	d->prev_scr = old;
	d->draw_prev_over_act = false;

	lv_display_send_event(d, LV_EVENT_SCREEN_LOADED, target);
	lv_obj_send_event(target, LV_EVENT_SCREEN_LOADED, NULL);

	int32_t w = lv_display_get_horizontal_resolution(d);
	nav_old = old;
	nav_new = target;
	nav_anim_running = true;
	lv_obj_set_x(target, forward ? w : -w);
	lv_obj_invalidate(target);

	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, &nav_new);
	lv_anim_set_exec_cb(&a, nav_exec_cb);
	lv_anim_set_values(&a, forward ? w : -w, 0);
	lv_anim_set_duration(&a, SX_ANIM_NAV_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_completed_cb(&a, nav_done_cb);
	lv_anim_start(&a);
}

bool switcher_nav_anim_running(void) { return nav_anim_running; }

void switch_screen(lv_obj_t *target_screen) {
	// Every page must exist before one is shown (see gui_init): a no-op except
	// in the instant between the first frame and the rest of the build.
	gui_init_rest();

	// The player is not a page: it is a panel that slides in over whatever is
	// already on screen, so there is nothing to load and nothing to remember.
	if (target_screen == player_screen) {
		// Except when arriving from a page the player opened itself: the queue,
		// an audiobook's chapters, a Qobuz list. Tapping a track there brings
		// the player back up, but if the sheet slid over the queue, the next
		// chevron press would pull the queue out again instead of the page the
		// player was first opened from. Picking a track from the queue is not a
		// step forward: it is the same playback, and the queue is done.
		//
		// So the queue is closed here, exactly as the chevron would (see
		// back_btn_cb): the player return is consumed, the page underneath is
		// popped, and the sheet reopens over that.
		if (back_goes_to_player()) {
			player_return_screen = NULL;
			lv_obj_t *previous = history_pop();
			load_screen(previous ? previous : main_menu_screen);
		}
		player_sheet_open(true);
		return;
	}

	lv_obj_t *current = lv_screen_active();
	nav_dir_t dir = NAV_FORWARD;

	if (target_screen == main_menu_screen) {
		screen_history_len = 0; // the menu is the root; nothing above it
		player_return_screen = NULL;
		dir = NAV_BACK;
	} else if (current && current != target_screen) {
		// Returning to the page just arrived from is a step back, not a step
		// forward: the history is shortened rather than extended.
		//
		// Without this check the stack grows like an accordion: leaving the
		// Wi-Fi password page once connected would record the password page,
		// and the next chevron press would pull it out again, asking for the
		// password of a network already connected to.
		if (screen_history_len > 0 && screen_history[screen_history_len - 1] == target_screen) {
			screen_history_len--;
			dir = NAV_BACK;
		} else {
			history_push(current);
		}
	}

	load_screen_dir(target_screen, dir);
}

// A page with its own close button up -- a full-screen dialog drawn on the page
// itself -- names its screen here; see back_btn_hide_on_screen().
static lv_obj_t *chevron_hidden_on;

// The pages the chevron is never drawn on: the main menu has nowhere to go
// back to, the scan pages exit through their own buttons, and a page showing
// a dialog with a close button already has its way out.
static bool page_has_no_chevron(lv_obj_t *screen) {
	return screen == main_menu_screen || screen == libraryscan_screen || screen == audiobookscan_screen ||
		   (screen && screen == chevron_hidden_on);
}

static void load_screen_dir(lv_obj_t *target_screen, nav_dir_t dir) {
	// A slide still running is finished first: its old page gets its
	// UNLOADED before anything else happens to it.
	nav_finish();

	// A page left to be built on first use is built now, before anything below
	// looks at it (see gui_page_defer). Once built, this is a scan of a short
	// list that finds nothing.
	gui_page_ensure(target_screen);

	// Every page keeps the status bar. The player is not a page -- it slides
	// over the top of one -- so nothing here has to make room for it.
	topbar_set_hidden(false);

	if (back_btn) {
		// The scan pages have their own way out (cancel / OK) and must not be
		// left halfway through by the chevron.
		if (page_has_no_chevron(target_screen)) {
			lv_obj_set_hidden(back_btn, true);
		} else {
			lv_obj_set_hidden(back_btn, false);
		}

		place_back_btn(true);
	}
	chevron_page = target_screen;
	chevron_apply_color();
	topbar_page_shown(target_screen);

	// Re-read the folder on the way in: the card may have finished mounting
	// (or changed) since the browser last looked at it.
	if (target_screen == browser_screen) {
		browser_refresh();
	}

	// Opening the scan page is the request to scan.
	if (target_screen == libraryscan_screen) {
		libraryscan_begin();
	}

	// A page hidden through a theme, accent or tint change carries the palette
	// it was last walked for; this is where it catches up, one page at a time
	// instead of all of them at the moment of the change.
	theme_notify_screen_shown(target_screen);

	// The buttons beside the page's title answer a fingertip and not just
	// their glyph (touch.h). Looked for here, on the way in, so that every
	// page's own way of building them is covered without each being told.
	touch_header(target_screen);
	if (switcher_cfg) {
		settingsrow_title_fit_header(target_screen, switcher_cfg);
	}

	// The bottom slot: lists given room at their end, bottom controls lifted
	// clear of the mini player, an idle card put in place (sx_frame.h).
	sx_frame_page_shown(target_screen);

	// A plain swap when nothing is seen sliding: under the open player, at the
	// end of a swipe that already moved the page away, or for a restore.
	if (dir == NAV_NONE || back_load_immediate || player_sheet_is_open()) {
		lv_screen_load(target_screen);
	} else {
		nav_slide_load(target_screen, dir == NAV_FORWARD);
	}

	// Some pages own the whole panel, some have a keyboard up: the mini player
	// looks at the page now on screen.
	sx_mini_update_visibility();
}

// Pages that have something to say before they will let go of the chevron.
// Both of the current ones are a page that is also the switch for a service --
// the Wi-Fi transfer and AirPlay -- and both ask before taking it down.
//
// A table rather than a single slot: with one slot, whichever page was built
// last would silently take the guard away from the other, and the symptom is a
// page that quietly stops asking.
//
// The table has to stay comfortably larger than the number of pages that
// register one, because filling it up is invisible from the outside: a dropped
// registration shows up only as a chevron that leaves the section instead of
// walking back through the page's own stack. That is what the log line below
// is for.
#define BACK_GUARDS_MAX 16

static struct {
	lv_obj_t *screen;
	bool (*handler)(void);
	bool (*peek)(void); // will the step back stay on this page?
} back_guards[BACK_GUARDS_MAX];

void switcher_set_back_guard(lv_obj_t *screen, bool (*handler)(void)) {
	if (!screen) {
		return;
	}

	// Setting the same page twice replaces its guard rather than using a
	// second slot.
	for (int i = 0; i < BACK_GUARDS_MAX; i++) {
		if (back_guards[i].screen == screen) {
			back_guards[i].handler = handler;
			return;
		}
	}

	for (int i = 0; i < BACK_GUARDS_MAX; i++) {
		if (!back_guards[i].screen) {
			back_guards[i].screen = screen;
			back_guards[i].handler = handler;
			return;
		}
	}

	// Not LV_LOG_WARN: that goes wherever LVGL's logging is pointed and is
	// compiled out in some builds, and what it costs to miss is a page whose
	// back button quietly leaves the section.
	fprintf(stderr, "switcher: no room for another back guard (the table holds %d)\n", BACK_GUARDS_MAX);
}

// The question the drag preview has to ask without moving anything: going back
// now, does the page stay the same? Pages that keep a stack of their own --
// Qobuz, Tidal, podcasts, every list that lives on one screen -- answer here,
// and the drag uses it to avoid drawing another screen's page underneath when
// that is not where the release will land.
void switcher_set_back_guard_peek(lv_obj_t *screen, bool (*peek)(void)) {
	if (!screen) {
		return;
	}
	for (int i = 0; i < BACK_GUARDS_MAX; i++) {
		if (back_guards[i].screen == screen) {
			back_guards[i].peek = peek;
			return;
		}
	}
}

// True when the step back from the active page will be consumed inside that
// page, i.e. its internal stack is not empty.
static bool back_stays_in_page(void) {
	lv_obj_t *active = lv_screen_active();
	for (int i = 0; i < BACK_GUARDS_MAX; i++) {
		if (back_guards[i].screen == active) {
			return back_guards[i].peek && back_guards[i].peek();
		}
	}
	return false;
}

// The guard for whatever page is on screen, or NULL.
static bool (*active_back_guard(void))(void) {
	lv_obj_t *active = lv_screen_active();
	for (int i = 0; i < BACK_GUARDS_MAX; i++) {
		if (back_guards[i].screen == active) {
			return back_guards[i].handler;
		}
	}
	return NULL;
}

void back_btn_cb(lv_event_t *e) {
	(void)e;

	// The player sits on top of everything else, so it is what the chevron
	// dismisses first.
	if (player_sheet_is_open()) {
		player_sheet_close(true);
		return;
	}

	// A page that wants a word first. It returns true when it has taken the
	// press for itself -- put up a question, say -- and false to let the
	// chevron carry on and leave.
	bool (*guard)(void) = active_back_guard();
	if (guard && guard()) {
		return;
	}

	// Inside the file browser the chevron is also what walks back up the
	// directory tree; it only leaves the page once there's nowhere left to go.
	if (lv_screen_active() == browser_screen && browser_go_up()) {
		return;
	}

	// A page opened from the player's own menu: leaving it goes back INTO
	// the player, over whatever page the sheet had originally slid across.
	if (back_goes_to_player()) {
		player_return_screen = NULL;
		lv_obj_t *previous = history_pop();
		load_screen(previous ? previous : main_menu_screen);
		player_sheet_open(false);
		return;
	}

	lv_obj_t *previous = history_pop();
	load_screen_dir(previous ? previous : main_menu_screen, NAV_BACK);
}

void switch_screen_cb(lv_event_t *e) {
	// A swipe that started on this button is a swipe, not a tap.
	if (player_sheet_drag_active() || switcher_back_drag_active() || coverflow_drag_active()) {
		return;
	}

	lv_obj_t *target_screen = (lv_obj_t *)lv_event_get_user_data(e);

	switch_screen(target_screen);
}

// The chevron sits over the artwork when the player is open with a cover
// loaded, and the artwork backdrop is always the dark treatment, so there it
// must be light regardless of the theme. Both halves of that condition are
// tracked, because either can change while the other holds.
static void chevron_apply_color(void) {
	if (!back_btn_icon) {
		return;
	}
	if (chevron_in_player && chevron_over_cover) {
		lv_obj_set_style_image_recolor(back_btn_icon, lv_color_white(), 0);
		lv_obj_set_style_image_recolor_opa(back_btn_icon, LV_OPA_COVER, 0);
	} else if (!chevron_in_player && chevron_tint_screen && chevron_page == chevron_tint_screen) {
		lv_obj_set_style_image_recolor(back_btn_icon, chevron_tint, 0);
		lv_obj_set_style_image_recolor_opa(back_btn_icon, LV_OPA_COVER, 0);
	} else {
		lv_obj_remove_local_style_prop(back_btn_icon, LV_STYLE_IMAGE_RECOLOR, 0);
		lv_obj_remove_local_style_prop(back_btn_icon, LV_STYLE_IMAGE_RECOLOR_OPA, 0);
	}
}

void back_btn_over_cover(bool over_cover) {
	chevron_over_cover = over_cover;
	chevron_apply_color();
}

void back_btn_tint_on_screen(lv_obj_t *screen, bool on, lv_color_t color) {
	if (on) {
		chevron_tint_screen = screen;
		chevron_tint = color;
	} else if (chevron_tint_screen == screen) {
		chevron_tint_screen = NULL;
	}
	if (!chevron_page) {
		chevron_page = lv_screen_active();
	}
	chevron_apply_color();
}

// Set true by a page that wants no chevron at all: Gearboy, while a game runs.
// This is not a case for the visibility rule below -- that rule says which
// pages have a way back, and Gearboy does. It is that while playing, the only
// exit is the menu, and an arrow drawn over the frame would be a control the
// touchscreen (which belongs to the emulator then) delivers to nobody.
// (Declared with the edge swipe, which honours it too.)

void back_btn_force_hidden(bool hidden) {
	chevron_forced_hidden = hidden;
	back_btn_sync_visibility();
}

void back_btn_hide_on_screen(lv_obj_t *screen, bool hidden) {
	if (hidden) {
		chevron_hidden_on = screen;
	} else if (chevron_hidden_on == screen) {
		chevron_hidden_on = NULL;
	}
	back_btn_sync_visibility();
}

// The one rule for the chevron's visibility, re-applied after any animation
// completes: the open player always shows it, the pages in
// page_has_no_chevron() never do, every other page keeps it. Keeping this
// idempotent and running it last means no unlucky ordering inside a transition
// can leave a stray chevron painted on the menu, even for a single frame.
void back_btn_sync_visibility(void) {
	if (!back_btn) {
		return;
	}
	if (chevron_forced_hidden) {
		lv_obj_set_hidden(back_btn, true);
		return;
	}
	if (player_sheet_is_open()) {
		lv_obj_set_hidden(back_btn, false);
		return;
	}
	if (page_has_no_chevron(lv_screen_active())) {
		lv_obj_set_hidden(back_btn, true);
		lv_obj_set_style_translate_x(back_btn, 0, 0);
	} else {
		lv_obj_set_hidden(back_btn, false);
	}
}

// The player hides the status bar and has no title row, so its chevron sits
// right up in the corner; every other page centres it on the title beside it.
// The button is also forced visible there: the sheet can be pulled in from the
// main menu, where the chevron is normally hidden, and the player must always
// offer the way back regardless of what page it slid over.
void back_btn_player_mode(bool in_player) {
	if (!back_btn) {
		return;
	}

	if (chevron_forced_hidden) {
		lv_obj_set_hidden(back_btn, true);
	} else if (in_player) {
		lv_obj_set_hidden(back_btn, false);
	} else if (page_has_no_chevron(lv_screen_active())) {
		lv_obj_set_hidden(back_btn, true);
	}
	chevron_in_player = in_player;
	// 2.0: the player is put away downwards, so its way out points down.
	if (back_btn_icon) {
		lv_image_set_src(back_btn_icon, in_player ? &icon_chevron_down : &icon_chevron_left);
	}
	chevron_apply_color();
	place_back_btn(!in_player);
}

// The chevron lives on the top layer, so one button serves every page.
void back_btn_init(gui_config_t *cfg) {
	switcher_cfg = cfg;
	back_btn_top_bar_height = cfg->top_bar_height;

	back_btn = lv_btn_create(lv_layer_top());
	lv_obj_set_size(back_btn, BACK_BTN_SIZE, BACK_BTN_SIZE);
	// Bare chevron, no disc behind it -- until pressed: then the round target
	// fills at once, as every 2.0 control does (principle 2).
	lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_radius(back_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_add_style(back_btn, &sx_style_press, LV_STATE_PRESSED);
	lv_obj_set_style_border_width(back_btn, 0, 0);
	lv_obj_set_style_shadow_width(back_btn, 0, 0);
	lv_obj_set_style_pad_all(back_btn, 0, 0);
	lv_obj_add_event_cb(back_btn, back_btn_cb, LV_EVENT_CLICKED, NULL);
	// The target is a fingertip already (SX_HEADER_BTN); a couple of pixels
	// more above and below reach the edges of the header row.
	touch_pad(back_btn, 0, 0, 2, 2);

	place_back_btn(true);

	back_btn_icon = lv_image_create(back_btn);
	lv_image_set_src(back_btn_icon, &icon_chevron_left);
	lv_obj_add_style(back_btn_icon, &theme_style_icon, 0);
	lv_obj_center(back_btn_icon);
}

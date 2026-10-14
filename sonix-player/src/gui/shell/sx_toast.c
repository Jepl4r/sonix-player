#include "sx_toast.h"

#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/spinner.h"
#include "src/gui/shell/sx_frame.h"
#include "src/gui/shell/sx_styles.h"
#include "src/gui/shell/sx_tokens.h"
#include "src/system/core/lang.h"

// Where the toast stands, from the bottom of the screen: over the player's
// control panel, over the bottom slot when something is in it, else near the
// edge (the handoff's 318 / 98 / 16).
#define BOTTOM_OVER_PLAYER (SX_NP_PANEL_H + 6)
#define BOTTOM_OVER_SLOT (SX_MINI_SLOT_H + 10)
#define BOTTOM_PLAIN 16

// A glyph at the start of the line is drawn no larger than this.
#define GLYPH_MAX 30

// A sentence longer than the line wraps, up to this many lines, and stays up
// a little longer, as the old toasts did: this much per character past
// QUICK_CHARS, up to MAX_MS.
#define MAX_LINES 3
#define QUICK_CHARS 60
#define MS_PER_CHAR 35
#define MAX_MS 6000

static lv_obj_t *toast;
static lv_obj_t *blocker; // under a busy toast that may not be put away
static lv_obj_t *glyph_img;
static lv_obj_t *spinner;
static lv_obj_t *label;
static lv_obj_t *undo_btn;
static lv_obj_t *undo_label;
static lv_obj_t *bar_track;
static lv_obj_t *bar_fill;
static lv_timer_t *end_timer;

static void (*undo_cb)(void *);
static void *undo_user;
static bool busy;
static bool busy_dismissable;
static int bar_w;

static lv_point_t press_at;
static bool press_tracking;

bool sx_toast_showing(void) { return toast && !lv_obj_is_hidden(toast); }

bool sx_toast_busy_showing(void) { return busy && sx_toast_showing(); }

static void bar_exec_cb(void *obj, int32_t v) { lv_obj_set_width((lv_obj_t *)obj, v); }

void sx_toast_dismiss(void) {
	if (!toast) {
		return;
	}
	lv_obj_set_hidden(toast, true);
	lv_obj_set_hidden(blocker, true);
	lv_timer_pause(end_timer);
	lv_anim_delete(bar_fill, bar_exec_cb);
	undo_cb = NULL;
	undo_user = NULL;
	busy = false;
	busy_dismissable = false;
	if (spinner) {
		lv_obj_delete(spinner);
		spinner = NULL;
	}
}

static void end_timer_cb(lv_timer_t *t) {
	(void)t;
	if (!busy) {
		sx_toast_dismiss();
	}
}

static void undo_clicked_cb(lv_event_t *e) {
	(void)e;
	void (*cb)(void *) = undo_cb;
	void *user = undo_user;
	sx_toast_dismiss();
	if (cb) {
		cb(user);
	}
}

// A pull down puts it away; so does a tap on it, as the old card's did.
static void toast_event_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_indev_t *indev = lv_indev_active();
	if (!indev) {
		return;
	}
	lv_point_t p;
	lv_indev_get_point(indev, &p);
	if (code == LV_EVENT_PRESSED) {
		press_at = p;
		press_tracking = true;
		return;
	}
	if (code == LV_EVENT_PRESS_LOST) {
		press_tracking = false;
		return;
	}
	if (code != LV_EVENT_RELEASED || !press_tracking) {
		return;
	}
	press_tracking = false;
	if (busy && !busy_dismissable) {
		return;
	}
	int dy = p.y - press_at.y;
	int dx = p.x - press_at.x;
	if (dy > SX_TOAST_DISMISS_PX || (LV_ABS(dx) < SX_SWIPE_LOCK_PX && LV_ABS(dy) < SX_SWIPE_LOCK_PX)) {
		sx_toast_dismiss();
	}
}

void sx_toast_init(void) {
	if (toast) {
		return;
	}
	lv_display_t *d = lv_display_get_default();
	int32_t w = lv_display_get_horizontal_resolution(d);

	// The busy toast that may not be put away holds the page while its work
	// runs, as the card it replaces did (several of those jobs have no guard
	// against being started twice): an invisible layer takes the taps. Not
	// dimmed -- a translucent layer the size of the screen is what the
	// platform cannot blend.
	blocker = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(blocker);
	lv_obj_set_size(blocker, lv_pct(100), lv_pct(100));
	lv_obj_set_scrollable(blocker, false);
	lv_obj_set_clickable(blocker, true);
	lv_obj_set_hidden(blocker, true);

	toast = sx_box(lv_layer_top());
	lv_obj_set_width(toast, w - 2 * SX_TOAST_MARGIN);
	lv_obj_set_height(toast, LV_SIZE_CONTENT);
	lv_obj_set_style_min_height(toast, SX_TOAST_H, 0);
	lv_obj_set_style_radius(toast, SX_TOAST_R, 0);
	lv_obj_set_style_bg_color(toast, lv_color_hex(SX_TOAST_BG), 0);
	lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, 0);
	lv_obj_set_style_text_color(toast, lv_color_hex(SX_TOAST_TEXT), 0);
	lv_obj_set_style_pad_left(toast, 22, 0);
	lv_obj_set_style_pad_right(toast, 8, 0);
	lv_obj_set_style_pad_ver(toast, 8, 0);
	lv_obj_set_style_pad_column(toast, 8, 0);
	lv_obj_set_flex_flow(toast, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(toast, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_clickable(toast, true);
	lv_obj_set_event_bubble(toast, false);
	lv_obj_add_event_cb(toast, toast_event_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(toast, toast_event_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(toast, toast_event_cb, LV_EVENT_PRESS_LOST, NULL);
	lv_obj_set_hidden(toast, true);

	glyph_img = lv_image_create(toast);
	lv_obj_set_style_image_recolor_opa(glyph_img, LV_OPA_COVER, 0);
	lv_obj_set_hidden(glyph_img, true);

	label = lv_label_create(toast);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_flex_grow(label, 1);
	lv_obj_set_style_max_height(label, MAX_LINES * lv_font_get_line_height(&font_ui_22), 0);

	undo_btn = lv_obj_create(toast);
	lv_obj_remove_style_all(undo_btn);
	lv_obj_set_size(undo_btn, LV_SIZE_CONTENT, SX_TOAST_H - 16);
	lv_obj_set_style_pad_hor(undo_btn, 18, 0);
	lv_obj_set_style_radius(undo_btn, SX_TOAST_R - 4, 0);
	lv_obj_set_style_bg_color(undo_btn, lv_color_hex(SX_L_PRESS), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(undo_btn, LV_OPA_COVER, LV_STATE_PRESSED);
	lv_obj_set_scrollable(undo_btn, false);
	lv_obj_set_ext_click_area(undo_btn, 8);
	lv_obj_add_event_cb(undo_btn, undo_clicked_cb, LV_EVENT_CLICKED, NULL);
	undo_label = lv_label_create(undo_btn);
	lv_obj_set_style_text_font(undo_label, &font_ui_22_bold, 0);
	lv_obj_set_style_text_color(undo_label, lv_color_hex(SX_TOAST_ACTION), 0);
	lv_obj_center(undo_label);
	lv_obj_set_hidden(undo_btn, true);

	// The countdown: 3 px along the bottom, 22 px in from each side.
	bar_w = (int)(w - 2 * SX_TOAST_MARGIN - 2 * 22);
	bar_track = sx_box(toast);
	lv_obj_set_ignore_layout(bar_track, true);
	lv_obj_set_size(bar_track, bar_w, 3);
	lv_obj_align(bar_track, LV_ALIGN_BOTTOM_LEFT, 0, 2);
	lv_obj_set_style_radius(bar_track, 2, 0);
	lv_obj_set_style_bg_color(bar_track, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(bar_track, 20, 0);
	bar_fill = sx_box(bar_track);
	lv_obj_set_size(bar_fill, bar_w, 3);
	lv_obj_set_style_radius(bar_fill, 2, 0);
	lv_obj_set_style_bg_color(bar_fill, lv_color_hex(SX_TOAST_ACTION), 0);
	lv_obj_set_style_bg_opa(bar_fill, LV_OPA_COVER, 0);
	lv_obj_set_hidden(bar_track, true);

	end_timer = lv_timer_create(end_timer_cb, SX_TOAST_MS, NULL);
	lv_timer_pause(end_timer);
}

static uint32_t shown_for(const char *text) {
	size_t length = text ? strlen(text) : 0;
	uint32_t ms = SX_TOAST_MS;
	if (length > QUICK_CHARS) {
		ms += (uint32_t)(length - QUICK_CHARS) * MS_PER_CHAR;
	}
	return ms < MAX_MS ? ms : MAX_MS;
}

static void place(void) {
	int bottom = BOTTOM_PLAIN;
	if (player_sheet_is_open()) {
		bottom = BOTTOM_OVER_PLAYER;
	} else if (sx_frame_slot_occupied()) {
		bottom = BOTTOM_OVER_SLOT;
	}
	lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -bottom);
}

void sx_toast_follow_slot(void) {
	if (sx_toast_showing()) {
		place();
	}
}

static void show(const lv_image_dsc_t *glyph, lv_color_t colour, const char *text, void (*undo)(void *),
				 void *user, bool is_busy, bool dismissable) {
	sx_toast_init();

	// A toast up is replaced: its undo, if any, is not offered any more.
	lv_anim_delete(bar_fill, bar_exec_cb);
	if (spinner) {
		lv_obj_delete(spinner);
		spinner = NULL;
	}

	busy = is_busy;
	busy_dismissable = dismissable;
	undo_cb = undo;
	undo_user = user;

	if (glyph) {
		lv_image_set_src(glyph_img, glyph);
		lv_obj_set_style_image_recolor(glyph_img, colour, 0);
		int32_t big = glyph->header.w > glyph->header.h ? glyph->header.w : glyph->header.h;
		uint32_t scale = big > GLYPH_MAX ? (uint32_t)(GLYPH_MAX * LV_SCALE_NONE / big) : LV_SCALE_NONE;
		lv_image_set_scale(glyph_img, scale);
		// The object keeps the bitmap's own size at any scale; shrink it to the
		// drawn size so the text starts where the glyph ends.
		lv_obj_set_size(glyph_img, big > GLYPH_MAX ? GLYPH_MAX : glyph->header.w,
						big > GLYPH_MAX ? GLYPH_MAX : glyph->header.h);
		lv_image_set_inner_align(glyph_img, LV_IMAGE_ALIGN_CENTER);
		lv_obj_set_hidden(glyph_img, false);
	} else {
		lv_obj_set_hidden(glyph_img, true);
	}
	if (is_busy) {
		spinner = spinner_create(toast, &icon_loader_small);
		lv_obj_move_to_index(spinner, 0);
	}

	const char *shown = text ? text : "";
	// With "Undo" beside it the line is the handoff's one line, ending in
	// "..." when it is too long: what changed is named in a few words, and a
	// wrap beside the button made a short name break over two lines and the
	// toast grow taller than the slot it stands over. Without the button a
	// longer sentence wraps rather than losing its end, as the old centred
	// card let it.
	lv_label_set_long_mode(label, undo ? LV_LABEL_LONG_DOT : LV_LABEL_LONG_WRAP);
	lv_obj_set_style_max_height(label, (undo ? 1 : MAX_LINES) * lv_font_get_line_height(&font_ui_22), 0);
	lv_label_set_text(label, shown);

	lv_obj_set_hidden(undo_btn, undo == NULL);
	if (undo) {
		lv_label_set_text(undo_label, tr("sx_undo"));
	}

	uint32_t ms = undo ? SX_TOAST_MS : shown_for(shown);
	lv_obj_set_hidden(bar_track, undo == NULL);
	if (undo) {
		lv_obj_set_width(bar_fill, bar_w);
		lv_anim_t a;
		lv_anim_init(&a);
		lv_anim_set_var(&a, bar_fill);
		lv_anim_set_exec_cb(&a, bar_exec_cb);
		lv_anim_set_values(&a, bar_w, 0);
		lv_anim_set_duration(&a, ms);
		lv_anim_set_path_cb(&a, lv_anim_path_linear);
		lv_anim_start(&a);
	}

	place();
	bool hold = is_busy && !dismissable;
	lv_obj_set_hidden(blocker, !hold);
	if (hold) {
		lv_obj_move_foreground(blocker);
	}
	lv_obj_set_hidden(toast, false);
	lv_obj_move_foreground(toast);

	if (is_busy) {
		lv_timer_pause(end_timer);
	} else {
		lv_timer_set_period(end_timer, ms);
		lv_timer_reset(end_timer);
		lv_timer_resume(end_timer);
	}
}

void sx_toast(const char *text) { show(NULL, lv_color_black(), text, NULL, NULL, false, false); }

void sx_toast_undo(const char *text, void (*undo)(void *user), void *user) {
	show(NULL, lv_color_black(), text, undo, user, false, false);
}

void sx_toast_glyph(const lv_image_dsc_t *glyph, lv_color_t colour, const char *text) {
	show(glyph, colour, text, NULL, NULL, false, false);
}

void sx_toast_busy(const char *text, bool dismissable) {
	show(NULL, lv_color_black(), text, NULL, NULL, true, dismissable);
}

void sx_toast_busy_end(void) {
	if (busy) {
		sx_toast_dismiss();
	}
}

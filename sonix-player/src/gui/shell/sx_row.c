#include "sx_row.h"

#include <stdio.h>
#include <stdlib.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/sx_palette.h"
#include "src/gui/shell/sx_styles.h"
#include "src/gui/shell/sx_tokens.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/touch.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"

typedef enum { LEAD_NONE, LEAD_THUMB, LEAD_NUMBER, LEAD_LETTER } lead_kind_t;

typedef struct {
	sx_row_handlers_t h;
	bool swipe_next_on;
	bool swipe_end_on;
	void *user;
	bool compact;
	bool playing;
	bool starred;
	bool lift;
	lv_color_t lift_page;
	lead_kind_t lead_kind;
	sx_row_trail_t trail_kind;

	lv_obj_t *underlay;
	lv_obj_t *under_icon;
	lv_obj_t *under_label;
	lv_obj_t *content;
	lv_obj_t *lead;
	lv_obj_t *lead_label;
	lv_obj_t *lead_image;
	lv_obj_t *eq;
	lv_obj_t *title;
	lv_obj_t *sub;
	lv_obj_t *badge;
	lv_obj_t *duration;
	lv_obj_t *trail;
	lv_obj_t *trail_icon;

	// The gesture in progress.
	lv_point_t press_at;
	bool tracking;
	bool swiping;
	bool long_pressed;
	int32_t dx;
	lv_timer_t *hold_timer;
	lv_obj_t *held_scroller; // the ancestor whose scroll is held off
} row_t;

static row_t *row_of(lv_obj_t *row) { return row ? lv_obj_get_user_data(row) : NULL; }

int sx_row_height(bool compact) {
	bool big = touch_large_rows();
	if (compact) {
		return big ? SX_ROW_H_COMPACT_BIG : SX_ROW_H_COMPACT;
	}
	return big ? SX_ROW_H_BIG : SX_ROW_H;
}

static int thumb_size(bool compact) {
	if (compact) {
		return SX_ROW_THUMB_COMPACT;
	}
	return touch_large_rows() ? SX_ROW_THUMB_BIG : SX_ROW_THUMB;
}

// ---------------------------------------------------------------------------
// The equaliser bars
// ---------------------------------------------------------------------------

#define EQ_BAR_W 5
#define EQ_BAR_H 22
#define EQ_GAP 3
#define EQ_W (3 * EQ_BAR_W + 2 * EQ_GAP)
#define EQ_STEPS 4

// Each bar steps between its resting height and the full one, four steps up
// and four down, at its own pace -- the prototype's 0.8, 0.6 and 1.0 s with
// steps(4), each a quarter of that per step.
static const uint8_t EQ_REST_PCT[3] = {50, 90, 35};
static const uint16_t EQ_STEP_MS[3] = {200, 150, 250};
static const uint16_t EQ_DELAY_MS[3] = {0, 150, 300};

// Every row that has ever played keeps its bars (made once, hidden after), and
// a list's pool rows each come to play in turn as it scrolls: room for all of
// them, or the later ones would stand still.
#define EQS_MAX 96
static lv_obj_t *eqs[EQS_MAX];
static int eq_count;
static bool eq_playing;
static lv_timer_t *eq_timer;
static uint32_t eq_clock_ms; // time the bars have run, which pausing stops

static int32_t eq_bar_height(int bar) {
	uint32_t t = eq_clock_ms + EQ_DELAY_MS[bar];
	uint32_t step = (t / EQ_STEP_MS[bar]) % (2 * EQ_STEPS);
	uint32_t k = step < EQ_STEPS ? step : 2 * EQ_STEPS - step; // up, then down
	uint32_t rest = EQ_REST_PCT[bar];
	uint32_t pct = rest + (100 - rest) * k / EQ_STEPS;
	return (int32_t)(EQ_BAR_H * pct / 100);
}

static void eq_paint(lv_obj_t *eq) {
	for (int i = 0; i < 3; i++) {
		lv_obj_t *bar = lv_obj_get_child(eq, i);
		// lv_obj_set_height() with the height it already has does nothing:
		// only a bar that changed step is redrawn.
		lv_obj_set_height(bar, eq_bar_height(i));
	}
}

static bool eq_any_shown(void) {
	for (int i = 0; i < eq_count; i++) {
		if (!lv_obj_is_hidden(eqs[i]) && lv_obj_is_visible(eqs[i])) {
			return true;
		}
	}
	return false;
}

// Runs while the music plays and a set of bars exists; steps them only while
// one of them is on screen. Pausing it when none is on screen would leave the
// bars standing still on coming back to their page until the next change of
// play state -- the visibility check is the cheaper price.
static void eq_timer_cb(lv_timer_t *t) {
	if (!eq_playing || eq_count == 0) {
		lv_timer_pause(t);
		return;
	}
	if (!eq_any_shown()) {
		return;
	}
	eq_clock_ms += SX_EQ_FRAME_MS;
	for (int i = 0; i < eq_count; i++) {
		if (!lv_obj_is_hidden(eqs[i])) {
			eq_paint(eqs[i]);
		}
	}
}

// Runs the timer exactly while there is something to move.
static void eq_wake(void) {
	if (!eq_timer) {
		eq_timer = lv_timer_create(eq_timer_cb, SX_EQ_FRAME_MS, NULL);
		// Nothing to animate for a dark panel.
		power_pause_in_standby(eq_timer);
	}
	bool run = eq_playing && eq_count > 0;
	if (run) {
		lv_timer_resume(eq_timer);
	} else {
		lv_timer_pause(eq_timer);
	}
}

static void eq_deleted_cb(lv_event_t *e) {
	lv_obj_t *eq = lv_event_get_target(e);
	for (int i = 0; i < eq_count; i++) {
		if (eqs[i] == eq) {
			eqs[i] = eqs[--eq_count];
			break;
		}
	}
}

lv_obj_t *sx_eq_create(lv_obj_t *parent) {
	lv_obj_t *eq = sx_box(parent);
	lv_obj_set_size(eq, EQ_W, EQ_BAR_H);
	lv_obj_set_flex_flow(eq, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(eq, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
	lv_obj_set_style_pad_column(eq, EQ_GAP, 0);
	for (int i = 0; i < 3; i++) {
		lv_obj_t *bar = sx_box(eq);
		lv_obj_set_size(bar, EQ_BAR_W, eq_bar_height(i));
		lv_obj_set_style_radius(bar, 2, 0);
		lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
		// The bars are drawn in the colour of what they stand on: the accent
		// on a number, white over a thumbnail. The caller can restyle.
		lv_obj_add_style(bar, &sx_style_accent, 0);
	}
	if (eq_count < EQS_MAX) {
		eqs[eq_count++] = eq;
		lv_obj_add_event_cb(eq, eq_deleted_cb, LV_EVENT_DELETE, NULL);
	}
	eq_wake();
	return eq;
}

void sx_eq_set_playing(bool playing) {
	if (playing == eq_playing) {
		return;
	}
	eq_playing = playing;
	eq_wake();
}

// ---------------------------------------------------------------------------
// The swipe
// ---------------------------------------------------------------------------

static void content_x_cb(void *obj, int32_t v) { lv_obj_set_style_translate_x((lv_obj_t *)obj, v, 0); }

static void snapped_cb(lv_anim_t *a) {
	row_t *r = lv_anim_get_user_data(a);
	if (r && r->dx == 0) {
		lv_obj_set_hidden(r->underlay, true);
		lv_obj_remove_style(r->content, &sx_style_page, 0);
		if (r->lift && !r->playing) {
			lv_obj_set_style_bg_opa(r->content, LV_OPA_TRANSP, 0);
		}
	}
}

static void snap_back(row_t *r) {
	int32_t from = lv_obj_get_style_translate_x(r->content, LV_PART_MAIN);
	r->dx = 0;
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, r->content);
	lv_anim_set_exec_cb(&a, content_x_cb);
	lv_anim_set_values(&a, from, 0);
	lv_anim_set_duration(&a, SX_ANIM_ROW_SNAP_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_user_data(&a, r);
	lv_anim_set_completed_cb(&a, snapped_cb);
	lv_anim_start(&a);
}

// The underlay for a pull to the right (dx > 0) or the left.
static void underlay_for(row_t *r, int32_t dx) {
	lv_obj_remove_style(r->underlay, &sx_style_accent, 0);
	lv_obj_remove_style(r->underlay, &sx_style_swipe_end, 0);
	lv_obj_remove_local_style_prop(r->underlay, LV_STYLE_BG_COLOR, 0);
	lv_obj_remove_local_style_prop(r->underlay, LV_STYLE_TEXT_COLOR, 0);
	lv_obj_remove_local_style_prop(r->under_icon, LV_STYLE_IMAGE_RECOLOR, 0);
	if (dx > 0) {
		lv_obj_add_style(r->underlay, &sx_style_accent, 0);
		lv_image_set_src(r->under_icon, &icon_list_start);
		lv_label_set_text(r->under_label, tr("sx_row_next"));
		lv_obj_set_flex_align(r->underlay, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	} else if (r->h.end_is_remove) {
		lv_obj_set_style_bg_color(r->underlay, lv_color_hex(SX_DANGER_BG), 0);
		lv_obj_set_style_text_color(r->underlay, lv_color_white(), 0);
		lv_obj_set_style_image_recolor(r->under_icon, lv_color_white(), 0);
		lv_image_set_src(r->under_icon, &icon_x_small);
		lv_label_set_text(r->under_label, tr("sx_row_remove"));
		lv_obj_set_flex_align(r->underlay, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	} else {
		lv_obj_add_style(r->underlay, &sx_style_swipe_end, 0);
		lv_image_set_src(r->under_icon, &icon_list_plus_small);
		lv_label_set_text(r->under_label, tr("sx_row_end"));
		lv_obj_set_flex_align(r->underlay, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	}
}

static lv_obj_t *scroll_ancestor(lv_obj_t *obj) {
	for (lv_obj_t *p = lv_obj_get_parent(obj); p; p = lv_obj_get_parent(p)) {
		if (lv_obj_is_scrollable(p) && (lv_obj_get_scroll_dir(p) & LV_DIR_VER)) {
			return p;
		}
	}
	return NULL;
}

static void release_scroller(row_t *r) {
	if (r->held_scroller) {
		lv_obj_set_scrollable(r->held_scroller, true);
		r->held_scroller = NULL;
	}
}

static void hold_fired_cb(lv_timer_t *t) {
	row_t *r = lv_timer_get_user_data(t);
	lv_timer_delete(t);
	if (!r) {
		return;
	}
	r->hold_timer = NULL;
	if (!r->tracking || r->swiping || !r->h.on_long_press) {
		return;
	}
	r->long_pressed = true;
	lv_obj_remove_state(r->content, LV_STATE_PRESSED);
	r->h.on_long_press(lv_obj_get_parent(r->content), r->user);
}

static void hold_cancel(row_t *r) {
	if (r->hold_timer) {
		lv_timer_delete(r->hold_timer);
		r->hold_timer = NULL;
	}
}

static void content_event_cb(lv_event_t *e) {
	lv_obj_t *row = lv_obj_get_parent(lv_event_get_current_target(e));
	row_t *r = row_of(row);
	lv_indev_t *indev = lv_indev_active();
	if (!r || !indev) {
		return;
	}
	lv_event_code_t code = lv_event_get_code(e);
	lv_point_t p;
	lv_indev_get_point(indev, &p);

	if (code == LV_EVENT_PRESSED) {
		lv_anim_delete(r->content, content_x_cb);
		r->press_at = p;
		r->tracking = true;
		r->swiping = false;
		r->long_pressed = false;
		r->dx = 0;
		hold_cancel(r);
		if (r->h.on_long_press) {
			r->hold_timer = lv_timer_create(hold_fired_cb, SX_LONGPRESS_MS, r);
		}
		return;
	}

	if (code == LV_EVENT_PRESSING) {
		if (!r->tracking) {
			return;
		}
		int32_t dx = p.x - r->press_at.x;
		int32_t dy = p.y - r->press_at.y;
		if (!r->swiping) {
			// The list took it as a scroll, or it is a vertical drag: the row
			// lets go, hold included.
			if (lv_indev_get_scroll_obj(indev) || LV_ABS(dy) > SX_SWIPE_LOCK_PX) {
				r->tracking = false;
				hold_cancel(r);
				return;
			}
			if (LV_ABS(dx) <= SX_SWIPE_LOCK_PX || LV_ABS(dx) <= LV_ABS(dy)) {
				return;
			}
			bool allowed = dx > 0 ? r->swipe_next_on : r->swipe_end_on;
			// A press in the left edge strip is the swipe back's.
			if (!allowed || r->press_at.x < SX_EDGE_BACK_W) {
				r->tracking = false;
				hold_cancel(r);
				return;
			}
			r->swiping = true;
			hold_cancel(r);
			lv_obj_remove_state(r->content, LV_STATE_PRESSED);
			// The handoff's way out of the conflict with the list: no vertical
			// scroll may start while the row is held sideways.
			r->held_scroller = scroll_ancestor(row);
			if (r->held_scroller) {
				lv_obj_set_scrollable(r->held_scroller, false);
			}
			// Opaque while it slides, so the underlay shows only where the row
			// has moved off it (the playing row is opaque already).
			if (r->lift) {
				// Over the gradient: its colour, the playing row's wash on top.
				lv_obj_set_style_bg_color(r->content,
										  r->playing ? lv_color_mix(lv_color_white(), r->lift_page, 41) : r->lift_page, 0);
				lv_obj_set_style_bg_opa(r->content, LV_OPA_COVER, 0);
			} else if (!r->playing) {
				lv_obj_add_style(r->content, &sx_style_page, 0);
			}
			lv_obj_set_hidden(r->underlay, false);
		}
		int32_t lo = r->swipe_end_on ? -SX_SWIPE_MAX_PX : 0;
		int32_t hi = r->swipe_next_on ? SX_SWIPE_MAX_PX : 0;
		int32_t x = dx < lo ? lo : (dx > hi ? hi : dx);
		if ((x > 0) != (r->dx > 0) || r->dx == 0) {
			underlay_for(r, x);
		}
		r->dx = x;
		lv_obj_set_style_translate_x(r->content, x, 0);
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		hold_cancel(r);
		bool was_swiping = r->swiping;
		int32_t dx = r->dx;
		r->tracking = false;
		r->swiping = false;
		release_scroller(r);
		if (!was_swiping) {
			return;
		}
		snap_back(r);
		if (code == LV_EVENT_PRESS_LOST) {
			return;
		}
		if (dx >= SX_SWIPE_COMMIT_PX && r->h.on_swipe_next) {
			r->h.on_swipe_next(row, r->user);
		} else if (dx <= -SX_SWIPE_COMMIT_PX && r->h.on_swipe_end) {
			r->h.on_swipe_end(row, r->user);
		}
		return;
	}

	if (code == LV_EVENT_CLICKED) {
		if (r->long_pressed || r->dx != 0 || player_sheet_drag_active() || switcher_back_drag_active()) {
			return;
		}
		if (r->h.on_tap) {
			r->h.on_tap(row, r->user);
		}
	}
}

static void star_clicked_cb(lv_event_t *e) {
	lv_obj_t *row = lv_event_get_user_data(e);
	row_t *r = row_of(row);
	if (!r || r->trail_kind != SX_ROW_TRAIL_STAR || switcher_back_drag_active()) {
		return;
	}
	if (r->h.on_star) {
		r->h.on_star(row, r->user);
	}
}

static void row_deleted_cb(lv_event_t *e) {
	row_t *r = lv_event_get_user_data(e);
	if (r) {
		hold_cancel(r);
		release_scroller(r);
		free(r);
	}
}

// ---------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------

static void apply_sizes(row_t *r, lv_obj_t *row) {
	int h = sx_row_height(r->compact);
	int t = thumb_size(r->compact);
	lv_obj_set_height(row, h);
	lv_obj_set_height(r->trail, h);
	if (r->lead_kind == LEAD_NUMBER) {
		lv_obj_set_size(r->lead, SX_ROW_NUM_W, t);
	} else {
		lv_obj_set_size(r->lead, t, t);
	}
}

lv_obj_t *sx_row_create(lv_obj_t *parent, const sx_row_handlers_t *handlers, bool compact) {
	row_t *r = calloc(1, sizeof(*r));
	if (!r) {
		return NULL;
	}
	if (handlers) {
		r->h = *handlers;
	}
	r->swipe_next_on = r->h.on_swipe_next != NULL;
	r->swipe_end_on = r->h.on_swipe_end != NULL;
	r->compact = compact;
	r->trail_kind = SX_ROW_TRAIL_NONE;

	lv_obj_t *row = sx_box(parent);
	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_style_radius(row, SX_ROW_R, 0);
	lv_obj_set_style_margin_bottom(row, SX_ROW_GAP, 0);
	lv_obj_set_user_data(row, r);
	lv_obj_add_event_cb(row, row_deleted_cb, LV_EVENT_DELETE, r);

	// What a pulled row shows underneath: the action's colour, its glyph and
	// its word at the side the row is pulled away from.
	r->underlay = sx_box(row);
	lv_obj_set_size(r->underlay, lv_pct(100), lv_pct(100));
	lv_obj_set_style_radius(r->underlay, SX_ROW_R, 0);
	lv_obj_set_style_bg_opa(r->underlay, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_hor(r->underlay, 18, 0);
	lv_obj_set_style_pad_column(r->underlay, 8, 0);
	lv_obj_set_flex_flow(r->underlay, LV_FLEX_FLOW_ROW);
	r->under_icon = lv_image_create(r->underlay);
	lv_obj_set_style_image_recolor_opa(r->under_icon, LV_OPA_COVER, 0);
	r->under_label = lv_label_create(r->underlay);
	lv_obj_set_style_text_font(r->under_label, &font_ui_20_bold, 0);
	lv_obj_set_hidden(r->underlay, true);

	// The part that slides: the whole visible row.
	r->content = lv_obj_create(row);
	lv_obj_remove_style_all(r->content);
	lv_obj_set_size(r->content, lv_pct(100), lv_pct(100));
	lv_obj_set_style_radius(r->content, SX_ROW_R, 0);
	lv_obj_add_style(r->content, &sx_style_press, LV_STATE_PRESSED);
	lv_obj_set_style_pad_left(r->content, SX_ROW_PAD_L, 0);
	lv_obj_set_style_pad_column(r->content, SX_ROW_INNER_GAP, 0);
	lv_obj_set_flex_flow(r->content, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r->content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(r->content, false);
	lv_obj_set_event_bubble(r->content, false);
	lv_obj_add_event_cb(r->content, content_event_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(r->content, content_event_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(r->content, content_event_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(r->content, content_event_cb, LV_EVENT_PRESS_LOST, NULL);
	lv_obj_add_event_cb(r->content, content_event_cb, LV_EVENT_CLICKED, NULL);

	r->lead = sx_box(r->content);
	lv_obj_set_style_radius(r->lead, SX_ROW_THUMB_R, 0);
	lv_obj_set_style_clip_corner(r->lead, false, 0);
	r->lead_label = lv_label_create(r->lead);
	lv_obj_center(r->lead_label);
	lv_label_set_text(r->lead_label, "");

	lv_obj_t *text = sx_box(r->content);
	lv_obj_set_height(text, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(text, 1);
	lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(text, 1, 0);

	r->title = sx_label(text, &font_ui_26, &sx_style_text);
	lv_obj_set_width(r->title, lv_pct(100));
	// One line, always: LV_LABEL_LONG_DOT wraps before it truncates, and a long
	// title on two lines pushes the line under it out of the row.
	lv_obj_set_height(r->title, lv_font_get_line_height(&font_ui_26));

	lv_obj_t *line = sx_box(text);
	lv_obj_set_size(line, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(line, 8, 0);
	r->sub = sx_label(line, &font_ui_20, &sx_style_text2);
	lv_obj_set_style_max_width(r->sub, lv_pct(75), 0);
	lv_obj_set_height(r->sub, lv_font_get_line_height(&font_ui_20));
	r->badge = sx_label(line, &font_ui_20, &sx_style_text_accent);
	r->duration = sx_label(line, &font_ui_20, &sx_style_text2);
	lv_obj_set_style_text_align(r->duration, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_set_hidden(r->badge, true);
	lv_obj_set_hidden(r->duration, true);

	// The trailing slot: a 72 px button of its own for the star (a tap on it
	// is not a tap on the row), the chevron otherwise.
	r->trail = lv_obj_create(r->content);
	lv_obj_remove_style_all(r->trail);
	lv_obj_set_width(r->trail, SX_ROW_TRAIL_W);
	lv_obj_set_scrollable(r->trail, false);
	lv_obj_set_event_bubble(r->trail, false);
	lv_obj_add_event_cb(r->trail, star_clicked_cb, LV_EVENT_CLICKED, row);
	r->trail_icon = lv_image_create(r->trail);
	lv_obj_center(r->trail_icon);
	lv_obj_set_hidden(r->trail, true);

	sx_row_set_no_lead(row);
	apply_sizes(r, row);
	return row;
}

void sx_row_set_compact(lv_obj_t *row, bool compact) {
	row_t *r = row_of(row);
	if (r) {
		r->compact = compact;
		apply_sizes(r, row);
	}
}

void sx_row_set_user(lv_obj_t *row, void *user) {
	row_t *r = row_of(row);
	if (r) {
		r->user = user;
	}
}

void *sx_row_get_user(lv_obj_t *row) {
	row_t *r = row_of(row);
	return r ? r->user : NULL;
}

void sx_row_set_title(lv_obj_t *row, const char *title) {
	row_t *r = row_of(row);
	if (r) {
		lv_label_set_text(r->title, title ? title : "");
	}
}

void sx_row_set_subtitle(lv_obj_t *row, const char *subtitle, const char *badge, const char *duration) {
	row_t *r = row_of(row);
	if (!r) {
		return;
	}
	lv_label_set_text(r->sub, subtitle ? subtitle : "");
	lv_obj_set_hidden(r->badge, !badge || !badge[0]);
	lv_label_set_text(r->badge, badge ? badge : "");
	// Whatever follows the subtitle takes the rest of the line, so the
	// duration ends up against the right edge either way.
	bool has_badge = badge && badge[0];
	lv_obj_set_flex_grow(r->badge, has_badge ? 1 : 0);
	lv_obj_set_flex_grow(r->duration, has_badge ? 0 : 1);
	lv_obj_set_hidden(r->duration, !duration || !duration[0]);
	lv_label_set_text(r->duration, duration ? duration : "");
}

// The lead's look for each kind; what the others left is taken off first, so
// a recycled row changes kind cleanly.
static void lead_reset(row_t *r) {
	lv_obj_remove_style(r->lead, &sx_style_cover_tone, 0);
	lv_obj_remove_local_style_prop(r->lead, LV_STYLE_BG_COLOR, 0);
	lv_obj_set_style_bg_opa(r->lead, LV_OPA_TRANSP, 0);
	lv_obj_set_style_radius(r->lead, SX_ROW_THUMB_R, 0);
	lv_obj_set_style_clip_corner(r->lead, false, 0);
	if (r->lead_image) {
		lv_obj_set_hidden(r->lead_image, true);
	}
	lv_obj_remove_style(r->lead_label, &sx_style_text2, 0);
	lv_obj_remove_style(r->lead_label, &sx_style_text_accent, 0);
	lv_obj_remove_local_style_prop(r->lead_label, LV_STYLE_TEXT_COLOR, 0);
	lv_obj_remove_local_style_prop(r->lead_label, LV_STYLE_TEXT_OPA, 0);
	lv_label_set_text(r->lead_label, "");
	lv_obj_set_hidden(r->lead, false);
}

static void eq_place(row_t *r);

// The accent on the album page's always-dark gradient: the player's accent
// (light, SX_T_NP_ACCENT) at the page's hue, whatever the theme.
static lv_color_t lift_accent(void) { return sx_tone(SX_T_NP_ACCENT, sx_pal()->hue); }

void sx_row_set_thumb(lv_obj_t *row, const lv_image_dsc_t *image, lv_color_t tone) {
	row_t *r = row_of(row);
	if (!r) {
		return;
	}
	lead_reset(r);
	r->lead_kind = LEAD_THUMB;
	lv_obj_set_style_bg_color(r->lead, tone, 0);
	lv_obj_set_style_bg_opa(r->lead, LV_OPA_COVER, 0);
	if (image) {
		if (!r->lead_image) {
			r->lead_image = lv_image_create(r->lead);
			lv_obj_center(r->lead_image);
		}
		lv_image_set_src(r->lead_image, image);
		// A 56 px thumbnail on a large row's 68 px lead is drawn to its size.
		int side = thumb_size(r->compact);
		int32_t w = (int32_t)image->header.w;
		lv_image_set_scale(r->lead_image, w > 0 && w != side ? (uint32_t)(side * LV_SCALE_NONE / w) : LV_SCALE_NONE);
		lv_obj_set_hidden(r->lead_image, false);
		lv_obj_set_style_clip_corner(r->lead, true, 0);
	}
	apply_sizes(r, row);
	eq_place(r);
}

void sx_row_set_number(lv_obj_t *row, int number) {
	row_t *r = row_of(row);
	if (!r) {
		return;
	}
	lead_reset(r);
	r->lead_kind = LEAD_NUMBER;
	// 0 or less: no number known (a track without one), an empty lead.
	char text[12] = "";
	if (number > 0) {
		snprintf(text, sizeof(text), "%d", number);
	}
	lv_label_set_text(r->lead_label, text);
	lv_obj_set_style_text_font(r->lead_label, &font_ui_22, 0);
	if (r->lift) {
		lv_obj_set_style_text_color(r->lead_label, lv_color_white(), 0);
		lv_obj_set_style_text_opa(r->lead_label, LV_OPA_80, 0);
	} else {
		lv_obj_remove_local_style_prop(r->lead_label, LV_STYLE_TEXT_OPA, 0);
		lv_obj_add_style(r->lead_label, &sx_style_text2, 0);
	}
	apply_sizes(r, row);
	eq_place(r);
}

void sx_row_set_letter(lv_obj_t *row, const char *letter, lv_color_t tone, bool round) {
	row_t *r = row_of(row);
	if (!r) {
		return;
	}
	lead_reset(r);
	r->lead_kind = LEAD_LETTER;
	lv_obj_set_style_bg_color(r->lead, tone, 0);
	lv_obj_set_style_bg_opa(r->lead, LV_OPA_COVER, 0);
	if (round) {
		lv_obj_set_style_radius(r->lead, LV_RADIUS_CIRCLE, 0);
	}
	lv_label_set_text(r->lead_label, letter ? letter : "");
	lv_obj_set_style_text_font(r->lead_label, &font_ui_26_bold, 0);
	lv_obj_set_style_text_color(r->lead_label, lv_color_white(), 0);
	apply_sizes(r, row);
	eq_place(r);
}

void sx_row_set_no_lead(lv_obj_t *row) {
	row_t *r = row_of(row);
	if (!r) {
		return;
	}
	lead_reset(r);
	r->lead_kind = LEAD_NONE;
	lv_obj_set_hidden(r->lead, true);
}

void sx_row_set_trail(lv_obj_t *row, sx_row_trail_t trail) {
	row_t *r = row_of(row);
	if (!r) {
		return;
	}
	r->trail_kind = trail;
	lv_obj_set_hidden(r->trail, trail == SX_ROW_TRAIL_NONE);
	lv_obj_set_clickable(r->trail, trail == SX_ROW_TRAIL_STAR || trail == SX_ROW_TRAIL_GRIP);
	// A grip is held and dragged: a drag on it must not scroll the list.
	lv_obj_set_scroll_chain_ver(r->trail, trail != SX_ROW_TRAIL_GRIP);
	lv_obj_remove_style(r->trail_icon, &sx_style_star_on, 0);
	lv_obj_remove_style(r->trail_icon, &sx_style_icon, 0);
	lv_obj_remove_style(r->trail_icon, &sx_style_icon_accent, 0);
	lv_obj_remove_local_style_prop(r->trail_icon, LV_STYLE_IMAGE_RECOLOR, 0);
	lv_obj_set_style_image_recolor_opa(r->trail_icon, LV_OPA_COVER, 0);
	lv_obj_set_style_image_opa(r->trail_icon, LV_OPA_COVER, 0);
	lv_obj_set_hidden(r->trail_icon, trail == SX_ROW_TRAIL_BLANK);
	if (trail == SX_ROW_TRAIL_CHEVRON) {
		lv_image_set_src(r->trail_icon, &icon_chevron_right);
		lv_obj_add_style(r->trail_icon, &sx_style_icon, 0);
		if (r->lift) {
			lv_obj_set_style_image_recolor(r->trail_icon, lv_color_white(), 0);
		}
		lv_obj_set_style_image_opa(r->trail_icon, LV_OPA_60, 0);
	} else if (trail == SX_ROW_TRAIL_CHECK) {
		lv_image_set_src(r->trail_icon, &icon_check);
		lv_obj_add_style(r->trail_icon, r->lift ? &sx_style_icon : &sx_style_icon_accent, 0);
		if (r->lift) {
			lv_obj_set_style_image_recolor(r->trail_icon, lv_color_white(), 0);
		}
	} else if (trail == SX_ROW_TRAIL_GRIP) {
		lv_image_set_src(r->trail_icon, &icon_grip);
		lv_obj_add_style(r->trail_icon, &sx_style_icon, 0);
	} else if (trail == SX_ROW_TRAIL_STAR) {
		lv_image_set_src(r->trail_icon, r->starred ? &icon_star_filled : &icon_star);
		if (r->starred && r->lift) {
			lv_obj_set_style_image_recolor(r->trail_icon, lv_color_hex(SX_STAR_ON_DARK), 0);
		} else if (r->starred) {
			lv_obj_add_style(r->trail_icon, &sx_style_star_on, 0);
		} else {
			lv_obj_set_style_image_recolor(r->trail_icon, lv_color_hex(SX_STAR_OFF), 0);
		}
	}
}

void sx_row_set_starred(lv_obj_t *row, bool starred) {
	row_t *r = row_of(row);
	if (!r) {
		return;
	}
	r->starred = starred;
	if (r->trail_kind == SX_ROW_TRAIL_STAR) {
		sx_row_set_trail(row, SX_ROW_TRAIL_STAR);
	}
}

// The bars stand in for the number, or over the middle of the thumbnail, while
// the row plays; made the first time a row plays.
static void eq_place(row_t *r) {
	bool show = r->playing && r->lead_kind != LEAD_NONE;
	if (!show) {
		if (r->eq) {
			lv_obj_set_hidden(r->eq, true);
		}
		if (r->lead_kind == LEAD_NUMBER) {
			lv_obj_set_hidden(r->lead_label, false);
		}
		return;
	}
	if (!r->eq) {
		r->eq = sx_eq_create(r->lead);
		lv_obj_set_ignore_layout(r->eq, true);
	}
	lv_obj_center(r->eq);
	lv_obj_set_hidden(r->eq, false);
	lv_obj_move_foreground(r->eq);
	// On a number the bars are the accent and replace it; over a picture or
	// a plaque they are white.
	for (int i = 0; i < 3; i++) {
		lv_obj_t *bar = lv_obj_get_child(r->eq, i);
		if (r->lead_kind == LEAD_NUMBER && r->lift) {
			lv_obj_remove_style(bar, &sx_style_accent, 0);
			lv_obj_set_style_bg_color(bar, lift_accent(), 0);
		} else if (r->lead_kind == LEAD_NUMBER) {
			lv_obj_remove_local_style_prop(bar, LV_STYLE_BG_COLOR, 0);
			lv_obj_add_style(bar, &sx_style_accent, 0);
		} else {
			lv_obj_remove_style(bar, &sx_style_accent, 0);
			lv_obj_set_style_bg_color(bar, lv_color_white(), 0);
		}
	}
	if (r->lead_kind == LEAD_NUMBER) {
		lv_obj_set_hidden(r->lead_label, true);
	}
	eq_wake();
}

void sx_row_set_playing(lv_obj_t *row, bool playing) {
	row_t *r = row_of(row);
	if (!r || r->playing == playing) {
		return;
	}
	r->playing = playing;
	lv_obj_remove_style(r->content, &sx_style_surface, 0);
	lv_obj_remove_style(r->title, &sx_style_text_accent, 0);
	if (r->lift) {
		// White at 16 % over the gradient (a fixed wash: the platform cannot
		// blend one live), the title in the always-dark accent.
		lv_obj_set_style_bg_color(r->content, lv_color_white(), 0);
		lv_obj_set_style_bg_opa(r->content, playing ? 41 : LV_OPA_TRANSP, 0);
		if (playing) {
			lv_obj_set_style_text_color(r->title, lift_accent(), 0);
		} else {
			lv_obj_set_style_text_color(r->title, lv_color_white(), 0);
		}
		eq_place(r);
		return;
	}
	if (playing) {
		// Under the pressed fill: added before it would lose to it, so the
		// pressed style is put back on top.
		lv_obj_remove_style(r->content, &sx_style_press, LV_STATE_PRESSED);
		lv_obj_add_style(r->content, &sx_style_surface, 0);
		lv_obj_add_style(r->content, &sx_style_press, LV_STATE_PRESSED);
		lv_obj_add_style(r->title, &sx_style_text_accent, 0);
	}
	eq_place(r);
}

void sx_row_set_swipes(lv_obj_t *row, bool next, bool end) {
	row_t *r = row_of(row);
	if (r) {
		r->swipe_next_on = next && r->h.on_swipe_next;
		r->swipe_end_on = end && r->h.on_swipe_end;
	}
}

void sx_row_set_lift(lv_obj_t *row, bool lift, lv_color_t page) {
	row_t *r = row_of(row);
	if (!r) {
		return;
	}
	r->lift_page = page;
	bool changed = r->lift != lift;
	r->lift = lift;
	lv_obj_t *lines[] = {r->sub, r->duration};
	if (changed && lift) {
		lv_obj_remove_style(r->content, &sx_style_press, LV_STATE_PRESSED);
		lv_obj_remove_style(r->content, &sx_style_surface, 0);
		lv_obj_set_style_bg_color(r->content, lv_color_white(), LV_STATE_PRESSED);
		lv_obj_set_style_bg_opa(r->content, 56, LV_STATE_PRESSED);
		lv_obj_set_style_text_color(r->title, lv_color_white(), 0);
		for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
			lv_obj_set_style_text_color(lines[i], lv_color_white(), 0);
			lv_obj_set_style_text_opa(lines[i], LV_OPA_80, 0);
		}
	} else if (changed) {
		lv_obj_remove_local_style_prop(r->content, LV_STYLE_BG_COLOR, LV_STATE_PRESSED);
		lv_obj_remove_local_style_prop(r->content, LV_STYLE_BG_OPA, LV_STATE_PRESSED);
		lv_obj_remove_local_style_prop(r->content, LV_STYLE_BG_COLOR, 0);
		lv_obj_remove_local_style_prop(r->content, LV_STYLE_BG_OPA, 0);
		lv_obj_add_style(r->content, &sx_style_press, LV_STATE_PRESSED);
		lv_obj_remove_local_style_prop(r->title, LV_STYLE_TEXT_COLOR, 0);
		for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
			lv_obj_remove_local_style_prop(lines[i], LV_STYLE_TEXT_COLOR, 0);
			lv_obj_remove_local_style_prop(lines[i], LV_STYLE_TEXT_OPA, 0);
		}
	}
	if (!changed && !lift) {
		return;
	}
	// The parts that look different on the gradient are painted again -- also
	// when the page's hue moved under a row already lifted.
	bool playing = r->playing;
	r->playing = !playing;
	sx_row_set_playing(row, playing);
	sx_row_set_trail(row, r->trail_kind);
	if (r->lead_kind == LEAD_NUMBER) {
		const char *text = lv_label_get_text(r->lead_label);
		sx_row_set_number(row, text && text[0] ? atoi(text) : 0);
	}
}

lv_obj_t *sx_row_trail(lv_obj_t *row) {
	row_t *r = row_of(row);
	return r ? r->trail : NULL;
}

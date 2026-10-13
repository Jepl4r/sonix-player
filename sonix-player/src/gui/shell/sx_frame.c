#include "sx_frame.h"

#include <stdlib.h>

#include "src/gui/shell/sx_mini.h"
#include "src/gui/shell/sx_tokens.h"

static gui_config_t *frame_cfg;
static int body_bottom;

#define BODY_CBS_MAX 16
static sx_frame_cb_t body_cbs[BODY_CBS_MAX];
static int body_cb_count;

// The objects that follow the slot: a scroller gets its padding, a bottom
// control its lift. `base` is the scroller's own bottom padding from before.
typedef enum { FOLLOW_RESERVE, FOLLOW_LIFT, FOLLOW_EXEMPT } follow_kind_t;

typedef struct {
	lv_obj_t *obj;
	follow_kind_t kind;
	int32_t base;
} follower_t;

static follower_t *followers;
static int follower_count;
static int follower_capacity;

// The idle cards: one per page that has one (home, Music).
#define IDLE_CARDS_MAX 4
static struct {
	lv_obj_t *screen;
	lv_obj_t *card;
} idle_cards[IDLE_CARDS_MAX];
static int idle_card_count;

int sx_frame_body_top(void) { return SX_BODY_TOP; }

int sx_frame_body_bottom(void) { return body_bottom; }

bool sx_frame_idle(void) { return !sx_mini_has_track(); }

void sx_frame_on_body_change(sx_frame_cb_t cb) {
	if (cb && body_cb_count < BODY_CBS_MAX) {
		body_cbs[body_cb_count++] = cb;
	}
}

static void apply_follower(const follower_t *f) {
	if (!f->obj) {
		return;
	}
	if (f->kind == FOLLOW_RESERVE) {
		lv_obj_set_style_pad_bottom(f->obj, f->base + body_bottom, 0);
	} else if (f->kind == FOLLOW_LIFT) {
		lv_obj_set_style_translate_y(f->obj, -body_bottom, 0);
	}
}

static void follower_deleted_cb(lv_event_t *e) {
	lv_obj_t *obj = lv_event_get_target(e);
	for (int i = 0; i < follower_count; i++) {
		if (followers[i].obj == obj) {
			followers[i] = followers[--follower_count];
			return;
		}
	}
}

static follower_t *find_follower(lv_obj_t *obj) {
	for (int i = 0; i < follower_count; i++) {
		if (followers[i].obj == obj) {
			return &followers[i];
		}
	}
	return NULL;
}

static void follow(lv_obj_t *obj, follow_kind_t kind) {
	if (!obj || find_follower(obj)) {
		return;
	}
	if (follower_count == follower_capacity) {
		int wanted = follower_capacity ? follower_capacity * 2 : 32;
		follower_t *grown = realloc(followers, (size_t)wanted * sizeof(*grown));
		if (!grown) {
			return; // the object keeps its place; at worst the mini player covers its end
		}
		followers = grown;
		follower_capacity = wanted;
	}
	follower_t *f = &followers[follower_count++];
	f->obj = obj;
	f->kind = kind;
	f->base = kind == FOLLOW_RESERVE ? lv_obj_get_style_pad_bottom(obj, LV_PART_MAIN) : 0;
	lv_obj_add_event_cb(obj, follower_deleted_cb, LV_EVENT_DELETE, NULL);
	apply_follower(f);
}

void sx_frame_reserve_bottom(lv_obj_t *scroller) { follow(scroller, FOLLOW_RESERVE); }

void sx_frame_lift(lv_obj_t *obj) { follow(obj, FOLLOW_LIFT); }

void sx_frame_exempt(lv_obj_t *obj) { follow(obj, FOLLOW_EXEMPT); }

// ---------------------------------------------------------------------------
// The idle cards
// ---------------------------------------------------------------------------

static void place_idle_cards(lv_obj_t *screen) {
	bool idle = sx_frame_idle();
	for (int i = 0; i < idle_card_count; i++) {
		lv_obj_set_hidden(idle_cards[i].card, !(idle && idle_cards[i].screen == screen));
	}
}

void sx_frame_set_idle_card(lv_obj_t *screen, lv_obj_t *card) {
	if (!screen || !card || !frame_cfg) {
		return;
	}
	for (int i = 0; i < idle_card_count; i++) {
		if (idle_cards[i].screen == screen) {
			idle_cards[i].card = card;
			goto placed;
		}
	}
	if (idle_card_count >= IDLE_CARDS_MAX) {
		return;
	}
	idle_cards[idle_card_count].screen = screen;
	idle_cards[idle_card_count].card = card;
	idle_card_count++;
placed:
	lv_obj_set_size(card, (int)frame_cfg->screen_width - 2 * SX_MINI_MARGIN_SIDE, SX_MINI_H);
	lv_obj_set_pos(card, SX_MINI_MARGIN_SIDE, (int)frame_cfg->screen_height - SX_MINI_SLOT_H + SX_MINI_MARGIN_TOP);
	sx_frame_exempt(card);
	place_idle_cards(lv_screen_active());
}

bool sx_frame_slot_occupied(void) {
	if (sx_mini_visible()) {
		return true;
	}
	for (int i = 0; i < idle_card_count; i++) {
		if (!lv_obj_is_hidden(idle_cards[i].card)) {
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// The way in
// ---------------------------------------------------------------------------

// The automatic treatment: a page's scrollable lists that reach the bottom of
// the screen, and its controls aligned to the bottom, two levels down. Asked of
// the styles the page set, not of the laid-out geometry: a page shown for the
// first time has not been laid out yet.
static bool reaches_bottom(lv_obj_t *obj, int32_t parent_h) {
	int32_t h = lv_obj_get_style_height(obj, LV_PART_MAIN);
	int32_t y = lv_obj_get_style_y(obj, LV_PART_MAIN);
	lv_align_t align = lv_obj_get_style_align(obj, LV_PART_MAIN);
	if (LV_COORD_IS_PCT(h)) {
		h = parent_h * LV_COORD_GET_PCT(h) / 100;
	}
	if (h <= 0 || LV_COORD_IS_SPEC(h)) {
		return false;
	}
	if (align == LV_ALIGN_BOTTOM_MID || align == LV_ALIGN_BOTTOM_LEFT || align == LV_ALIGN_BOTTOM_RIGHT) {
		return y >= -2;
	}
	if (align == LV_ALIGN_DEFAULT || align == LV_ALIGN_TOP_LEFT || align == LV_ALIGN_TOP_MID ||
		align == LV_ALIGN_TOP_RIGHT) {
		return y + h >= parent_h - 2;
	}
	return false;
}

static void treat(lv_obj_t *parent, int32_t parent_h, int depth) {
	uint32_t count = lv_obj_get_child_count(parent);
	for (uint32_t i = 0; i < count; i++) {
		lv_obj_t *child = lv_obj_get_child(parent, (int32_t)i);
		if (find_follower(child)) {
			continue;
		}
		lv_align_t align = lv_obj_get_style_align(child, LV_PART_MAIN);
		bool bottom_aligned =
			align == LV_ALIGN_BOTTOM_MID || align == LV_ALIGN_BOTTOM_LEFT || align == LV_ALIGN_BOTTOM_RIGHT;
		if (lv_obj_is_scrollable(child) && (lv_obj_get_scroll_dir(child) & LV_DIR_VER) &&
			reaches_bottom(child, parent_h)) {
			sx_frame_reserve_bottom(child);
			continue;
		}
		if (bottom_aligned && depth == 0 && lv_obj_get_style_y(child, LV_PART_MAIN) < 0) {
			// A control floated above the bottom edge (a button, a note): it
			// is lifted clear of the slot. Something flush with the edge -- a
			// keyboard tray, a panel -- is the page's own bottom and stays.
			sx_frame_lift(child);
			continue;
		}
		if (depth == 0 && !lv_obj_is_scrollable(child)) {
			int32_t h = lv_obj_get_style_height(child, LV_PART_MAIN);
			if (h > 0 && !LV_COORD_IS_SPEC(h) && !LV_COORD_IS_PCT(h)) {
				treat(child, h, depth + 1);
			} else if (LV_COORD_IS_PCT(h)) {
				treat(child, parent_h * LV_COORD_GET_PCT(h) / 100, depth + 1);
			}
		}
	}
}

void sx_frame_page_shown(lv_obj_t *screen) {
	if (!screen || !frame_cfg) {
		return;
	}
	treat(screen, (int32_t)frame_cfg->screen_height, 0);
	place_idle_cards(screen);
}

void sx_frame_slot_changed(void) {
	int now = sx_mini_visible() ? SX_MINI_SLOT_H : 0;
	if (now != body_bottom) {
		body_bottom = now;
		for (int i = 0; i < follower_count; i++) {
			apply_follower(&followers[i]);
		}
		for (int i = 0; i < body_cb_count; i++) {
			body_cbs[i]();
		}
	}
	place_idle_cards(lv_screen_active());
}

void sx_frame_init(gui_config_t *cfg) {
	frame_cfg = cfg;
	sx_mini_init(cfg);
}

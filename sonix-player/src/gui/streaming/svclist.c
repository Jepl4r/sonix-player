#include "svclist.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/sx_frame.h"
#include "src/gui/shell/sx_styles.h"
#include "src/gui/shell/sx_tokens.h"

// See svclist.h.

// A heading: the prototype's group line, 16 px mono caps with 14 px over it
// and 6 under, in the secondary colour.
#define HEADING_H 46
#define HEADING_POOL 6

struct svclist {
	lv_obj_t *scroller;
	lv_obj_t *end; // a point at the bottom: gives the scroll its height
	lv_obj_t **rows;
	int *row_index; // what each row shows now, -1 none
	int pool;
	bool compact;
	int pitch; // a row and the gap under it
	svclist_cbs_t cbs;
	int count;
	// Grouped lists only: where each entry's row starts, and whether a
	// heading stands over it. NULL for a list without groups.
	int *y;
	unsigned char *headed;
	int height;
	int top; // room above the first entry, for the page's own objects
	lv_obj_t *headings[HEADING_POOL];
	int heading_index[HEADING_POOL];
	bool near_end_said; // once per approach, not once per scroll event
};

static int entry_y(const svclist_t *l, int index) { return l->top + (l->y ? l->y[index] : index * l->pitch); }

// The first entry whose row reaches below `y`.
static int entry_at(const svclist_t *l, int y) {
	if (l->count <= 0) {
		return 0;
	}
	y -= l->top;
	if (!l->y) {
		int i = y / l->pitch;
		return i < 0 ? 0 : (i >= l->count ? l->count - 1 : i);
	}
	int lo = 0, hi = l->count - 1;
	while (lo < hi) {
		int mid = (lo + hi + 1) / 2;
		if (l->y[mid] <= y) {
			lo = mid;
		} else {
			hi = mid - 1;
		}
	}
	return lo;
}

static void layout(svclist_t *l) {
	free(l->y);
	free(l->headed);
	l->y = NULL;
	l->headed = NULL;
	l->height = l->count * l->pitch;
	if (!l->cbs.heading || l->count <= 0) {
		return;
	}
	l->y = malloc((size_t)l->count * sizeof(*l->y));
	l->headed = calloc((size_t)l->count, 1);
	if (!l->y || !l->headed) {
		// Out of memory: the list without its headings, still whole.
		free(l->y);
		free(l->headed);
		l->y = NULL;
		l->headed = NULL;
		return;
	}
	int y = 0;
	char text[8];
	for (int i = 0; i < l->count; i++) {
		if (l->cbs.heading(i, text, sizeof(text))) {
			l->headed[i] = 1;
			y += HEADING_H;
		}
		l->y[i] = y;
		y += l->pitch;
	}
	l->height = y;
}

static void bind_row(svclist_t *l, int slot, int index) {
	lv_obj_t *row = l->rows[slot];
	l->row_index[slot] = index;
	sx_row_set_user(row, (void *)(intptr_t)index);
	lv_obj_set_pos(row, 0, entry_y(l, index));
	l->cbs.bind(row, index);
	lv_obj_set_hidden(row, false);
}

static void place_headings(svclist_t *l, int first, int last) {
	int used = 0;
	if (l->headed) {
		for (int i = first; i <= last && i < l->count && used < HEADING_POOL; i++) {
			if (!l->headed[i]) {
				continue;
			}
			lv_obj_t *h = l->headings[used];
			if (!h) {
				h = sx_label(l->scroller, &font_ui_17, &sx_style_text2);
				lv_obj_set_style_text_letter_space(h, 1, 0);
				lv_obj_set_width(h, lv_pct(100));
				l->headings[used] = h;
				l->heading_index[used] = -1;
			}
			if (l->heading_index[used] != i) {
				char text[160];
				if (!l->cbs.heading(i, text, sizeof(text))) {
					text[0] = '\0';
				}
				lv_label_set_text(h, text);
				l->heading_index[used] = i;
			}
			lv_obj_set_pos(h, 6, l->top + l->y[i] - HEADING_H + 14);
			lv_obj_set_hidden(h, false);
			used++;
		}
	}
	for (int k = used; k < HEADING_POOL; k++) {
		if (l->headings[k]) {
			lv_obj_set_hidden(l->headings[k], true);
			l->heading_index[k] = -1;
		}
	}
}

static void bind_view(svclist_t *l, bool force) {
	int scroll = lv_obj_get_scroll_y(l->scroller);
	int view = lv_obj_get_height(l->scroller);
	if (scroll < 0) {
		scroll = 0;
	}
	// A third of the pool above the view, the rest from its top down: a
	// scroll either way finds rows already bound.
	int first = entry_at(l, scroll) - l->pool / 3;
	if (first > l->count - l->pool) {
		first = l->count - l->pool;
	}
	if (first < 0) {
		first = 0;
	}
	for (int k = 0; k < l->pool; k++) {
		int index = first + k;
		int slot = index % l->pool;
		if (index >= l->count) {
			// Not every slot is reached by an index past the end: hide by
			// slot below.
			continue;
		}
		if (force || l->row_index[slot] != index) {
			bind_row(l, slot, index);
		}
	}
	for (int s = 0; s < l->pool; s++) {
		int index = l->row_index[s];
		if (index < first || index >= first + l->pool || index >= l->count) {
			l->row_index[s] = -1;
			lv_obj_set_hidden(l->rows[s], true);
		}
	}
	int last_shown = first + l->pool - 1 < l->count - 1 ? first + l->pool - 1 : l->count - 1;
	place_headings(l, first, last_shown);

	if (l->cbs.visible && l->count > 0) {
		int a = entry_at(l, scroll);
		int b = entry_at(l, scroll + view);
		l->cbs.visible(a, b);
	}
	if (l->cbs.near_end && l->count > 0) {
		bool near = l->top + l->height - (scroll + view) < view;
		if (near && !l->near_end_said) {
			l->near_end_said = true;
			l->cbs.near_end();
		} else if (!near) {
			l->near_end_said = false;
		}
	}
}

static void scrolled_cb(lv_event_t *e) {
	svclist_t *l = lv_event_get_user_data(e);
	if (l) {
		bind_view(l, false);
	}
}

static void size_changed_cb(lv_event_t *e) {
	svclist_t *l = lv_event_get_user_data(e);
	if (l && l->count > 0) {
		bind_view(l, false);
	}
}

static void deleted_cb(lv_event_t *e) {
	svclist_t *l = lv_event_get_user_data(e);
	if (l) {
		free(l->rows);
		free(l->row_index);
		free(l->y);
		free(l->headed);
		free(l);
	}
}

svclist_t *svclist_create(lv_obj_t *parent, const sx_row_handlers_t *handlers, const svclist_cbs_t *cbs, int pool,
						  bool compact) {
	svclist_t *l = calloc(1, sizeof(*l));
	if (!l) {
		return NULL;
	}
	l->rows = calloc((size_t)pool, sizeof(*l->rows));
	l->row_index = malloc((size_t)pool * sizeof(*l->row_index));
	if (!l->rows || !l->row_index || !cbs || !cbs->bind) {
		free(l->rows);
		free(l->row_index);
		free(l);
		return NULL;
	}
	l->pool = pool;
	l->compact = compact;
	l->cbs = *cbs;
	l->pitch = sx_row_height(compact) + SX_ROW_GAP;

	lv_obj_t *s = lv_obj_create(parent);
	lv_obj_set_size(s, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_opa(s, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(s, 0, 0);
	lv_obj_set_style_radius(s, 0, 0);
	lv_obj_set_style_pad_all(s, 0, 0);
	lv_obj_set_scroll_dir(s, LV_DIR_VER);
	lv_obj_add_event_cb(s, scrolled_cb, LV_EVENT_SCROLL, l);
	lv_obj_add_event_cb(s, size_changed_cb, LV_EVENT_SIZE_CHANGED, l);
	lv_obj_add_event_cb(s, deleted_cb, LV_EVENT_DELETE, l);
	l->scroller = s;

	l->end = lv_obj_create(s);
	lv_obj_remove_style_all(l->end);
	lv_obj_set_size(l->end, 1, 1);
	lv_obj_set_clickable(l->end, false);

	for (int i = 0; i < pool; i++) {
		l->rows[i] = sx_row_create(s, handlers, compact);
		l->row_index[i] = -1;
		lv_obj_set_hidden(l->rows[i], true);
	}
	sx_frame_reserve_bottom(s);
	return l;
}

lv_obj_t *svclist_obj(const svclist_t *list) { return list ? list->scroller : NULL; }

void svclist_set_count(svclist_t *l, int count, bool keep_scroll) {
	if (!l) {
		return;
	}
	l->count = count > 0 ? count : 0;
	// Large rows may have changed since the rows were made.
	int pitch = sx_row_height(l->compact) + SX_ROW_GAP;
	if (pitch != l->pitch) {
		l->pitch = pitch;
		for (int i = 0; i < l->pool; i++) {
			sx_row_set_compact(l->rows[i], l->compact);
		}
	}
	layout(l);
	lv_obj_set_pos(l->end, 0, l->top + l->height > 0 ? l->top + l->height - 1 : 0);
	if (!keep_scroll) {
		lv_obj_scroll_to_y(l->scroller, 0, LV_ANIM_OFF);
		l->near_end_said = false;
	}
	for (int i = 0; i < l->pool; i++) {
		l->row_index[i] = -1;
	}
	for (int k = 0; k < HEADING_POOL; k++) {
		l->heading_index[k] = -1;
	}
	lv_obj_update_layout(l->scroller);
	bind_view(l, true);
}

int svclist_count(const svclist_t *list) { return list ? list->count : 0; }

void svclist_set_top(svclist_t *l, int top) {
	if (l) {
		l->top = top > 0 ? top : 0;
	}
}

void svclist_rebind(svclist_t *l) {
	if (l && l->count > 0) {
		bind_view(l, true);
	}
}

void svclist_rebind_index(svclist_t *l, int index) {
	if (!l || index < 0 || index >= l->count) {
		return;
	}
	int slot = index % l->pool;
	if (l->row_index[slot] == index) {
		bind_row(l, slot, index);
	}
}

void svclist_visible(const svclist_t *l, int *first, int *last) {
	*first = -1;
	*last = -2;
	if (!l || l->count <= 0) {
		return;
	}
	int scroll = lv_obj_get_scroll_y(l->scroller);
	*first = entry_at(l, scroll < 0 ? 0 : scroll);
	*last = entry_at(l, scroll + lv_obj_get_height(l->scroller));
}

int svclist_index_of(lv_obj_t *row) { return row ? (int)(intptr_t)sx_row_get_user(row) : -1; }

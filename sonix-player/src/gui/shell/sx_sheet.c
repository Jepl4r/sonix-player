#include "sx_sheet.h"

#include <stdlib.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/sx_palette.h"
#include "src/gui/shell/sx_styles.h"
#include "src/gui/shell/sx_tokens.h"
#include "src/gui/shell/theme.h"

// The panel runs SX_SHEET_R past the bottom of the screen: its corners are
// rounded all round, and the two lower ones are simply never on screen.
#define ICON_SLOT 36
#define TOP_MARGIN 40 // the least of the page left showing above a full sheet

static lv_obj_t *hit;	   // the screen under the status bar: a tap on it closes
static lv_obj_t *panel;	   // the sheet itself
static lv_obj_t *handle;
static lv_obj_t *head;	   // title and subtitle
static lv_obj_t *title_label;
static lv_obj_t *sub_label;
static lv_obj_t *list;	   // the items, scrolling past what fits
static lv_obj_t *buttons;  // the pair
static lv_obj_t *cancel_btn;
static lv_obj_t *cancel_label;
static lv_obj_t *action_btn;
static lv_obj_t *action_label;

static int screen_h;
static bool open_now;

// What is up: a copy of the caller's description and items.
static sx_sheet_t shown;
static sx_sheet_item_t *items;
static int item_capacity;

// The drag down.
static lv_point_t drag_start;
static bool drag_tracking;
static bool drag_engaged;
static int32_t drag_dy;

bool sx_sheet_is_open(void) { return open_now; }

// ---------------------------------------------------------------------------
// Callbacks, one tick after the sheet starts closing
// ---------------------------------------------------------------------------

typedef struct {
	void (*fn)(void *);
	void *user;
} pending_t;

static void pending_run_cb(void *data) {
	pending_t *p = data;
	void (*fn)(void *) = p->fn;
	void *user = p->user;
	free(p);
	if (fn) {
		fn(user);
	}
}

static void call_later(void (*fn)(void *), void *user) {
	if (!fn) {
		return;
	}
	pending_t *p = malloc(sizeof(*p));
	if (!p) {
		fn(user); // better now than never
		return;
	}
	p->fn = fn;
	p->user = user;
	lv_async_call(pending_run_cb, p);
}

// ---------------------------------------------------------------------------
// Sliding
// ---------------------------------------------------------------------------

static void slide_exec_cb(void *obj, int32_t v) { lv_obj_set_style_translate_y((lv_obj_t *)obj, v, 0); }

static void slid_away_cb(lv_anim_t *a) {
	(void)a;
	if (open_now) {
		return; // reopened while it was going
	}
	lv_obj_set_hidden(hit, true);
	lv_obj_clean(list); // the items' objects are not kept while nothing shows
}

static void slide_to(int32_t to, bool away) {
	lv_anim_delete(panel, slide_exec_cb);
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, panel);
	lv_anim_set_exec_cb(&a, slide_exec_cb);
	lv_anim_set_values(&a, lv_obj_get_style_translate_y(panel, LV_PART_MAIN), to);
	lv_anim_set_duration(&a, SX_ANIM_SHEET_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	if (away) {
		lv_anim_set_completed_cb(&a, slid_away_cb);
	}
	lv_anim_start(&a);
}

static int32_t panel_height(void) {
	lv_obj_update_layout(panel);
	return lv_obj_get_height(panel);
}

// Takes the sheet down. `fn` is what runs once it is going.
static void close_with(void (*fn)(void *), void *user) {
	if (!open_now) {
		return;
	}
	open_now = false;
	drag_tracking = false;
	slide_to(panel_height(), true);
	call_later(fn, user);
}

void sx_sheet_close(void) { close_with(NULL, NULL); }

static void dismiss(void) { close_with(shown.on_dismiss, shown.user); }

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

static void hit_clicked_cb(lv_event_t *e) {
	if (lv_event_get_target(e) != hit) {
		return; // a click on the sheet bubbling up
	}
	dismiss();
}

static void item_clicked_cb(lv_event_t *e) {
	if (drag_engaged) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= shown.item_count) {
		return;
	}
	close_with(items[index].action, items[index].user);
}

static void action_clicked_cb(lv_event_t *e) {
	(void)e;
	if (!drag_engaged) {
		close_with(shown.on_action, shown.user);
	}
}

static void cancel_clicked_cb(lv_event_t *e) {
	(void)e;
	if (!drag_engaged) {
		dismiss();
	}
}

static void drag_flag_clear_cb(void *unused) {
	(void)unused;
	drag_engaged = false;
}

// The pull down. It begins only once the finger has moved SX_DRAG_LOCK_PX
// down, more down than across, and nothing has started scrolling: a long list
// scrolls first, and only pulls the sheet once it is back at its top (its
// elastic end is off, so LVGL leaves that pull alone).
static void panel_drag_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_indev_t *indev = lv_indev_active();
	if (!indev || !open_now) {
		return;
	}
	lv_point_t p;
	lv_indev_get_point(indev, &p);

	if (code == LV_EVENT_PRESSED) {
		drag_start = p;
		drag_tracking = true;
		drag_engaged = false;
		drag_dy = 0;
		return;
	}
	if (!drag_tracking) {
		return;
	}
	int32_t dx = p.x - drag_start.x;
	int32_t dy = p.y - drag_start.y;

	if (code == LV_EVENT_PRESSING) {
		if (!drag_engaged) {
			if (lv_indev_get_scroll_obj(indev)) {
				drag_tracking = false;
				return;
			}
			if (dy < SX_DRAG_LOCK_PX || dy <= LV_ABS(dx)) {
				if (LV_ABS(dx) > SX_DRAG_LOCK_PX || dy < -SX_DRAG_LOCK_PX) {
					drag_tracking = false;
				}
				return;
			}
			drag_engaged = true;
		}
		drag_dy = dy > 0 ? dy : 0;
		lv_anim_delete(panel, slide_exec_cb);
		lv_obj_set_style_translate_y(panel, drag_dy, 0);
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		drag_tracking = false;
		if (!drag_engaged) {
			return;
		}
		if (drag_dy > SX_SHEET_CLOSE_PX) {
			dismiss();
		} else {
			slide_to(0, false);
		}
		// Cleared a tick later: the click this same release produces still has
		// to see it.
		lv_async_call(drag_flag_clear_cb, NULL);
	}
}

// ---------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------

static lv_obj_t *make_button(lv_obj_t *parent, lv_obj_t **label_out, lv_event_cb_t cb) {
	lv_obj_t *btn = lv_obj_create(parent);
	lv_obj_remove_style_all(btn);
	lv_obj_set_height(btn, SX_SHEET_BTN_H);
	lv_obj_set_flex_grow(btn, 1);
	lv_obj_set_style_radius(btn, SX_SHEET_BTN_H / 2, 0);
	lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
	lv_obj_set_scrollable(btn, false);
	lv_obj_set_event_bubble(btn, true);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *label = lv_label_create(btn);
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_style_max_width(label, lv_pct(90), 0);
	lv_obj_center(label);
	*label_out = label;
	return btn;
}

void sx_sheet_init(void) {
	if (hit) {
		return;
	}
	lv_display_t *d = lv_display_get_default();
	int32_t w = lv_display_get_horizontal_resolution(d);
	screen_h = (int)lv_display_get_vertical_resolution(d);

	// Under the status bar only: the bar stays usable, and the control centre
	// is reached from it as ever.
	hit = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(hit);
	lv_obj_set_size(hit, w, screen_h - SX_STATUSBAR_H);
	lv_obj_set_pos(hit, 0, SX_STATUSBAR_H);
	lv_obj_set_scrollable(hit, false);
	lv_obj_add_event_cb(hit, hit_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_set_hidden(hit, true);

	panel = sx_box(hit);
	lv_obj_set_width(panel, w);
	lv_obj_set_height(panel, LV_SIZE_CONTENT);
	lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, SX_SHEET_R);
	lv_obj_add_style(panel, &sx_style_surface, 0);
	lv_obj_set_style_radius(panel, SX_SHEET_R, 0);
	lv_obj_set_style_pad_top(panel, 10, 0);
	lv_obj_set_style_pad_bottom(panel, 12 + SX_SHEET_R, 0);
	lv_obj_set_style_max_height(panel, screen_h - SX_STATUSBAR_H - TOP_MARGIN + SX_SHEET_R, 0);
	lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	// The panel swallows its own taps (they are not a tap off the sheet) and
	// takes the drags its children bubble up.
	lv_obj_set_clickable(panel, true);
	lv_obj_set_event_bubble(panel, false);
	lv_obj_add_event_cb(panel, panel_drag_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(panel, panel_drag_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(panel, panel_drag_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(panel, panel_drag_cb, LV_EVENT_PRESS_LOST, NULL);

	// The hairline along the top, inset by the radius so it does not stick
	// out past the rounded corners.
	lv_obj_t *hairline = sx_box(panel);
	lv_obj_set_ignore_layout(hairline, true);
	lv_obj_set_size(hairline, w - 2 * SX_SHEET_R, 1);
	lv_obj_set_pos(hairline, SX_SHEET_R, -10);
	lv_obj_add_style(hairline, &sx_style_hairline, 0);

	handle = sx_box(panel);
	lv_obj_set_size(handle, SX_SHEET_HANDLE_W, SX_SHEET_HANDLE_H);
	lv_obj_set_style_radius(handle, 3, 0);
	lv_obj_set_style_bg_opa(handle, LV_OPA_50, 0);

	head = sx_box(panel);
	lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_hor(head, SX_SHEET_PAD_H, 0);
	lv_obj_set_style_pad_top(head, 10, 0);
	lv_obj_set_style_pad_bottom(head, 6, 0);
	lv_obj_set_flex_flow(head, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(head, 2, 0);

	title_label = lv_label_create(head);
	lv_obj_set_width(title_label, lv_pct(100));
	lv_label_set_long_mode(title_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_font(title_label, &font_ui_26_bold, 0);
	lv_obj_add_style(title_label, &sx_style_text, 0);

	sub_label = lv_label_create(head);
	lv_obj_set_width(sub_label, lv_pct(100));
	lv_label_set_long_mode(sub_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_font(sub_label, &font_ui_21, 0);
	lv_obj_add_style(sub_label, &sx_style_text2, 0);

	list = sx_box(panel);
	lv_obj_set_width(list, lv_pct(100));
	lv_obj_set_height(list, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(list, 0);
	lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scrollable(list, true);
	lv_obj_set_scroll_dir(list, LV_DIR_VER);
	lv_obj_set_scroll_elastic(list, false);
	lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ACTIVE);
	// Not clickable itself, so a press between items reaches the panel.
	lv_obj_set_clickable(list, false);

	buttons = sx_box(panel);
	lv_obj_set_size(buttons, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_hor(buttons, 24, 0);
	lv_obj_set_style_pad_top(buttons, 12, 0);
	lv_obj_set_style_pad_bottom(buttons, 8, 0);
	lv_obj_set_style_pad_column(buttons, 12, 0);
	lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);

	cancel_btn = make_button(buttons, &cancel_label, cancel_clicked_cb);
	lv_obj_add_style(cancel_btn, &sx_style_press, 0);
	lv_obj_add_style(cancel_label, &sx_style_text, 0);
	action_btn = make_button(buttons, &action_label, action_clicked_cb);
	lv_obj_set_style_text_font(action_label, &font_ui_24_bold, 0);
}

static lv_obj_t *add_item(int index) {
	const sx_sheet_item_t *it = &items[index];

	lv_obj_t *row = lv_obj_create(list);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, lv_pct(100), SX_SHEET_ITEM_H);
	lv_obj_set_style_pad_hor(row, SX_SHEET_PAD_H, 0);
	lv_obj_set_style_pad_column(row, 20, 0);
	lv_obj_add_style(row, &sx_style_press, LV_STATE_PRESSED);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_event_bubble(row, true);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_event_cb(row, item_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

	lv_color_t danger = lv_color_hex(SX_DANGER_TEXT);

	// The glyph column, there whether or not this item has a glyph so that the
	// labels line up. Without a glyph of its own it carries the tick.
	lv_obj_t *slot = sx_box(row);
	lv_obj_set_size(slot, ICON_SLOT, ICON_SLOT);
	const lv_image_dsc_t *glyph = it->icon ? it->icon : (it->checked ? &icon_check : NULL);
	if (glyph) {
		lv_obj_t *img = lv_image_create(slot);
		lv_image_set_src(img, glyph);
		lv_obj_center(img);
		if (it->destructive) {
			lv_obj_set_style_image_recolor(img, danger, 0);
			lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
		} else {
			lv_obj_add_style(img, &sx_style_icon_accent, 0);
		}
	}

	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text(label, it->label ? it->label : "");
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_flex_grow(label, 1);
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	if (it->destructive) {
		lv_obj_set_style_text_color(label, danger, 0);
	} else {
		lv_obj_add_style(label, &sx_style_text, 0);
	}

	// A glyph of its own and checked as well: the tick goes to the end.
	if (it->icon && it->checked) {
		lv_obj_t *tick = lv_image_create(row);
		lv_image_set_src(tick, &icon_check);
		lv_obj_add_style(tick, &sx_style_icon_accent, 0);
	}
	return row;
}

static void fill(void) {
	bool has_title = shown.title && shown.title[0];
	bool has_sub = shown.subtitle && shown.subtitle[0];
	lv_label_set_text(title_label, has_title ? shown.title : "");
	lv_label_set_text(sub_label, has_sub ? shown.subtitle : "");
	lv_obj_set_hidden(title_label, !has_title);
	lv_obj_set_hidden(sub_label, !has_sub);
	lv_obj_set_hidden(head, !has_title && !has_sub);

	lv_obj_clean(list);
	for (int i = 0; i < shown.item_count; i++) {
		add_item(i);
	}
	lv_obj_set_hidden(list, shown.item_count == 0);
	lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);

	bool has_action = shown.action_label && shown.action_label[0];
	bool has_cancel = shown.cancel_label && shown.cancel_label[0];
	lv_obj_set_hidden(buttons, !has_action && !has_cancel);
	lv_obj_set_hidden(cancel_btn, !has_cancel);
	lv_obj_set_hidden(action_btn, !has_action);
	if (has_cancel) {
		lv_label_set_text(cancel_label, shown.cancel_label);
	}
	if (has_action) {
		lv_label_set_text(action_label, shown.action_label);
		lv_obj_remove_style(action_btn, &sx_style_accent, 0);
		if (shown.action_destructive) {
			lv_obj_set_style_bg_color(action_btn, lv_color_hex(SX_DANGER_BG), 0);
			lv_obj_set_style_text_color(action_label, lv_color_white(), 0);
		} else {
			lv_obj_remove_local_style_prop(action_btn, LV_STYLE_BG_COLOR, 0);
			lv_obj_remove_local_style_prop(action_label, LV_STYLE_TEXT_COLOR, 0);
			lv_obj_add_style(action_btn, &sx_style_accent, 0);
		}
	}

	// The one colour set by hand, read again on every show.
	lv_obj_set_style_bg_color(handle, theme()->text_secondary, 0);

	// The list takes what the panel has left once the rest is laid out.
	lv_obj_update_layout(panel);
	int32_t rest = lv_obj_get_height(panel) - lv_obj_get_height(list);
	int32_t room = screen_h - SX_STATUSBAR_H - TOP_MARGIN + SX_SHEET_R - rest;
	lv_obj_set_style_max_height(list, room > SX_SHEET_ITEM_H ? room : SX_SHEET_ITEM_H, 0);
}

void sx_sheet_open(const sx_sheet_t *sheet) {
	if (!sheet) {
		return;
	}
	sx_sheet_init();

	// Another sheet up: it is dismissed, as if tapped away.
	if (open_now) {
		void (*fn)(void *) = shown.on_dismiss;
		void *user = shown.user;
		open_now = false;
		call_later(fn, user);
	}

	int count = sheet->items && sheet->item_count > 0 ? sheet->item_count : 0;
	if (count > item_capacity) {
		sx_sheet_item_t *grown = realloc(items, (size_t)count * sizeof(*grown));
		if (!grown) {
			return;
		}
		items = grown;
		item_capacity = count;
	}
	if (count) {
		memcpy(items, sheet->items, (size_t)count * sizeof(*items));
	}
	shown = *sheet;
	shown.items = items;
	shown.item_count = count;

	lv_anim_delete(panel, slide_exec_cb);
	fill();

	open_now = true;
	drag_tracking = false;
	drag_engaged = false;
	lv_obj_set_hidden(hit, false);
	lv_obj_move_foreground(hit);
	int32_t h = panel_height();
	lv_obj_set_style_translate_y(panel, h, 0);
	slide_to(0, false);
}

void sx_sheet_menu(const char *title, const char *subtitle, const sx_sheet_item_t *menu_items, int count) {
	sx_sheet_t sheet = {
		.title = title,
		.subtitle = subtitle,
		.items = menu_items,
		.item_count = count,
	};
	sx_sheet_open(&sheet);
}

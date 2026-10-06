#include "artistexceptions.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"
#include "src/system/library/library.h"

#define NAME_MAX_LEN 120

lv_obj_t *artistexceptions_screen;

static void (*changed_cb)(void);
static lv_obj_t *list;
static lv_obj_t *empty_label;
static lv_obj_t *name_layer;
static lv_obj_t *name_field;
static keyboard_t *name_keyboard;

static char **names;
static int name_count;

static void rebuild(void);

static void save(void) {
	library_set_artist_exceptions((const char *const *)names, name_count);
	if (changed_cb) {
		changed_cb();
	}
}

static void delete_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	if (i < 0 || i >= name_count) {
		return;
	}
	free(names[i]);
	memmove(&names[i], &names[i + 1], (size_t)(name_count - i - 1) * sizeof(names[0]));
	name_count--;
	save();
	rebuild();
}

static void rebuild(void) {
	lv_obj_clean(list);
	for (int i = 0; i < name_count; i++) {
		lv_obj_t *row = lv_obj_create(list);
		lv_obj_set_size(row, lv_pct(100), 84);
		lv_obj_add_style(row, &theme_style_card, 0);
		lv_obj_set_style_radius(row, 12, 0);
		lv_obj_set_style_border_width(row, 0, 0);
		lv_obj_set_style_shadow_width(row, 0, 0);
		lv_obj_set_style_pad_left(row, 20, 0);
		lv_obj_set_style_pad_right(row, 8, 0);
		lv_obj_set_style_pad_ver(row, 0, 0);
		lv_obj_set_scrollable(row, false);
		lv_obj_set_event_bubble(row, true);
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

		lv_obj_t *label = lv_label_create(row);
		lv_label_set_text(label, names[i]);
		lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
		lv_obj_set_flex_grow(label, 1);
		lv_obj_add_style(label, &theme_style_text, 0);
		lv_obj_set_style_text_font(label, &font_ui_24, 0);

		lv_obj_t *del = lv_btn_create(row);
		lv_obj_set_size(del, 64, 64);
		lv_obj_set_style_bg_opa(del, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(del, 0, 0);
		lv_obj_set_style_shadow_width(del, 0, 0);
		lv_obj_set_style_pad_all(del, 0, 0);
		lv_obj_add_event_cb(del, delete_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

		lv_obj_t *icon = lv_image_create(del);
		lv_image_set_src(icon, &icon_trash);
		lv_obj_add_style(icon, &theme_style_icon, 0);
		lv_obj_center(icon);
	}
	if (name_count == 0) {
		lv_obj_set_hidden(empty_label, false);
	} else {
		lv_obj_set_hidden(empty_label, true);
	}
}

static void load(void) {
	library_artist_exceptions_free(names, name_count);
	names = library_artist_exceptions(&name_count);
}

static void name_layer_hide(void) { lv_obj_set_hidden(name_layer, true); }

static void add_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	lv_textarea_set_text(name_field, "");
	keyboard_reset(name_keyboard);
	// Nothing focuses the field -- the keyboard types into it untouched -- so
	// the focus that starts the caret blinking is sent by hand.
	lv_obj_add_state(name_field, LV_STATE_FOCUSED);
	lv_obj_send_event(name_field, LV_EVENT_FOCUSED, NULL);
	lv_obj_set_hidden(name_layer, false);
	lv_obj_move_foreground(name_layer);
}

static void cancel_cb(lv_event_t *e) {
	(void)e;
	name_layer_hide();
}

static void accept_cb(lv_event_t *e) {
	(void)e;
	char name[NAME_MAX_LEN + 1];
	const char *typed = lv_textarea_get_text(name_field);
	snprintf(name, sizeof(name), "%s", typed ? typed : "");
	char *start = name;
	while (*start == ' ') {
		start++;
	}
	size_t len = strlen(start);
	while (len > 0 && start[len - 1] == ' ') {
		start[--len] = '\0';
	}
	name_layer_hide();
	if (len == 0) {
		return;
	}
	for (int i = 0; i < name_count; i++) {
		if (strcasecmp(names[i], start) == 0) {
			return;
		}
	}
	char **bigger = realloc(names, (size_t)(name_count + 1) * sizeof(*bigger));
	char *copy = strdup(start);
	if (!bigger || !copy) {
		free(copy);
		if (bigger) {
			names = bigger;
		}
		return;
	}
	names = bigger;
	names[name_count++] = copy;
	save();
	rebuild();
}

static void screen_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	if (code == LV_EVENT_SCREEN_LOAD_START) {
		name_layer_hide();
		load();
		rebuild();
	} else if (code == LV_EVENT_SCREEN_UNLOADED) {
		name_layer_hide();
	}
}

void artistexceptions_init(gui_config_t *cfg, void (*changed)(void)) {
	changed_cb = changed;
	artistexceptions_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(artistexceptions_screen, cfg, "musicsettings_unsplit_artists");
	settingsrow_title_corner_slots(settingsrow_page_title(artistexceptions_screen), cfg, 1);

	lv_obj_t *add = lv_btn_create(artistexceptions_screen);
	settingsrow_place_corner_button(add, cfg, 0);
	lv_obj_set_style_bg_opa(add, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(add, 0, 0);
	lv_obj_set_style_shadow_width(add, 0, 0);
	lv_obj_set_style_pad_all(add, 0, 0);
	lv_obj_add_event_cb(add, add_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *plus = lv_image_create(add);
	lv_image_set_src(plus, &icon_plus);
	lv_obj_add_style(plus, &theme_style_icon, 0);
	lv_obj_center(plus);

	lv_obj_t *note = lv_label_create(container);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_22, 0);
	lv_label_set_text(note, tr("artistexceptions_note"));

	list = lv_obj_create(container);
	lv_obj_remove_style_all(list);
	lv_obj_set_size(list, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_row(list, 8, 0);
	lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_event_bubble(list, true);

	empty_label = lv_label_create(container);
	lv_label_set_long_mode(empty_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(empty_label, lv_pct(100));
	lv_obj_set_style_text_align(empty_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(empty_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(empty_label, &font_ui_24, 0);
	lv_obj_set_style_pad_top(empty_label, 40, 0);
	lv_label_set_text(empty_label, tr("artistexceptions_empty"));

	// The naming dialog: the field at the top and the shared keyboard at the
	// bottom, the same layer the playlist page names a playlist on.
	name_layer = lv_obj_create(artistexceptions_screen);
	lv_obj_set_size(name_layer, cfg->screen_width, cfg->screen_height);
	lv_obj_align(name_layer, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_add_style(name_layer, &theme_style_screen, 0);
	lv_obj_set_style_bg_opa(name_layer, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(name_layer, 0, 0);
	lv_obj_set_style_radius(name_layer, 0, 0);
	lv_obj_set_style_pad_all(name_layer, 0, 0);
	lv_obj_set_scrollable(name_layer, false);
	lv_obj_set_hidden(name_layer, true);

	lv_obj_t *heading = lv_label_create(name_layer);
	lv_label_set_text(heading, tr("artistexceptions_add"));
	lv_obj_add_style(heading, &theme_style_text, 0);
	lv_obj_set_style_text_font(heading, &font_ui_24, 0);
	// Clear of the floating back chevron, which sits on top of this layer.
	lv_obj_align(heading, LV_ALIGN_TOP_LEFT, cfg->padding + 56 + 14, cfg->padding + cfg->top_bar_height + 10);

	lv_obj_t *cancel = lv_btn_create(name_layer);
	settingsrow_place_corner_button(cancel, cfg, 0);
	lv_obj_set_style_bg_opa(cancel, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(cancel, 0, 0);
	lv_obj_set_style_shadow_width(cancel, 0, 0);
	lv_obj_set_style_pad_all(cancel, 0, 0);
	lv_obj_add_event_cb(cancel, cancel_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *cancel_icon = lv_image_create(cancel);
	lv_image_set_src(cancel_icon, &icon_close);
	lv_obj_add_style(cancel_icon, &theme_style_icon, 0);
	lv_obj_center(cancel_icon);

	name_field = lv_textarea_create(name_layer);
	lv_textarea_set_one_line(name_field, true);
	lv_textarea_set_max_length(name_field, NAME_MAX_LEN);
	lv_textarea_set_placeholder_text(name_field, tr("name"));
	lv_obj_set_size(name_field, cfg->screen_width - 2 * cfg->padding, 62);
	lv_obj_set_scrollbar_mode(name_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(name_field, LV_ALIGN_TOP_LEFT, cfg->padding, cfg->padding + cfg->top_bar_height + 60);
	lv_obj_add_style(name_field, &theme_style_card, 0);
	lv_obj_set_style_radius(name_field, 12, 0);
	lv_obj_set_style_border_width(name_field, 0, 0);
	lv_obj_set_style_shadow_width(name_field, 0, 0);
	lv_obj_set_style_pad_all(name_field, 14, 0);
	lv_obj_set_style_text_font(name_field, &font_ui_24, 0);
	keyboard_style_caret(name_field);

	name_keyboard = keyboard_create(name_layer, cfg->screen_width, cfg->screen_width < 320 ? 144 : 316, name_field, NULL, "ok", accept_cb, NULL);

	lv_obj_add_event_cb(artistexceptions_screen, screen_cb, LV_EVENT_SCREEN_LOAD_START, NULL);
	lv_obj_add_event_cb(artistexceptions_screen, screen_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(artistexceptions_screen);
}

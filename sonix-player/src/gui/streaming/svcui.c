#include "svcui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/queueundo.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/spinner.h"
#include "src/gui/shell/sx_card.h"
#include "src/gui/shell/sx_frame.h"
#include "src/gui/shell/sx_palette.h"
#include "src/gui/shell/sx_styles.h"
#include "src/gui/shell/sx_toast.h"
#include "src/gui/shell/sx_tokens.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"
#include "src/system/playback/device_state.h"

// See svcui.h.

lv_obj_t *svcui_header_button(lv_obj_t *screen, gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph,
							  lv_event_cb_t cb, void *user) {
	lv_obj_t *button = lv_btn_create(screen);
	lv_obj_set_size(button, 56, 56);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	// The header's own target size, without moving the glyph: 72 x 72 by
	// the handoff, of which 56 are drawn.
	lv_obj_set_ext_click_area(button, (SX_HEADER_BTN - 56) / 2);
	lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -cfg->padding - slot * (56 + 6), cfg->padding + cfg->top_bar_height);
	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, glyph);
	lv_obj_add_style(icon, &sx_style_icon, 0);
	lv_obj_center(icon);
	if (cb) {
		lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, user);
	}
	return button;
}

lv_obj_t *svcui_group(lv_obj_t *parent, const char *text) {
	lv_obj_t *label = sx_label(parent, &font_ui_17, &sx_style_text2);
	lv_obj_set_style_text_letter_space(label, 1, 0);
	lv_obj_set_size(label, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_top(label, 14, 0);
	lv_obj_set_style_pad_bottom(label, 6, 0);
	lv_obj_set_style_pad_left(label, 6, 0);
	lv_label_set_text(label, text ? text : "");
	return label;
}

lv_obj_t *svcui_nav_card(lv_obj_t *parent, const char *title, const char *subtitle, lv_event_cb_t cb, void *user) {
	lv_obj_t *card = sx_card_create(parent);
	sx_card_title(card, title ? title : "");
	if (subtitle && subtitle[0]) {
		sx_card_subtitle(card, subtitle, false);
	}
	sx_card_chevron(card);
	if (cb) {
		sx_card_on_click(card, cb, user);
	}
	return card;
}

lv_obj_t *svcui_column(lv_obj_t *screen, gui_config_t *cfg) {
	int top = settingsrow_content_top(cfg);
	lv_obj_t *c = sx_box(screen);
	lv_obj_set_size(c, cfg->screen_width - 2 * SX_BODY_PAD_L, cfg->screen_height - top);
	lv_obj_set_pos(c, SX_BODY_PAD_L, top);
	lv_obj_set_style_pad_row(c, SX_CARD_GAP, 0);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scroll_dir(c, LV_DIR_VER);
	lv_obj_set_scrollable(c, true);
	lv_obj_set_clickable(c, true); // a drag from the gaps between the cards scrolls too
	lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_OFF);
	sx_frame_reserve_bottom(c);
	return c;
}

lv_obj_t *svcui_note(lv_obj_t *parent, const char *text) {
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_add_style(label, &sx_style_text2, 0);
	lv_obj_set_style_text_font(label, &font_ui_20, 0);
	lv_obj_set_style_pad_hor(label, 6, 0);
	lv_obj_set_style_pad_top(label, 4, 0);
	lv_obj_set_style_pad_bottom(label, 12, 0);
	lv_obj_set_style_text_line_space(label, 4, 0);
	lv_obj_set_clickable(label, false);
	lv_label_set_text(label, text ? text : "");
	return label;
}

lv_color_t svcui_tone(const char *name) {
	sx_lc_t lc = SX_T_COVER_TONE;
	// The token's own chroma, not the playing cover's scale: a row's plaque
	// is about the row, and a grey album playing must not grey the list.
	return sx_oklch(lc.L, lc.C, sx_palette_hue_of_name(name && name[0] ? name : "?"));
}

// ---------------------------------------------------------------------------
// Queueing, with "Undo"
//
// The two lines the Library's rows offer, said the 2.0 way: what went where
// in the toast, and Undo -- the queue's one undo (queueundo.h), the same the
// library lists and the player use, so an Undo here puts the queue back
// exactly as it was, whatever moved meanwhile.
// ---------------------------------------------------------------------------

int svcui_queue(const char *const *paths, int count, playlist_queue_where_t where, const char *what) {
	int queued = queue_undo_add_paths(paths, count, where);
	if (queued <= 0) {
		return 0; // the reason has been said
	}
	char line[320];
	lv_snprintf(line, sizeof(line), tr(where == PLAYLIST_QUEUE_NEXT ? "svc_queued_next" : "svc_queued_end"),
				what && what[0] ? what : "");
	// Without "Undo" when nothing was queued before: the set became the queue,
	// and taking it out again would be emptying it.
	queue_undo_commit(line);
	return queued;
}

void svcui_star_toast(bool on, const char *on_key, const char *off_key, const char *what, void (*undo)(void *user),
					  void *user) {
	char line[320];
	lv_snprintf(line, sizeof(line), tr(on ? on_key : off_key), what && what[0] ? what : "");
	if (undo) {
		sx_toast_undo(line, undo, user);
	} else {
		sx_toast(line);
	}
}

lv_obj_t *svcui_spinner(lv_obj_t *screen) {
	lv_obj_t *spin = spinner_create(screen, &icon_loader_big);
	lv_obj_add_style(spin, &sx_style_icon2, 0);
	lv_obj_center(spin);
	lv_obj_set_hidden(spin, true);
	return spin;
}

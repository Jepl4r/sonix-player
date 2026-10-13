#include "svcui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/spinner.h"
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
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_set_style_pad_top(label, 14, 0);
	lv_obj_set_style_pad_bottom(label, 6, 0);
	lv_obj_set_style_pad_left(label, 6, 0);
	lv_label_set_text(label, text ? text : "");
	return label;
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
// in the toast, and Undo. playlist_queue_paths() puts the set either after
// the playing track (NEXT, after the previous "next" while that chain holds)
// or at the end of the playback order (END), contiguous either way, so the
// set is found again by its paths right where it was put: Undo takes those
// entries out (playlist_remove) -- and only if they are still there in that
// order, since the queue moves on underneath a toast.
// ---------------------------------------------------------------------------

typedef struct {
	int first;
	int count;
	char **paths;
} queued_set_t;

static queued_set_t last_set;

static void set_forget(queued_set_t *s) {
	for (int i = 0; i < s->count; i++) {
		free(s->paths[i]);
	}
	free(s->paths);
	memset(s, 0, sizeof(*s));
}

static bool set_matches_at(const queued_set_t *s, int at) {
	char path[1024];
	for (int i = 0; i < s->count; i++) {
		if (!playlist_path_at(at + i, path, sizeof(path)) || strcmp(path, s->paths[i]) != 0) {
			return false;
		}
	}
	return true;
}

static void undo_queue(void *user) {
	(void)user;
	queued_set_t *s = &last_set;
	if (s->count <= 0) {
		return;
	}
	int *indices = malloc((size_t)s->count * sizeof(*indices));
	if (indices && set_matches_at(s, s->first)) {
		for (int i = 0; i < s->count; i++) {
			indices[i] = s->first + i;
		}
		if (playlist_remove(indices, s->count)) {
			device_state_queue_changed();
		}
	}
	free(indices);
	set_forget(s);
}

int svcui_queue(const char *const *paths, int count, playlist_queue_where_t where, const char *what) {
	set_forget(&last_set);
	int before = playlist_count();
	int current = playlist_current_index();
	int queued = playlist_queue_paths(paths, count, where);
	if (queued <= 0) {
		gui_notify_popup("playlist_cannot_add_the_tracks");
		return 0;
	}
	// Written down now, once for the whole set (as trackmenu_queue_paths).
	device_state_queue_changed();

	char line[320];
	lv_snprintf(line, sizeof(line), tr(where == PLAYLIST_QUEUE_NEXT ? "svc_queued_next" : "svc_queued_end"),
				what && what[0] ? what : "");

	// Nothing was queued before: the set became the queue, and taking it out
	// again would be emptying it -- not an undo anyone wants from a toast.
	if (before <= 0) {
		sx_toast(line);
		return queued;
	}

	queued_set_t *s = &last_set;
	s->paths = calloc((size_t)queued, sizeof(*s->paths));
	if (!s->paths) {
		sx_toast(line);
		return queued;
	}
	for (int i = 0, k = 0; i < count && k < queued; i++) {
		if (paths[i] && paths[i][0]) {
			s->paths[k] = strdup(paths[i]);
			if (!s->paths[k]) {
				break;
			}
			s->count = ++k;
		}
	}
	if (s->count != queued) {
		set_forget(s);
		sx_toast(line);
		return queued;
	}
	// Where the set landed: the end of the order, or the first place after
	// the playing track where the whole run reads back.
	int total = playlist_count();
	s->first = -1;
	if (where == PLAYLIST_QUEUE_END) {
		if (set_matches_at(s, total - queued)) {
			s->first = total - queued;
		}
	} else {
		for (int at = current + 1; at >= 0 && at + queued <= total; at++) {
			if (set_matches_at(s, at)) {
				s->first = at;
				break;
			}
		}
	}
	if (s->first < 0) {
		set_forget(s);
		sx_toast(line);
		return queued;
	}
	sx_toast_undo(line, undo_queue, NULL);
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

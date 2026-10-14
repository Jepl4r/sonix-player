#include "onscreen.h"

// See onscreen.h. One small record per bound object, held by the events: the
// screen's two carry it to show/hide, the object's delete event frees it.

typedef struct {
	lv_obj_t *obj;
	lv_obj_t *screen;
	void (*show)(lv_obj_t *obj);
	void (*hide)(lv_obj_t *obj);
} binding_t;

static void screen_event_cb(lv_event_t *e) {
	binding_t *b = lv_event_get_user_data(e);
	if (lv_event_get_code(e) == LV_EVENT_SCREEN_LOAD_START) {
		b->show(b->obj);
	} else {
		b->hide(b->obj);
	}
}

static void obj_delete_cb(lv_event_t *e) {
	binding_t *b = lv_event_get_user_data(e);
	// A screen being deleted has already dropped its events (LVGL clears them
	// before it deletes the children), so this finds nothing there then.
	lv_obj_remove_event_cb_with_user_data(b->screen, screen_event_cb, b);
	lv_free(b);
}

static bool shown_now(lv_obj_t *screen) {
	lv_display_t *disp = lv_obj_get_display(screen);
	return screen == lv_display_get_screen_active(disp) || screen == lv_display_get_layer_top(disp) ||
		   screen == lv_display_get_layer_sys(disp) || screen == lv_display_get_layer_bottom(disp);
}

void onscreen_bind(lv_obj_t *obj, void (*show)(lv_obj_t *obj), void (*hide)(lv_obj_t *obj)) {
	if (!obj || !show || !hide) {
		return;
	}
	lv_obj_t *screen = lv_obj_get_screen(obj);
	if (!screen) {
		return;
	}
	// Bound already (a restyle can come through here again): once is enough.
	for (uint32_t i = 0; i < lv_obj_get_event_count(obj); i++) {
		if (lv_event_dsc_get_cb(lv_obj_get_event_dsc(obj, i)) == obj_delete_cb) {
			return;
		}
	}
	if (shown_now(screen)) {
		show(obj);
	} else {
		hide(obj);
	}
	// A layer is never loaded or unloaded: nothing to follow.
	lv_display_t *disp = lv_obj_get_display(screen);
	if (screen == lv_display_get_layer_top(disp) || screen == lv_display_get_layer_sys(disp) ||
		screen == lv_display_get_layer_bottom(disp)) {
		return;
	}
	binding_t *b = lv_malloc(sizeof(*b));
	if (!b) {
		return; // stays as decided above; only the parking is lost
	}
	b->obj = obj;
	b->screen = screen;
	b->show = show;
	b->hide = hide;
	lv_obj_add_event_cb(screen, screen_event_cb, LV_EVENT_SCREEN_LOAD_START, b);
	lv_obj_add_event_cb(screen, screen_event_cb, LV_EVENT_SCREEN_UNLOADED, b);
	lv_obj_add_event_cb(obj, obj_delete_cb, LV_EVENT_DELETE, b);
}

#include "toast.h"

#include "src/gui/shell/icons.h"
#include "src/gui/shell/sx_toast.h"
#include "src/system/core/lang.h"

// Since 2.0 every one of these is the toast line at the bottom of the screen
// (sx_toast.h) instead of a card in the middle over a dimmed page: the same
// words, with the glyph at the start of the line, and the page untouched
// around it. The text is a translation tag, as before.

// The Adwaita "success" green and the red beside it, on the toast's light
// ground.
#define TOAST_GREEN lv_color_make(0x2e, 0xc2, 0x7e)
#define TOAST_RED lv_color_make(0xe0, 0x1b, 0x24)

void toast_init(gui_config_t *cfg) {
	(void)cfg;
	sx_toast_init();
}

void toast_success(const char *text) { sx_toast_glyph(&icon_circle_check_small, TOAST_GREEN, text ? tr(text) : ""); }

void toast_error(const char *text) { sx_toast_glyph(&icon_circle_alert_small, TOAST_RED, text ? tr(text) : ""); }

void toast_plain(const char *text) { sx_toast(text ? tr(text) : ""); }

void toast_glyph(const lv_image_dsc_t *glyph, const char *text) {
	sx_toast_glyph(glyph, TOAST_GREEN, text ? tr(text) : "");
}

void toast_busy(const char *text) { sx_toast_busy(text ? tr(text) : "", false); }

void toast_busy_end(void) { sx_toast_busy_end(); }

void toast_busy_dismissable(const char *text) { sx_toast_busy(text ? tr(text) : "", true); }

bool toast_busy_showing(void) { return sx_toast_busy_showing(); }

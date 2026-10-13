#include "confirm.h"

#include <string.h>

#include "src/gui/shell/sx_sheet.h"
#include "src/system/core/lang.h"

// Since 2.0 the question is the bottom sheet (sx_sheet.h): the title and the
// message over a "Cancel / OK" pair, coming up from the bottom of the screen
// over the undimmed page. The signatures are the ones every caller already
// uses; nothing about them changed.
//
// What did change: a tap off the sheet, or pulling it down, is a "no" now --
// the sheet's one way of being put away, the same on every sheet -- where the
// old card swallowed such taps. A "no" does nothing, so nothing is lost by it.
//
// The action still runs after the sheet is on its way out, one tick later, so
// a callback that loads another screen never does it from under the question.

// The actions whose button goes red (handoff: destructive is red). Asked of
// the button's own tag, which says what will happen; the list is every
// irreversible one the interface puts this way.
static const char *const DESTRUCTIVE_TAGS[] = {
	"delete", "remove", "reset", "leave", "trackmenu_queue_clear", "playlist_overwrite", "downloads_clear",
};

static bool is_destructive(const char *tag) {
	if (!tag) {
		return false;
	}
	for (size_t i = 0; i < sizeof(DESTRUCTIVE_TAGS) / sizeof(DESTRUCTIVE_TAGS[0]); i++) {
		if (strcmp(tag, DESTRUCTIVE_TAGS[i]) == 0) {
			return true;
		}
	}
	return false;
}

void confirm_init(gui_config_t *cfg) {
	(void)cfg;
	sx_sheet_init();
}

void confirm_close(void) { sx_sheet_close(); }

void confirm_show(const char *title, const char *message, const char *ok_text, void (*on_ok)(void *user),
				  void *user) {
	const char *ok_tag = ok_text && ok_text[0] ? ok_text : "ok";
	sx_sheet_t sheet = {
		.title = title ? tr(title) : NULL,
		.subtitle = message && message[0] ? tr(message) : NULL,
		.cancel_label = tr("cancel"),
		.action_label = tr(ok_tag),
		.action_destructive = is_destructive(ok_tag),
		.on_action = on_ok,
		.user = user,
	};
	sx_sheet_open(&sheet);
}

void confirm_notice(const char *title, const char *message) {
	// A notice has nothing to cancel: one way out, across the whole width.
	sx_sheet_t sheet = {
		.title = title ? tr(title) : NULL,
		.subtitle = message && message[0] ? tr(message) : NULL,
		.action_label = tr("ok"),
	};
	sx_sheet_open(&sheet);
}

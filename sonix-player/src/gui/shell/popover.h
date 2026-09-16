#ifndef POPOVER_H
#define POPOVER_H

#include <stdbool.h>

#include "lvgl/lvgl.h"

// A small modal menu that opens next to the control that asked for it --
// iOS-style: the page stays visible, a dim veil catches taps, and a compact
// card with a few actions appears by the anchor. One popover exists at a
// time; showing a new one replaces the old.

typedef struct {
	const char *label;
	void (*action)(void *user); // called after the popover has closed
	void *user;
	// A tick at the right of the row, for when the menu is not a list of
	// actions but a choice between alternatives (Qobuz audio quality): there
	// the current entry has to be visible.
	bool checked;
} popover_item_t;

// Builds the (hidden) veil and card once. Call from gui_init.
void popover_init(void);

// Opens the menu near `anchor` with `count` items (at most 6 are shown).
void popover_show(lv_obj_t *anchor, const popover_item_t *items, int count);

// Closes it (the veil tap and every item do this on their own).
void popover_close(void);

#endif /* POPOVER_H */

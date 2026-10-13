#ifndef POPOVER_H
#define POPOVER_H

#include <stdbool.h>

#include "lvgl/lvgl.h"

// A menu of a few actions for the control that asked for it. Since 2.0 it is
// the bottom sheet (sx_sheet.h): it comes up from the bottom of the screen
// over the undimmed page, a tap off it or a pull down closes it. One exists at
// a time; showing a new one replaces the old. New code uses sx_sheet directly,
// which has titles, glyphs and destructive entries.

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

// Opens the menu with `count` items; it scrolls when they do not fit. `anchor`
// is no longer used (the sheet always comes from the bottom) and may be NULL.
void popover_show(lv_obj_t *anchor, const popover_item_t *items, int count);

// Closes it (the veil tap and every item do this on their own).
void popover_close(void);

#endif /* POPOVER_H */

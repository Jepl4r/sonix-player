#ifndef GUI_SX_SHEET_H
#define GUI_SX_SHEET_H

#include <stdbool.h>

#include "lvgl/lvgl.h"

// ---------------------------------------------------------------------------
// Sonix UI 2.0: the bottom sheet
//
// Principle 5, "one menu for everything": a row's long press, a header's ⋮,
// a sort, a sleep timer, an "are you sure?" -- all of them come up as this
// sheet from the bottom of the screen. One instance on lv_layer_top(), built
// once and refilled on every show; showing another replaces the one up.
//
// The page stays visible and undimmed around it (no full-screen veil: the
// platform cannot blend one); the sheet stands out as a lighter surface with a
// 1 px hairline along its rounded top. A tap anywhere off it closes it, and so
// does pulling it down past SX_SHEET_CLOSE_PX -- it follows the finger, and
// springs back when let go short of that. It slides in and out in
// SX_ANIM_SHEET_MS.
//
// Layout, top to bottom: a 44 x 5 handle; the title (26 bold) and an optional
// subtitle (21, secondary, wrapping); the items, SX_SHEET_ITEM_H each -- an
// optional glyph in the accent colour, the label, a destructive one in red, a
// checked one marked with a tick -- which scroll when there are more than the
// screen holds (no fixed limit); and optionally a pair of buttons, "Cancel"
// and the action (accent, or red when destructive), or the action alone.
//
// Every string is shown as given: translate before handing it over.
//
// Callbacks run after the sheet has started closing, one tick later (as the
// popover and the confirmation did), so an action can open another page or
// another sheet. UI thread only.
//
// Cost: about 12 objects when idle (built once), plus 3 per item while shown.
// Nothing runs while it is closed.
// ---------------------------------------------------------------------------

typedef struct {
	const char *label;
	const lv_image_dsc_t *icon; // NULL: no glyph (the column stays, so labels line up)
	bool checked;				// a choice among alternatives: the current one
	bool destructive;			// red
	void (*action)(void *user); // NULL: the item only closes the sheet
	void *user;
} sx_sheet_item_t;

typedef struct {
	const char *title;	  // NULL or "": no title
	const char *subtitle; // NULL or "": none
	const sx_sheet_item_t *items;
	int item_count;

	// The buttons. With both labels: "Cancel" on the left, the action on the
	// right. With only `action_label`: the action across the whole width. With
	// neither: no buttons.
	const char *cancel_label;
	const char *action_label;
	bool action_destructive; // the action in red (irreversible: clearing a cache)
	void (*on_action)(void *user);

	// Closed without an item or the action: a tap off it, pulled down,
	// "Cancel", or replaced by another sheet. May be NULL.
	void (*on_dismiss)(void *user);
	void *user; // for on_action and on_dismiss
} sx_sheet_t;

// Builds it, hidden. gui_init(), once; safe to call again.
void sx_sheet_init(void);

// Shows `sheet` (copied: the caller's struct and item array may go; the
// strings must outlive the sheet -- translation-table strings and literals do).
void sx_sheet_open(const sx_sheet_t *sheet);

// A sheet with items only: the short form of the above.
void sx_sheet_menu(const char *title, const char *subtitle, const sx_sheet_item_t *items, int count);

// Closes it (animated) without calling anything.
void sx_sheet_close(void);

bool sx_sheet_is_open(void);

#endif /* GUI_SX_SHEET_H */

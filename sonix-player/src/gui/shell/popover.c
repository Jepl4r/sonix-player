#include "popover.h"

#include <stdlib.h>
#include <string.h>

#include "src/gui/shell/sx_sheet.h"
#include "src/system/core/lang.h"

// Since 2.0 the popover is the bottom sheet (sx_sheet.h, principle 5: one
// menu for everything): every popover_show() in the interface comes up from
// the bottom of the screen, its entries 64 px tall, with no veil over the
// page. The anchor no longer places anything; it is kept in the signature so
// that no caller has to change. Nor is there a limit of seven entries any
// more: the sheet scrolls.
//
// The entries' labels are translation tags, as they always were; the actions
// run after the sheet has started closing, as they always did.

static popover_item_t *items_copy;
static sx_sheet_item_t *sheet_items;
static int capacity;

void popover_close(void) { sx_sheet_close(); }

// The entry's own action, with its own user data: the sheet calls this with a
// pointer to the copied entry.
static void run_item(void *data) {
	const popover_item_t *item = data;
	if (item && item->action) {
		item->action(item->user);
	}
}

void popover_init(void) { sx_sheet_init(); }

void popover_show(lv_obj_t *anchor, const popover_item_t *items, int count) {
	(void)anchor;
	if (!items || count <= 0) {
		return;
	}
	if (count > capacity) {
		popover_item_t *grown_items = realloc(items_copy, (size_t)count * sizeof(*grown_items));
		if (!grown_items) {
			return;
		}
		items_copy = grown_items;
		sx_sheet_item_t *grown_sheet = realloc(sheet_items, (size_t)count * sizeof(*grown_sheet));
		if (!grown_sheet) {
			return;
		}
		sheet_items = grown_sheet;
		capacity = count;
	}

	// Copied: the sheet hands the entry back after the caller's array is gone.
	// A sheet still closing holds pointers into the old copy only until its
	// callback has run, which is before anything can call this again.
	memcpy(items_copy, items, (size_t)count * sizeof(*items));
	for (int i = 0; i < count; i++) {
		sheet_items[i] = (sx_sheet_item_t){
			.label = tr(items[i].label),
			.checked = items[i].checked,
			.action = run_item,
			.user = &items_copy[i],
		};
	}
	sx_sheet_menu(NULL, NULL, sheet_items, count);
}

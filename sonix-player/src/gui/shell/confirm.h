#ifndef CONFIRM_H
#define CONFIRM_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The yes/no question every "are you sure?" in the interface uses. Since 2.0
// it is the bottom sheet (sx_sheet.h): title, message, and "Cancel" beside the
// action -- red when the action cannot be undone (deleting, removing,
// resetting, clearing). One exists at a time; asking again replaces it. A tap
// off the sheet or a pull down is a "no".
//
// The action runs after the sheet has started closing, so it can switch
// screens or start a long job without pulling the question out from under
// itself.

void confirm_init(gui_config_t *cfg);

// `message` may be NULL for a title-only question. `ok_label` is the accent
// button's text ("Scan", "Delete", ...); NULL uses "OK".
void confirm_show(const char *title, const char *message, const char *ok_label, void (*on_ok)(void *user),
				  void *user);

// The same card with one button: something the user has to be told, not asked.
// Dismissing it does nothing beyond closing it.
void confirm_notice(const char *title, const char *message);

void confirm_close(void);

#endif /* CONFIRM_H */

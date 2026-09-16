#ifndef TRACKMENU_H
#define TRACKMENU_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The player's overflow menu (the ellipsis opposite the repeat button) and
// the two pages it opens: the queue (what is lined up after the current track)
// and the details page (everything known about the playing file).
extern lv_obj_t *queue_screen;
extern lv_obj_t *details_screen;

void trackmenu_init(gui_config_t *cfg);

// Opens the popover next to `anchor` with the queue and details entries.
void trackmenu_open(lv_obj_t *anchor);

// Opens the queue page directly, without the menu in front. For the player with
// a podcast playing: there the button is the episodes button, and the episodes
// are the queue.
void trackmenu_open_queue(void);

// Opens the details page for a specific file (from a library list's menu).
void trackmenu_details_open(const char *path);

// The playing track changed: move the accent mark in the queue to the row that
// carries it now. The rows themselves do not change -- the queue is the same
// list -- so nothing here rebinds or re-reads anything.
void trackmenu_notify_now_playing(void);

#endif /* TRACKMENU_H */

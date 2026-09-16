#ifndef FILESPAGE_H
#define FILESPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// File manager
//
// What the Wi-Fi transfer page does from a browser, done on the device: walk
// the card, see what is on it and how big, and rename, delete or make a folder
// without needing another machine on the same network.
//
// It is not the music browser. That one lists what can be played and hides the
// rest, because a list of tracks with a stylesheet and a thumbnail cache in it
// is a worse list of tracks. Here everything is shown, because a file manager
// that hides files is not one.

extern lv_obj_t *filespage_screen;

void filespage_init(gui_config_t *cfg);

// Opens it at the root of the card. An action rather than a plain screen swap:
// the listing is read when the page is entered, so a card that changed under
// the player shows up without a restart.
void filespage_open(void);

#endif /* FILESPAGE_H */

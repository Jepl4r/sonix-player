#ifndef LIBRARYSCAN_H
#define LIBRARYSCAN_H

#include "src/gui/shell/gui.h"
#include "lvgl/lvgl.h"

// The page a music scan runs on: one card with the tracks found so far, the
// folder being read, a bar and Stop; when the scan ends it gives way to the
// Music page with a toast. And the page of the card's top-level folders the
// scan reads (Settings > Music > Scan options > Folders to scan).
extern lv_obj_t *libraryscan_screen;
extern lv_obj_t *libraryscan_folders_screen;

void libraryscan_init(gui_config_t *cfg);

// Opening the page starts a scan. Called by the switcher when the page is
// loaded, so nothing else has to remember to kick it off.
void libraryscan_begin(void);

// Starts a scan of the folders chosen on the folders page (the whole card
// until somebody chooses), at once: 2.0 asks nothing before a scan. Kept
// under its old name for its callers (the Music page's empty state and
// rescan card); the folders are chosen on their own page now.
void libraryscan_choose_folders(void);

// The folders page.
void libraryscan_open_folders(void);

#endif /* LIBRARYSCAN_H */

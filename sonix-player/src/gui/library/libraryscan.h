#ifndef LIBRARYSCAN_H
#define LIBRARYSCAN_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The page that indexes the card: a big note, a count that climbs while the
// scan runs, and an OK button once it is done.
extern lv_obj_t *libraryscan_screen;

void libraryscan_init(gui_config_t *cfg);

// Opening attaches to a running scan/update/Smart Mix job; otherwise starts a
// scan. Leaving the page keeps the job running until explicitly cancelled.
void libraryscan_begin(void);

// Asks which of the card's top-level folders to scan, saves the choice
// (library_scan_folders_set) and opens the page, which starts the scan.
void libraryscan_choose_folders(void);

#endif /* LIBRARYSCAN_H */

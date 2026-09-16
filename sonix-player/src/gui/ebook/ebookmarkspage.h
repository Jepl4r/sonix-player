#ifndef EBOOKMARKSPAGE_H
#define EBOOKMARKSPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

extern lv_obj_t *ebookmarkspage_screen;
extern lv_obj_t *ebookmarkbookpage_screen;

void ebookmarkspage_init(gui_config_t *cfg);

// Reads the Ebook folder again and shows the books that have bookmarks. What
// the corner button on the shelf calls.
void ebookmarkspage_open(void);

#endif /* EBOOKMARKSPAGE_H */

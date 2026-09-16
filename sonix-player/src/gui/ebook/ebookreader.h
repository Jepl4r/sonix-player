#ifndef EBOOKREADER_H
#define EBOOKREADER_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

extern lv_obj_t *ebookreader_screen;

void ebookreader_init(gui_config_t *cfg);

// Opens `path` and shows it at wherever it was left off. The book stays open
// for as long as this screen is up and is closed -- arenas, ZIP and fonts --
// the moment it is left.
void ebookreader_open(const char *path);

// The same, but landing at a place somebody named instead of where the reading
// was left: what the bookmarks page opens a book with.
void ebookreader_open_at(const char *path, uint32_t spine, uint32_t block, uint32_t offset);

#endif /* EBOOKREADER_H */

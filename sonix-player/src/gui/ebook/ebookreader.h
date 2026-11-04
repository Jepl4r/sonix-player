#ifndef EBOOKREADER_H
#define EBOOKREADER_H

#include <stdbool.h>

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

// A side key, before it reaches the player. True when the reader took it:
// while a book is being read one word or one line at a time, play stops and
// starts the ticker and the skip keys move by a sentence. On the pages, and
// whenever the reader is not the screen, the keys stay the music's.
bool ebookreader_key(gui_key_t key);

#endif /* EBOOKREADER_H */

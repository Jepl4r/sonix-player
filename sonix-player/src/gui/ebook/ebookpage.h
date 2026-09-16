#ifndef EBOOKPAGE_H
#define EBOOKPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

extern lv_obj_t *ebookpage_screen;

void ebookpage_init(gui_config_t *cfg);

// Reads the Ebook folder again and shows the shelf. What the tile on the More
// page calls, so a book copied onto the card while the player was running
// turns up without a restart.
void ebookpage_open(void);

#endif /* EBOOKPAGE_H */

#ifndef KBLAYOUTPAGE_H
#define KBLAYOUTPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Settings > More > Keyboard language: which alphabets the on-screen keyboard
// can be laid out in, dragged by their handles between "In use" and "Others".
// The first of the ones in use is the layout every keyboard opens in, and the
// rest are what a long press on the "123" key offers. What the lists mean and
// where they are kept is kblayout.h; this page is only the way to edit them.

extern lv_obj_t *kblayoutpage_screen;

void kblayoutpage_init(gui_config_t *cfg);

#endif /* KBLAYOUTPAGE_H */

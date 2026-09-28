#ifndef PEQAUTOEQ_H
#define PEQAUTOEQ_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

void peqautoeq_init(gui_config_t *cfg);
lv_obj_t *peqautoeq_screen(void);
void peqautoeq_open(void);
void peqautoeq_set_reload_cb(void (*cb)(void));

#endif /* PEQAUTOEQ_H */

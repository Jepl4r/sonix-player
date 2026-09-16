#ifndef RADIOPAGE_H
#define RADIOPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Internet radio: the hub (Language / Country / Genre, with the starred
// stations, the recently played and the search on the corner buttons), the
// list page all of those open, and the search page.
//
// The Wi-Fi rule is the transfer page's, minus the part about staying open:
// nothing can be browsed or played without a connection, and the page says so
// and offers the way to the Wi-Fi settings. Once a station is playing the page
// is free to be left -- the stream belongs to the player, not to this screen.
extern lv_obj_t *radiopage_screen;
extern lv_obj_t *radiolist_screen;
extern lv_obj_t *radiosearch_screen;

void radiopage_init(gui_config_t *cfg);

#endif /* RADIOPAGE_H */

#ifndef SUBSONICPAGE_H
#define SUBSONICPAGE_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Subsonic pages: Navidrome and every other server that speaks the same
// API. The home page holds the server, the user and the password; behind it is
// a single list page reused for the three levels of the browser -- artists,
// an artist's albums, an album's songs.
void subsonicpage_init(gui_config_t *cfg);

// The home page, for the streaming grid.
extern lv_obj_t *subsonic_screen;

#endif /* SUBSONICPAGE_H */

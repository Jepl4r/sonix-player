#ifndef QOBUZPAGE_H
#define QOBUZPAGE_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Qobuz pages: the main one, login, search and the results list (a single
// list, reused for tracks, albums, artists and playlists).
void qobuzpage_init(gui_config_t *cfg);

// The main page, for the streaming grid.
extern lv_obj_t *qobuz_screen;

// Opens the track list of a Qobuz album from outside: the player's "show album"
// when what is playing came from Qobuz. False when it cannot be done (not
// logged in, missing id).
bool qobuzpage_open_album(const char *album_id, const char *title);

// The list page, for callers that need to know where back leads.
extern lv_obj_t *qobuz_list_screen;

#endif /* QOBUZPAGE_H */

#ifndef TIDALPAGE_H
#define TIDALPAGE_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Tidal pages: the main one, login, search and the results list (a single
// list, reused for tracks, albums, artists and playlists).
void tidalpage_init(gui_config_t *cfg);

// The main page, for the streaming grid.
extern lv_obj_t *tidal_screen;

// Opens the track list of a Tidal album from outside: the player's "show album"
// when what is playing came from Tidal. False when it cannot be done (not
// logged in, missing id).
bool tidalpage_open_album(const char *album_id, const char *title);

// The list page, for callers that need to know where back leads.
extern lv_obj_t *tidal_list_screen;

// "Can this file be played right now?", for the queue. tidalpage_init()
// registers it itself with device_state_add_prepare_cb(); it is declared here
// because it is defined elsewhere in the file than where it is registered.
bool tidalpage_prepare_track(const char *path);

#endif /* TIDALPAGE_H */

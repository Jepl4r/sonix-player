#ifndef SUBSONICPAGE_H
#define SUBSONICPAGE_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Subsonic pages (Navidrome, Gonic, Airsonic, ...): the main one, sign-in,
// search, settings and the results list (a single list, reused for tracks,
// albums, artists and playlists).
void subsonicpage_init(gui_config_t *cfg);

// The main page, for the streaming grid.
extern lv_obj_t *subsonic_screen;

// Opens the track list of a Subsonic album from outside: the player's "show
// album" when what is playing came from the server. False when it cannot be done (not
// logged in, missing id).
bool subsonicpage_open_album(const char *album_id, const char *title);

// The list page, for callers that need to know where back leads.
extern lv_obj_t *subsonic_list_screen;

#endif /* SUBSONICPAGE_H */

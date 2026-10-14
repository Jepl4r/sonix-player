#ifndef ZVUKPAGE_H
#define ZVUKPAGE_H

#include <stdbool.h>

#include "lvgl/lvgl.h"

#include "src/gui/shell/gui.h"

// The Zvuk pages (fork only, SONIX_ZVUK): home -- a column of cards, and with
// no session the way in, which is a file on the card (zvuk_token.txt,
// cardimport.h) and nothing else; one list page reused for liked tracks,
// albums, artists, playlists, an album, an artist, search results (with the
// kind of result as pills) and what is saved on the card; and the search
// field.
//
// The lists are 2.0 rows (svclist.h) with the gesture matrix: a tap plays the
// list from the row and the page stays, a swipe right puts it next, a swipe
// left at the end of the queue (both with Undo), a hold brings up the row's
// sheet, the star likes it (with Undo). A failed load is the error panel with
// "Retry" (streamerror.h).
//
// GUI thread; the network on the page's own worker, back through gui_post().
// The worker, the rows and the list are made the first time they are needed.

void zvukpage_init(gui_config_t *cfg);

extern lv_obj_t *zvuk_screen;

// The tile and the badge (zvukicons.c, tools/zvuk_icons.py).
extern const lv_image_dsc_t icon_menu_zvuk;
extern const lv_image_dsc_t icon_zvuk_badge;

// "Show album" from the player.
bool zvukpage_open_album(const char *album_id, const char *title);

// The developer options' "Zvuk: connection check": the profile and one
// getStream on the worker, the result in a sheet. An lv_event_cb_t.
void zvukpage_check_connection_cb(lv_event_t *e);

#endif /* ZVUKPAGE_H */

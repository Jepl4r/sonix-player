#ifndef YANDEXPAGE_H
#define YANDEXPAGE_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Yandex Music pages: home (My Wave, liked tracks, playlists, search),
// sign-in with a code and a QR, and one list page reused for tracks, albums,
// artists and playlists.
//
// Everything here runs on the GUI thread; the network is on the page's own
// worker thread, whose results come back through gui_post(). Nothing is
// allocated until the page is first opened.

// Builds the pages and hands the service's GUI parts to the provider registry
// (yandex.c itself is started by main(), and the cache follows the card from
// there). Call once at startup from the streaming hub.
void yandexpage_init(gui_config_t *cfg);

// Whether the tile belongs in the streaming grid: false without the [yandex]
// keys, and then the service does not exist as far as the user can see.
bool yandexpage_available(void);

extern lv_obj_t *yandex_screen;

// "Show album" from the player: opens the album's list. False when it cannot
// (not signed in, no id).
bool yandexpage_open_album(const char *album_id, const char *title);

// "Can this file be played right now?", for the queue (registered by init).
bool yandexpage_prepare_track(const char *path);

#endif /* YANDEXPAGE_H */

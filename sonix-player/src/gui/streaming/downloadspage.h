#ifndef DOWNLOADSPAGE_H
#define DOWNLOADSPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Downloads: what the streaming services keep on the card, and what is
// coming down right now.
//
// One card per service that can work in this firmware (Qobuz, Tidal, Subsonic,
// Yandex Music): how much of its cache is used against its ceiling, how many
// tracks that is, the track downloading at this moment with its progress, and
// a "Clear cache" that calls the service's own clear. Yandex's card also shows
// the "Save to library" queue -- the file in progress, how many wait -- with a
// way to stop it.
//
// Opened from the download button in the Streaming page's top bar, beside
// "+": the caches are the services' together, so the page belongs to the
// section rather than to any one service's settings.
//
// Costs nothing while closed: the page is built on its first opening, its
// once-a-second refresh timer runs only while it is on screen, and the cache
// folders are measured (on a thread of its own, below the interface) when it
// opens, after a clear and when a download ends -- not every second.
//
// GUI thread only.

void downloadspage_open(gui_config_t *cfg);

#endif /* DOWNLOADSPAGE_H */

#ifndef PODCASTPAGE_H
#define PODCASTPAGE_H

#include "lvgl.h"
#include "src/gui/shell/gui.h"

void podcastpage_init(gui_config_t *cfg);

// The section's screen, for the streaming grid. There is only one: the two
// entries are pills -- like the audiobooks' all / recent / finished -- and a
// pill changes what is underneath rather than navigating elsewhere, so the
// list sits on the same screen.
extern lv_obj_t *podcast_screen;

// Where leaving the player returns to after a podcast was opened from there.
// The same object as podcast_screen; both names remain because they say
// different things to the reader.
extern lv_obj_t *podcast_list_screen;

// Opens a podcast's episode list. Called by "show podcast" in the track menu,
// which read the id from the sidecar file of the episode playing. False when it
// cannot be done (section disabled, no network).
bool podcastpage_open_feed(long long feed_id, const char *title);

#endif /* PODCASTPAGE_H */

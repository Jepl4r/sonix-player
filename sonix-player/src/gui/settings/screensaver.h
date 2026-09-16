#ifndef SCREENSAVER_H
#define SCREENSAVER_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"

// The screensaver: what the panel shows for a moment when it lights back up.
// The loaded track's artwork fills the screen, and underneath it a strip of
// the blurred cover carries the title, the artist and the time in large
// figures. A chevron pointing up says how to get rid of it: swipe up.
//
// The artwork is borrowed from the player, which has already decoded it for
// the current track -- nothing is decoded here, so a wake stays instant.

void screensaver_init(gui_config_t *cfg);

// Config-backed on/off ("screen"/"screensaver"), set from Settings > Display.
// Off is the default.
void screensaver_set_enabled(bool enabled);
bool screensaver_get_enabled(void);

// Where the picture comes from.
//
// Album is the artwork of what is playing, borrowed from the player and free.
// Images is the card's own Screensaver folder, and costs a decode at every
// wake -- which is why it is not the default and why the folder is judged
// before the option can be chosen at all.
typedef enum {
	SCREENSAVER_SOURCE_ALBUM = 0,
	SCREENSAVER_SOURCE_IMAGES,
} screensaver_source_t;

screensaver_source_t screensaver_source(void);
void screensaver_set_source(screensaver_source_t source);

// Where the card is mounted, for the Screensaver folder below. Called at
// startup and again whenever a card is attached, like every other module that
// keeps a path on the card (see card_databases_attach).
void screensaver_set_card_root(const char *root);

// Whether that folder holds at least one picture this screen can show: a
// baseline (not progressive) JPEG, no wider than the screen and no taller,
// and portrait rather than landscape. Stops at the first one it finds, so the
// answer costs one readable file and not a whole folder.
//
// What the settings page asks before letting Images be chosen: an option that
// would show nothing is worse than an option that is not offered.
bool screensaver_has_images(void);

// Raises it now. Does nothing when the option is off.
void screensaver_show(void);

// What the power code calls both when the panel goes dark (so the frame left
// in the framebuffer is the one the next wake should open on) and when it
// lights up again: raises the screensaver if it is enabled, and refreshes what
// it shows either way -- the track may well have moved on while the screen was
// off.
bool screensaver_prepare(void);

// Lets go of the artwork it borrowed from the player. The player calls this
// through its release hook immediately before freeing those pixels, because
// the screensaver can be up (invisible, behind a blanked panel) while a track
// change happens.
void screensaver_release_artwork(void);
void screensaver_hide(void);
bool screensaver_is_visible(void);

#endif /* SCREENSAVER_H */

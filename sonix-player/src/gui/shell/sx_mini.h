#ifndef GUI_SX_MINI_H
#define GUI_SX_MINI_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// ---------------------------------------------------------------------------
// Sonix UI 2.0: the mini player
//
// One object on lv_layer_top(), shared by every page: GUI_SX_MINI_H (72) tall in
// the SX_MINI_SLOT_H (88) slot at the bottom of the screen, always dark in the
// playing album's colour (sx_pal()->mini_bg), with a 1 px light line along its
// top. Left to right: the cover (a plaque in the album's tone until a
// thumbnail is handed over), the title and the signal chain under it
// ("● LDAC 990 · 44.1 kHz", amber when the codec is not the one asked for --
// sx_chain.h), and a white play/pause disc; a 3 px progress line along the
// bottom edge.
//
// Gestures (handoff matrix): a tap opens the player; a swipe left of
// SX_MINI_SWIPE_PX plays the next track, right the previous one; a swipe up
// of SX_MINI_SWIPE_UP_PX opens the player too. The disc plays and pauses.
//
// It is not there on the player, while nothing has ever played (the idle
// state: sx_frame.h's idle cards take the slot), on the pages that own the
// whole screen (sx_mini_hide_on_screen) and while an on-screen keyboard is up
// on the page. After a reboot it shows the remembered track, paused.
//
// The player feeds it (player.c): the track on a change, the state on every
// change of it, the position on its poll. Nothing here has a timer: hidden or
// still, it costs nothing; playing, a redraw of the 3 px line each time it
// grows by a pixel, and of the chain line when its text changes.
//
// Size: 14 objects, about 3 KB with their styles and texts (see the report
// in the commit introducing it).
//
// UI thread only.
// ---------------------------------------------------------------------------

// Built by sx_frame_init().
void sx_mini_init(gui_config_t *cfg);

// What is loaded. `title` is shown as given (a tag, not a translation key);
// `track_path` is the file (NULL for a stream), `live` a stream without a
// position. NULL title with no path and not live: nothing is loaded, the mini
// player goes (idle).
void sx_mini_set_track(const char *title, const char *track_path, bool live);

void sx_mini_set_playing(bool playing);

// Seconds in and the length; `total` 0 for a live stream (the line stays
// full) or a length not known yet (empty).
void sx_mini_set_progress(double position, double total);

// Reads the signal chain again (sx_chain_short) and rewrites the line if it
// changed. The player calls it with the track and now and then on its poll.
void sx_mini_refresh_chain(void);

// The cover's thumbnail, SX_MINI_COVER square (or larger: it is clipped to the
// rounded plaque, centred). NULL goes back to the tone plaque. The image is
// borrowed: it must stay valid until replaced or cleared.
void sx_mini_set_cover(const lv_image_dsc_t *thumbnail);

// The player opened (true) or closed: the mini player gives way to it.
void sx_mini_player_shown(bool open);

// `screen` owns the whole panel -- a game, the book reader, a scan in
// progress -- and the mini player stays off it.
void sx_mini_hide_on_screen(lv_obj_t *screen);

// Looks again at whether it should be up: called when the page changes and
// when a keyboard comes or goes.
void sx_mini_update_visibility(void);

bool sx_mini_visible(void);

// Something is loaded (whether or not the mini player is up just now). False
// is the idle state.
bool sx_mini_has_track(void);

// The container, for a page that needs to place something against it.
lv_obj_t *sx_mini_obj(void);

#endif /* GUI_SX_MINI_H */

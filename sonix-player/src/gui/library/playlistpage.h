#ifndef PLAYLISTPAGE_H
#define PLAYLISTPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Playlist page: the .m3u files in the card's Playlist folder, one row
// each, in one of two modes. Opened from the Music page it is a browser --
// tapping a playlist opens its tracks. Opened from a track's "Add to playlist"
// it is a picker: the same list, with the tapped playlist getting the track,
// and a "New playlist" row that asks for a name first.
extern lv_obj_t *playlistpage_screen;

void playlistpage_init(gui_config_t *cfg);

// Browse: the playlists themselves.
void playlistpage_open(void);

// Opens the playlist `name` as its own track list, exactly as tapping its row
// does. A caller that has just written a playlist hands the user straight to it
// this way, instead of back to wherever the track was picked up.
void playlistpage_open_named(const char *name);

// Pick: choose (or create) the playlist `track_path` is added to.
void playlistpage_add_track(const char *track_path);

// Pick for a set of local tracks: the chosen playlist gets all of them, in this
// order, written in the background. The page keeps its own copy of the paths.
void playlistpage_add_tracks(const char *const *paths, int count);

// Stars a set of local tracks, in this order, in the background: not a picker,
// only a card while it works and one that says how it went. A track already
// starred stays where it is. The page keeps its own copy of the paths.
void playlistpage_add_favourites(const char *const *paths, int count);

// A set of local tracks into the playlist called `name`, made first when there
// is none: what the picker does once a playlist is chosen, for a caller that
// already knows which (SonixLink). In the background, with the same cards.
// False when the playlist could not be made or the work not started.
bool playlistpage_add_tracks_to(const char *name, const char *const *paths, int count);

#endif /* PLAYLISTPAGE_H */

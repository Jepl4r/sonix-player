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

// Pick: choose (or create) the playlist `track_path` is added to.
void playlistpage_add_track(const char *track_path);

#endif /* PLAYLISTPAGE_H */

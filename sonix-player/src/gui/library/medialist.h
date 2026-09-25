#ifndef MEDIALIST_H
#define MEDIALIST_H

#include "src/gui/shell/gui.h"
#include "src/system/library/library.h"

#include "lvgl/lvgl.h"

// The library index pages: one screen for name lists (albums, artists, album
// artists, genres), one for one artist's records, and one for track lists (all
// tracks, and the tracks of whatever was tapped). A screen each rather than one
// reused, so the back chevron walks artists -> that artist's albums -> the
// album's tracks and back out again the way it should.
//
// Rows are a fixed pool windowed over the scroll position, the same technique
// as the file browser, and the data is windowed with them: the page holds the
// row ids the list is made of (four bytes each) and reads a bandful of actual
// rows back as the viewport moves, so the library has no size ceiling.
extern lv_obj_t *medialist_screen;
extern lv_obj_t *medialist_tracks_screen;
// The middle level: one artist's records. Its own screen and not the first
// one reused, because the way back from an album's tracks is that artist's
// albums and the way back from those is the artist list -- three lists, and a
// screen cannot be two of them at once.
extern lv_obj_t *medialist_albums_screen;

void medialist_init(gui_config_t *cfg);

// Whether tapping an artist opens their records or their tracks. On by default;
// Music > Display options turns it off, and an artist page is then a flat list
// of that artist's tracks, with the album grouping on the corner button.
bool medialist_album_view(void);
void medialist_set_album_view(bool on);

// Whether a track row wears a small badge saying what it is -- lossy, CD,
// hi-res or DSD -- under its title. Off by default.
bool medialist_quality_badges(void);
void medialist_set_quality_badges(bool on);

// "Show artist": the artist under the title of each row, on the lists `lists`
// picks out -- all the tracks, the albums, the tracks of a genre. The tracks
// show their own artist, the albums their album artist.
#define MEDIALIST_ARTIST_TRACKS 1
#define MEDIALIST_ARTIST_ALBUMS 2
#define MEDIALIST_ARTIST_GENRES 4
bool medialist_show_artist(void);
int medialist_artist_lists(void);
void medialist_set_show_artist(bool on, int lists);

// Loads the list and switches to the right screen. For LIBRARY_LIST_TRACKS
// the filter narrows to one album/artist/genre (LIBRARY_FILTER_NONE = all);
// for the name kinds both filter arguments are ignored.
void medialist_open(const char *title, library_list_t kind, library_filter_t filter, const char *filter_value);

// Re-reads what is playing and moves the accent mark to whichever rows now
// carry it. Called from the player whenever the track changes; cheap enough
// to call on a track that has not (it walks two dozen pool rows).
void medialist_notify_now_playing(void);

// The same track list, filled from an explicit set of paths rather than from
// the index: what a playlist's contents are. There is no query behind it, so
// this is the one list still held whole in RAM -- a playlist file is a few
// hundred lines, not a library.
//
// `names` may be NULL, or hold NULLs, in which case the file name is used;
// `paths` and `names` are copied. `playlist` is the name of the playlist the
// list came from, or NULL when it came from somewhere else -- it is what makes
// the ellipsis offer to take a track out of the list rather than add it to
// another one.
void medialist_open_paths(const char *title, const char *const *paths, const char *const *names,
						  const char *const *artists, int count, const char *playlist);

// A playlist was renamed while its track list was still open behind the
// playlists page. The list keeps the name it was opened with, and would go on
// writing to a file that no longer exists, so it is told the new one.
void medialist_playlist_renamed(const char *old_name, const char *new_name);

#endif /* MEDIALIST_H */

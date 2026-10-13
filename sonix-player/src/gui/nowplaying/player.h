#ifndef PLAYER_H
#define PLAYER_H

#include "src/gui/nowplaying/cover.h"
#include "src/gui/shell/gui.h"
#include "src/system/library/library.h"
#include "src/system/streaming/podcast.h" // podcast_feed_t

#include "lvgl/lvgl.h"

extern lv_obj_t *player_screen;

void player_init(gui_config_t *cfg);

// Start playing the given file and switch its now-playing info in the UI.
// Does not bring the player up; call player_sheet_open() separately.
void player_play_file(const char *filepath);

// Re-reads the loaded track and repaints the now-playing UI. For callers
// that set up playback through device_state directly (queue jumps, list
// playback).
void player_refresh_now_playing(void);

// Redraws only the artist line, after "Use track artist" has been switched.
void player_refresh_artist(void);

// The "4/12" under the progress bar: the place in the queue as the screen shows
// it (the deal's order under shuffle) and the length. False, with both zero,
// where the screen shows nothing -- one track, a book, DLNA, a stream.
bool player_queue_position(int *position, int *count);

// ---------------------------------------------------------------------------
// The three arrangements of the now-playing page
//
// Standard is the one this player has always had. Waveform moves the title and
// the star onto the sleeve and turns the progress bar into the shape of the
// track, all of it drawn in the sleeve's own colour. Studio centres the sleeve
// with the title across the top and the format under it, over a blurred copy of
// the same artwork filling the screen.
//
// The setting lives in Music > Display options; these are how that page reads
// and writes it. The numbers are what goes in the config, so the first two keep
// the values they have always had.
//
// Which one is actually in force also depends on what is playing: see the note
// on layout_alternative in player.c. Waveform is for a file on the card; Studio
// takes whatever is playing, a station included.
//
// The names below are the ones the code has always used; the pills say
// Normale, Waveform and Alternativo. ALTERNATIVE is the one the pill calls
// Waveform, and STUDIO the one it calls Alternativo.
// ---------------------------------------------------------------------------
typedef enum {
	PLAYER_LAYOUT_STANDARD = 0,
	PLAYER_LAYOUT_ALTERNATIVE = 1,
	PLAYER_LAYOUT_STUDIO = 2,
} player_layout_t;

player_layout_t player_layout_get(void);
void player_layout_set(player_layout_t layout);

// Whether the Standard and Studio arrangements take the sleeve's colour as
// their accent, as Waveform always does. Off by default; saved in the config.
bool player_cover_accent(void);
void player_cover_accent_set(bool on);

// The output the music was going to has just been unplugged -- a jack pulled
// out, a USB-C DAC removed, headphones that walked out of range. Pauses, so
// that taking the headphones off does not cost three tracks of an album, and
// so that nothing is left playing into a socket with nothing in it.
//
// Does nothing unless something is actually playing. `what` names the output
// for the log and is not shown to anyone. Callers decide whether the output
// that went away was the one in use; this only acts on the decision.
void player_output_unplugged(const char *what);

// ---------------------------------------------------------------------------
// The player as a sheet
//
// The player is not a page you navigate to but a panel over whatever page is
// on screen. Since 2.0 it comes up from the bottom -- the mini player's tap or
// upward swipe (sx_mini.h), or anything that switches to player_screen -- and
// goes back down: the chevron (pointing down now) or a pull down past
// SX_NP_COLLAPSE_PX, which it follows. It is an object on the top layer
// rather than an LVGL screen, so it can be drawn over the page it came up on.
// ---------------------------------------------------------------------------

// The hardware transport keys, dispatched here from the button threads.
void player_key_play_pause(void);
void player_key_next(void);
void player_key_prev(void);
// The side buttons' remappable extras (keymap.h). Each says what it did in a
// toast, since the player is usually in a pocket when they are pressed.
void player_key_favourite(void);   // the star, add only: never takes one off
void player_key_next_album(void);  // device_state_next_album()
void player_key_cycle_mode(void);  // the repeat/shuffle button

// What the player's on-screen prev/next buttons do: seconds-sized skips on an
// audiobook or podcast, previous/next track otherwise. The control centre
// calls these so that it behaves exactly like the player.
void player_screen_prev(void);
void player_screen_next(void);

// The feed of the episode being played, for the control centre's star button.
// Returns true and fills id/feed when a podcast is playing.
bool player_current_podcast_feed(long long *id_out, podcast_feed_t *feed_out);

bool player_sheet_is_open(void);

// True while the gesture in progress has been claimed as the pull down. Tap
// handlers check this so that the end of a drag does not also press a button.
bool player_sheet_drag_active(void);
void player_sheet_open(bool animate);
void player_sheet_close(bool animate);

// Makes `obj` a place the sheet can be dragged from. `opening` false: a
// surface of the sheet itself, which pulls it down. `opening` true was a page
// surface that pulled the player in with a leftward drag before 2.0; that is
// the mini player's job now, and the call only makes `obj` clickable.
void player_sheet_attach_drag(lv_obj_t *obj, bool opening);

// Boot-time "Remember track": loads the remembered track, paused at position.
void player_restore_track(const char *filepath, double position);

// The boot-time restore of a queue that was a library list: the handle takes
// the place of the list of paths, so next/prev work from the first press
// instead of landing back on the remembered track. Takes ownership of `ix`.
void player_restore_index(library_index_t *ix, int start_index, const char *const *extra, const int *extra_slots,
						  int extra_count, const char *filepath, double position);

// The same for a queue saved as a plain list of paths. `custom` restores the
// queue's own custom flag, which the list of paths on its own cannot carry.
void player_restore_list(const char *const *list, int count, int start_index, bool custom, const char *filepath,
						 double position);

// The loaded track's artwork, already decoded and scaled by the player: the
// cover itself, and the blurred copy the controls are drawn on. NULL when the
// track has no artwork (or its decode has not landed yet). Borrowed, not
// owned -- the player frees these at the next track change, so nothing may
// keep them across one.
const lv_image_dsc_t *player_cover_image(void);
const lv_image_dsc_t *player_backdrop_image(void);

// Registers a callback run immediately before that artwork is freed, so
// whoever borrowed it can drop its references first. One at a time; NULL
// clears it.
void player_set_cover_release_cb(void (*cb)(void));

// The size the standard arrangement draws the cover at. A page that shows the
// same cover asks for it at this size, so the player's table of covers serves
// both (coverloader_request_shared). 0 x 0 before the player is built.
void player_cover_box(int *w, int *h);

// The cover the player shows now, scaled to a square of `size`, with its
// colour; `id` is the picture's cover_source_id(), 0 while not known yet.
// False with no cover up, or one still on its way.
bool player_cover_copy(int size, cover_image_t *out, uint64_t *id);

// The status bar kept up over the player too (Settings > Appearance > Always
// show the status bar). Not saved here: the settings page writes the config.
void player_set_statusbar_always(bool on);
bool player_statusbar_always(void);


#endif

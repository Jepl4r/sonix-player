#ifndef GUI_SX_ROW_H
#define GUI_SX_ROW_H

#include <stdbool.h>

#include "lvgl/lvgl.h"

// ---------------------------------------------------------------------------
// Sonix UI 2.0: the list row
//
// One row of a list: tracks, albums, artists, genres, years, stations, books,
// the queue. Left to right: a lead (the cover's thumbnail -- a plaque in the
// cover's tone until there is one -- a track number, or a letter on a tone
// plaque, round for an artist), the title (26) with a subtitle line under it
// (20, secondary: the artist, then an accent badge such as "· 24/96", and the
// duration at the right), and a trailing slot: the star button (72 wide), a
// chevron, or nothing.
//
// States: normal (transparent on the page), pressed (the pressed fill, at
// once), playing (the surface, the title in the accent, the equaliser bars
// in place of the number or over the thumbnail), swiped (sliding over its
// underlay).
//
// Gestures, the handoff's matrix:
//   tap                       on_tap -- play a track, open an album
//   drag right past 100 px    on_swipe_next -- "⇥ Play next", accent underlay
//   drag left past 100 px     on_swipe_end -- "+ Add to the end", or, with
//                             end_is_remove, the queue's red "✕ Remove"
//   hold 450 ms               on_long_press -- the item's bottom sheet
//   the star                  on_star
// A drag is taken sideways only after SX_SWIPE_LOCK_PX and only when more
// across than down; it follows the finger up to SX_SWIPE_MAX_PX and springs
// back in SX_ANIM_ROW_SNAP_MS. While it is taken, the list's vertical scroll is
// held off (the scrollable ancestor's flag is lifted for the gesture, as the
// handoff advises). A vertical drag stays the list's scroll; a press that
// starts in the left edge strip stays the swipe back (switcher.h). A NULL
// handler turns its gesture off: an artist row has no swipes, a station no
// sheet.
//
// Recycling: a list keeps a pool of rows and rebinds them as it scrolls
// (medialist keeps twelve). Every setter below only changes what the row
// shows -- no object is created or deleted after sx_row_create(), except the
// thumbnail's image object, made once on first use -- so rebinding is a
// handful of label and style changes. sx_row_set_user() says which item the
// row stands for now; the handlers get it back.
//
// The row's height follows Appearance > Large rows (SX_ROW_H / SX_ROW_H_BIG,
// compact SX_ROW_H_COMPACT / _BIG) as it is when the row is created or
// sx_row_set_compact() is called.
//
// UI thread only. Cost: 13 objects per row (16 with a thumbnail image); an
// idle row runs nothing. The equaliser runs one 10 fps timer while a set of
// bars exists and the music plays (stepping them only while one is on screen,
// paused with the panel dark), redrawing only their 21 x 22 px.
// ---------------------------------------------------------------------------

typedef struct {
	void (*on_tap)(lv_obj_t *row, void *user);
	void (*on_long_press)(lv_obj_t *row, void *user);
	void (*on_swipe_next)(lv_obj_t *row, void *user);
	void (*on_swipe_end)(lv_obj_t *row, void *user);
	void (*on_star)(lv_obj_t *row, void *user);
	// The left swipe removes (the queue): red underlay, "✕ Remove".
	bool end_is_remove;
} sx_row_handlers_t;

typedef enum {
	SX_ROW_TRAIL_NONE,
	SX_ROW_TRAIL_STAR,
	SX_ROW_TRAIL_CHEVRON,
} sx_row_trail_t;

// Builds a row in `parent` (a list: flex column, or the caller places it).
// `handlers` is copied. `compact`: the queue's 64 px row with a 48 px lead.
lv_obj_t *sx_row_create(lv_obj_t *parent, const sx_row_handlers_t *handlers, bool compact);

// The row's height for `compact`, with Large rows as it is now: for a list
// that lays its rows out itself.
int sx_row_height(bool compact);

// Re-reads Large rows and resizes.
void sx_row_set_compact(lv_obj_t *row, bool compact);

void sx_row_set_user(lv_obj_t *row, void *user);
void *sx_row_get_user(lv_obj_t *row);

void sx_row_set_title(lv_obj_t *row, const char *title);
// `subtitle` in the secondary colour, then `badge` in the accent (NULL for
// none), and `duration` at the right (NULL for none).
void sx_row_set_subtitle(lv_obj_t *row, const char *subtitle, const char *badge, const char *duration);

// The lead. A thumbnail (`image` NULL: the tone plaque in `tone`), a track
// number, or a letter or two on a tone plaque (`round` for an artist).
void sx_row_set_thumb(lv_obj_t *row, const lv_image_dsc_t *image, lv_color_t tone);
void sx_row_set_number(lv_obj_t *row, int number);
void sx_row_set_letter(lv_obj_t *row, const char *letter, lv_color_t tone, bool round);
void sx_row_set_no_lead(lv_obj_t *row);

void sx_row_set_trail(lv_obj_t *row, sx_row_trail_t trail);
void sx_row_set_starred(lv_obj_t *row, bool starred);

// The playing row: surface, accent title, equaliser bars.
void sx_row_set_playing(lv_obj_t *row, bool playing);

// Turns the left swipe into the queue's "Remove" (or back), for one row: the
// current track in the queue cannot be removed and passes NULL handlers
// instead -- see sx_row_set_swipes().
void sx_row_set_swipes(lv_obj_t *row, bool next, bool end);

// ---------------------------------------------------------------------------
// The equaliser bars
//
// Three bars 5 x 22, 3 apart, stepping in four steps at about 10 frames a
// second, each at its own tempo; frozen while the music is paused. The rows
// use them; a page can put one anywhere else (the album page's playing track
// number).
// ---------------------------------------------------------------------------

lv_obj_t *sx_eq_create(lv_obj_t *parent);

// Whether the music plays: the bars move or freeze, all of them. The player
// calls it with every change of state.
void sx_eq_set_playing(bool playing);

#endif /* GUI_SX_ROW_H */

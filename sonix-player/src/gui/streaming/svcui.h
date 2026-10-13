#ifndef SVCUI_H
#define SVCUI_H

#include <stdbool.h>
#include <stddef.h>

#include "lvgl/lvgl.h"

#include "src/gui/shell/gui.h"
#include "src/system/playback/playlist.h"

// ---------------------------------------------------------------------------
// The small parts every streaming page shares in 2.0
//
// What the Radio, Subsonic, Yandex, Qobuz, Tidal, podcast and Downloads
// pages each built for themselves before -- the header's buttons, a group's
// heading, the plaque a row shows until it has a picture, the "Play next" /
// "Add to the end" with its toast -- once, so the services read as one
// interface. The list itself is svclist.h; the failed-load panel is
// streamerror.h. UI thread only; nothing here runs while it is not called.
// ---------------------------------------------------------------------------

// A button in the header, right of the title: slot 0 in the corner, slot 1
// to its left. The same 56 px target and spacing as the Music page's.
lv_obj_t *svcui_header_button(lv_obj_t *screen, gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph,
							  lv_event_cb_t cb, void *user);

// The handoff's group heading -- "FAVOURITES · 3" -- as a line in a column of
// cards (a list draws its own, svclist.h). Mono caps in the secondary colour.
lv_obj_t *svcui_group(lv_obj_t *parent, const char *text);

// A note under the cards: a sentence in the secondary colour, wrapping.
lv_obj_t *svcui_note(lv_obj_t *parent, const char *text);

// The plaque colour for something without a picture yet: the cover tone at
// the hue `name` hashes to (a station, an album without art, a playlist).
lv_color_t svcui_tone(const char *name);

// "Play next" / "Add to the end of the queue" from a streaming row: the
// tracks (their files, or the cache paths their service fetches when they
// come up) go into the queue, the queue is written to the card once, and the
// toast says what went where -- with "Undo", which takes exactly those
// entries out again while they are still where they were put. `what` names
// them in the toast (a title, an album). Returns how many went in; a notice
// says so when none could.
int svcui_queue(const char *const *paths, int count, playlist_queue_where_t where, const char *what);

// The toast for a star turned on or off from a row, with "Undo" calling
// `undo(user)`. `on_key` / `off_key`: the translation keys of the two
// sentences, each with one %s for `what`.
void svcui_star_toast(bool on, const char *on_key, const char *off_key, const char *what, void (*undo)(void *user),
					  void *user);

// A wait in the middle of a page, for a list on its way: the spinner alone,
// over the page, not blocking it (the handoff has no veils). Shown and hidden
// by the page; one per screen.
lv_obj_t *svcui_spinner(lv_obj_t *screen);

#endif /* SVCUI_H */

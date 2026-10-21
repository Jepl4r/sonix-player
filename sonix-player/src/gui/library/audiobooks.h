#ifndef AUDIOBOOKS_H
#define AUDIOBOOKS_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The audiobooks section: four tiles (library, series, authors, continue
// listening) and corner buttons for the bookmarks and the finished books, over
// a books page and an authors-or-series page, an options page carrying the
// scan and the transport-button setting, the page that setting opens, and the
// scan progress page (the music one's layout with its own icon and words).
extern lv_obj_t *audiobooks_screen;
extern lv_obj_t *audiobooksettings_screen;
extern lv_obj_t *audiobookcontrols_screen;
extern lv_obj_t *audiobookscan_screen;

void audiobooks_init(gui_config_t *cfg);

// What the audiobook equalizer is set to, in words: "Same as music", "Off", a
// built-in curve by its translated name, or a user preset by its name.
void audiobooks_eq_choice_text(char *out, size_t size);

// Starts `book` (a file, or a folder book's folder) in `file` at `seconds`,
// and writes that as where the book stands. The player is left as it is: the
// caller decides whether it comes up.
void audiobooks_play_at(const char *book, const char *file, double seconds);

#endif /* AUDIOBOOKS_H */

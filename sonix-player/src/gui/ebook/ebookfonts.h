#ifndef EBOOKFONTS_H
#define EBOOKFONTS_H

#include <stdbool.h>

#include "lvgl/lvgl.h"

// ---------------------------------------------------------------------------
// The four faces a book is set in, loaded from the card and only while reading
//
// Bookerly, in the four real styles rather than one face slanted and
// emboldened by the renderer. The firmware ships them in
// usr/resource/sonix/fonts/ebook; a set in `.local/fonts/` on the card is used
// when that folder has none.
//
// So: nothing is loaded until the reader opens, the regular face is loaded
// then, and the other three only when a book actually uses them -- a novel
// with no bold never pays for the bold face. Closing the reader destroys all
// four and the glyph cache with them.
//
// A second set of the same files at another size exists for the one-word view
// (ebookrsvp.h), which sets a word two sizes up from the page, and is loaded
// and dropped the same lazy way.
//
// The one rule that cannot be broken: no LVGL object may still point at one of
// these fonts when it is destroyed. ebookfonts_close() is therefore called
// after the reader's objects are deleted, never before.
// ---------------------------------------------------------------------------

// Opens the regular face at `size` pixels. False when neither folder has
// Bookerly -- the reader then falls back to the interface font, which is
// a worse book but still a book.
//
// `japanese` sets the book in the interface's Japanese faces (Rodin, regular
// and bold, from the firmware's fonts folder or the card's .local/fonts)
// instead: Bookerly has no kana or kanji.
bool ebookfonts_open(const char *sd_root, int size, bool japanese);

// Everything above, undone. Safe when nothing was opened.
void ebookfonts_close(void);

// Re-opens every face that is currently loaded at a new size. What the font
// size setting calls. The large set is dropped and comes back when next asked
// for.
bool ebookfonts_set_size(int size);
int ebookfonts_size(void);

// The face for a run of text. `bold` and `italic` come from the book; a face
// that is not on the card falls back the way the markdown asks -- bold italic
// to italic to bold to regular -- rather than making the book unreadable.
const lv_font_t *ebookfonts_face(bool bold, bool italic);

// The same face at `size` pixels out of the second set, for the one-word
// view. Opened the first time it is asked for and again when `size` changes;
// the objects showing the old size must be gone by then.
const lv_font_t *ebookfonts_large_face(bool bold, bool italic, int size);

// True when Bookerly was found. The settings page says so, because a reader
// that quietly uses the interface font looks like a reader with a bug.
bool ebookfonts_present(void);

#endif /* EBOOKFONTS_H */

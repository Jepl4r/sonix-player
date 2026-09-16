#ifndef EBOOKFONTS_H
#define EBOOKFONTS_H

#include <stdbool.h>

#include "lvgl/lvgl.h"

// ---------------------------------------------------------------------------
// The four faces a book is set in, loaded from the card and only while reading
//
// Bookerly, from `.local/fonts/` on the card, in the four real styles rather
// than one face slanted and emboldened by the renderer. The files are not
// compiled into the player: they belong to whoever put them on the card, they
// are a megabyte each, and the player has no business carrying them in .rodata
// for the sake of a page nobody may ever open.
//
// So: nothing is loaded until the reader opens, the regular face is loaded
// then, and the other three only when a book actually uses them -- a novel
// with no bold never pays for the bold face. Closing the reader destroys all
// four and the glyph cache with them.
//
// The one rule that cannot be broken: no LVGL object may still point at one of
// these fonts when it is destroyed. ebookfonts_close() is therefore called
// after the reader's objects are deleted, never before.
// ---------------------------------------------------------------------------

// Opens the regular face at `size` pixels. False when `.local/fonts` has no
// Bookerly at all -- the reader then falls back to the interface font, which is
// a worse book but still a book.
bool ebookfonts_open(const char *sd_root, int size);

// Everything above, undone. Safe when nothing was opened.
void ebookfonts_close(void);

// Re-opens every face that is currently loaded at a new size. What the font
// size setting calls.
bool ebookfonts_set_size(int size);
int ebookfonts_size(void);

// The face for a run of text. `bold` and `italic` come from the book; a face
// that is not on the card falls back the way the markdown asks -- bold italic
// to italic to bold to regular -- rather than making the book unreadable.
const lv_font_t *ebookfonts_face(bool bold, bool italic);

// True when Bookerly was found. The settings page says so, because a reader
// that quietly uses the interface font looks like a reader with a bug.
bool ebookfonts_present(void);

#endif /* EBOOKFONTS_H */

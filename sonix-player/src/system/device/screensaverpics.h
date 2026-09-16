#ifndef SCREENSAVERPICS_H
#define SCREENSAVERPICS_H

#include <stdbool.h>
#include <stddef.h>

// The Screensaver folder: which of its files this screen can show, and one of
// them at random.
//
// A folder in the root of the card, beside Playlist, holding the pictures the
// screensaver puts up instead of the record's artwork. The rules are narrow on
// purpose:
//
//   * JPEG, .jpg or .jpeg, and BASELINE -- the scaled decoder cannot read a
//     progressive one (see jpeg_scaled.h) and the fallback path is what runs a
//     64 MB device out of memory;
//   * no wider and no taller than the screen, and portrait rather than
//     landscape. The picture is drawn at full size on a panel held upright: one
//     that would have to be shrunk to fit is one that was not prepared for
//     this.
//
// A file breaking any of those is passed over in silence. With several in the
// folder the others still come up, and a picture drawn wrong is worse than a
// picture not drawn.
//
// Nothing here decodes anything: the rules are all decided from the JPEG
// headers, which is what lets a folder be judged at every wake.

// Where the card is mounted. Called at startup and again when a card is
// attached; passing NULL or "" leaves the folder unreachable, which is the
// right answer with no card in.
void screensaverpics_set_root(const char *card_root);

// The folder itself, or "" when there is no card.
const char *screensaverpics_dir(void);

// Whether one named file in the folder is a picture that can be shown.
bool screensaverpics_usable(const char *dir, const char *name, int max_w, int max_h);

// Whether the folder holds at least one. Stops at the first, so the answer
// costs one readable file rather than a whole folder.
bool screensaverpics_any(int max_w, int max_h);

// One picture at random, as a full path. `current` is the one already on
// screen (a path or a bare name, either will do) and is avoided while there is
// anything else to choose. False when nothing usable turned up, leaving `out`
// empty.
bool screensaverpics_pick(const char *current, int max_w, int max_h, char *out, size_t out_size);

#endif /* SCREENSAVERPICS_H */

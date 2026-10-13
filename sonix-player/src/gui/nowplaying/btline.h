#ifndef BTLINE_H
#define BTLINE_H

#include <stdbool.h>
#include <stddef.h>

#include "lvgl/lvgl.h"

// ---------------------------------------------------------------------------
// The Bluetooth line on Now Playing
//
// While the sound goes to Bluetooth headphones, a quiet line under the artist
// says what the link carries -- "LDAC 990 · 96 kHz", the same words as the
// headphones' row on the Bluetooth page -- and, when the track does not go out
// at its own rate, from which rate to which: "LDAC 990 · 44.1 → 48 kHz". That
// is the one thing about the sound a listener cannot otherwise see, and it
// changes with the "Sample rate" setting and with every track on "Follow the
// music".
//
// No timer of its own: the player asks for a refresh where it already looks
// at the track -- at each track change and on its playback poll, which slows
// down with the screen off -- and the line is only written when its text
// changes. Bluetooth audio settings > "Show details on Now Playing" turns it
// off ([bluetooth] nowplaying_details, on by default).
//
// UI thread only.
// ---------------------------------------------------------------------------

// Builds the line, hidden, as the last child of `parent` (the player's column
// with the title and the artist), so it follows them through every layout.
void btline_create(lv_obj_t *parent);

// Shows, hides or rewrites the line from the output and the track as they are
// now. Cheap: a few cached numbers when Bluetooth is not the output.
void btline_refresh(void);

// The line's text as it would be shown now -- "LDAC 990 · 96 kHz", "LDAC 990
// · 44.1 → 48 kHz" -- into `out`, or "" and false when the sound is not going
// to Bluetooth headphones. Whether the line is switched on does not matter
// here: this is for the 2.0 mini
// player's signal chain (sx_chain.h), which says the same thing.
bool btline_compose(char *out, size_t size);

bool btline_enabled(void);
void btline_set_enabled(bool on);

#endif /* BTLINE_H */

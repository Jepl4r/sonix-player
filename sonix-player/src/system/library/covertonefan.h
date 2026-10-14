#ifndef COVERTONEFAN_H
#define COVERTONEFAN_H

#include <stdbool.h>

// ---------------------------------------------------------------------------
// More than one listener for covertone's "new tones or thumbnails landed"
//
// covertone_set_listener() has a single slot, and since 2.0 several parts of
// the interface want the news: the lists (their rows' plaques become
// thumbnails), the player and the mini player (the playing album's 56 and 240
// px pictures and its hue), the queue page. Whoever sets the slot directly
// silences everyone else, so nobody should: register here instead. The first
// registration takes the slot for the fan-out; callbacks run in the order
// they were added, on the UI thread, at most about four times a second (the
// rate covertone calls its listener at). Up to COVERTONEFAN_MAX.
//
// UI thread only (registration and the calls both).
// ---------------------------------------------------------------------------

#define COVERTONEFAN_MAX 8

// Adds `cb` (once: adding it again does nothing). False when the table is full.
bool covertonefan_add(void (*cb)(void));

// Takes `cb` out again.
void covertonefan_remove(void (*cb)(void));

#endif /* COVERTONEFAN_H */

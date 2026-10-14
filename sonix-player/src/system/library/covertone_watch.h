#ifndef COVERTONE_WATCH_H
#define COVERTONE_WATCH_H

// ---------------------------------------------------------------------------
// Several listeners for covertone's one
//
// covertone_set_listener() takes a single callback, and more than one part of
// the interface wants to hear when tones and thumbnails land: the library
// lists (their plaques turn into pictures), the mini player and the player
// (their cover), an album page. This holds a short list of them and installs
// itself as covertone's listener while the list is not empty, so whoever uses
// it never calls covertone_set_listener() directly -- which would knock the
// others off.
//
// UI thread only (covertone calls its listener there, at most about four times
// a second). Adding a callback already in the list does nothing. Up to
// COVERTONE_WATCH_MAX callbacks. Nothing runs while the list is empty.
// ---------------------------------------------------------------------------

#define COVERTONE_WATCH_MAX 8

void covertone_watch_add(void (*changed)(void));
void covertone_watch_remove(void (*changed)(void));

#endif // COVERTONE_WATCH_H

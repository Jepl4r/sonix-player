#ifndef GENIUSUI_H
#define GENIUSUI_H

#include <stdbool.h>

// ---------------------------------------------------------------------------
// Library lists, search and the player share one background Smart Mix job and its
// progress overlay, so a mix can start from any screen on the GUI thread.
// ---------------------------------------------------------------------------

// Cheap GUI-thread check for a card with a graph available for track lookup.
bool geniusui_available(void);

// Builds and plays an ordered mix, with progress and errors. GUI thread only.
void geniusui_start(const char *seed_path);

// Builds the same mix and writes it to the card as a playlist named after the
// seed, leaving what is playing alone. The name is built here, from the
// language's own word for the feature, because the worker that writes the
// playlist cannot reach the language table. GUI thread only.
void geniusui_create_playlist(const char *seed_path);

#endif /* GENIUSUI_H */

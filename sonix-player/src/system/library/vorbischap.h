#ifndef VORBISCHAP_H
#define VORBISCHAP_H

#include <stdbool.h>

// ---------------------------------------------------------------------------
// The chapters inside an Opus, Ogg Vorbis or FLAC file
//
// These formats keep chapters in their Vorbis comments, a pair of tags each:
//
//     CHAPTER001=00:00:00.000
//     CHAPTER001NAME=Chapter one
//
// The number has any count of digits and may start at 000 (ffmpeg) or 001;
// the time is HH:MM:SS with optional fractions, or MM:SS. A chapter whose time
// cannot be read is left out, and the rest come back in time order.
// ---------------------------------------------------------------------------

typedef struct {
	double start; // seconds from the beginning of the file
	char title[128];
} vorbischap_t;

// Reads up to `max` chapters into `out` and returns how many the file has. `out`
// may be NULL (and `max` 0) to only count them. 0 for a file of another format.
int vorbischap_read(const char *path, vorbischap_t *out, int max);

// True when the file has at least one.
bool vorbischap_present(const char *path);

#endif /* VORBISCHAP_H */

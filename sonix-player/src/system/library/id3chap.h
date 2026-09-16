#ifndef ID3CHAP_H
#define ID3CHAP_H

#include <stdbool.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// The chapters inside an MP3
//
// An audiobook is not always an .m4b. A great many are one long .mp3 with ID3v2
// CHAP frames in it -- that is what every tool that splits an audiobook by
// chapter writes, and what Audible conversions come out as -- and to a player
// that only reads MP4 chapters those look like a four-hour track with no marks.
//
// This reads the CHAP frames and nothing else. It is deliberately not a general
// ID3 parser: metadata.c already reads the tags that name a track, and the one
// thing missing was the chapter list.
//
// It is fed files off a card, so every length in the file is treated as a claim
// and not as a fact: a frame that says it is bigger than what is left is the end
// of the parse, not a read past the buffer.
// ---------------------------------------------------------------------------

typedef struct {
	double start; // seconds from the beginning of the file
	char title[128];
} id3chap_t;

// Reads up to `max` chapters into `out`, in the order the file lists them, and
// returns how many the file has. `out` may be NULL (and `max` 0) to only ask
// whether there are any -- which is what the audiobook scan does, and why that
// case stops at the first one found.
int id3chap_read(const char *path, id3chap_t *out, int max);

// True when the file has at least one. Reads no further than it has to.
bool id3chap_present(const char *path);

#endif /* ID3CHAP_H */

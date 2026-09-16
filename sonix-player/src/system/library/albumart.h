#ifndef ALBUMART_H
#define ALBUMART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A cover image exactly as it is stored on disk / inside the tag: still
// compressed (JPEG or PNG). Decoding it into pixels is the GUI's job (see
// src/gui/cover.h), this module only finds the bytes.
typedef struct {
	uint8_t *data; // malloc'd, owned by the caller -- free with albumart_free()
	size_t size;
} albumart_t;

// The largest picture worth pulling into RAM. Eight megabytes is far more than
// any real cover -- a 4000x4000 JPEG at high quality is about four -- and the
// cap matters because this allocation happens on the loader thread while a
// track is playing: on a 64 MB device a looser limit lets one file with an
// absurd tag evict the page cache the audio decoder reads through.
#define ALBUMART_MAX_BYTES (8 * 1024 * 1024)

// How many places a cover can come from, for the candidate loaders below.
#define ALBUMART_CANDIDATES 2

// Finds the cover art for an audio file. Tries the picture embedded in the
// file's tags first (ID3v2 APIC for MP3, .dsf and AIFF, PICTURE block for FLAC,
// METADATA_BLOCK_PICTURE / COVERART for OGG and Opus, `covr` for MP4/ALAC),
// then falls back to a cover image file sitting in the same folder (cover.jpg,
// folder.jpg, front.png, ...). Returns true and fills `out` on success; `out`
// is left zeroed otherwise.
bool albumart_load_for_file(const char *filepath, albumart_t *out);

// Same, but only looks for a cover image file inside `dirpath`. Used for
// folder rows in the file browser, where there is no single track to read.
bool albumart_load_for_dir(const char *dirpath, albumart_t *out);

// The same two sources, one at a time and best first (0 = the embedded
// picture, 1 = a cover file in the folder; for a directory, 0 = the cover file,
// 1 = the first track's embedded picture). The loaders above simply walk them
// in order and stop at the first that yields bytes.
//
// A caller that decodes what it gets wants the loop instead: a picture whose
// bytes exist but cannot be decoded -- a progressive JPEG larger than the
// device's decode budget, a truncated APIC -- must not hide the perfectly good
// cover.jpg lying next to the track. Returns false when there is nothing at
// that index.
bool albumart_load_candidate(const char *filepath, int index, albumart_t *out);
bool albumart_load_dir_candidate(const char *dirpath, int index, albumart_t *out);

// Frees the image buffer and zeroes the struct. Safe to call on a zeroed one.
void albumart_free(albumart_t *art);

#endif // ALBUMART_H

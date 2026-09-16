#ifndef JPEGINFO_H
#define JPEGINFO_H

#include <stdbool.h>
#include <stddef.h>

// A JPEG's size and coding, read from its headers.
//
// Nothing is decoded: the markers before the first scan carry the dimensions
// and say whether the file is baseline or progressive, and that is all a
// caller needs to decide whether to show a picture at all. Deciding it by
// decoding would mean paying for every file that is then thrown away -- and
// the screensaver has to judge a folder of them at every wake.
//
// Progressive matters because the scaled decoder cannot read one (see
// jpeg_scaled.h): it would fall through to the unbounded path, which on this
// device is how a picture becomes an out-of-memory instead of a picture.

typedef struct {
	int width;
	int height;
	bool progressive;
} jpeg_info_t;

// Fills `out` from the first few kilobytes of `path`. False when the file
// cannot be opened, is not a JPEG, or ends before it says how big it is.
bool jpeg_info_read(const char *path, jpeg_info_t *out);

// The same from bytes already in hand.
bool jpeg_info_parse(const unsigned char *data, size_t size, jpeg_info_t *out);

// Whether a name ends in .jpg or .jpeg, either case. The only two extensions
// the screensaver folder is read for.
bool jpeg_info_has_extension(const char *name);

#endif /* JPEGINFO_H */

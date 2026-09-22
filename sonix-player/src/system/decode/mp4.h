#ifndef MP4_H
#define MP4_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// An MP4/M4A/M4B reader: the container an audiobook comes in.
//
// Written here rather than pulled in, because the two things a book needs from
// the container -- the AAC frames and the chapter marks -- are a small part of
// what a general MP4 library does, and none of the small public-domain ones
// carry chapters at all.
//
// Memory is the reason this is not the usual "read the whole moov into RAM"
// parser. A ten-hour book is about a million and a half AAC frames, and its
// sample-size table alone is six megabytes; on a player with the RAM this one
// has, holding that for the duration of a book is not on. So the big tables
// (sample sizes, chunk offsets) are left in the file and read a slice at a
// time through pread(), while only the small ones (the chunk map, the time
// map, the chapter list, the tags) are loaded. What that costs is one short
// read per chunk of audio -- a few hundred bytes every second or so.

typedef struct mp4_file mp4_file_t;

// Which of the two playable codecs the file holds. The extension does not tell
// them apart -- a .m4a can be either -- and the sample entry is the only place
// it is written down.
typedef enum {
	MP4_CODEC_NONE = 0,
	MP4_CODEC_AAC,	// sample entry `mp4a`, config in the esds
	MP4_CODEC_ALAC, // sample entry `alac`, config in the `alac` box
} mp4_codec_t;

typedef struct {
	double start; // seconds from the start of the book
	char title[192];
} mp4_chapter_t;

// Opens the file and reads its structure. NULL when it is not an MP4 at all,
// when it has no audio track, or when that track is neither AAC nor ALAC.
mp4_file_t *mp4_open(const char *path);
void mp4_close(mp4_file_t *m);

// Which decoder this file needs.
mp4_codec_t mp4_audio_codec(const mp4_file_t *m);

// Whether the file holds real video.
//
// The MP4 container is the same for an ALAC album and for a film: the
// difference is in the tracks, not the extension. True when there is a 'vide'
// track with more than one frame. The "more than one" is deliberate: cover art
// in a .m4a is sometimes written as a video track of a single frame, and that
// is artwork, not a film.
bool mp4_has_video(const mp4_file_t *m);

// The same judgement from a path, for callers deciding whether to index a file
// before opening it for tags. False when the file is not a readable MP4 at
// all: there is no video to keep out in that case.
bool mp4_file_has_video(const char *path);

// The decoder config from the sample description: AudioSpecificConfig for AAC,
// without which the first frame means nothing; ALACSpecificConfig for ALAC,
// twenty-four big-endian bytes.
const unsigned char *mp4_audio_config(const mp4_file_t *m, int *len);

int mp4_audio_channels(const mp4_file_t *m);
int mp4_audio_sample_rate(const mp4_file_t *m); // as declared; SBR may double it
double mp4_duration_seconds(const mp4_file_t *m);

// The AAC frames ("samples" in the container's own language: one frame of
// 1024 PCM samples each, typically).
uint32_t mp4_audio_frame_count(const mp4_file_t *m);
// The bitrate declared in the esds, in kbps. 0 when the file does not say.
// Only meaningful for AAC: ALAC is lossless and the number does not describe
// it.
int mp4_audio_bitrate_kbps(const mp4_file_t *m);
// Copies frame `index` into `buf`. Returns its length, 0 on failure, and a
// negative number when the frame does not fit (its size, negated).
int mp4_read_audio_frame(mp4_file_t *m, uint32_t index, unsigned char *buf, uint32_t buf_size);
// The largest frame in the file, so a caller can size its buffer once.
uint32_t mp4_max_frame_size(const mp4_file_t *m);

double mp4_frame_time(const mp4_file_t *m, uint32_t index);	  // when frame N starts
uint32_t mp4_frame_at_time(const mp4_file_t *m, double secs); // which frame covers it

// Chapters, from either of the two ways an audiobook carries them: the Nero
// `chpl` list, or the QuickTime text track the audio track points at. Sorted,
// deduplicated, and empty when the file has none.
int mp4_chapter_count(const mp4_file_t *m);
const mp4_chapter_t *mp4_chapter(const mp4_file_t *m, int index);
// Which chapter contains `secs`, or -1 when there are no chapters.
int mp4_chapter_at_time(const mp4_file_t *m, double secs);

// iTunes-style tags. Empty strings when absent, never NULL.
const char *mp4_tag_title(const mp4_file_t *m);
const char *mp4_tag_artist(const mp4_file_t *m);
const char *mp4_tag_album_artist(const mp4_file_t *m);
const char *mp4_tag_album(const mp4_file_t *m);
const char *mp4_tag_genre(const mp4_file_t *m);
int mp4_tag_year(const mp4_file_t *m);

// The value of an iTunes freeform tag -- a "----" atom, whose name sits in a
// `name` box beside the data. ReplayGain on an .m4a is written this way and
// nowhere else. NULL when the file has no such tag; `name` is matched without
// regard to case, as taggers disagree about it.
const char *mp4_tag_freeform(const mp4_file_t *m, const char *name);
int mp4_tag_track_number(const mp4_file_t *m);

// The `disk` atom's first half. 0 when the file carries no disc tag.
int mp4_tag_disc_number(const mp4_file_t *m);

// Cover art: where it is in the file and how big, so the caller can read it
// without this module allocating a megabyte it does not own. `is_png` tells
// the two encodings apart. False when the file carries no artwork.
bool mp4_cover_art(const mp4_file_t *m, uint64_t *offset, uint32_t *size, bool *is_png);

// True when the file looks like an MP4 at all (cheap: reads the first box).
bool mp4_probe(const char *path);

#endif /* MP4_H */

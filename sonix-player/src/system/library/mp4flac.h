#ifndef MP4FLAC_H
#define MP4FLAC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Extracting a real FLAC file out of MP4 segments.
//
// Tidal hi-res does not arrive as a file: it arrives as DASH, an
// initialisation segment plus a few dozen audio segments, all in fragmented
// MP4. What is inside is not AAC but FLAC -- the same frames a .flac file
// would hold, byte for byte, only wrapped.
//
// The player's decoders open a .flac or an .m4a; none of them opens a
// fragmented MP4 with FLAC inside. Rather than put a demuxer inside libFLAC or
// add a general one (ffmpeg is not on this device and is too large to add), the
// stream is unwrapped while downloading into a .flac the usual decoder opens
// knowing nothing about any of this.
//
// That is small, because the FLAC inside an MP4 is not transformed. Compared
// with a real .flac file only two things are missing:
//
//   * the four-byte "fLaC" signature at the head;
//   * the STREAMINFO block, which in the MP4 sits in a `dfLa` box inside the
//     sample description.
//
// So: read the init segment, pull out STREAMINFO, write signature +
// STREAMINFO, then copy the segments' `mdat` payloads one after another. The
// result opens anywhere.
//
// AAC works through the same path with no extra effort. When the codec is not
// FLAC (the low qualities, when they arrive over DASH) nothing is unwrapped:
// the init segment is written followed by all the others, unchanged. That is
// already a valid fragmented MP4, and it is saved with a .m4a extension. Hence
// mp4flac_begin(): it decides once, at the start, which of the two it is
// doing, and behaves accordingly from then on.

typedef struct {
	FILE *out;

	// True while unwrapping FLAC; false while merely concatenating (AAC and
	// anything else).
	bool unwrap;

	// Bytes written so far. The progress bar and growfile need it: the file
	// that grows is this one, not the sum of the downloaded segments.
	long written;

	// How many of those bytes are header only, that is how many there were
	// before the first sample arrived. The downloader compares it with
	// `written` at the end: if they are still equal no audio arrived, and a
	// header-only file must not be marked complete -- it would stay in the
	// cache forever and the track would never be fetched again.
	long header_bytes;

	// How much audio went through, for the STREAMINFO a fragmented file leaves
	// at zero (its sample tables are empty; the length is spread over the
	// fragments). Counted from the sample tables of a whole file and from each
	// fragment's `trun`, in the track's timescale; see mp4flac_total_samples().
	uint64_t duration;
	uint32_t timescale;		   // from `mdhd`, 0 when not found
	uint32_t default_duration; // from `trex`: a fragment that does not say
	uint32_t sample_rate;	   // from the STREAMINFO
} mp4flac_t;

// The extension the file will take, from the codec declared in the manifest
// ("flac", "mp4a.40.2", ...). Needed to name the file before the first byte is
// downloaded.
const char *mp4flac_extension_for(const char *codecs);

// Starts, from the init segment already in memory.
//
// `codecs` comes from the manifest and decides unwrap versus concatenate. On
// success the file already holds what it needs (the FLAC header, or the init
// segment verbatim) and segments can be fed in.
//
// `out` stays the caller's: this module never closes it.
bool mp4flac_begin(mp4flac_t *m, FILE *out, const char *codecs, const uint8_t *init, size_t init_size);

// One audio segment. When unwrapping, the `mdat` contents come out; otherwise
// the whole segment. False when the write failed (card full).
bool mp4flac_segment(mp4flac_t *m, const uint8_t *data, size_t size);

// Audio bytes that are already FLAC frames, written as they are: the payload
// of an `mdat` streamed straight through, for a file that is not fragmented
// (moov first, then one mdat) and so never comes in segments.
bool mp4flac_payload(mp4flac_t *m, const uint8_t *data, size_t size);

// The length of what went through, in samples at the STREAMINFO rate; 0 when
// it could not be told.
uint64_t mp4flac_total_samples(const mp4flac_t *m);

// Writes `samples` into the STREAMINFO of the finished .flac at `path` when it
// says 0 (unknown) there, so players and the library scan show a length and
// can seek by it. Five bytes rewritten in place; nothing else is touched.
bool mp4flac_patch_total_samples(const char *path, uint64_t samples);

#endif /* MP4FLAC_H */

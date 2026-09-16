#ifndef ALAC_DECODE_H
#define ALAC_DECODE_H

#include <stdbool.h>
#include <stdint.h>

// ALAC (Apple Lossless) inside a .m4a.
//
// The container is the same one audiobooks use and mp4.c already reads it: a
// .m4a can hold AAC or ALAC, and only the sample entry says which. So there is
// no demuxer here -- only the decoder, which decode.c feeds one access unit at
// a time exactly as it does for AAC.
//
// The decoder itself is David Hammerton's, in alac/: see alac/README.md for
// provenance, licence and the two local changes.
//
// Covers 16 and 24 bit, mono and stereo. A 20- or 32-bit file, or one with more
// than two channels, is rejected at open with a log line instead of producing
// silence.

typedef struct alacdec alacdec_t;

// `cookie` is the body of the sample entry's `alac` box without its first four
// version and flag bytes, that is the ALACSpecificConfig: 24 big-endian bytes.
// NULL when the config is unusable.
alacdec_t *alacdec_open(const unsigned char *cookie, int cookie_len);
void alacdec_close(alacdec_t *a);

int alacdec_channels(const alacdec_t *a);
int alacdec_sample_rate(const alacdec_t *a);
int alacdec_bits(const alacdec_t *a);			 // 16 or 24
int alacdec_frame_samples(const alacdec_t *a); // PCM frames per access unit, at most

// Decodes one access unit. Returns the PCM frames produced, 0 when the frame
// yielded nothing. Output is interleaved and left-justified in 32 bits, like
// dr_flac and libsndfile, so a 24-bit ALAC reaches the DAC with all
// twenty-four of its bits.
int alacdec_decode(alacdec_t *a, const unsigned char *au, int au_len, int32_t *out, int out_frames);

#endif /* ALAC_DECODE_H */

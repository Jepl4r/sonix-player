#ifndef SNDFILE_DECODE_H
#define SNDFILE_DECODE_H

#include <stdbool.h>
#include <stdint.h>

// The formats the firmware's own libsndfile can read, borrowed the same way
// libfdk-aac is (see aacdec.h): opened at run time by name, and simply absent
// on a device that does not have it.
//
// The extensions routed here (see sndfile_handles):
//
//     .aif .aiff .aifc   AIFF and AIFF-C, the lossless format a Mac records to
//     .caf               Core Audio
//
// libsndfile reads more than that -- Wave64, RF64, Sun/NeXT -- but those are
// not claimed, since every extra extension costs a scan in every folder.
//
// ALAC inside a CAF may well play, since the firmware's copy lists "CAF (Apple
// 16 bit ALAC)" among its formats, but it is not claimed. A file libsndfile
// will not take fails to open and says why in the log.
//
// WAV is not sent here: audio.c plays it itself.
//
// What it does not bring: ALAC inside an .m4a. libsndfile decodes ALAC only in
// a CAF wrapper, and its decoder is internal -- no symbol for it is exported.
// That case is handled by alacdec.c instead.

typedef struct sndfile sndfile_t;

// True when the library was found. The first call loads it; later ones are
// free.
bool sndfile_available(void);

// Why it is not available, for the log.
const char *sndfile_last_error(void);

// Whether this extension is one handed to libsndfile.
bool sndfile_handles(const char *filepath);

sndfile_t *sndfile_open(const char *filepath);
void sndfile_close(sndfile_t *s);

int sndfile_channels(const sndfile_t *s);
int sndfile_sample_rate(const sndfile_t *s);
int sndfile_bits(const sndfile_t *s); // 16, 24 or 32 -- what the file holds
uint64_t sndfile_total_frames(const sndfile_t *s);

// Interleaved frames. The 32-bit form is left-justified, like every other
// decoder here, so a 24-bit AIFF reaches the DAC with all 24 bits.
uint64_t sndfile_read_s16(sndfile_t *s, uint64_t frames, short *out);
uint64_t sndfile_read_s32(sndfile_t *s, uint64_t frames, int32_t *out);

bool sndfile_seek(sndfile_t *s, uint64_t frame);

#endif /* SNDFILE_DECODE_H */

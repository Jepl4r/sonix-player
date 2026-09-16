#ifndef OPUS_DECODE_H
#define OPUS_DECODE_H

#include <stdbool.h>
#include <stdint.h>

// Opus in an Ogg container: .opus files.
//
// Unlike AAC and libsndfile, this library is not borrowed from the firmware.
// The device does carry a libopus.so, but that is the bare codec: it decodes an
// Opus packet and nothing else. A .opus file is Opus inside Ogg, and there is
// no Ogg demuxer there.
//
// So libogg, libopus and opusfile are built statically into the binary by the
// Makefile, as FreeType already is. Besides the demuxer, that means no
// dependency on what the firmware happens to expose, and the version is chosen
// here.
//
// Opus always decodes at 48 kHz whatever the file says: that is the codec, not
// a choice made here. It is lossy, so the source counts as 16 bit like mp3 and
// Vorbis.

typedef struct opusdec opusdec_t;

// NULL when the file does not open or is not valid Opus; the reason goes to the
// log.
opusdec_t *opusdec_open(const char *filepath);
void opusdec_close(opusdec_t *o);

int opusdec_channels(const opusdec_t *o);
int opusdec_sample_rate(const opusdec_t *o); // always 48000
uint64_t opusdec_total_frames(const opusdec_t *o);

// Interleaved 16-bit frames. Returns how many were actually written; 0 means
// end of stream.
uint64_t opusdec_read_s16(opusdec_t *o, uint64_t frames, short *out);

bool opusdec_seek(opusdec_t *o, uint64_t frame);

#endif /* OPUS_DECODE_H */

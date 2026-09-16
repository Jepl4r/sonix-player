#ifndef AACDEC_H
#define AACDEC_H

#include <stdbool.h>

// AAC decoding, through the device's own libfdk-aac.
//
// The library is not linked in: the firmware already carries
// /usr/lib/libfdk-aac.so.2 (bluealsa uses it for the Bluetooth AAC codec), so
// it is opened at run time with dlopen() -- the same arrangement as OpenSSL in
// tls.c.
//
// Everything here is a no-op when the library is missing, so a build without
// it still runs; only .m4b files stop opening.

typedef struct aacdec aacdec_t;

// Whether the library could be loaded at all. Cheap after the first call.
bool aacdec_available(void);

// Opens a decoder configured from an AudioSpecificConfig (the `esds` payload
// out of the MP4). NULL when the library is missing or the config is rejected.
aacdec_t *aacdec_open(const unsigned char *asc, int asc_len);

// Opens a decoder for raw ADTS AAC -- the continuous stream with a sync header
// in front of every frame, which is how AAC arrives inside a radio's HLS.
//
// No AudioSpecificConfig to pass: in ADTS the rate, channels and profile sit in
// every header, and the decoder configures itself from the first frame. That
// holds when they change mid-stream too, which on a radio happens between
// programmes.
//
// Used with aacdec_fill()/aacdec_pull() rather than aacdec_decode(): there the
// container knows the frame boundaries, here the decoder finds them in the
// stream.
aacdec_t *aacdec_open_adts(void);

// Feeds the decoder. Returns how many bytes it took (the caller must drop those
// from its buffer and re-present the rest), or -1 when it refused.
int aacdec_fill(aacdec_t *d, const unsigned char *data, int len);

// Pulls one frame out of what it has buffered. 0 means "feed me more", not "end
// of stream". Returns the PCM frames written.
int aacdec_pull(aacdec_t *d, short *out, int out_samples);

void aacdec_close(aacdec_t *d);

// Decodes one access unit into interleaved 16-bit PCM. `out_samples` is the
// room in `out` counted in samples, not frames. Returns the number of PCM
// frames written (0 is normal for the first frame or two after a seek, and
// for a frame the decoder swallowed), or a negative number on a hard error.
int aacdec_decode(aacdec_t *d, const unsigned char *au, int au_len, short *out, int out_samples);

// What the decoder is actually producing, which is not always what the
// container said: HE-AAC hands back twice the rate in the sample description,
// and parametric stereo hands back two channels for a mono core. Valid only
// once a frame has come out.
int aacdec_channels(const aacdec_t *d);
int aacdec_sample_rate(const aacdec_t *d);
int aacdec_frame_size(const aacdec_t *d); // PCM frames per access unit

// Drops whatever is buffered. Called after a seek, before feeding frames from
// the new position.
void aacdec_flush(aacdec_t *d);

// Why the last open failed, in Italian, for the interface to show.
const char *aacdec_last_error(void);

#endif /* AACDEC_H */

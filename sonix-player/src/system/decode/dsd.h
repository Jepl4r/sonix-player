#ifndef DSD_H
#define DSD_H

#include <stdbool.h>
#include <stdint.h>

// DSD: .dsf and .dff, up to DSD256.
//
// DSD is not samples, it is a one-bit stream at a few megahertz -- 2.8224 MHz
// for DSD64, twice that for DSD128, four times for DSD256. Getting it to the
// ears takes one of two routes, and this device wants the first:
//
//   * DoP (DSD over PCM). The bits are packed sixteen at a time into ordinary
//     24-bit PCM frames with a marker byte on top, sent at a sixteenth of the
//     DSD rate, and the DAC recognises the marker and switches itself into
//     DSD. This is what the hardware here is built for: the sound card driver
//     (x1600_hiby_r3proii_sound_card.ko) carries a mixer control called
//     DOP_EN whose handler calls cs43198_set_dsd_en() straight into the codec.
//     Nothing may touch the samples on the way -- a volume change or an EQ
//     band would destroy the markers and the DAC would fall back to hearing
//     white noise.
//
//   * Conversion to PCM. A low-pass filter and decimation down to a normal
//     rate, after which it is a track like any other. Needed when the output
//     cannot take the DoP rate -- DSD256 over DoP is 705.6 kHz -- and useful
//     when something in the chain (the equaliser, the balance) is wanted.
//
// Which one is used is a setting; DoP falls back to conversion by itself when
// the PCM device refuses the rate.

typedef enum {
	DSD_OUT_DOP = 0, // bit-exact, the DAC does the work
	DSD_OUT_PCM = 1, // filtered and decimated here
} dsd_output_t;

// The rate everything is converted to when it is converted: a hi-res rate the
// DAC certainly takes, and the same one DoP asks for at DSD64.
#define DSD_PCM_RATE 176400

typedef struct dsd_file dsd_file_t;

// Opens a .dsf or .dff. NULL when it is neither, when the rate is not one of
// the three, or when the file has more channels than can be played.
dsd_file_t *dsd_open(const char *path, dsd_output_t mode);
void dsd_close(dsd_file_t *d);

int dsd_channels(const dsd_file_t *d);
uint32_t dsd_rate(const dsd_file_t *d);	   // the DSD rate itself (2822400, ...)
int dsd_multiple(const dsd_file_t *d);	   // 64, 128 or 256
int dsd_output_rate(const dsd_file_t *d);  // what comes out of dsd_read()
bool dsd_is_dop(const dsd_file_t *d);
uint64_t dsd_total_frames(const dsd_file_t *d); // in output frames

// Reads interleaved 32-bit frames: DoP words left-justified in 32 bits when
// the mode is DoP, ordinary left-justified PCM otherwise. Returns frames read,
// 0 at the end.
uint64_t dsd_read(dsd_file_t *d, uint64_t frames, int32_t *out);

// Seeks to an output frame. The filter state is dropped, which is right: it
// belongs to the part of the stream being left.
bool dsd_seek(dsd_file_t *d, uint64_t frame);

#endif /* DSD_H */

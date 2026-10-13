#ifndef BTRATE_H
#define BTRATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The rate the Bluetooth link runs at, as a policy: which rate to ask the
// headphones for, given what the user chose on the Bluetooth audio page, the
// codec in force, the track about to play and the rates both ends can do.
//
// Only decisions live here -- no bus, no lock, no state -- so the whole of it
// can be checked on the desktop (tests/btrate_test.c). bluetooth.c asks the
// headphones; audio.c says when (between tracks, never during one).
//
// Any thread.

// [wireless] bt_sample_rate, as the page offers it.
//
//   auto     bluealsa's own pick: the highest rate up to 48 kHz both ends can
//            do, which is 48 kHz for nearly every pair of headphones
//   44100    the CD rate, by a start-up option of bluealsa
//            (--a2dp-force-audio-cd); a 44.1 kHz track goes out untouched
//   follow   the rate of each track's family, changed between tracks
//   96000    LDAC at 96 kHz (or the highest the pair takes below it)
typedef enum {
	BT_RATE_AUTO = 0,
	BT_RATE_44K,
	BT_RATE_FOLLOW,
	BT_RATE_96K,
} bt_rate_mode_t;

// Whether `rate` is one of `rates[0..count)`.
bool btrate_supported(unsigned rate, const unsigned *rates, int count);

// The highest of `rates` not above `limit`; 0 when there is none.
unsigned btrate_highest_upto(unsigned limit, const unsigned *rates, int count);

// "Follow the music": the link rate for a track at `track_rate`.
//
// Rates come in two families, multiples of 44.1 kHz and of 48 kHz, and only
// within a family does a track reach the link by an integer step -- untouched,
// or halved by the decimator (decimate.c) -- instead of through alsa's linear
// converter. So the link takes the track's family: 44.1 for a 44.1 kHz track,
// 88.2 for 88.2 and 176.4 (and DSD, which is a 44.1 kHz family rate) when the
// pair can do 88.2; the same with 48 and 96 for the other family. A family
// whose base rate the pair cannot do at all gives 0: the link is left alone.
unsigned btrate_follow(unsigned track_rate, bool dsd, const unsigned *rates, int count);

// The rate to ask for under `mode`, or 0 to leave the link as bluealsa chose
// it. `codec` is bluealsa's name for the codec in force ("LDAC", "AAC", ...);
// `track_rate` 0 when no track is concerned (a connection, a change of the
// setting). "96 kHz" is an LDAC choice: on any other codec it is "Auto".
unsigned btrate_wanted(bt_rate_mode_t mode, const char *codec, unsigned track_rate, bool dsd, const unsigned *rates,
					   int count);

// The configuration blob SelectCodec takes for LDAC ("Configuration", ay):
// bluez-alsa's a2dp_ldac_t (shared/a2dp-codecs.h), which is the A2DP codec
// information element as it travels:
//
//   0..3  vendor id 0x0000012D (Sony), little-endian
//   4..5  codec id 0x00AA, little-endian
//   6     sampling frequency, one bit in the low six:
//         0x20 44.1, 0x10 48, 0x08 88.2, 0x04 96, 0x02 176.4, 0x01 192 kHz
//   7     channel mode in the low three bits: 0x01 stereo
//
// Writes the blob for `rate`, stereo, and returns its size; 0 for a rate LDAC
// does not have.
#define BTRATE_LDAC_CONFIG_SIZE 8
size_t btrate_ldac_config(unsigned rate, uint8_t out[BTRATE_LDAC_CONFIG_SIZE]);

#endif /* BTRATE_H */

#ifndef SWVOLUME_H
#define SWVOLUME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The SW/HDB curve out of the stock ot_devices.json, applied to the samples
// themselves, for the one route that has nowhere else to put the volume: a
// device on the USB-C port with no volume control of its own.
//
// It is NOT part of the volume law on the jacks. There the CS43198's own
// attenuation is the whole of it, and the hardware tables say so on their own:
// each steps 0.5 dB per index from 100 down to 45, 1 dB down to 10, then 2 and
// 5 -- a complete taper, fine at the top and coarse at the bottom -- and MDB
// sits exactly 6.0 dB under HDB at every single index. Adding this curve on
// top gives 0.8 dB per index over the first stretch and 1.3, 1.5, 1.7, 2.2,
// 3.4 below it, which is neither the stock scale nor an even one.
//
// The curve, and the arithmetic, are HiBy's (routine 0x730c20 in the stock
// binary): 101 attenuations in tenths of a dB, turned into a linear gain by
//
//     gain = 10 ^ (SW[index] / 200)
//
// and then into a Q31 fixed-point coefficient by multiplying by 2^31. The
// division by 200 rather than 20 is the tenths: SW[index] = dB x 10.
//
// The stock engine also carries an offset per hardware table -- LDB -12 dB,
// MDB -6 dB, HDB 0 -- under SW_MIX_GAIN_ENABLE. It is not applied here and
// there is nowhere it could be: it is the software share of the difference
// between two CONVERTER tables, and this curve only ever runs where the
// converter is out of the circuit and the gain switch moves nothing.

// The UI volume index, 0..100. Cheap -- it stores an int; the coefficients were
// worked out once at startup.
void swvolume_set_index(int percent);

// Whether the attenuation is being applied to the stream right now. True only
// on the USB-C port to a device with no volume control of its own: on the jacks
// the converter holds the level, over Bluetooth the headphones do, and a USB
// device with a control of its own is written directly.
bool swvolume_active(void);

// The attenuation for an index, in tenths of a dB, straight off the curve.
int swvolume_millibel(int percent);

// The Q31 coefficient for an index: the number the stock binary computes. Index
// 0 is 0 -- silence, and not the -150 dB entry the curve carries there.
int64_t swvolume_coefficient(int percent);

// The scaling itself, applied in place to interleaved samples. `count` is
// samples, not frames.
//
// One per PCM format, and the caller has to pick by the format the device was
// opened with rather than by the sample width: 24-bit is S24_3LE, three bytes
// per sample with no padding, and handing that buffer to the 32-bit routine
// reads across sample boundaries and writes past the last frame. 8-bit is
// unsigned, with silence at 128 rather than 0.
//
// All four return at once when the software volume is not in circuit, and fill
// the buffer with silence at index 0.
void swvolume_apply_u8(unsigned char *samples, int count);
void swvolume_apply_s16(short *samples, int count);
void swvolume_apply_s24_3le(unsigned char *samples, int count);
void swvolume_apply_s32(int32_t *samples, int count);

#endif /* SWVOLUME_H */

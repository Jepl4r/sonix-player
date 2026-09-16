#ifndef SWVOLUME_H
#define SWVOLUME_H

#include <stdbool.h>
#include <stdint.h>

// The stock player's software volume: the SW/HDB curve out of ot_devices.json,
// applied to the samples themselves.
//
// It is not an alternative to the CS43198's attenuation but the other half of
// it. The stock configuration says so in its first line -- "SET_HW_SW_VOL_BOTH":
// 1 -- and the two tables confirm it: added together they step exactly 0.8 dB
// per index from 100 down to 45, which neither of them does alone. The volume
// law of this player is a converter register and a multiply on the stream.
//
// The curve, and the arithmetic, are HiBy's (routine 0x730c20 in the stock
// binary): 101 attenuations in tenths of a dB, turned into a linear gain by
//
//     gain = 10 ^ (SW[index] / 200)
//
// and then into a Q31 fixed-point coefficient by multiplying by 2^31. The
// division by 200 rather than 20 is the tenths: SW[index] = dB x 10.
//
// It also answers the case the converter cannot reach at all: playback leaving
// as USB packets to a headset or a DAC on the USB-C port, where that register
// is out of the circuit and nothing is left between the file and the plug.

// The UI volume index, 0..100. Cheap -- it stores an int; the coefficients were
// worked out once at startup.
void swvolume_set_index(int percent);

// Whether the attenuation is being applied to the stream right now. False over
// Bluetooth, where the headphones hold the level, and on a USB device that has
// a volume control of its own.
bool swvolume_active(void);

// Takes the software half out of the volume law, leaving the converter's
// register alone on the samples. It is a trade, not a free "more bit-perfect"
// switch: the hardware curve alone is irregular -- 0.5 dB between one pair of
// indices and 1 dB between the next -- so the scale stops being even and the
// same number on the dial means a different level.
//
// It has no effect on the one path where this curve is all there is: a USB
// device with no volume control of its own has no converter register behind it,
// and switching the attenuation off there would leave the stream at full scale
// with the volume keys doing nothing at all.
void swvolume_set_disabled(bool disabled);
bool swvolume_disabled(void);

// The attenuation for an index, in tenths of a dB, straight off the curve.
int swvolume_millibel(int percent);

// The Q31 coefficient for an index: the number the stock binary computes.
int64_t swvolume_coefficient(int percent);

// The scaling itself, applied in place to interleaved samples. `count` is
// samples, not frames. Both return at once when the software volume is not in
// circuit or the index is 100, so the call costs nothing on the ordinary path.
void swvolume_apply_s16(short *samples, int count);
void swvolume_apply_s32(int32_t *samples, int count);

#endif /* SWVOLUME_H */

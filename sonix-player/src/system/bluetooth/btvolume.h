#ifndef BTVOLUME_H
#define BTVOLUME_H

#include <stdbool.h>

// The bridge between the player's volume and the volume of a pair of Bluetooth
// headphones, over bluealsa's D-Bus interface.
//
// What is on the other side. bluealsa exports one object per stream:
//
//     /org/bluealsa/hci0/dev_AA_BB_CC_DD_EE_FF/a2dpsrc/sink
//
// with two writable properties on org.bluealsa.PCM1 that decide everything
// here. Volume is a 16-bit word -- the left channel in the upper byte, the
// right in the lower, the top bit of each a mute switch and the remaining seven
// a level from 0 to 127. SoftVolume says who applies it:
//
//   SoftVolume = false  bluealsa passes the level on as an AVRCP absolute
//                       volume, so it lands on the headphones themselves and
//                       whatever they do to their own volume comes back;
//   SoftVolume = true   bluealsa attenuates the stream itself and the
//                       headphones never hear about it, so the two levels are
//                       independent gain stages.
//
// Those are exactly the two halves of the "synchronised volume" setting, which
// is why this module has no policy of its own beyond them: synchronised means
// native volume and following what arrives back, separate means software
// attenuation and ignoring it.
//
// Why D-Bus and not bluealsa-cli. Following the headphones through the command
// line means asking every second and a half for as long as the music plays,
// which is a fork and an exec each time on a device with one slow core. The bus
// says so itself, through PropertiesChanged, the moment it happens. The command
// line stays as the fallback for writing (see bluetooth.c) in case this path is
// not there at all.
//
// Threading: one worker owns every call, so no other thread ever blocks on the
// bus. Signals arrive on the dbuslite reader thread and are only recorded.

// Starts the worker. It connects only while there is a stream to talk about,
// and retries quietly.
void btvolume_init(void);

// Stops the worker and closes the connection.
void btvolume_stop(void);

// The stream to bridge, as the address of the connected device (":" or "-"
// separated, either way), or NULL when there is none. Cheap to call on every
// round of a poll: the same address twice does nothing.
void btvolume_set_device(const char *mac);

// Whether the volume is synchronised with the headphones' own.
void btvolume_set_sync(bool on);

// The player's volume has moved and the stream has to follow. Returns at once;
// the write happens on the worker.
void btvolume_notify_local(int percent);

// Whether the bus path is up and can carry a volume. False means the caller
// should fall back to bluealsa-cli.
bool btvolume_available(void);

#endif /* BTVOLUME_H */

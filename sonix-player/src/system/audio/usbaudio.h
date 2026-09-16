#ifndef USBAUDIO_H
#define USBAUDIO_H

#include <stdbool.h>
#include <stddef.h>

// Audio out of the USB-C port: a DAC or a pair of USB-C headphones, driven as
// a USB Audio Class device with this player as the host.
//
// The port comes up as a sink and a device, and in that state it never sees a
// peripheral at all -- no Type-C partner, no role change, nothing on the bus.
// One write changes that:
//
//     echo dual > /sys/class/typec/port0/port_type
//
// With that, the port toggles its CC pull-ups instead of only ever offering
// Rd, sees the headphones, becomes source and host, and the kernel enumerates
// them. Writing "host" to data_role is refused -- a role swap is not something
// userspace declares -- and is not needed: the port swaps by itself once it is
// allowed to be dual.
//
// So the whole feature is: allow dual role at startup, watch for a sound card
// that belongs to the USB bus, and point playback at it while it is there.

// Allows the port to take a peripheral. Returns whether the write was accepted.
// Safe when the port is already dual, and harmless where the attribute does not
// exist. Charging and the USB DAC mode are unaffected: a charger and a PC both
// present as sources, so the port still settles as sink and device with them.
bool usbaudio_init(void);

// The same write, made again whenever the port is found not to be dual. The
// setting does not stay put on its own -- see the .c -- so the poll re-asserts
// it rather than trusting the one at startup.
bool usbaudio_ensure_dual(void);

// Looks for a USB sound card and points playback at it, or takes playback back
// when it goes. Cheap: one readdir of /sys/class/sound, and it only acts when
// what it finds has changed. Called from the same once-a-second poll that
// watches the headphone jacks.
void usbaudio_poll(void);

// Whether the sound is going out over the port right now.
bool usbaudio_active(void);

// The card's name ("Headset", "USB Audio DAC"...), empty when there is none.
void usbaudio_card_name(char *out, size_t out_size);

// The volume, on the USB device's own control. The CS43198's attenuation is
// not in the path any more once the audio leaves as USB packets, so the
// built-in volume does nothing and this is what has to move instead. No-op
// when nothing is attached, or when the device exposes no volume control --
// some do not, and then the level belongs to whatever is on the far end.
void usbaudio_apply_volume(int percent);

// Whether the device on the port has such a control. False also when the port
// is empty. When it is false and the port is busy, nothing in the chain holds
// the level any more and the software volume takes it over -- see swvolume.c.
bool usbaudio_has_volume_control(void);

#endif /* USBAUDIO_H */

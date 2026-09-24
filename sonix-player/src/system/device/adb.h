#ifndef ADB_H
#define ADB_H

#include <stdbool.h>

// ADB on/off.
//
// On this firmware ADB is not started by the boot sequence: /etc/init.d/rcS
// only runs the S?? scripts, and the adb service is a T?? one. The stock player
// starts it itself when the USB mode calls for it. A replacement player has to
// do the same, or the device has no way in at all.
//
// Both entry points from the firmware are used, picked the way its own T90adb
// script picks them: the configfs gadget (S440adb) or the older android_usb
// one (S310adb).

// True if adbd is currently running.
bool adb_is_running(void);

// Starts or stops the service and remembers the choice in the config, so the
// next boot comes up the same way. Returns whether the request was launched --
// not whether adbd has finished starting, which takes a moment.
bool adb_set_enabled(bool enabled);

// Whether the switch is on: set by the two functions around it, so it says
// what the user asked for, not whether adbd has come up yet. Safe from any
// thread.
bool adb_switched_on(void);

// Applies the remembered choice. Call once at startup, after config_init().
void adb_apply_saved_state(void);

#endif /* ADB_H */

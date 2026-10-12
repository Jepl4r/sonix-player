#ifndef EQPROFILE_H
#define EQPROFILE_H

#include <stdbool.h>

// EQ per output: the equaliser state each output was last used with, put back
// when the sound moves to it.
//
// The use it exists for: headphones with an equaliser of their own (a
// Soundcore Space One Pro) want the player's off, while wired IEMs want their
// AutoEq PEQ on -- and switching by hand every time the headphones connect is
// the kind of chore nobody keeps up. With the option on, whatever the user sets
// while listening on an output is what that output gets the next time.
//
// What is remembered per output: graphic EQ on/off and its ten bands, PEQ
// on/off and the name of the PEQ preset the bands came from, MSEB on/off. The
// PEQ bands themselves are not copied -- they are the preset, reloaded from
// <card>/PEQ by name. A PEQ edited by hand has no name (eq.h,
// peq_current_preset), so for that output only the on/off is switched and the
// bands are left as they are.
//
// Outputs: the 3.5 mm socket, the 4.4 mm one (R3 Pro II), USB-C, Bluetooth in
// general, and each pair of Bluetooth headphones by address. A pair with
// nothing of its own yet starts from the last state used on any Bluetooth
// output.
//
// Off by default, which is the behaviour before it existed: one EQ state for
// everything. Kept in [eq] per_output, with the states in [eq] po_<output> and
// pn_<output>, and the output whose state is the live one in [eq] po_live.
//
// UI thread only: it calls the eq.c setters, which are UI-thread functions.

// The output keys eqprofile_output_changed() takes.
#define EQPROFILE_PHONES "phones"
#define EQPROFILE_BALANCED "balanced"
#define EQPROFILE_USB "usb"
#define EQPROFILE_BLUETOOTH "bt" // and "bt" + 12 hex digits for one pair

bool eqprofile_enabled(void);
void eqprofile_set_enabled(bool on);

// Where the sound goes now. Cheap when nothing changed (a string compare), so it
// can be called on every routing tick. `mac` may be NULL or "" for Bluetooth
// whose address is not known.
void eqprofile_output_changed(const char *output, const char *mac);

#endif

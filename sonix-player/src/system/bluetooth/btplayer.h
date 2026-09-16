#ifndef BTPLAYER_H
#define BTPLAYER_H

#include <stdbool.h>

// The media player this device shows to bluez, so a pair of headphones can
// learn what is being played.
//
// Why it has to exist. AVRCP has two halves. One is the headphones sending
// transport commands, which arrive as key events on a /dev/input node bluez
// builds. The other is this device telling the headphones what it is doing:
// bluez answers a controller's EVENT_PLAYBACK_STATUS_CHANGED subscription from
// a registered media player object, and with no such object it answers STOPPED
// for ever. The headphones then believe nothing is playing and send PLAY at
// every tap, including the taps meant to pause.
//
// So this registers an MPRIS player with bluez over the system bus:
//
//     org.bluez.Media1.RegisterPlayer(/org/mpris/MediaPlayer2, {...})
//
// and serves that object -- org.mpris.MediaPlayer2.Player and
// org.freedesktop.DBus.Properties -- keeping PlaybackStatus and Metadata in
// step with what the player is really doing, and emitting PropertiesChanged
// when they move. bluez turns those into AVRCP notifications.
//
// The transport commands can then also arrive as method calls on this object
// (Play, Pause, Next...), so both roads lead to the same place: gui_notify_key,
// which is what the /dev/input reader already uses.
//
// The D-Bus itself is dbuslite.h, written here because libdbus is not in the
// toolchain's sysroot.

// Starts the worker that connects, registers and keeps the state in step.
// Returns immediately; everything happens on its own thread, and it retries
// quietly for as long as Bluetooth is switched on.
void btplayer_init(void);

// Unregisters and closes the connection. Safe to call when nothing was ever
// registered.
void btplayer_stop(void);

// Records that a transport command is being carried out now, and says whether
// it is the duplicate of one already carried out a moment ago.
//
// AVRCP can reach this player twice for a single tap: as a key event on the
// /dev/input node bluez builds, and as a method call on the player registered
// here. Which of the two bluez uses -- or whether it uses both -- depends on
// its version and on whether a player is registered at all, and acting on both
// would toggle playback twice, which from a finger's point of view is not
// acting at all.
//
// So both roads ask here first, and whichever arrives first wins. The window is
// short on purpose: two copies of one gesture arrive milliseconds apart, while
// a second deliberate press is a fifth of a second away at the very least.
bool btplayer_command_is_duplicate(void);

// Whether bluez currently holds this player's registration. What the log and a
// report; nothing depends on it.
bool btplayer_registered(void);

#endif /* BTPLAYER_H */

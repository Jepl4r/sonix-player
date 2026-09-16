#ifndef SYSSERVER_H
#define SYSSERVER_H

#include <stdbool.h>
#include <stddef.h>

// Client for HiBy OS's own system daemon.
//
// On the R3 Pro II nothing in the boot sequence mounts the microSD card:
// /etc/mdev.conf has rules for USB mass storage (sd[a-z]) but none for mmcblk,
// and sys_server -- started by /etc/init.d/S50sys_server, independently of the
// UI -- never scans for cards on its own. It is a request/response server
// listening on the UNIX socket /var/run/sys_server, and the thing that sends it
// the requests is the stock hiby_player: the player binary is also the hotplug
// manager (it reads kernel uevents, sees DEVNAME=mmcblk0p1, and answers with
// "MOUNT:MOUNT:<device> <mount point>").
//
// That is why the card only appears while the stock UI is running -- and why a
// player started in its place sees an empty mount point.
//
// Going through sys_server instead of mounting directly has two concrete
// advantages: it runs busybox `mount -t vfat,exfat,ntfs`, which can use the
// userspace/FUSE helpers for exFAT and NTFS that a raw mount(2) syscall cannot
// reach, and it does the fork/exec in its own process rather than the player's,
// which matters on a device with ~10MB free.
//
// The same daemon owns the two radios: it is what runs wifi_on.sh, wpa_cli and
// the bluez-tools binaries, and what writes the result files the UI reads
// (/data/wifi_result.txt, /data/bt_list.txt and friends). src/system/wifi.c
// and src/system/bluetooth.c are its two other callers.
//
// All calls fail fast and harmlessly when the socket isn't there (host build,
// or a firmware without sys_server), so the caller can always fall back to
// doing the work itself.

// True if the sys_server socket exists and accepts a connection.
bool sysserver_available(void);

// Sends one command and waits for the reply. Returns 0 when the daemon
// accepted the command -- which is all its answer means, see
// sysserver_reply_ok() below. For anything whose outcome matters (pairing,
// connecting) the real verdict arrives later on the notification socket.
//
// `reply` may be NULL when the caller only cares whether it was accepted.
// Blocks for up to a couple of seconds -- never call it from the interface
// thread; the radios each have a worker thread for exactly this reason.
int sysserver_request(const char *command, char *reply, size_t reply_size);

// True when a reply means "accepted". sys_server answers requests with a bare
// "OK" or "FAIL"; the "<COMMAND>:OK" form is what it pushes to the
// notification socket afterwards. Both are accepted here.
bool sysserver_reply_ok(const char *reply);

// The same, with the socket timeout raised. The default is two seconds, which
// is right for mounting and far too short for the radios: the daemon runs
// bt-adapter/bt-device/wpa_cli through a blocking system(), so a scan takes ten
// seconds and a pairing can take thirty.
int sysserver_request_timeout(const char *command, char *reply, size_t reply_size, int timeout_ms);

// ---------------------------------------------------------------------------
// Text of the user's, on its way into the daemon's shell
//
// sys_server pastes its arguments into a command line and runs it through
// system(), so anything a user typed reaches a shell. Read out of the daemon's
// own format strings:
//
//     dmrd  -f "%s" &                        the DLNA name, inside real quotes
//     /usr/bin/shairport_on.sh <name>        the AirPlay name, bare
//     wpa_cli -i %s set_network %s ssid \"%s\"   a network name, inside quotes
//                                                that are LITERAL, not quoting
//
// The third one truncates a network called "carrier pigeon #6298" to "carrier
// pigeon": the backslashes make those quotes ordinary characters, the # follows
// a space, and a shell reads that as the start of a comment.
//
// The two below repair a string so the shell hands it on whole, each for the
// place it goes: inside real double quotes only four characters are still read
// by the shell, while a bare argument is at the mercy of all of them. What
// cannot be carried becomes '_', and the return says whether anything had to
// be, so the caller can put it in the log.
//
// Repair suits a name, which is a label and survives losing a character. It
// does NOT suit text that has to match something exactly -- a network name
// with a '_' in place of a '#' is a different network -- and wifi.c refuses
// such a request instead of repairing it.
bool sysserver_safe_quoted(const char *text, char *out, size_t out_size);
bool sysserver_safe_bare(const char *text, char *out, size_t out_size);

// Asks sys_server to mount `device` (e.g. "/dev/mmcblk0p1") on `mount_point`
// (e.g. "/mnt/sd_0"). Returns 0 on success.
int sysserver_mount(const char *device, const char *mount_point);

// Asks sys_server to unmount `mount_point`. Returns 0 on success.
int sysserver_umount(const char *mount_point);

// ---------------------------------------------------------------------------
// The other direction
//
// sys_server does not only answer requests: it also pushes events, by
// connecting to a socket the player is expected to be listening on at
// /var/run/sys_client. So does the firmware's patched bluetoothd, which is
// where the only truthful "the A2DP profile is up" notice comes from:
//
//     BT:CHANGE <mac> AudioSink connected <vendor-id>
//     BT:CHANGE <mac> AudioSink disconnected <vendor-id>
//     BT:CODEC  <mac> Playback <codec>
//
// The stock player creates this socket at startup and keys everything about
// Bluetooth off what arrives on it. This one does not: btstack.c reads the same
// facts from bluez and bluealsa over D-Bus, where they are exact and where a
// question can be asked back. What arrives here is used only as a free hint
// that something moved, which saves waiting for the next poll.
//
// `cb` is called on the listener thread, once per message, with a
// NUL-terminated string. It must not block for long and must not touch LVGL.
// Calling this twice is a no-op; it fails quietly when the socket cannot be
// created (another player already owns it, or a read-only /var/run).
void sysserver_listen(void (*cb)(const char *message));

#endif /* SYSSERVER_H */

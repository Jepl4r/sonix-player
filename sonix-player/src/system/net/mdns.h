#ifndef MDNS_H
#define MDNS_H

#include <stdbool.h>
#include <stdint.h>

// A minimal multicast-DNS responder, so the Wi-Fi transfer page can be reached
// by name -- http://sonix-transfer.local -- instead of by address.
//
// It answers exactly one question: "what is the A record for <name>.local?".
// That is what a browser asks when someone types the name, and it is all this
// needs to do.
//
// Why it is here at all: the firmware has no mDNS responder. There is one
// statically linked inside /usr/bin/shairport (tinysvcmdns, for the _raop._tcp
// advertisement AirPlay needs) but nothing that can be told to publish another
// name, and no avahi anywhere.
//
// What resolves .local names, and what does not:
//
//   macOS, iOS            built in
//   Windows 10 1803+      built in
//   Linux desktops        with avahi-daemon + nss-mdns, which most have
//   Android browsers      no -- Chrome and Firefox on Android do not resolve
//                         .local at all
//
// That last line is why the player's screen shows the address as well as the
// name. Anyone whose client cannot do this still has a way in.
//
// Deliberately not implemented: service advertisement (_http._tcp PTR/SRV/TXT,
// which would make it show up in Bonjour browsers), IPv6, and the conflict
// probing RFC 6762 asks for before claiming a name. The first is a nicety, and
// the last matters only if something else on the network has already taken
// "sonix-transfer", which is not a name anything else uses.

// Starts answering for "<name>.local" on the current Wi-Fi address. `name` is
// copied. Calling it twice replaces the name.
//
// Returns immediately: the socket work happens on this module's own thread,
// which also notices when the address changes under it and re-joins the group.
bool mdns_start(const char *name, uint16_t port);

// Stops answering and closes the socket.
void mdns_stop(void);

// True while the responder thread is up.
bool mdns_running(void);

// The name being answered for, with ".local" on the end, or an empty string.
// Safe to call from the interface thread.
void mdns_hostname(char *out, int size);

#endif /* MDNS_H */

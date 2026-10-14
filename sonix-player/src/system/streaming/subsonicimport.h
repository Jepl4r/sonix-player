#ifndef SUBSONICIMPORT_H
#define SUBSONICIMPORT_H

// Signing in to a Subsonic server from "subsonic.txt" at the root of the card
// (cardimport.h): the first service on the generic mechanism, because it
// already works and so proves the mechanism on its own.
//
// The file, plain text, UTF-8 (a BOM and Windows line ends are fine):
//
//   https://music.example.org
//   alice
//   secret
//
// -- the server's address (without a scheme it is http://, as on the sign-in
// page), the login and the password, one per line, in that order. The same in
// any order as "key = value" lines is read too:
//
//   server = 192.168.1.10:4533
//   user = alice
//   password = secret
//
// ("url" for server; "login" or "username" for user; "pass" for password).
// Spaces at both ends of a line are dropped, so a password cannot begin or end
// with one. On success the account is kept as a sign-in on the page keeps it
// and the file is deleted; a server that refuses the login renames it to
// "subsonic.txt.bad"; no network leaves it for the next start, the next card
// insertion or the next opening of the Subsonic page.

#define SUBSONICIMPORT_FILE "subsonic.txt"

// Registers the file with cardimport. Once, at start-up.
void subsonicimport_init(void);

#endif /* SUBSONICIMPORT_H */

#ifndef SUBSONICACCOUNT_H
#define SUBSONICACCOUNT_H

#include <stdbool.h>
#include <stddef.h>

// The server's password, kept out of device_config.ini.
//
// Two reasons, both already learned elsewhere in this tree:
//
//   * config.c truncates a value at 128 bytes and has no permissions at all;
//   * the configuration file is the first thing anybody copies off the card.
//
// The file lives beside the configuration (<SONIX_CONFIG>.subsonic-account),
// is written through a temporary and a rename, is created 0600, and is read
// with O_NOFOLLOW and checked for owner, mode and size before its contents are
// believed. The format is a magic line and the password, nothing else: the
// server and the user are not secrets and belong in the configuration, where
// the settings page can change them without touching this file.
//
// The password is not encrypted. The device has no key storage to encrypt it
// with, and a key compiled into the binary is the same secret in a worse place
// -- see the note in seal_streamkeys.py.

// Writes the password for `server` and `user`. False when the path cannot be
// written or the password does not fit.
bool subsonicaccount_save(const char *server, const char *user, const char *password);

// Copies the stored password into `out`. False when there is no file, it does
// not carry the magic, or its permissions are wrong (a file somebody else
// could write is not evidence of anything).
bool subsonicaccount_load(char *out, size_t size);

// Removes the file. Nothing to remove is not an error.
void subsonicaccount_clear(void);

#endif /* SUBSONICACCOUNT_H */

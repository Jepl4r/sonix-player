#ifndef MD5_H
#define MD5_H

#include <stddef.h>

// MD5, for one reason: Qobuz signs the request for a track's URL with it. Not a
// choice made here, and not a defensible one in 2026 -- MD5 has been broken for
// collisions for twenty years -- but it protects nothing here: it is the lock
// the service puts on its own door, and getting in means holding the key it
// asks for.
//
// Written here rather than taken from the libcrypto the device already carries
// (see tls.h) because it is a hundred and twenty dependency-free lines testable
// against the RFC 1321 vectors, and there is no reason for playing a track to
// depend on a library loading.
//
// Not to be used for anything that must withstand an attacker.

#define MD5_DIGEST_LEN 16
#define MD5_HEX_LEN 33 // 32 characters plus the terminator

void md5(const void *data, size_t len, unsigned char out[MD5_DIGEST_LEN]);

// The same, written as lower-case hex: the form the signature takes in the URL.
void md5_hex(const void *data, size_t len, char out[MD5_HEX_LEN]);

#endif /* MD5_H */

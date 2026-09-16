#ifndef SHA1_H
#define SHA1_H

#include <stddef.h>

// SHA-1, for one reason: Podcast Index signs every request with it.
//
// As with the MD5 next door, this is not a choice made here and it protects
// nothing of this project's -- it is the lock the service puts on its own door,
// and getting in means holding the key it asks for. SHA-1 has been broken for
// collisions since 2017; nothing is being signed here, only knowledge of a
// secret demonstrated, which it is still adequate for.
//
// Written here rather than taken from the libcrypto the device already carries
// (see tls.h) for the same two reasons as MD5: it is a hundred and fifty
// dependency-free lines testable against the FIPS 180-1 vectors, and that
// library is opened at run time and may be absent -- making the podcast list
// depend on a TLS library loading would be a pointless coupling, since nothing
// in the signature is TLS.
//
// Not to be used for anything that must withstand an attacker.

#define SHA1_DIGEST_LEN 20
#define SHA1_HEX_LEN 41 // 40 characters plus the terminator

void sha1(const void *data, size_t len, unsigned char out[SHA1_DIGEST_LEN]);

// The same, written as lower-case hex: the form the signature takes in the
// Authorization header.
void sha1_hex(const void *data, size_t len, char out[SHA1_HEX_LEN]);

#endif /* SHA1_H */

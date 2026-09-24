#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

// SHA-256 (FIPS 180-4), incremental: a firmware image is hashed a piece at a
// time as it arrives, and is never in memory whole.

#define SHA256_DIGEST_LEN 32
#define SHA256_HEX_LEN 65 // 64 characters plus the terminator

typedef struct {
	uint32_t h[8];
	uint64_t bits;
	unsigned char block[64];
	size_t have;
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);

// Writes the digest as lower-case hex. The context is spent afterwards.
void sha256_final_hex(sha256_ctx *c, char out[SHA256_HEX_LEN]);

#endif /* SHA256_H */

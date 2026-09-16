#include "sha1.h"

#include <stdint.h>
#include <string.h>

// FIPS 180-1, written to be readable rather than fast: this file produces one
// signature per network request, so a few microseconds go unnoticed.

typedef struct {
	uint32_t h[5];
	uint64_t bits;
	unsigned char block[64];
	size_t have;
} sha1_ctx;

static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_block(sha1_ctx *c, const unsigned char *p) {
	uint32_t w[80];

	// The first sixteen are the block's bytes, big-endian as the standard
	// requires; the other sixty-four are derived from those.
	for (int i = 0; i < 16; i++) {
		w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) | ((uint32_t)p[i * 4 + 2] << 8) |
			   (uint32_t)p[i * 4 + 3];
	}
	for (int i = 16; i < 80; i++) {
		w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	}

	uint32_t a = c->h[0], b = c->h[1], d = c->h[2], e = c->h[3], f = c->h[4];

	for (int i = 0; i < 80; i++) {
		uint32_t mix, k;
		if (i < 20) {
			mix = (b & d) | (~b & e);
			k = 0x5A827999u;
		} else if (i < 40) {
			mix = b ^ d ^ e;
			k = 0x6ED9EBA1u;
		} else if (i < 60) {
			mix = (b & d) | (b & e) | (d & e);
			k = 0x8F1BBCDCu;
		} else {
			mix = b ^ d ^ e;
			k = 0xCA62C1D6u;
		}
		uint32_t t = rol(a, 5) + mix + f + k + w[i];
		f = e;
		e = d;
		d = rol(b, 30);
		b = a;
		a = t;
	}

	c->h[0] += a;
	c->h[1] += b;
	c->h[2] += d;
	c->h[3] += e;
	c->h[4] += f;
}

static void sha1_init(sha1_ctx *c) {
	c->h[0] = 0x67452301u;
	c->h[1] = 0xEFCDAB89u;
	c->h[2] = 0x98BADCFEu;
	c->h[3] = 0x10325476u;
	c->h[4] = 0xC3D2E1F0u;
	c->bits = 0;
	c->have = 0;
}

static void sha1_update(sha1_ctx *c, const unsigned char *data, size_t len) {
	c->bits += (uint64_t)len * 8;

	// Finish the block already started, if one is half full.
	if (c->have) {
		size_t need = 64 - c->have;
		size_t take = len < need ? len : need;
		memcpy(c->block + c->have, data, take);
		c->have += take;
		data += take;
		len -= take;
		if (c->have < 64) {
			return;
		}
		sha1_block(c, c->block);
		c->have = 0;
	}

	while (len >= 64) {
		sha1_block(c, data);
		data += 64;
		len -= 64;
	}

	if (len) {
		memcpy(c->block, data, len);
		c->have = len;
	}
}

static void sha1_final(sha1_ctx *c, unsigned char out[SHA1_DIGEST_LEN]) {
	uint64_t bits = c->bits;

	// Padding: a one bit, then zeros until eight bytes are left, then the length
	// in bits. If they do not fit, this block is closed and another one made
	// entirely of padding follows.
	unsigned char pad = 0x80;
	sha1_update(c, &pad, 1);
	c->bits = bits; // sha1_update counted the padding, which must not count

	unsigned char zero = 0;
	while (c->have != 56) {
		sha1_update(c, &zero, 1);
		c->bits = bits;
	}

	unsigned char tail[8];
	for (int i = 0; i < 8; i++) {
		tail[i] = (unsigned char)(bits >> (56 - i * 8));
	}
	sha1_update(c, tail, 8);

	for (int i = 0; i < 5; i++) {
		out[i * 4] = (unsigned char)(c->h[i] >> 24);
		out[i * 4 + 1] = (unsigned char)(c->h[i] >> 16);
		out[i * 4 + 2] = (unsigned char)(c->h[i] >> 8);
		out[i * 4 + 3] = (unsigned char)c->h[i];
	}
}

void sha1(const void *data, size_t len, unsigned char out[SHA1_DIGEST_LEN]) {
	sha1_ctx c;
	sha1_init(&c);
	sha1_update(&c, (const unsigned char *)data, len);
	sha1_final(&c, out);
}

void sha1_hex(const void *data, size_t len, char out[SHA1_HEX_LEN]) {
	static const char HEX[] = "0123456789abcdef";
	unsigned char digest[SHA1_DIGEST_LEN];
	sha1(data, len, digest);
	for (int i = 0; i < SHA1_DIGEST_LEN; i++) {
		out[i * 2] = HEX[digest[i] >> 4];
		out[i * 2 + 1] = HEX[digest[i] & 0x0F];
	}
	out[SHA1_HEX_LEN - 1] = '\0';
}

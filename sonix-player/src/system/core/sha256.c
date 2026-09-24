#include "sha256.h"

#include <stdio.h>
#include <string.h>

static const uint32_t K[64] = {
	0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
	0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
	0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
	0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
	0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
	0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
	0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
	0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t ror(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }

static void sha256_block(sha256_ctx *c, const unsigned char *p) {
	uint32_t w[64];
	for (int i = 0; i < 16; i++) {
		w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) | ((uint32_t)p[i * 4 + 2] << 8) |
			   (uint32_t)p[i * 4 + 3];
	}
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	uint32_t a = c->h[0], b = c->h[1], d = c->h[2], e = c->h[3];
	uint32_t f = c->h[4], g = c->h[5], h = c->h[6], k = c->h[7];

	for (int i = 0; i < 64; i++) {
		uint32_t s1 = ror(f, 6) ^ ror(f, 11) ^ ror(f, 25);
		uint32_t ch = (f & g) ^ (~f & h);
		uint32_t t1 = k + s1 + ch + K[i] + w[i];
		uint32_t s0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
		uint32_t maj = (a & b) ^ (a & d) ^ (b & d);
		uint32_t t2 = s0 + maj;
		k = h;
		h = g;
		g = f;
		f = e + t1;
		e = d;
		d = b;
		b = a;
		a = t1 + t2;
	}

	c->h[0] += a;
	c->h[1] += b;
	c->h[2] += d;
	c->h[3] += e;
	c->h[4] += f;
	c->h[5] += g;
	c->h[6] += h;
	c->h[7] += k;
}

void sha256_init(sha256_ctx *c) {
	static const uint32_t IV[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
								   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
	memcpy(c->h, IV, sizeof(IV));
	c->bits = 0;
	c->have = 0;
}

void sha256_update(sha256_ctx *c, const void *data, size_t len) {
	const unsigned char *p = data;
	c->bits += (uint64_t)len * 8;

	if (c->have) {
		size_t take = 64 - c->have;
		if (take > len) {
			take = len;
		}
		memcpy(c->block + c->have, p, take);
		c->have += take;
		p += take;
		len -= take;
		if (c->have < 64) {
			return;
		}
		sha256_block(c, c->block);
		c->have = 0;
	}
	while (len >= 64) {
		sha256_block(c, p);
		p += 64;
		len -= 64;
	}
	memcpy(c->block, p, len);
	c->have = len;
}

void sha256_final_hex(sha256_ctx *c, char out[SHA256_HEX_LEN]) {
	uint64_t bits = c->bits;

	// Padding: a 1 bit, zeros up to 56 bytes into a block, the length in bits
	// as a big-endian 64-bit number.
	c->block[c->have++] = 0x80;
	if (c->have > 56) {
		memset(c->block + c->have, 0, 64 - c->have);
		sha256_block(c, c->block);
		c->have = 0;
	}
	memset(c->block + c->have, 0, 56 - c->have);
	for (int i = 0; i < 8; i++) {
		c->block[56 + i] = (unsigned char)(bits >> (56 - 8 * i));
	}
	sha256_block(c, c->block);

	for (int i = 0; i < 8; i++) {
		snprintf(out + i * 8, 9, "%08x", (unsigned)c->h[i]);
	}
	out[64] = '\0';
}

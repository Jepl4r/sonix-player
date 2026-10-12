#define _GNU_SOURCE 1

#include "crypto.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// The bits of OpenSSL's EVP interface used here, restated
// ---------------------------------------------------------------------------
//
// As in tls.c: no openssl/*.h in the cross toolchain, so the opaque types and
// prototypes are written out. All of these are in 1.1.1 (the target) and 3.x
// (the host) with the same signatures.

typedef struct evp_cipher_st EVP_CIPHER;
typedef struct evp_cipher_ctx_st EVP_CIPHER_CTX;
typedef struct engine_st ENGINE;

struct evp_api {
	EVP_CIPHER_CTX *(*EVP_CIPHER_CTX_new)(void);
	void (*EVP_CIPHER_CTX_free)(EVP_CIPHER_CTX *);
	int (*EVP_CIPHER_CTX_set_padding)(EVP_CIPHER_CTX *, int pad);
	const EVP_CIPHER *(*EVP_aes_128_ctr)(void);
	const EVP_CIPHER *(*EVP_aes_128_cbc)(void);
	int (*EVP_EncryptInit_ex)(EVP_CIPHER_CTX *, const EVP_CIPHER *, ENGINE *, const unsigned char *key, const unsigned char *iv);
	int (*EVP_EncryptUpdate)(EVP_CIPHER_CTX *, unsigned char *out, int *outl, const unsigned char *in, int inl);
	int (*EVP_DecryptInit_ex)(EVP_CIPHER_CTX *, const EVP_CIPHER *, ENGINE *, const unsigned char *key, const unsigned char *iv);
	int (*EVP_DecryptUpdate)(EVP_CIPHER_CTX *, unsigned char *out, int *outl, const unsigned char *in, int inl);
};

static struct evp_api evp;
static void *lib_crypto;
static enum { LOAD_UNTRIED, LOAD_OK, LOAD_FAILED } load_state = LOAD_UNTRIED;
static pthread_mutex_t load_lock = PTHREAD_MUTEX_INITIALIZER;

// EVP takes an int length; anything bigger is fed in pieces of this size,
// which is a whole number of blocks so CBC and CTR both carry on seamlessly
// from one piece to the next inside the same context.
#define CHUNK (1u << 20)

// ---------------------------------------------------------------------------
// The two modes, without the availability check
// ---------------------------------------------------------------------------

// Adds `blocks` to a 16-byte big-endian counter, carrying across all 128
// bits. OpenSSL's CTR mode increments the whole block the same way, so a
// counter advanced here and one advanced by OpenSSL agree.
static void counter_add(uint8_t ctr[CRYPTO_AES_BLOCK_SIZE], uint64_t blocks) {
	for (int i = CRYPTO_AES_BLOCK_SIZE - 1; i >= 0 && blocks; i--) {
		uint64_t sum = (uint64_t)ctr[i] + (blocks & 0xff);
		ctr[i] = (uint8_t)sum;
		blocks = (blocks >> 8) + (sum >> 8);
	}
}

static bool ctr_raw(const uint8_t *key, const uint8_t *iv, const uint8_t *in, uint8_t *out, size_t len, uint64_t offset) {
	uint8_t ctr[CRYPTO_AES_BLOCK_SIZE];
	memcpy(ctr, iv, sizeof(ctr));
	counter_add(ctr, offset / CRYPTO_AES_BLOCK_SIZE);

	EVP_CIPHER_CTX *cx = evp.EVP_CIPHER_CTX_new();
	if (!cx) {
		return false;
	}
	bool ok = evp.EVP_EncryptInit_ex(cx, evp.EVP_aes_128_ctr(), NULL, key, ctr) == 1;

	// An offset inside a block: run the keystream past the bytes before it.
	// Encrypting zeros is the cheapest way to make OpenSSL do that, and it
	// leaves the context exactly where in[0] belongs.
	unsigned skip = (unsigned)(offset % CRYPTO_AES_BLOCK_SIZE);
	if (ok && skip) {
		uint8_t zeros[CRYPTO_AES_BLOCK_SIZE] = {0}, sink[CRYPTO_AES_BLOCK_SIZE];
		int n = 0;
		ok = evp.EVP_EncryptUpdate(cx, sink, &n, zeros, (int)skip) == 1;
	}
	while (ok && len) {
		int piece = len > CHUNK ? (int)CHUNK : (int)len;
		int n = 0;
		ok = evp.EVP_EncryptUpdate(cx, out, &n, in, piece) == 1 && n == piece;
		in += piece;
		out += piece;
		len -= (size_t)piece;
	}
	evp.EVP_CIPHER_CTX_free(cx);
	return ok;
}

static bool cbc_raw(const uint8_t *key, const uint8_t *iv, const uint8_t *in, uint8_t *out, size_t len) {
	EVP_CIPHER_CTX *cx = evp.EVP_CIPHER_CTX_new();
	if (!cx) {
		return false;
	}
	bool ok = evp.EVP_DecryptInit_ex(cx, evp.EVP_aes_128_cbc(), NULL, key, iv) == 1;
	// Padding is checked by hand below: with OpenSSL's own, the last block is
	// held back until a Final call, which would make an in-place decryption
	// of a mid-segment chunk awkward for no benefit.
	if (ok) {
		evp.EVP_CIPHER_CTX_set_padding(cx, 0);
	}
	while (ok && len) {
		int piece = len > CHUNK ? (int)CHUNK : (int)len;
		int n = 0;
		ok = evp.EVP_DecryptUpdate(cx, out, &n, in, piece) == 1 && n == piece;
		in += piece;
		out += piece;
		len -= (size_t)piece;
	}
	evp.EVP_CIPHER_CTX_free(cx);
	return ok;
}

// ---------------------------------------------------------------------------
// Loading and the self-test
// ---------------------------------------------------------------------------

#define BIND(name)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     \
	do {                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               \
		void *sym = dlsym(lib_crypto, #name);                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          \
		if (!sym) {                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    \
			fprintf(stderr, "crypto: %s not found in libcrypto\n", #name);                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             \
			return false;                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              \
		}                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              \
		memcpy(&evp.name, &sym, sizeof(sym));                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          \
	} while (0)

static bool load_symbols(void) {
	static const char *names[] = {"libcrypto.so.1.1", "libcrypto.so.3", "libcrypto.so", NULL};
	for (int i = 0; names[i] && !lib_crypto; i++) {
		lib_crypto = dlopen(names[i], RTLD_NOW | RTLD_GLOBAL);
	}
	if (!lib_crypto) {
		fprintf(stderr, "crypto: libcrypto not loadable (%s); AES unavailable\n", dlerror());
		return false;
	}
	BIND(EVP_CIPHER_CTX_new);
	BIND(EVP_CIPHER_CTX_free);
	BIND(EVP_CIPHER_CTX_set_padding);
	BIND(EVP_aes_128_ctr);
	BIND(EVP_aes_128_cbc);
	BIND(EVP_EncryptInit_ex);
	BIND(EVP_EncryptUpdate);
	BIND(EVP_DecryptInit_ex);
	BIND(EVP_DecryptUpdate);
	return true;
}

#undef BIND

// NIST SP 800-38A, F.5.1 (CTR-AES128.Encrypt) and F.2.2 (CBC-AES128.Decrypt):
// the same key and plaintext, four blocks.
static const uint8_t sp_key[16] = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6, 0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
static const uint8_t sp_plain[64] = {0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a, 0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c, 0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51, 0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4, 0x11, 0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef, 0xf6, 0x9f, 0x24, 0x45, 0xdf, 0x4f, 0x9b, 0x17, 0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10};
static const uint8_t sp_ctr_iv[16] = {0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff};
static const uint8_t sp_ctr_cipher[64] = {0x87, 0x4d, 0x61, 0x91, 0xb6, 0x20, 0xe3, 0x26, 0x1b, 0xef, 0x68, 0x64, 0x99, 0x0d, 0xb6, 0xce, 0x98, 0x06, 0xf6, 0x6b, 0x79, 0x70, 0xfd, 0xff, 0x86, 0x17, 0x18, 0x7b, 0xb9, 0xff, 0xfd, 0xff, 0x5a, 0xe4, 0xdf, 0x3e, 0xdb, 0xd5, 0xd3, 0x5e, 0x5b, 0x4f, 0x09, 0x02, 0x0d, 0xb0, 0x3e, 0xab, 0x1e, 0x03, 0x1d, 0xda, 0x2f, 0xbe, 0x03, 0xd1, 0x79, 0x21, 0x70, 0xa0, 0xf3, 0x00, 0x9c, 0xee};
static const uint8_t sp_cbc_iv[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
static const uint8_t sp_cbc_cipher[64] = {0x76, 0x49, 0xab, 0xac, 0x81, 0x19, 0xb2, 0x46, 0xce, 0xe9, 0x8e, 0x9b, 0x12, 0xe9, 0x19, 0x7d, 0x50, 0x86, 0xcb, 0x9b, 0x50, 0x72, 0x19, 0xee, 0x95, 0xdb, 0x11, 0x3a, 0x91, 0x76, 0x78, 0xb2, 0x73, 0xbe, 0xd6, 0xb8, 0xe3, 0xc1, 0x74, 0x3b, 0x71, 0x16, 0xe6, 0x9e, 0x22, 0x22, 0x95, 0x16, 0x3f, 0xf1, 0xca, 0xa1, 0x68, 0x1f, 0xac, 0x09, 0x12, 0x0e, 0xca, 0x30, 0x75, 0x86, 0xe1, 0xa7};

// Checks the vectors, and the offset arithmetic with them: the stream cut at
// an odd byte in the second block (and in place) must still decrypt to the
// rest of the plaintext, which is the property a resumed download relies on.
static bool self_test(void) {
	uint8_t buf[64];
	if (!ctr_raw(sp_key, sp_ctr_iv, sp_ctr_cipher, buf, 64, 0) || memcmp(buf, sp_plain, 64) != 0) {
		return false;
	}
	memcpy(buf, sp_ctr_cipher + 21, 43);
	if (!ctr_raw(sp_key, sp_ctr_iv, buf, buf, 43, 21) || memcmp(buf, sp_plain + 21, 43) != 0) {
		return false;
	}
	if (!cbc_raw(sp_key, sp_cbc_iv, sp_cbc_cipher, buf, 64) || memcmp(buf, sp_plain, 64) != 0) {
		return false;
	}
	return true;
}

static bool ensure_loaded(void) {
	pthread_mutex_lock(&load_lock);
	if (load_state == LOAD_UNTRIED) {
		load_state = LOAD_FAILED;
		if (load_symbols()) {
			if (self_test()) {
				load_state = LOAD_OK;
			} else {
				fprintf(stderr, "crypto: AES self-test failed; AES unavailable\n");
			}
		}
	}
	bool ok = load_state == LOAD_OK;
	pthread_mutex_unlock(&load_lock);
	return ok;
}

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

bool crypto_available(void) { return ensure_loaded(); }

bool crypto_aes128_ctr(const uint8_t key[CRYPTO_AES128_KEY_SIZE], const uint8_t iv[CRYPTO_AES_BLOCK_SIZE], const uint8_t *in, uint8_t *out, size_t len, uint64_t counter_offset) {
	if (!ensure_loaded()) {
		return false;
	}
	return len == 0 || ctr_raw(key, iv, in, out, len, counter_offset);
}

bool crypto_aes128_cbc_decrypt(const uint8_t key[CRYPTO_AES128_KEY_SIZE], const uint8_t iv[CRYPTO_AES_BLOCK_SIZE], const uint8_t *in, uint8_t *out, size_t len, bool strip_padding, size_t *out_len) {
	if (out_len) {
		*out_len = 0;
	}
	if (len % CRYPTO_AES_BLOCK_SIZE || (strip_padding && len == 0) || !ensure_loaded()) {
		return false;
	}
	if (len && !cbc_raw(key, iv, in, out, len)) {
		return false;
	}
	size_t plain = len;
	if (strip_padding) {
		// PKCS#7: the last byte says how many bytes of padding there are, 1
		// to 16, and every one of them carries that same value. Anything else
		// means a wrong key or IV, and is better refused than played.
		uint8_t pad = out[len - 1];
		if (pad == 0 || pad > CRYPTO_AES_BLOCK_SIZE) {
			return false;
		}
		for (size_t i = len - pad; i < len; i++) {
			if (out[i] != pad) {
				return false;
			}
		}
		plain = len - pad;
	}
	if (out_len) {
		*out_len = plain;
	}
	return true;
}

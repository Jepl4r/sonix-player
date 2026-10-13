#ifndef CRYPTO_H
#define CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// AES-128 (and AES-256 in counter mode) for the streaming services, borrowed from the OpenSSL the device
// already ships.
//
// libcrypto is opened with dlopen() for the same reason tls.c does it (the
// cross toolchain has no headers and nothing to link against, see tls.h), but
// independently of TLS: a decryption must not depend on a CA bundle being
// present, and tls.c only declares itself loaded once it has found one. The
// two dlopen()s of the same library return the same handle, so the library is
// mapped once whichever side asks first.
//
// Nothing here runs until the first call: an untouched API costs one static
// struct and no I/O. The first call resolves the EVP_* functions and checks
// both modes against the NIST SP 800-38A test vectors before trusting them,
// so a rootfs whose libcrypto is broken or built without AES reports itself
// as unavailable instead of producing noise that the decoder would play.
//
// Thread contract: every function may be called from any thread, concurrently.
// Each call uses a cipher context of its own and the one-time loading is
// guarded by a mutex.

#define CRYPTO_AES128_KEY_SIZE 16
#define CRYPTO_AES256_KEY_SIZE 32
#define CRYPTO_AES_BLOCK_SIZE 16

// True when libcrypto was found, the symbols resolved and the self-test
// passed. The first call does the loading; later ones are a load of a flag.
bool crypto_available(void);

// AES-128 in counter mode -- encryption and decryption are the same thing.
// `iv` is the 16-byte initial counter block, incremented as a 128-bit
// big-endian number per block (as Tidal's older encrypted streams and NIST
// SP 800-38A do). `counter_offset` is the position, in bytes from the start
// of the stream, of in[0]: it need not be a multiple of the block size, which
// is what lets a cache resume decrypting a file from wherever the download
// stopped or a seek landed, without having to keep any state between calls.
// `in` and `out` may be the same buffer. Returns false when AES is
// unavailable or OpenSSL reports an error; `out` is then undefined.
bool crypto_aes128_ctr(const uint8_t key[CRYPTO_AES128_KEY_SIZE], const uint8_t iv[CRYPTO_AES_BLOCK_SIZE], const uint8_t *in, uint8_t *out, size_t len, uint64_t counter_offset);

// The same with the key size given: 16 bytes (AES-128) or 32 (AES-256, what
// Yandex Music's "encraw" files use since 2025). AES-256 is checked against
// its own NIST vector (SP 800-38A F.5.5) at load, separately: a libcrypto that
// lacks it still does AES-128, and this returns false for a 32-byte key.
bool crypto_aes_ctr(const uint8_t *key, size_t key_len, const uint8_t iv[CRYPTO_AES_BLOCK_SIZE], const uint8_t *in, uint8_t *out, size_t len, uint64_t counter_offset);

// Whether crypto_aes_ctr() would work with a key of `key_len` bytes. Loads
// libcrypto on first use, like crypto_available().
bool crypto_aes_ctr_available(size_t key_len);

// AES-128-CBC decryption, as HLS #EXT-X-KEY:METHOD=AES-128 uses it on whole
// segments. `len` must be a multiple of the block size. With `strip_padding`
// the PKCS#7 padding of the last block is checked and removed (an HLS segment
// is padded; a chunk from its middle is not), and *out_len is the plaintext
// length; without it *out_len is `len`. `out` needs room for `len` bytes and
// may be the same buffer as `in`. Returns false on bad input, bad padding or
// unavailable AES.
bool crypto_aes128_cbc_decrypt(const uint8_t key[CRYPTO_AES128_KEY_SIZE], const uint8_t iv[CRYPTO_AES_BLOCK_SIZE], const uint8_t *in, uint8_t *out, size_t len, bool strip_padding, size_t *out_len);

#endif /* CRYPTO_H */

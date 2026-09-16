#include "png_write.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// CRC32 (for the PNG chunks) and Adler32 (for the zlib wrapper)
// ---------------------------------------------------------------------------

static uint32_t crc_table[256];
static bool crc_table_ready;

static void crc_table_build(void) {
	for (uint32_t n = 0; n < 256; n++) {
		uint32_t c = n;
		for (int k = 0; k < 8; k++) {
			c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
		}
		crc_table[n] = c;
	}
	crc_table_ready = true;
}

static uint32_t crc32_of(const uint8_t *data, size_t len, uint32_t start) {
	if (!crc_table_ready) {
		crc_table_build();
	}
	uint32_t c = start ^ 0xFFFFFFFFu;
	for (size_t i = 0; i < len; i++) {
		c = crc_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
	}
	return c ^ 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------
// the device's libz, if it has one
// ---------------------------------------------------------------------------

// zlib's own struct, copied out instead of included: this file does not compile
// against zlib.h, it opens the library at runtime with dlopen. Every field is a
// pointer, an unsigned or an unsigned long, so the size comes out equal to the
// real one on both 32 and 64 bit -- and deflateInit_ checks it, so a mistake
// here would be caught at once instead of writing out of place.
typedef struct {
	const uint8_t *next_in;
	unsigned avail_in;
	unsigned long total_in;
	uint8_t *next_out;
	unsigned avail_out;
	unsigned long total_out;
	const char *msg;
	void *state;
	void *(*zalloc)(void *, unsigned, unsigned);
	void (*zfree)(void *, void *);
	void *opaque;
	int data_type;
	unsigned long adler;
	unsigned long reserved;
} z_stream_min;

#define Z_OK 0
#define Z_STREAM_END 1
#define Z_NO_FLUSH 0
#define Z_FINISH 4

static int (*z_deflate_init)(z_stream_min *strm, int level, const char *version, int stream_size);
static int (*z_deflate)(z_stream_min *strm, int flush);
static int (*z_deflate_end)(z_stream_min *strm);
static bool zlib_tried;
static bool zlib_ok;
static pthread_mutex_t zlib_lock = PTHREAD_MUTEX_INITIALIZER;

static bool zlib_available(void) {
	pthread_mutex_lock(&zlib_lock);
	if (!zlib_tried) {
		zlib_tried = true;
		static const char *const NAMES[] = {"libz.so.1", "libz.so"};
		void *lib = NULL;
		for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]) && !lib; i++) {
			lib = dlopen(NAMES[i], RTLD_NOW | RTLD_LOCAL);
		}
		if (lib) {
			*(void **)&z_deflate_init = dlsym(lib, "deflateInit_");
			*(void **)&z_deflate = dlsym(lib, "deflate");
			*(void **)&z_deflate_end = dlsym(lib, "deflateEnd");
			zlib_ok = z_deflate_init && z_deflate && z_deflate_end;
		}
		printf("png: compression %s\n", zlib_ok ? "with libz" : "none (uncompressed blocks)");
	}
	bool ok = zlib_ok;
	pthread_mutex_unlock(&zlib_lock);
	return ok;
}

// ---------------------------------------------------------------------------
// the zlib stream
// ---------------------------------------------------------------------------

// The streaming writer.
//
// The image is encoded one row at a time: a single input row plus a 32 KB output
// buffer, which becomes an IDAT chunk written to the file every time it fills.
// PNG allows any number of IDAT chunks -- readers concatenate them in order --
// so the peak stays at tens of kilobytes whatever the frame size. Building the
// whole image and the whole compressed result in memory would ask for two
// contiguous megabytes on a device that cannot spare them.

#define IDAT_BUF 32768

typedef struct {
	FILE *f;
	uint8_t out[IDAT_BUF];
	size_t used;
	bool ok;

	// The fallback without libz: "stored" blocks, the bytes as they are. It
	// needs the adler32 of everything that passes through and how much of the
	// current block is left.
	bool stored;
	uint32_t adler_a, adler_b;
	size_t stored_left;
} png_stream_t;

static bool write_chunk(FILE *f, const char type[4], const uint8_t *data, size_t len);

// Writes out whatever the buffer holds, as an IDAT.
static void stream_flush(png_stream_t *st) {
	if (st->used == 0) {
		return;
	}
	if (st->ok && !write_chunk(st->f, "IDAT", st->out, st->used)) {
		st->ok = false;
	}
	st->used = 0;
}

static void stream_put(png_stream_t *st, const uint8_t *data, size_t len) {
	while (len > 0 && st->ok) {
		size_t room = IDAT_BUF - st->used;
		size_t take = len < room ? len : room;
		memcpy(st->out + st->used, data, take);
		st->used += take;
		data += take;
		len -= take;
		if (st->used == IDAT_BUF) {
			stream_flush(st);
		}
	}
}

// --- the fallback: uncompressed blocks, streamed like the rest -------------
//
// Deflate with "stored" blocks only. Each block starts byte aligned, so its
// three-bit header (BFINAL plus BTYPE = 00) occupies a whole byte, followed by
// LEN, its one's complement, and the bytes as they are. No compression, but a
// valid zlib stream. LEN is 16 bits, so a block holds at most 65535 bytes.

static void stored_begin(png_stream_t *st) {
	uint8_t header[2] = {0x78, 0x01}; // CM = 8, 32 KB window, and 0x7801 is divisible by 31
	stream_put(st, header, 2);
	st->adler_a = 1;
	st->adler_b = 0;
	st->stored_left = 0;
}

static void stored_adler(png_stream_t *st, const uint8_t *data, size_t len) {
	for (size_t i = 0; i < len; i++) {
		st->adler_a = (st->adler_a + data[i]) % 65521;
		st->adler_b = (st->adler_b + st->adler_a) % 65521;
	}
}

// `remaining` is how much is still to be written after this piece: it tells
// whether the block being opened is the last one.
static void stored_put(png_stream_t *st, const uint8_t *data, size_t len, size_t remaining) {
	stored_adler(st, data, len);
	while (len > 0) {
		if (st->stored_left == 0) {
			size_t total = len + remaining;
			size_t block = total < 65535 ? total : 65535;
			bool final = block == total;
			uint8_t head[5] = {(uint8_t)(final ? 1 : 0), (uint8_t)(block & 0xFF), (uint8_t)((block >> 8) & 0xFF),
							   (uint8_t)(~block & 0xFF), (uint8_t)((~block >> 8) & 0xFF)};
			stream_put(st, head, 5);
			st->stored_left = block;
		}
		size_t take = len < st->stored_left ? len : st->stored_left;
		stream_put(st, data, take);
		st->stored_left -= take;
		data += take;
		len -= take;
	}
}

static void stored_end(png_stream_t *st) {
	uint32_t adler = (st->adler_b << 16) | st->adler_a;
	uint8_t tail[4] = {(uint8_t)(adler >> 24), (uint8_t)(adler >> 16), (uint8_t)(adler >> 8), (uint8_t)adler};
	stream_put(st, tail, 4);
}

// ---------------------------------------------------------------------------
// the PNG chunks
// ---------------------------------------------------------------------------

static void put_be32(uint8_t *p, uint32_t v) {
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static bool write_chunk(FILE *f, const char type[4], const uint8_t *data, size_t len) {
	uint8_t head[8];
	put_be32(head, (uint32_t)len);
	memcpy(head + 4, type, 4);
	if (fwrite(head, 1, 8, f) != 8) {
		return false;
	}
	if (len && fwrite(data, 1, len, f) != len) {
		return false;
	}

	// The CRC covers the type and the data, not the length.
	uint32_t crc = crc32_of((const uint8_t *)type, 4, 0);
	if (len) {
		crc = crc32_of(data, len, crc);
	}
	uint8_t tail[4];
	put_be32(tail, crc);
	return fwrite(tail, 1, 4, f) == 4;
}


bool png_write_rgb565(const char *path, const uint16_t *pixels, int width, int height) {
	if (!path || !pixels || width <= 0 || height <= 0) {
		return false;
	}

	// A PNG row is a filter byte followed by the pixels. The filter stays zero:
	// on a flat screenshot the predictive filters gain almost nothing and would
	// cost another pass over a million pixels.
	size_t stride = 1 + (size_t)width * 3;
	size_t raw_len = stride * (size_t)height;

	uint8_t *row = malloc(stride);
	if (!row) {
		fprintf(stderr, "png: no memory for a %zu byte row\n", stride);
		return false;
	}

	FILE *f = fopen(path, "wb");
	if (!f) {
		fprintf(stderr, "png: %s does not open for writing: %s\n", path, strerror(errno));
		free(row);
		return false;
	}

	static const uint8_t SIGNATURE[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	bool ok = fwrite(SIGNATURE, 1, 8, f) == 8;

	uint8_t ihdr[13];
	put_be32(ihdr + 0, (uint32_t)width);
	put_be32(ihdr + 4, (uint32_t)height);
	ihdr[8] = 8;  // bits per channel
	ihdr[9] = 2;  // truecolour, no alpha channel
	ihdr[10] = 0; // compression: deflate, the only one there is
	ihdr[11] = 0; // filters: the standard set
	ihdr[12] = 0; // no interlacing
	ok = ok && write_chunk(f, "IHDR", ihdr, sizeof(ihdr));

	png_stream_t *st = calloc(1, sizeof(*st));
	if (!st) {
		fprintf(stderr, "png: no memory for the stream\n");
		fclose(f);
		remove(path);
		free(row);
		return false;
	}
	st->f = f;
	st->ok = ok;
	st->stored = !zlib_available();

	z_stream_min zs;
	memset(&zs, 0, sizeof(zs));
	if (!st->stored) {
		// Level 6, zlib's default. On an interface screenshot, all flat tints,
		// it gets down to a few per cent of the original, and level 9 would cost
		// seconds of CPU to gain almost nothing.
		if (z_deflate_init(&zs, 6, "1.2.11", (int)sizeof(zs)) != Z_OK) {
			fprintf(stderr, "png: deflateInit failed; falling back to uncompressed blocks\n");
			st->stored = true;
		}
	}
	if (st->stored) {
		stored_begin(st);
	}

	uint8_t packed[IDAT_BUF];
	size_t written = 0;

	for (int y = 0; y < height && st->ok; y++) {
		uint8_t *p = row;
		*p++ = 0; // filter: none
		const uint16_t *src = pixels + (size_t)y * width;
		for (int x = 0; x < width; x++) {
			uint16_t v = src[x];
			int r = (v >> 11) & 0x1F;
			int g = (v >> 5) & 0x3F;
			int b = v & 0x1F;
			// From 5 and 6 bits to 8 by replicating the high bits into the low
			// ones, not by shifting alone: full white stays 255 instead of
			// becoming 248.
			*p++ = (uint8_t)((r << 3) | (r >> 2));
			*p++ = (uint8_t)((g << 2) | (g >> 4));
			*p++ = (uint8_t)((b << 3) | (b >> 2));
		}
		written += stride;

		if (st->stored) {
			stored_put(st, row, stride, raw_len - written);
			continue;
		}

		zs.next_in = row;
		zs.avail_in = (unsigned)stride;
		while (zs.avail_in > 0 && st->ok) {
			zs.next_out = packed;
			zs.avail_out = (unsigned)sizeof(packed);
			if (z_deflate(&zs, Z_NO_FLUSH) != Z_OK) {
				st->ok = false;
				break;
			}
			stream_put(st, packed, sizeof(packed) - zs.avail_out);
		}
	}

	if (st->stored) {
		stored_end(st);
	} else {
		// The tail: keep asking until zlib says it has finished.
		int rc = Z_OK;
		while (rc != Z_STREAM_END && st->ok) {
			zs.next_in = NULL;
			zs.avail_in = 0;
			zs.next_out = packed;
			zs.avail_out = (unsigned)sizeof(packed);
			rc = z_deflate(&zs, Z_FINISH);
			if (rc != Z_OK && rc != Z_STREAM_END) {
				st->ok = false;
				break;
			}
			stream_put(st, packed, sizeof(packed) - zs.avail_out);
		}
		z_deflate_end(&zs);
	}

	stream_flush(st);
	ok = st->ok && write_chunk(f, "IEND", NULL, 0);
	free(st);
	free(row);

	if (fclose(f) != 0) {
		ok = false;
	}

	if (!ok) {
		fprintf(stderr, "png: writing %s failed: %s\n", path, strerror(errno));
		remove(path);
	}
	return ok;
}

#include "base64.h"

#include <stdlib.h>

static int value_of(char c) {
	if (c >= 'A' && c <= 'Z') {
		return c - 'A';
	}
	if (c >= 'a' && c <= 'z') {
		return c - 'a' + 26;
	}
	if (c >= '0' && c <= '9') {
		return c - '0' + 52;
	}
	// '+' and '/' from the standard alphabet, '-' and '_' from the URL-safe one.
	// Accepting all four costs two lines and removes a whole class of silent
	// decode failures.
	if (c == '+' || c == '-') {
		return 62;
	}
	if (c == '/' || c == '_') {
		return 63;
	}
	return -1; // padding, whitespace or garbage: skipped
}

uint8_t *base64_decode(const char *text, size_t text_len, size_t max_bytes, size_t *out_size) {
	if (!text || text_len == 0 || !out_size) {
		return NULL;
	}

	// The cap is checked BEFORE allocating, against the worst-case output
	// length (four characters make three bytes). Allocating first would let an
	// arbitrarily long input decide how much memory to take, on a device with
	// 64 MB in total.
	size_t worst = text_len / 4 * 3 + 3;
	if (worst > max_bytes) {
		return NULL;
	}

	uint8_t *out = malloc(worst + 1); // +1: the terminating NUL, see base64.h
	if (!out) {
		return NULL;
	}

	size_t written = 0;
	uint32_t accum = 0;
	int bits = 0;

	for (size_t i = 0; i < text_len; i++) {
		int v = value_of(text[i]);
		if (v < 0) {
			continue;
		}
		accum = (accum << 6) | (uint32_t)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out[written++] = (uint8_t)((accum >> bits) & 0xFF);
		}
	}

	if (written == 0) {
		free(out);
		return NULL;
	}

	out[written] = '\0';
	*out_size = written;
	return out;
}

#include "ebook_utf8.h"

uint32_t ebook_utf8_take(const char *text, uint32_t len, uint32_t *at) {
	const unsigned char *s = (const unsigned char *)text + *at;
	uint32_t left = len - *at;
	uint32_t cp = s[0];
	uint32_t n = 1;
	if (cp >= 0xF0 && left >= 4) {
		cp = ((cp & 0x07u) << 18) | ((s[1] & 0x3Fu) << 12) | ((s[2] & 0x3Fu) << 6) | (s[3] & 0x3Fu);
		n = 4;
	} else if (cp >= 0xE0 && left >= 3) {
		cp = ((cp & 0x0Fu) << 12) | ((s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu);
		n = 3;
	} else if (cp >= 0xC0 && left >= 2) {
		cp = ((cp & 0x1Fu) << 6) | (s[1] & 0x3Fu);
		n = 2;
	}
	*at += n;
	return cp;
}

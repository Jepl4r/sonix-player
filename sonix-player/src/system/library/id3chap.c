#include "src/system/library/id3chap.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// See id3chap.h. The shape of what is read:
//
//   "ID3" ver(2) flags(1) size(4, syncsafe)     the tag header, 10 bytes
//   frame id(4) size(4) flags(2) payload        repeated to the end of the tag
//
//   a CHAP payload is:
//     element id, NUL-terminated
//     start ms (4), end ms (4), start offset (4), end offset (4)
//     sub-frames, in the same shape as a frame -- TIT2 is the chapter's name
//
// In ID3v2.4 a frame's size is syncsafe (seven bits a byte); in 2.3 it is a
// plain big-endian number. Getting that wrong reads a frame's length as roughly
// half of what it is, which walks into the middle of the next frame and finds
// nothing -- so the version is read and honoured rather than assumed.

// A tag bigger than this is not a tag, it is a file that happens to start with
// three familiar bytes. Real ones with cover art run to a megabyte.
#define MAX_TAG (4u * 1024u * 1024u)
#define MAX_CHAPTERS 1024

static uint32_t be32(const uint8_t *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// Seven bits a byte, high bit always clear, so a tag can never contain a byte
// that looks like the start of an MPEG frame.
static uint32_t syncsafe32(const uint8_t *p) {
	return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) | ((uint32_t)(p[2] & 0x7F) << 7) |
		   (uint32_t)(p[3] & 0x7F);
}

// One text field out of a sub-frame: an encoding byte and then the text.
// Encodings 1 and 2 are UTF-16 and are read as their low bytes, which is right
// up to U+00FF and a question mark above it; anything else is copied byte for
// byte.
static void read_text(const uint8_t *data, uint32_t len, char *out, size_t out_size) {
	out[0] = '\0';
	if (!len) {
		return;
	}
	uint8_t encoding = data[0];
	const uint8_t *text = data + 1;
	uint32_t text_len = len - 1;
	size_t w = 0;

	if (encoding == 1 || encoding == 2) {
		// UTF-16. Skip a byte-order mark and step two bytes at a time; which of
		// the pair is the character depends on the order.
		bool little = encoding == 1 && text_len >= 2 && text[0] == 0xFF && text[1] == 0xFE;
		if (encoding == 1 && text_len >= 2) {
			text += 2;
			text_len -= 2;
		}
		for (uint32_t i = 0; i + 1 < text_len && w + 1 < out_size; i += 2) {
			uint8_t c = little ? text[i] : text[i + 1];
			uint8_t hi = little ? text[i + 1] : text[i];
			if (!c && !hi) {
				break;
			}
			// Above U+00FF there is no single byte to write; a question mark
			// says "a character was here" instead of writing half of one.
			out[w++] = hi ? '?' : (char)c;
		}
	} else {
		for (uint32_t i = 0; i < text_len && w + 1 < out_size; i++) {
			if (!text[i]) {
				break;
			}
			out[w++] = (char)text[i];
		}
	}
	out[w] = '\0';

	// Trailing whitespace is common in tags written by hand.
	while (w > 0 && (out[w - 1] == ' ' || out[w - 1] == '\r' || out[w - 1] == '\n')) {
		out[--w] = '\0';
	}
}

// The TIT2 inside a CHAP's payload, after the fixed part.
static void read_chapter_title(const uint8_t *payload, uint32_t len, bool syncsafe_frames, char *out,
							   size_t out_size) {
	out[0] = '\0';

	// The element id, which is a NUL-terminated string nobody needs.
	uint32_t at = 0;
	while (at < len && payload[at]) {
		at++;
	}
	at++; // the NUL itself
	// Four times four bytes of times and offsets.
	if (at + 16 > len) {
		return;
	}
	at += 16;

	while (at + 10 <= len) {
		const uint8_t *frame = payload + at;
		uint32_t size = syncsafe_frames ? syncsafe32(frame + 4) : be32(frame + 4);
		at += 10;
		if (size == 0 || size > len - at) {
			return; // a claim bigger than what is left: stop rather than read on
		}
		if (memcmp(frame, "TIT2", 4) == 0) {
			read_text(payload + at, size, out, out_size);
			return;
		}
		at += size;
	}
}

int id3chap_read(const char *path, id3chap_t *out, int max) {
	if (!path || !path[0]) {
		return 0;
	}
	FILE *f = fopen(path, "rb");
	if (!f) {
		return 0;
	}

	uint8_t header[10];
	if (fread(header, 1, sizeof(header), f) != sizeof(header) || memcmp(header, "ID3", 3) != 0) {
		fclose(f);
		return 0;
	}
	uint8_t version = header[3];
	uint32_t tag_size = syncsafe32(header + 6);
	if (!tag_size || tag_size > MAX_TAG) {
		fclose(f);
		return 0;
	}

	uint8_t *tag = malloc(tag_size);
	if (!tag) {
		fclose(f);
		return 0;
	}
	size_t got = fread(tag, 1, tag_size, f);
	fclose(f);
	if (got == 0) {
		free(tag);
		return 0;
	}
	tag_size = (uint32_t)got; // a truncated file is read as far as it goes

	// An extended header, when the flags say so: its own length comes first.
	uint32_t at = 0;
	if (header[5] & 0x40) {
		if (tag_size < 4) {
			free(tag);
			return 0;
		}
		uint32_t ext = version >= 4 ? syncsafe32(tag) : be32(tag) + 4u;
		if (ext >= tag_size) {
			free(tag);
			return 0;
		}
		at = ext;
	}

	bool syncsafe_frames = version >= 4;
	int found = 0;

	while (at + 10 <= tag_size && found < MAX_CHAPTERS) {
		const uint8_t *frame = tag + at;
		if (!frame[0]) {
			break; // padding: the rest of the tag is zeroes
		}
		uint32_t size = syncsafe_frames ? syncsafe32(frame + 4) : be32(frame + 4);
		at += 10;
		if (size == 0 || size > tag_size - at) {
			break;
		}

		if (memcmp(frame, "CHAP", 4) == 0) {
			// The start time sits after the element id.
			uint32_t p = 0;
			while (p < size && tag[at + p]) {
				p++;
			}
			p++;
			if (p + 16 <= size) {
				if (out && found < max) {
					out[found].start = (double)be32(tag + at + p) / 1000.0;
					read_chapter_title(tag + at, size, syncsafe_frames, out[found].title, sizeof(out[found].title));
				}
				found++;
				if (!out) {
					break; // only asked whether there are any
				}
			}
		}
		at += size;
	}

	free(tag);
	return found;
}

bool id3chap_present(const char *path) { return id3chap_read(path, NULL, 0) > 0; }

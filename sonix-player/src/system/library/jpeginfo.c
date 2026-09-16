#include "jpeginfo.h"

#include <stdio.h>
#include <string.h>
#include <strings.h> // strcasecmp

// The frame header is near the front of every JPEG -- after the application
// and quantisation segments, before the first scan -- so a few kilobytes is
// more than enough and a file that has not declared itself by then is one this
// player is not going to show anyway.
#define JPEG_HEAD_BYTES 8192

// Start Of Frame markers. The progressive ones are the second column: SOF2 and
// its arithmetic and hierarchical relatives, all of which the scaled decoder
// refuses. Everything else in the C0..CF block that is not a frame header --
// DHT (C4), JPG (C8), DAC (CC) -- is skipped like an ordinary segment.
static bool is_frame_marker(unsigned char m) {
	if (m == 0xC4 || m == 0xC8 || m == 0xCC) {
		return false;
	}
	return m >= 0xC0 && m <= 0xCF;
}

static bool is_progressive_marker(unsigned char m) {
	return m == 0xC2 || m == 0xC6 || m == 0xCA || m == 0xCE;
}

bool jpeg_info_parse(const unsigned char *data, size_t size, jpeg_info_t *out) {
	if (!data || !out) {
		return false;
	}
	memset(out, 0, sizeof(*out));

	if (size < 4 || data[0] != 0xFF || data[1] != 0xD8) {
		return false; // no SOI: not a JPEG
	}

	size_t i = 2;
	while (i + 1 < size) {
		// Segments are separated by fill bytes in some encoders' output: any
		// number of 0xFF may precede the marker itself.
		if (data[i] != 0xFF) {
			return false; // out of step with the segment structure
		}
		while (i < size && data[i] == 0xFF) {
			i++;
		}
		if (i >= size) {
			return false;
		}
		unsigned char marker = data[i++];

		// The standalone markers carry no length: restart intervals, SOI, EOI
		// and TEM. Nothing follows them to skip.
		if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD9)) {
			continue;
		}
		if (marker == 0xDA) {
			return false; // the scan begins and no frame header was found
		}

		if (i + 1 >= size) {
			return false;
		}
		unsigned int length = ((unsigned int)data[i] << 8) | data[i + 1];
		if (length < 2) {
			return false;
		}

		if (is_frame_marker(marker)) {
			// SOFn payload: length(2), precision(1), height(2), width(2).
			if (length < 7 || i + 6 >= size) {
				return false;
			}
			out->height = ((int)data[i + 3] << 8) | data[i + 4];
			out->width = ((int)data[i + 5] << 8) | data[i + 6];
			out->progressive = is_progressive_marker(marker);
			return out->width > 0 && out->height > 0;
		}

		i += length; // the length counts itself, so this lands on the next marker
	}
	return false;
}

bool jpeg_info_read(const char *path, jpeg_info_t *out) {
	if (out) {
		memset(out, 0, sizeof(*out));
	}
	if (!path || !path[0] || !out) {
		return false;
	}

	FILE *f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	unsigned char head[JPEG_HEAD_BYTES];
	size_t got = fread(head, 1, sizeof(head), f);
	fclose(f);

	return jpeg_info_parse(head, got, out);
}

bool jpeg_info_has_extension(const char *name) {
	if (!name) {
		return false;
	}
	const char *dot = strrchr(name, '.');
	if (!dot) {
		return false;
	}
	return strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0;
}

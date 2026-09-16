#include "metadata.h"

#include "src/system/library/cue.h"
#include "src/system/decode/decode.h"
#include "src/system/decode/dr_flac.h"
#include "src/system/decode/mp4.h"
#include "src/system/decode/stb_vorbis_decl.h"
#include "src/system/decode/wavpackdec.h"
#include "src/system/core/utils.h"

#include <opusfile.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Standard ID3v1 genre list, used to resolve numeric genre references from
// both ID3v1 tags and ID3v2 TCON frames written in the old "(N)" style.
static const char *id3v1_genres[] = {
	"Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge",
	"Hip-Hop", "Jazz", "Metal", "New Age", "Oldies", "Other", "Pop", "R&B",
	"Rap", "Reggae", "Rock", "Techno", "Industrial", "Alternative", "Ska",
	"Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient",
	"Trip-Hop", "Vocal", "Jazz+Funk", "Fusion", "Trance", "Classical",
	"Instrumental", "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise",
	"AlternRock", "Bass", "Soul", "Punk", "Space", "Meditative",
	"Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic", "Darkwave",
	"Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream",
	"Southern Rock", "Comedy", "Cult", "Gangsta", "Top 40", "Christian Rap",
	"Pop/Funk", "Jungle", "Native American", "Cabaret", "New Wave",
	"Psychedelic", "Rave", "Showtunes", "Trailer", "Lo-Fi", "Tribal",
	"Acid Punk", "Acid Jazz", "Polka", "Retro", "Musical", "Rock & Roll",
	"Hard Rock",
};
#define ID3V1_GENRE_COUNT (sizeof(id3v1_genres) / sizeof(id3v1_genres[0]))

// Copies a null-terminated string into a fixed-size destination, truncating
// safely. Avoids -Wformat-truncation noise from snprintf("%s", ...) when the
// compiler can't prove the source is short enough.
static void copy_bounded(char *dst, size_t dst_size, const char *src) {
	if (dst_size == 0)
		return;
	size_t len = strlen(src);
	size_t copy_len = len < dst_size - 1 ? len : dst_size - 1;
	memcpy(dst, src, copy_len);
	dst[copy_len] = '\0';
}

// ---------------------------------------------------------------------------
// Vorbis comment parsing, shared by FLAC and OGG Vorbis
// ---------------------------------------------------------------------------

// ReplayGain arrives as text in every format that carries it: "-7.50 dB" for a
// gain, "0.988525" for a peak. One place to turn either into a number, so the
// four parsers below agree about what counts as present.
static bool replaygain_field(song_metadata_t *out, const char *key, const char *value, size_t value_len) {
	char buf[32];
	size_t n = value_len < sizeof(buf) - 1 ? value_len : sizeof(buf) - 1;
	memcpy(buf, value, n);
	buf[n] = '\0';

	char *end = NULL;
	double number = strtod(buf, &end);
	if (end == buf) {
		return false; // not a number at all
	}

	if (strcmp(key, "REPLAYGAIN_TRACK_GAIN") == 0) {
		out->track_gain_db = (float)number;
		out->has_track_gain = true;
	} else if (strcmp(key, "REPLAYGAIN_ALBUM_GAIN") == 0) {
		out->album_gain_db = (float)number;
		out->has_album_gain = true;
	} else if (strcmp(key, "REPLAYGAIN_TRACK_PEAK") == 0) {
		out->track_peak = (float)number;
	} else if (strcmp(key, "REPLAYGAIN_ALBUM_PEAK") == 0) {
		out->album_peak = (float)number;
	} else {
		return false;
	}
	return true;
}

// Applies a single "KEY=VALUE" Vorbis comment string (not null-terminated) to
// the relevant metadata field.
static void apply_vorbis_comment(song_metadata_t *out, const char *comment, size_t len) {
	const char *eq = memchr(comment, '=', len);
	if (!eq)
		return;

	size_t key_len = (size_t)(eq - comment);
	size_t value_len = len - key_len - 1;
	const char *value = eq + 1;

	char key[32];
	if (key_len >= sizeof(key))
		return;
	for (size_t i = 0; i < key_len; i++) {
		key[i] = (char)toupper((unsigned char)comment[i]);
	}
	key[key_len] = '\0';

	char *dst = NULL;
	size_t dst_size = 0;
	if (strcmp(key, "TITLE") == 0) {
		dst = out->title;
		dst_size = sizeof(out->title);
	} else if (strcmp(key, "ARTIST") == 0) {
		dst = out->artist;
		dst_size = sizeof(out->artist);
	} else if (strcmp(key, "ALBUMARTIST") == 0 || strcmp(key, "ALBUM ARTIST") == 0) {
		dst = out->album_artist;
		dst_size = sizeof(out->album_artist);
	} else if (strcmp(key, "ALBUM") == 0) {
		dst = out->album;
		dst_size = sizeof(out->album);
	} else if (strcmp(key, "GENRE") == 0) {
		dst = out->genre;
		dst_size = sizeof(out->genre);
	} else if (strcmp(key, "TRACKNUMBER") == 0) {
		char num_buf[16];
		size_t n = value_len < sizeof(num_buf) - 1 ? value_len : sizeof(num_buf) - 1;
		memcpy(num_buf, value, n);
		num_buf[n] = '\0';
		out->track_number = (int)strtol(num_buf, NULL, 10);
		return;
	} else if (strcmp(key, "DATE") == 0) {
		char num_buf[16];
		size_t n = value_len < sizeof(num_buf) - 1 ? value_len : sizeof(num_buf) - 1;
		memcpy(num_buf, value, n);
		num_buf[n] = '\0';
		out->year = (int)strtol(num_buf, NULL, 10);
		return;
	} else if (strncmp(key, "REPLAYGAIN_", 11) == 0) {
		replaygain_field(out, key, value, value_len);
		return;
	} else {
		return;
	}

	size_t copy_len = value_len < dst_size - 1 ? value_len : dst_size - 1;
	memcpy(dst, value, copy_len);
	dst[copy_len] = '\0';
}

static void flac_meta_callback(void *pUserData, drflac_metadata *pMetadata) {
	if (pMetadata->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT)
		return;

	song_metadata_t *out = (song_metadata_t *)pUserData;

	drflac_vorbis_comment_iterator iter;
	drflac_init_vorbis_comment_iterator(&iter, pMetadata->data.vorbis_comment.commentCount,
										 pMetadata->data.vorbis_comment.pComments);

	drflac_uint32 comment_len;
	const char *comment;
	while ((comment = drflac_next_vorbis_comment(&iter, &comment_len)) != NULL) {
		apply_vorbis_comment(out, comment, comment_len);
	}
}

static void read_flac_metadata(const char *filepath, song_metadata_t *out) {
	drflac *flac = drflac_open_file_with_metadata(filepath, flac_meta_callback, out, NULL);
	if (flac) {
		drflac_close(flac);
	}
}

static void read_ogg_metadata(const char *filepath, song_metadata_t *out) {
	int error = 0;
	stb_vorbis *vorbis = stb_vorbis_open_filename(filepath, &error, NULL);
	if (!vorbis)
		return;

	stb_vorbis_comment comment = stb_vorbis_get_comment(vorbis);
	for (int i = 0; i < comment.comment_list_length; i++) {
		apply_vorbis_comment(out, comment.comment_list[i], strlen(comment.comment_list[i]));
	}

	stb_vorbis_close(vorbis);
}

// ---------------------------------------------------------------------------
// MP3 (ID3v1 / ID3v2) parsing
// ---------------------------------------------------------------------------

static uint32_t syncsafe_to_uint32(const uint8_t b[4]) {
	return ((uint32_t)(b[0] & 0x7F) << 21) | ((uint32_t)(b[1] & 0x7F) << 14) |
		   ((uint32_t)(b[2] & 0x7F) << 7) | (uint32_t)(b[3] & 0x7F);
}

static uint32_t plain_to_uint32(const uint8_t b[4]) {
	return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

// Converts UTF-16 (LE or BE) text to UTF-8, stopping at a null terminator.
static void utf16_to_utf8(const uint8_t *data, size_t len, bool big_endian, char *out, size_t out_size) {
	size_t oi = 0;
	size_t i = 0;
	while (i + 1 < len && oi + 4 < out_size) {
		uint32_t unit = big_endian ? ((uint32_t)data[i] << 8 | data[i + 1]) : ((uint32_t)data[i + 1] << 8 | data[i]);
		i += 2;
		if (unit == 0)
			break;

		uint32_t cp = unit;
		if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < len) {
			uint32_t unit2 = big_endian ? ((uint32_t)data[i] << 8 | data[i + 1]) : ((uint32_t)data[i + 1] << 8 | data[i]);
			if (unit2 >= 0xDC00 && unit2 <= 0xDFFF) {
				cp = 0x10000 + ((unit - 0xD800) << 10) + (unit2 - 0xDC00);
				i += 2;
			}
		}

		if (cp < 0x80) {
			out[oi++] = (char)cp;
		} else if (cp < 0x800) {
			out[oi++] = (char)(0xC0 | (cp >> 6));
			out[oi++] = (char)(0x80 | (cp & 0x3F));
		} else if (cp < 0x10000) {
			out[oi++] = (char)(0xE0 | (cp >> 12));
			out[oi++] = (char)(0x80 | ((cp >> 6) & 0x3F));
			out[oi++] = (char)(0x80 | (cp & 0x3F));
		} else {
			out[oi++] = (char)(0xF0 | (cp >> 18));
			out[oi++] = (char)(0x80 | ((cp >> 12) & 0x3F));
			out[oi++] = (char)(0x80 | ((cp >> 6) & 0x3F));
			out[oi++] = (char)(0x80 | (cp & 0x3F));
		}
	}
	out[oi] = '\0';
}

// ---------------------------------------------------------------------------
// Windows-1251 in a Latin-1 frame
//
// An ID3v2 frame that declares encoding 0x00 says its bytes are ISO-8859-1, and
// most taggers tell the truth. Russian ones often do not: they write
// Windows-1251 and leave the encoding byte at zero, as does every ID3v1 tag,
// which has no encoding byte at all. Read as Latin-1, "Алиса" comes out as
// "Àëèñà" -- still in the database and still sorting, but under A, in
// characters nobody is looking for.
//
// The two encodings cannot be told apart by the bytes alone, so this goes by
// how they are arranged. A Latin-1 name spends its high bytes one at a time --
// the e in "Café", the u in "Müller" -- with letters either side. Cyrillic
// written as 1251 is high bytes all the way through a word. So a run of two or
// more Cyrillic-range letters with no ASCII letter or digit touching either end
// is 1251; anything else is left as Latin-1.
// ---------------------------------------------------------------------------

// The half of the code page that is not simply А-я: punctuation, the Ukrainian
// and Belarusian letters, and the currency signs. 0 means "not in the page".
static const uint16_t CP1251_HIGH[64] = {
	0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, // 80-87
	0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F, // 88-8F
	0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, // 90-97
	0x0000, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F, // 98-9F
	0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, // A0-A7
	0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407, // A8-AF
	0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7, // B0-B7
	0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457, // B8-BF
};

// One byte of the code page as a Unicode code point.
static uint32_t cp1251_code(uint8_t c) {
	if (c < 0x80) {
		return c;
	}
	if (c >= 0xC0) {
		// А-Я then а-я, in one contiguous stretch each.
		return (c < 0xE0) ? (uint32_t)(0x0410 + (c - 0xC0)) : (uint32_t)(0x0430 + (c - 0xE0));
	}
	uint16_t mapped = CP1251_HIGH[c - 0x80];
	return mapped ? mapped : (uint32_t)c;
}

// A Cyrillic LETTER in the code page -- what a word is made of, as opposed to
// the quotes and the currency signs that share the upper half.
static bool cp1251_is_letter(uint8_t c) { return c >= 0xC0 || c == 0xA8 || c == 0xB8; }

static bool ascii_alnum(uint8_t c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// A whole word of high bytes, with no ASCII letter touching either end, is the
// tell. An accent inside a Latin word always has letters beside it -- the u in
// "Müller", the o in "Motörhead", the three umlauts in "ÄÖÜX" -- while a
// Cyrillic word written as 1251 runs from one space to the next without an
// ASCII letter in sight. Two letters is enough of a word; the high bytes that
// are punctuation in both code pages are neither evidence nor objection.
static bool looks_like_cp1251(const uint8_t *data, size_t len) {
	size_t i = 0;
	while (i < len && data[i]) {
		if (!cp1251_is_letter(data[i])) {
			i++;
			continue;
		}

		size_t start = i;
		while (i < len && data[i] && cp1251_is_letter(data[i])) {
			i++;
		}

		bool free_left = start == 0 || !ascii_alnum(data[start - 1]);
		bool free_right = i >= len || data[i] == 0 || !ascii_alnum(data[i]);
		if (i - start >= 2 && free_left && free_right) {
			return true;
		}
	}
	return false;
}

// The same bytes as UTF-8, read through the code page.
static void cp1251_to_utf8(const uint8_t *data, size_t len, char *out, size_t out_size) {
	size_t oi = 0;
	for (size_t i = 0; i < len && data[i]; i++) {
		uint32_t code = cp1251_code(data[i]);
		if (code < 0x80) {
			if (oi + 1 >= out_size) {
				break;
			}
			out[oi++] = (char)code;
		} else if (code < 0x800) {
			if (oi + 2 >= out_size) {
				break;
			}
			out[oi++] = (char)(0xC0 | (code >> 6));
			out[oi++] = (char)(0x80 | (code & 0x3F));
		} else {
			if (oi + 3 >= out_size) {
				break;
			}
			out[oi++] = (char)(0xE0 | (code >> 12));
			out[oi++] = (char)(0x80 | ((code >> 6) & 0x3F));
			out[oi++] = (char)(0x80 | (code & 0x3F));
		}
	}
	out[oi] = '\0';
}

// Decodes ID3v2 frame text (encoding byte 0x00-0x03) into UTF-8.
static void id3_decode_text(uint8_t encoding, const uint8_t *data, size_t len, char *out, size_t out_size) {
	if (out_size == 0)
		return;
	out[0] = '\0';
	if (len == 0)
		return;

	switch (encoding) {
	case 0x00: { // ISO-8859-1 (Latin-1), or Windows-1251 pretending to be it
		if (looks_like_cp1251(data, len)) {
			cp1251_to_utf8(data, len, out, out_size);
			break;
		}
		size_t oi = 0;
		for (size_t i = 0; i < len && oi + 2 < out_size; i++) {
			uint8_t c = data[i];
			if (c == 0)
				break;
			if (c < 0x80) {
				out[oi++] = (char)c;
			} else {
				out[oi++] = (char)(0xC0 | (c >> 6));
				out[oi++] = (char)(0x80 | (c & 0x3F));
			}
		}
		out[oi] = '\0';
		break;
	}
	case 0x01: { // UTF-16 with BOM
		bool big_endian = false;
		size_t offset = 0;
		if (len >= 2) {
			if (data[0] == 0xFE && data[1] == 0xFF) {
				big_endian = true;
				offset = 2;
			} else if (data[0] == 0xFF && data[1] == 0xFE) {
				big_endian = false;
				offset = 2;
			}
		}
		utf16_to_utf8(data + offset, len - offset, big_endian, out, out_size);
		break;
	}
	case 0x02: // UTF-16BE, no BOM
		utf16_to_utf8(data, len, true, out, out_size);
		break;
	case 0x03: // UTF-8
	default: {
		size_t copy_len = len < out_size - 1 ? len : out_size - 1;
		size_t actual = 0;
		while (actual < copy_len && data[actual] != 0)
			actual++;
		memcpy(out, data, actual);
		out[actual] = '\0';
		break;
	}
	}
}

// ID3v2 TCON content is either plain text, or the old-style "(N)" / "(N)Text"
// format referencing an ID3v1 genre index.
static void resolve_tcon_genre(const char *raw, char *out, size_t out_size) {
	if (raw[0] == '(') {
		const char *end = strchr(raw, ')');
		if (end) {
			char num_buf[8];
			size_t num_len = (size_t)(end - raw - 1);
			if (num_len > 0 && num_len < sizeof(num_buf)) {
				memcpy(num_buf, raw + 1, num_len);
				num_buf[num_len] = '\0';

				char *endptr;
				long idx = strtol(num_buf, &endptr, 10);
				if (*endptr == '\0' && idx >= 0) {
					const char *rest = end + 1;
					if (*rest != '\0') {
						copy_bounded(out, out_size, rest);
						return;
					}
					if ((size_t)idx < ID3V1_GENRE_COUNT) {
						copy_bounded(out, out_size, id3v1_genres[idx]);
						return;
					}
				}
			}
		}
	}
	copy_bounded(out, out_size, raw);
}

// The largest text frame worth reading. A very long title of a few thousand
// characters fits comfortably; cover art does not, but it is not wanted here
// (APIC frames are skipped without being read), and a size larger than this
// only means the file declares something it is not.
#define ID3_MAX_TEXT_FRAME (512u * 1024u)

// How many frames to look at before giving up. A tag holds a dozen; thousands
// mean the walk has wandered into random data.
#define ID3_MAX_FRAMES 256

// Reads an ID3v2 tag at the start of the file, if present. Returns true if a
// tag was found (regardless of whether any wanted frames were in it).
static bool read_id3v2(FILE *f, song_metadata_t *out) {
	uint8_t header[10];
	if (fread(header, 1, 10, f) != 10)
		return false;
	if (memcmp(header, "ID3", 3) != 0)
		return false;

	uint8_t major_version = header[3];
	uint8_t flags = header[5];
	uint32_t tag_size = syncsafe_to_uint32(&header[6]);

	long tag_data_start = ftell(f);
	long tag_end = tag_data_start + (long)tag_size;

	if (flags & 0x40) { // extended header present
		uint8_t ext_size_bytes[4];
		if (fread(ext_size_bytes, 1, 4, f) != 4)
			return true;
		uint32_t ext_size = (major_version >= 4) ? syncsafe_to_uint32(ext_size_bytes) : plain_to_uint32(ext_size_bytes);
		long remaining = (major_version >= 4) ? (long)ext_size - 4 : (long)ext_size;
		if (remaining > 0)
			fseek(f, remaining, SEEK_CUR);
	}

	// ID3v2.2 is a different shape: six-byte frame headers with three-character
	// identifiers and a three-byte size, where 2.3 and 2.4 have ten and four.
	// Walking a 2.2 tag with the ten-byte reader takes the size from the middle
	// of the next frame's id, which stops the loop at the first frame.
	bool v22 = major_version == 2;
	int header_size = v22 ? 6 : 10;

	for (int frames = 0; frames < ID3_MAX_FRAMES && ftell(f) < tag_end - header_size; frames++) {
		long frame_at = ftell(f);
		uint8_t frame_header[10];
		if (fread(frame_header, 1, (size_t)header_size, f) != (size_t)header_size)
			break;
		if (frame_header[0] == 0) // padding reached
			break;

		char frame_id[5] = {0};
		uint32_t frame_size;
		if (v22) {
			memcpy(frame_id, frame_header, 3);
			frame_size = ((uint32_t)frame_header[3] << 16) | ((uint32_t)frame_header[4] << 8) | frame_header[5];
		} else {
			memcpy(frame_id, frame_header, 4);
			frame_size =
				(major_version >= 4) ? syncsafe_to_uint32(&frame_header[4]) : plain_to_uint32(&frame_header[4]);
		}

		if (frame_size == 0 || (long)frame_size > tag_end - ftell(f))
			break;

		// The allocation cap. The length field is 28 bits, so a damaged (or
		// merely badly written) file can declare a frame of two hundred and
		// fifty megabytes, and until here the only bound was the tag size,
		// which the same file declares. No title, artist or ReplayGain reaches
		// half a megabyte: past that the frame is skipped instead of allocated
		// for. The jump to the next frame uses an absolute position computed
		// from the start of this one, so the position always moves forward
		// whatever happened in between.
		long next_frame = frame_at + header_size + (long)frame_size;
		if (next_frame <= frame_at) {
			break; // arithmetic that does not add up: stop instead of spinning
		}

		if (frame_size > ID3_MAX_TEXT_FRAME) {
			fseek(f, next_frame, SEEK_SET);
			continue;
		}

		// The 2.2 identifiers are the same fields under their older names; they
		// are translated here so that everything below reads one set.
		if (v22) {
			static const char *const V22_MAP[][2] = {{"TT2", "TIT2"}, {"TP1", "TPE1"}, {"TP2", "TPE2"},
													 {"TAL", "TALB"}, {"TCO", "TCON"}, {"TRK", "TRCK"},
													 {"TYE", "TYER"}, {"TXX", "TXXX"}};
			for (size_t i = 0; i < sizeof(V22_MAP) / sizeof(V22_MAP[0]); i++) {
				if (strcmp(frame_id, V22_MAP[i][0]) == 0) {
					snprintf(frame_id, sizeof(frame_id), "%s", V22_MAP[i][1]);
					break;
				}
			}
		}

		bool wanted = strcmp(frame_id, "TIT2") == 0 || strcmp(frame_id, "TPE1") == 0 ||
					  strcmp(frame_id, "TPE2") == 0 || strcmp(frame_id, "TALB") == 0 || strcmp(frame_id, "TCON") == 0 ||
					  strcmp(frame_id, "TRCK") == 0 || strcmp(frame_id, "TYER") == 0 ||
					  strcmp(frame_id, "TDRC") == 0 || strcmp(frame_id, "TXXX") == 0;

		if (!wanted) {
			fseek(f, next_frame, SEEK_SET);
			continue;
		}

		uint8_t *buf = malloc(frame_size);
		if (!buf)
			break;
		if (fread(buf, 1, frame_size, f) != frame_size) {
			free(buf);
			break;
		}

		uint8_t encoding = buf[0];

		// TXXX is a pair, not a value: an encoded description, a terminator,
		// then the text. It is where every tagger writes ReplayGain on an MP3.
		if (strcmp(frame_id, "TXXX") == 0) {
			// Only the single-byte encodings are handled here. UTF-16 TXXX
			// ReplayGain exists in theory and in no tagger anyone uses, and
			// guessing at a two-byte terminator to find out would be worse
			// than not reading it.
			if (encoding == 0 || encoding == 3) {
				const char *text = (const char *)buf + 1;
				size_t left = frame_size - 1;
				size_t desc_len = strnlen(text, left);
				if (desc_len < left) {
					char key[40];
					size_t k = desc_len < sizeof(key) - 1 ? desc_len : sizeof(key) - 1;
					for (size_t i = 0; i < k; i++) {
						key[i] = (char)toupper((unsigned char)text[i]);
					}
					key[k] = '\0';
					replaygain_field(out, key, text + desc_len + 1, left - desc_len - 1);
				}
			}
			free(buf);
			continue;
		}

		char decoded[512];
		id3_decode_text(encoding, buf + 1, frame_size - 1, decoded, sizeof(decoded));
		free(buf);

		if (strcmp(frame_id, "TIT2") == 0) {
			copy_bounded(out->title, sizeof(out->title), decoded);
		} else if (strcmp(frame_id, "TPE1") == 0) {
			copy_bounded(out->artist, sizeof(out->artist), decoded);
		} else if (strcmp(frame_id, "TPE2") == 0) {
			copy_bounded(out->album_artist, sizeof(out->album_artist), decoded);
		} else if (strcmp(frame_id, "TALB") == 0) {
			copy_bounded(out->album, sizeof(out->album), decoded);
		} else if (strcmp(frame_id, "TCON") == 0) {
			resolve_tcon_genre(decoded, out->genre, sizeof(out->genre));
		} else if (strcmp(frame_id, "TRCK") == 0) {
			out->track_number = (int)strtol(decoded, NULL, 10);
		} else if (strcmp(frame_id, "TYER") == 0 || strcmp(frame_id, "TDRC") == 0) {
			if (out->year == 0)
				out->year = (int)strtol(decoded, NULL, 10);
		}
	}

	return true;
}

// Copies up to `max_src_len` raw ISO-8859-1 bytes from `src`, trimming
// trailing spaces, and decodes them into `dst` as UTF-8.
static void set_field_bounded(char *dst, size_t dst_size, const char *src, size_t max_src_len) {
	size_t len = 0;
	while (len < max_src_len && src[len] != '\0')
		len++;
	while (len > 0 && src[len - 1] == ' ')
		len--;
	id3_decode_text(0x00, (const uint8_t *)src, len, dst, dst_size);
}

// Reads the trailing 128-byte ID3v1 tag, filling only fields left empty by
// a preceding ID3v2 pass (if any).
static void read_id3v1(FILE *f, song_metadata_t *out) {
	if (fseek(f, -128, SEEK_END) != 0)
		return;

	uint8_t tag[128];
	if (fread(tag, 1, 128, f) != 128)
		return;
	if (memcmp(tag, "TAG", 3) != 0)
		return;

	if (out->title[0] == '\0')
		set_field_bounded(out->title, sizeof(out->title), (const char *)tag + 3, 30);
	if (out->artist[0] == '\0')
		set_field_bounded(out->artist, sizeof(out->artist), (const char *)tag + 33, 30);
	if (out->album[0] == '\0')
		set_field_bounded(out->album, sizeof(out->album), (const char *)tag + 63, 30);

	if (out->year == 0) {
		char year_buf[5] = {0};
		memcpy(year_buf, tag + 93, 4);
		out->year = (int)strtol(year_buf, NULL, 10);
	}

	if (out->genre[0] == '\0') {
		uint8_t genre_idx = tag[127];
		if (genre_idx < ID3V1_GENRE_COUNT) {
			snprintf(out->genre, sizeof(out->genre), "%s", id3v1_genres[genre_idx]);
		}
	}
}

static void read_mp3_metadata(const char *filepath, song_metadata_t *out) {
	FILE *f = fopen(filepath, "rb");
	if (!f)
		return;

	read_id3v2(f, out);
	read_id3v1(f, out);

	fclose(f);
}

// ---------------------------------------------------------------------------
// DSD
//
// Neither container invents a tag format of its own. A .dsf points at an
// ordinary ID3v2 tag with the eight bytes at offset 20 of its header -- the
// same tag an MP3 carries, so the same reader handles it -- and a .dff spells
// artist and title out in DIIN sub-chunks.
// ---------------------------------------------------------------------------

static void read_dsf_metadata(FILE *f, song_metadata_t *out) {
	unsigned char head[28];
	if (fread(head, 1, 28, f) != 28 || memcmp(head, "DSD ", 4) != 0) {
		return;
	}
	unsigned long long tag_at = 0;
	for (int i = 7; i >= 0; i--) {
		tag_at = (tag_at << 8) | head[20 + i];
	}
	// Zero means the file simply has no tag, which is common and not an error.
	if (tag_at == 0) {
		return;
	}
	// The offset is 64-bit and comes from the file, while `long` here is 32-bit.
	// Unchecked, a monstrous value truncates and sends the read to a random
	// point in the file, where anything resembling "ID3" is taken for a tag.
	if (fseeko(f, 0, SEEK_END) != 0) {
		return;
	}
	off_t size = ftello(f);
	if (size <= 0 || tag_at >= (unsigned long long)size) {
		return;
	}
	if (fseeko(f, (off_t)tag_at, SEEK_SET) != 0) {
		return;
	}
	read_id3v2(f, out);
}

static void read_dff_metadata(FILE *f, song_metadata_t *out) {
	unsigned char head[16];
	if (fread(head, 1, 16, f) != 16 || memcmp(head, "FRM8", 4) != 0) {
		return;
	}

	// The declared lengths are 64-bit and come off a card, so they are kept
	// 64-bit here and checked against the real end of the file: truncated into
	// a 32-bit position they can turn negative and send a walk backwards.
	if (fseeko(f, 0, SEEK_END) != 0) {
		return;
	}
	off_t file_end = ftello(f);
	if (file_end < 16) {
		return;
	}

	// DIIN sits at the top level after the audio, so the walk is flat: step
	// over every chunk (each padded to an even length, which the size field
	// does not include) and descend only into DIIN itself.
	off_t pos = 16;
	for (int guard = 0; guard < 64; guard++) {
		unsigned char ch[12];
		if (fseeko(f, pos, SEEK_SET) != 0 || fread(ch, 1, 12, f) != 12) {
			return;
		}
		unsigned long long size = 0;
		for (int i = 0; i < 8; i++) {
			size = (size << 8) | ch[4 + i];
		}
		off_t body = pos + 12;
		if (size > (unsigned long long)(file_end - body)) {
			return;
		}

		if (memcmp(ch, "DIIN", 4) == 0) {
			off_t sub = body;
			off_t end = body + (off_t)size;
			for (int sub_guard = 0; sub_guard < 64 && sub + 12 <= end; sub_guard++) {
				unsigned char sh[12];
				if (fseeko(f, sub, SEEK_SET) != 0 || fread(sh, 1, 12, f) != 12) {
					return;
				}
				unsigned long long ssize = 0;
				for (int i = 0; i < 8; i++) {
					ssize = (ssize << 8) | sh[4 + i];
				}
				if (ssize > (unsigned long long)(end - (sub + 12))) {
					return;
				}
				bool artist = memcmp(sh, "DIAR", 4) == 0;
				bool title = memcmp(sh, "DITI", 4) == 0;
				if ((artist || title) && ssize >= 4 && ssize < 4096) {
					// Four bytes of length, then the text, not terminated.
					unsigned char len_be[4];
					if (fread(len_be, 1, 4, f) == 4) {
						unsigned long len = ((unsigned long)len_be[0] << 24) | ((unsigned long)len_be[1] << 16) |
											((unsigned long)len_be[2] << 8) | len_be[3];
						if (len > ssize - 4) {
							len = (unsigned long)ssize - 4;
						}
						char text[256] = {0};
						if (len >= sizeof(text)) {
							len = sizeof(text) - 1;
						}
						if (fread(text, 1, len, f) == len) {
							copy_bounded(artist ? out->artist : out->title,
										 artist ? sizeof(out->artist) : sizeof(out->title), text);
						}
					}
				}
				sub += 12 + (off_t)ssize + (off_t)(ssize & 1);
			}
			return;
		}

		pos = body + (off_t)size + (off_t)(size & 1);
	}
}

static void read_dsd_metadata(const char *filepath, song_metadata_t *out) {
	FILE *f = fopen(filepath, "rb");
	if (!f) {
		return;
	}
	if (has_extension(filepath, ".dsf")) {
		read_dsf_metadata(f, out);
	} else {
		read_dff_metadata(f, out);
	}
	fclose(f);
}

// ---------------------------------------------------------------------------
// WAV (RIFF LIST/INFO chunk) parsing
// ---------------------------------------------------------------------------

static void read_wav_metadata(const char *filepath, song_metadata_t *out) {
	FILE *f = fopen(filepath, "rb");
	if (!f)
		return;

	char riff_header[12];
	if (fread(riff_header, 1, 12, f) != 12 || memcmp(riff_header, "RIFF", 4) != 0 ||
		memcmp(riff_header + 8, "WAVE", 4) != 0) {
		fclose(f);
		return;
	}

	// The real end of the file. Chunk lengths are declared by the file and are
	// unsigned 32-bit, while `long` here is signed 32-bit: a length past two
	// gigabytes turns negative, so the next position lands before the current
	// one and the loop rereads the same bytes forever.
	if (fseeko(f, 0, SEEK_END) != 0) {
		fclose(f);
		return;
	}
	off_t file_end = ftello(f);
	if (fseeko(f, 12, SEEK_SET) != 0) {
		fclose(f);
		return;
	}

	struct {
		char id[4];
		uint32_t size;
	} chunk;

	while (fread(&chunk, 1, sizeof(chunk), f) == sizeof(chunk)) {
		off_t chunk_data_start = ftello(f);
		if (chunk_data_start < 0) {
			break;
		}

		// The declared length has to fit inside the file: if it does not, the
		// file is lying and there is nothing further on to read.
		if ((off_t)chunk_data_start + (off_t)chunk.size > file_end) {
			break;
		}

		if (memcmp(chunk.id, "LIST", 4) == 0 && chunk.size >= 4) {
			char list_type[4];
			if (fread(list_type, 1, 4, f) == 4 && memcmp(list_type, "INFO", 4) == 0) {
				off_t list_end = chunk_data_start + (off_t)chunk.size;

				struct {
					char id[4];
					uint32_t size;
				} sub;
				while (ftello(f) + (off_t)sizeof(sub) <= list_end && fread(&sub, 1, sizeof(sub), f) == sizeof(sub)) {
					char value[1024];
					uint32_t read_size = sub.size < sizeof(value) - 1 ? sub.size : sizeof(value) - 1;
					if (fread(value, 1, read_size, f) != read_size)
						break;
					value[read_size] = '\0';

					char *dst = NULL;
					size_t dst_size = 0;
					if (memcmp(sub.id, "INAM", 4) == 0) {
						dst = out->title;
						dst_size = sizeof(out->title);
					} else if (memcmp(sub.id, "IART", 4) == 0) {
						dst = out->artist;
						dst_size = sizeof(out->artist);
					} else if (memcmp(sub.id, "IPRD", 4) == 0) {
						dst = out->album;
						dst_size = sizeof(out->album);
					} else if (memcmp(sub.id, "IGNR", 4) == 0) {
						dst = out->genre;
						dst_size = sizeof(out->genre);
					}
					if (dst) {
						size_t copy_len = read_size < dst_size - 1 ? read_size : dst_size - 1;
						memcpy(dst, value, copy_len);
						dst[copy_len] = '\0';
					}

					off_t sub_data_start = ftello(f) - (off_t)read_size;
					off_t next_sub_pos = sub_data_start + (off_t)sub.size + (sub.size & 1);
					// Same rule as the outer walk: a sub-chunk that does not
					// move the position forward would be read again forever.
					if (next_sub_pos <= sub_data_start - (off_t)sizeof(sub) || next_sub_pos > list_end) {
						break;
					}
					if (fseeko(f, next_sub_pos, SEEK_SET) != 0) {
						break;
					}
				}
			}
		}

		off_t next_chunk_pos = (off_t)chunk_data_start + (off_t)chunk.size + (chunk.size & 1);
		// Always forward: a chunk that does not advance the position is the one
		// way this loop can fail to end.
		if (next_chunk_pos <= (off_t)chunk_data_start - (off_t)sizeof(chunk) || next_chunk_pos > file_end) {
			break;
		}
		if (fseeko(f, next_chunk_pos, SEEK_SET) != 0) {
			break;
		}
	}

	fclose(f);
}

// ---------------------------------------------------------------------------
// MP4 / M4A / M4B: iTunes-style atoms, read by the container parser
// ---------------------------------------------------------------------------

static void read_mp4_metadata(const char *filepath, song_metadata_t *out) {
	mp4_file_t *m = mp4_open(filepath);
	if (!m) {
		return;
	}

	snprintf(out->title, sizeof(out->title), "%s", mp4_tag_title(m));
	snprintf(out->artist, sizeof(out->artist), "%s", mp4_tag_artist(m));
	snprintf(out->album_artist, sizeof(out->album_artist), "%s", mp4_tag_album_artist(m));
	snprintf(out->album, sizeof(out->album), "%s", mp4_tag_album(m));
	snprintf(out->genre, sizeof(out->genre), "%s", mp4_tag_genre(m));
	out->year = mp4_tag_year(m);
	out->track_number = mp4_tag_track_number(m);

	// The freeform "----" atoms, where iTunes-style ReplayGain lives.
	const char *value;
	if ((value = mp4_tag_freeform(m, "replaygain_track_gain")) != NULL && value[0]) {
		replaygain_field(out, "REPLAYGAIN_TRACK_GAIN", value, strlen(value));
	}
	if ((value = mp4_tag_freeform(m, "replaygain_album_gain")) != NULL && value[0]) {
		replaygain_field(out, "REPLAYGAIN_ALBUM_GAIN", value, strlen(value));
	}
	if ((value = mp4_tag_freeform(m, "replaygain_track_peak")) != NULL && value[0]) {
		replaygain_field(out, "REPLAYGAIN_TRACK_PEAK", value, strlen(value));
	}
	if ((value = mp4_tag_freeform(m, "replaygain_album_peak")) != NULL && value[0]) {
		replaygain_field(out, "REPLAYGAIN_ALBUM_PEAK", value, strlen(value));
	}

	mp4_close(m);
}

// ---------------------------------------------------------------------------
// Opus and WavPack
// ---------------------------------------------------------------------------

// An Opus file's tags are Vorbis comments, the same as an .ogg's: the packet is
// called OpusTags and its contents have the same "KEY=value" form, so nothing
// new is needed -- they are poured into apply_vorbis_comment().
static void read_opus_metadata(const char *filepath, song_metadata_t *out) {
	int err = 0;
	OggOpusFile *of = op_open_file(filepath, &err);
	if (!of) {
		return;
	}

	const OpusTags *tags = op_tags(of, -1);
	if (tags) {
		for (int i = 0; i < tags->comments; i++) {
			if (tags->user_comments[i] && tags->comment_lengths[i] > 0) {
				apply_vorbis_comment(out, tags->user_comments[i], (size_t)tags->comment_lengths[i]);
			}
		}
	}

	op_free(of);
}

// A .wv carries APEv2 tags. The names are not the Vorbis comment ones --
// "Track" instead of "TRACKNUMBER", "Year" instead of "DATE" -- but the rest
// matches, so the few that differ are renamed and the same reader is reused.
// The ReplayGain entries already share their names.
static void wavpack_tag_cb(void *user, const char *key, const char *value) {
	song_metadata_t *out = (song_metadata_t *)user;

	const char *vorbis = key;
	if (strcasecmp(key, "Track") == 0) {
		vorbis = "TRACKNUMBER";
	} else if (strcasecmp(key, "Year") == 0 || strcasecmp(key, "date") == 0) {
		vorbis = "DATE";
	} else if (strcasecmp(key, "Album Artist") == 0 || strcasecmp(key, "AlbumArtist") == 0 ||
			   strcasecmp(key, "album_artist") == 0) {
		vorbis = "ALBUMARTIST";
	}

	char line[320];
	int n = snprintf(line, sizeof(line), "%s=%s", vorbis, value);
	if (n > 0) {
		size_t len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
		apply_vorbis_comment(out, line, len);
	}
}

static void read_wavpack_metadata(const char *filepath, song_metadata_t *out) {
	wavpackdec_tags(filepath, wavpack_tag_cb, out);
}

// ---------------------------------------------------------------------------
// Sidecar files
//
// A track downloaded from a streaming service can arrive with no tags at all:
// Qobuz FLACs are often bare. What the file does not say the API does, and the
// downloader writes it alongside, in "<file>.tags":
//
//     title=...
//     artist=...
//     album=...
//     track=3
//
// It is read after the real tags and overrides whatever they said (see the
// note in read_sidecar_tags). The same function then serves the player, the
// details page, the favourites and the library scan without any of them
// knowing that streaming exists.
// ---------------------------------------------------------------------------

static void read_sidecar_tags(const char *filepath, song_metadata_t *out) {
	char path[600];
	if ((size_t)snprintf(path, sizeof(path), "%.560s.tags", filepath) >= sizeof(path)) {
		return;
	}

	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}

	char line[512];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		char *equals = strchr(line, '=');
		if (!equals) {
			continue;
		}
		*equals = '\0';
		const char *key = line;
		const char *value = equals + 1;
		if (!value[0]) {
			continue;
		}

		// The sidecar wins over the file's own tags. It exists only beside a
		// cached track (Qobuz, Tidal, a podcast episode) and was written from
		// what the service says, while the file's tags are whatever someone else
		// published -- a podcast episode's MP3 very often carries the tags of the
		// show, or of the most recent episode, or none at all.
		//
		// Music on the card is unaffected: no .tags file is there.
		if (strcmp(key, "title") == 0) {
			snprintf(out->title, sizeof(out->title), "%s", value);
		} else if (strcmp(key, "artist") == 0) {
			snprintf(out->artist, sizeof(out->artist), "%s", value);
		} else if (strcmp(key, "album") == 0) {
			snprintf(out->album, sizeof(out->album), "%s", value);
		} else if (strcmp(key, "album_artist") == 0) {
			snprintf(out->album_artist, sizeof(out->album_artist), "%s", value);
		} else if (strcmp(key, "track") == 0) {
			out->track_number = atoi(value);
		} else if (strcmp(key, "year") == 0) {
			out->year = atoi(value);
		}
	}
	fclose(f);
}

// One track of a CUE sheet: the tags are in the sheet, not in the audio file.
// Reading the file would give the whole disc's tags for every track.
static bool read_cue_metadata(const char *filepath, song_metadata_t *out) {
	char sheet_path[512];
	int track = cue_split_path(filepath, sheet_path, sizeof(sheet_path));
	if (track <= 0) {
		return false;
	}

	// A sheet is thirty-five kilobytes, and tags are read from threads that
	// already carry deep call stacks.
	cue_sheet_t *cue = malloc(sizeof(*cue));
	if (!cue) {
		return false;
	}
	if (!cue_parse(sheet_path, cue) || track > cue->track_count) {
		free(cue);
		return false;
	}
	const cue_track_t *t = &cue->tracks[track - 1];

	copy_bounded(out->title, sizeof(out->title), t->title);
	copy_bounded(out->artist, sizeof(out->artist), t->performer[0] ? t->performer : cue->performer);
	copy_bounded(out->album, sizeof(out->album), cue->album);
	copy_bounded(out->album_artist, sizeof(out->album_artist), cue->performer);
	copy_bounded(out->genre, sizeof(out->genre), cue->genre);
	out->year = cue->year;
	out->track_number = t->number;
	out->has_tags = out->title[0] || out->artist[0] || out->album[0] || out->genre[0];
	free(cue);
	return true;
}

void metadata_read(const char *filepath, song_metadata_t *out) {
	memset(out, 0, sizeof(*out));

	if (read_cue_metadata(filepath, out)) {
		return;
	}

	decode_format_t format = decode_detect_format(filepath);
	switch (format) {
	case DECODE_FORMAT_MP3:
	// A raw .aac has no container to put tags in, so it carries them up front
	// the same way an mp3 does: an ID3v2 before the first frame, which is
	// exactly what the decoder skips on open.
	case DECODE_FORMAT_AAC_ADTS:
		read_mp3_metadata(filepath, out);
		break;
	case DECODE_FORMAT_FLAC:
		read_flac_metadata(filepath, out);
		break;
	case DECODE_FORMAT_OGG_VORBIS:
		read_ogg_metadata(filepath, out);
		break;
	case DECODE_FORMAT_AAC_MP4:
		read_mp4_metadata(filepath, out);
		break;
	case DECODE_FORMAT_DSD:
		read_dsd_metadata(filepath, out);
		break;
	case DECODE_FORMAT_OPUS:
		read_opus_metadata(filepath, out);
		break;
	case DECODE_FORMAT_WAVPACK:
		read_wavpack_metadata(filepath, out);
		break;
	default:
		if (has_extension(filepath, ".wav")) {
			read_wav_metadata(filepath, out);
		}
		break;
	}

	read_sidecar_tags(filepath, out);

	out->has_tags = out->title[0] != '\0' || out->artist[0] != '\0' || out->album[0] != '\0' || out->genre[0] != '\0';
}

#include "albumart.h"

#include "src/system/library/cue.h"
#include "src/system/decode/decode.h"
#include "src/system/decode/dr_flac.h"
#include "src/system/decode/mp4.h"
#include "src/system/decode/stb_vorbis_decl.h"
#include "src/system/decode/wavpackdec.h"
#include "src/system/core/utils.h"

#include <opusfile.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

// APIC / FLAC PICTURE picture type for "front cover". When a file carries
// several pictures (front, back, artist, ...) this is the one to keep.
#define PIC_TYPE_FRONT_COVER 3

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

static uint32_t be32(const uint8_t *b) {
	return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

static uint32_t syncsafe32(const uint8_t *b) {
	return ((uint32_t)(b[0] & 0x7F) << 21) | ((uint32_t)(b[1] & 0x7F) << 14) | ((uint32_t)(b[2] & 0x7F) << 7) |
		   (uint32_t)(b[3] & 0x7F);
}

// Only JPEG and PNG reach the GUI: those are the two formats the image decoder
// is built with, and together they cover essentially every tagged file in the
// wild.
static bool is_supported_image(const uint8_t *data, size_t size) {
	if (!data || size < 12)
		return false;

	// JPEG: FF D8 FF
	if (data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
		return true;

	// PNG: 89 'P' 'N' 'G' 0D 0A 1A 0A
	static const uint8_t png_magic[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
	if (memcmp(data, png_magic, sizeof(png_magic)) == 0)
		return true;

	return false;
}

// Validates and takes a private copy of a picture payload. Returns false (and
// leaves `out` untouched) if the payload is not a usable image.
static bool take_picture(albumart_t *out, const uint8_t *data, size_t size) {
	if (size == 0 || size > ALBUMART_MAX_BYTES || !is_supported_image(data, size))
		return false;

	uint8_t *copy = malloc(size);
	if (!copy)
		return false;

	memcpy(copy, data, size);

	albumart_free(out); // drop any lower-priority picture picked up earlier
	out->data = copy;
	out->size = size;
	return true;
}

void albumart_free(albumart_t *art) {
	if (!art)
		return;
	free(art->data);
	art->data = NULL;
	art->size = 0;
}

// ---------------------------------------------------------------------------
// MP3: ID3v2 APIC (v2.3/v2.4) and PIC (v2.2)
// ---------------------------------------------------------------------------

// Reverses ID3v2 unsynchronisation in place (every FF 00 pair becomes a plain
// FF) and returns the new length.
static size_t de_unsynchronise(uint8_t *data, size_t size) {
	size_t w = 0;
	for (size_t r = 0; r < size; r++) {
		data[w++] = data[r];
		if (data[r] == 0xFF && r + 1 < size && data[r + 1] == 0x00)
			r++; // swallow the inserted zero byte
	}
	return w;
}

// Skips a text string inside an APIC frame, honouring the frame's text
// encoding (UTF-16 variants are terminated by two zero bytes, not one).
// Returns the offset just past the terminator, or `size` if none was found.
static size_t skip_encoded_string(const uint8_t *data, size_t size, size_t pos, uint8_t encoding) {
	bool utf16 = (encoding == 0x01 || encoding == 0x02);

	if (utf16) {
		while (pos + 1 < size) {
			if (data[pos] == 0 && data[pos + 1] == 0)
				return pos + 2;
			pos += 2;
		}
		return size;
	}

	while (pos < size) {
		if (data[pos] == 0)
			return pos + 1;
		pos++;
	}
	return size;
}

// Parses one APIC (v2.3/v2.4) or PIC (v2.2) frame body. `is_v22` selects the
// old layout, whose "MIME type" is a fixed 3-character format id ("JPG"/"PNG")
// instead of a null-terminated string.
// Returns the picture type (0-20) via `out_type`.
static bool parse_apic_body(const uint8_t *body, size_t size, bool is_v22, albumart_t *out, uint8_t *out_type) {
	if (size < 4)
		return false;

	uint8_t encoding = body[0];
	size_t pos = 1;

	if (is_v22) {
		pos += 3; // image format, e.g. "JPG"
	} else {
		pos = skip_encoded_string(body, size, pos, 0x00); // MIME type is always ISO-8859-1
	}
	if (pos >= size)
		return false;

	*out_type = body[pos];
	pos++;

	pos = skip_encoded_string(body, size, pos, encoding); // description
	if (pos >= size)
		return false;

	return take_picture(out, body + pos, size - pos);
}

// Whether `pos` is where another frame header begins -- or where the tag ends,
// either by running out or by turning into padding. Frame ids are upper-case
// letters and digits, which is what makes the guess reliable enough to choose
// between two readings of a frame size.
static bool frame_starts_at(const uint8_t *tag, size_t tag_size, size_t pos) {
	if (pos >= tag_size)
		return pos == tag_size;
	if (tag[pos] == 0)
		return true; // padding
	if (pos + 4 > tag_size)
		return false;

	for (size_t i = 0; i < 4; i++) {
		uint8_t c = tag[pos + i];
		if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
			return false;
	}
	return true;
}

// Reads the ID3v2 tag that starts at the file's current position and keeps its
// best picture. Separate from the callers below because three containers put
// the very same tag in three different places: an MP3 or a raw .aac at byte
// zero, a .dsf at the offset its header points to, an AIFF inside an "ID3 "
// chunk.
static bool read_id3v2_picture(FILE *f, albumart_t *out) {
	uint8_t header[10];
	if (fread(header, 1, sizeof(header), f) != sizeof(header) || memcmp(header, "ID3", 3) != 0) {
		return false;
	}

	uint8_t major = header[3];
	uint8_t tag_flags = header[5];
	size_t tag_size = syncsafe32(&header[6]);

	// A tag that cannot hold a sane picture (or is absurdly large) is not worth
	// pulling into RAM on a 64 MB device.
	if (tag_size < 16 || tag_size > ALBUMART_MAX_BYTES + (1024 * 1024)) {
		return false;
	}

	uint8_t *tag = malloc(tag_size);
	if (!tag) {
		return false;
	}
	size_t got = fread(tag, 1, tag_size, f);
	tag_size = got;

	// v2.3 applies unsynchronisation to the whole tag, so undo it up front and
	// parse plain bytes from here on. (v2.4 does it per frame, handled below.)
	bool tag_unsync = (tag_flags & 0x80) != 0;
	if (tag_unsync && major < 4)
		tag_size = de_unsynchronise(tag, tag_size);

	size_t pos = 0;

	// Skip the extended header if there is one.
	if (tag_flags & 0x40) {
		if (major >= 3 && pos + 4 <= tag_size) {
			uint32_t ext_size = (major >= 4) ? syncsafe32(tag + pos) : be32(tag + pos);
			pos += (major >= 4) ? ext_size : ext_size + 4;
		}
	}

	size_t header_len = (major < 3) ? 6 : 10;
	bool found = false;
	uint8_t best_type = 0xFF;

	while (pos + header_len <= tag_size) {
		const uint8_t *fh = tag + pos;
		if (fh[0] == 0)
			break; // padding

		char id[5] = {0};
		size_t frame_size;
		uint16_t frame_flags = 0;

		if (major < 3) {
			memcpy(id, fh, 3);
			frame_size = ((size_t)fh[3] << 16) | ((size_t)fh[4] << 8) | (size_t)fh[5];
		} else {
			memcpy(id, fh, 4);
			frame_size = (major >= 4) ? syncsafe32(fh + 4) : be32(fh + 4);
			frame_flags = ((uint16_t)fh[8] << 8) | fh[9];
		}

		pos += header_len;

		// Several widely used taggers write v2.4 frame sizes as plain 32-bit
		// integers rather than the syncsafe ones the version calls for. Read
		// the syncsafe way such a size is far too small, the walk lands inside
		// the picture data, and everything from there on -- the cover
		// included -- is lost. Take the plain reading when it is the one that
		// lands on the next frame.
		if (major >= 4) {
			size_t plain = be32(fh + 4);
			if (plain != frame_size && plain <= tag_size - pos && !frame_starts_at(tag, tag_size, pos + frame_size) &&
				frame_starts_at(tag, tag_size, pos + plain)) {
				frame_size = plain;
			}
		}

		if (frame_size == 0 || frame_size > tag_size - pos)
			break;

		bool is_picture = (major < 3) ? (strcmp(id, "PIC") == 0) : (strcmp(id, "APIC") == 0);
		if (is_picture) {
			uint8_t *body = tag + pos;
			size_t body_size = frame_size;

			// v2.4 per-frame unsynchronisation.
			uint8_t *unsync_copy = NULL;
			if (major >= 4 && (frame_flags & 0x0002)) {
				unsync_copy = malloc(body_size);
				if (unsync_copy) {
					memcpy(unsync_copy, body, body_size);
					body_size = de_unsynchronise(unsync_copy, body_size);
					body = unsync_copy;
				}
			}

			uint8_t type = 0xFF;
			// Keep the first usable picture, but let a front cover replace it.
			if ((!found || best_type != PIC_TYPE_FRONT_COVER) &&
				parse_apic_body(body, body_size, major < 3, out, &type)) {
				found = true;
				best_type = type;
			}

			free(unsync_copy);

			if (found && best_type == PIC_TYPE_FRONT_COVER)
				break; // the front cover: nothing better can follow
		}

		pos += frame_size;
	}

	free(tag);
	return found;
}

static bool read_mp3_embedded(const char *filepath, albumart_t *out) {
	FILE *f = fopen(filepath, "rb");
	if (!f)
		return false;

	bool found = read_id3v2_picture(f, out);
	fclose(f);
	return found;
}

// ---------------------------------------------------------------------------
// DSD: the ID3v2 tag a .dsf points at
//
// A .dsf keeps an ordinary ID3v2 tag -- APIC and all -- at the offset written
// in the eight bytes at 20 of its header. A .dff has no picture: its DIIN
// chunks hold text only, so those fall through to the folder.
// ---------------------------------------------------------------------------

static bool read_dsf_embedded(const char *filepath, albumart_t *out) {
	FILE *f = fopen(filepath, "rb");
	if (!f)
		return false;

	bool found = false;
	uint8_t head[28];
	if (fread(head, 1, sizeof(head), f) == sizeof(head) && memcmp(head, "DSD ", 4) == 0) {
		uint64_t tag_at = 0;
		for (int i = 7; i >= 0; i--) {
			tag_at = (tag_at << 8) | head[20 + i];
		}
		// Zero means no tag, which is common. Anything past the end of the file
		// is a corrupt offset that would otherwise send the read somewhere
		// arbitrary.
		if (fseeko(f, 0, SEEK_END) == 0) {
			off_t size = ftello(f);
			if (tag_at != 0 && size > 0 && tag_at < (uint64_t)size && fseeko(f, (off_t)tag_at, SEEK_SET) == 0) {
				found = read_id3v2_picture(f, out);
			}
		}
	}

	fclose(f);
	return found;
}

// ---------------------------------------------------------------------------
// AIFF: the ID3 chunk
//
// AIFF has no tag format of its own, so taggers park a whole ID3v2 tag in an
// "ID3 " chunk. libsndfile plays these files but hands over no pictures, so the
// chunk is walked here.
// ---------------------------------------------------------------------------

static bool read_aiff_embedded(const char *filepath, albumart_t *out) {
	FILE *f = fopen(filepath, "rb");
	if (!f)
		return false;

	bool found = false;
	uint8_t form[12];
	if (fread(form, 1, sizeof(form), f) == sizeof(form) && memcmp(form, "FORM", 4) == 0) {
		// Chunk bodies are padded to an even length, which the size field does
		// not count. The guard stops a corrupt file from walking for ever.
		long pos = 12;
		for (int guard = 0; guard < 64 && !found; guard++) {
			uint8_t ch[8];
			if (fseek(f, pos, SEEK_SET) != 0 || fread(ch, 1, sizeof(ch), f) != sizeof(ch)) {
				break;
			}
			uint32_t size = be32(ch + 4);
			if (memcmp(ch, "ID3 ", 4) == 0 || memcmp(ch, "id3 ", 4) == 0) {
				found = read_id3v2_picture(f, out);
				break;
			}
			pos += 8 + (long)size + (long)(size & 1);
		}
	}

	fclose(f);
	return found;
}

// ---------------------------------------------------------------------------
// FLAC: PICTURE metadata block
// ---------------------------------------------------------------------------

typedef struct {
	albumart_t *out;
	bool found;
	uint8_t best_type;
} flac_picture_ctx_t;

static void flac_picture_callback(void *user_data, drflac_metadata *meta) {
	if (meta->type != DRFLAC_METADATA_BLOCK_TYPE_PICTURE)
		return;

	flac_picture_ctx_t *ctx = user_data;
	if (ctx->found && ctx->best_type == PIC_TYPE_FRONT_COVER)
		return; // the front cover is already in hand

	if (take_picture(ctx->out, meta->data.picture.pPictureData, meta->data.picture.pictureDataSize)) {
		ctx->found = true;
		ctx->best_type = (uint8_t)meta->data.picture.type;
	}
}

static bool read_flac_embedded(const char *filepath, albumart_t *out) {
	flac_picture_ctx_t ctx = {.out = out, .found = false, .best_type = 0xFF};

	drflac *flac = drflac_open_file_with_metadata(filepath, flac_picture_callback, &ctx, NULL);
	if (flac)
		drflac_close(flac);

	return ctx.found;
}

// ---------------------------------------------------------------------------
// OGG Vorbis: METADATA_BLOCK_PICTURE / COVERART comments
// ---------------------------------------------------------------------------

static int base64_value(char c) {
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;
	return -1; // padding, whitespace or garbage
}

// Decodes base64 text into a freshly allocated buffer. Returns NULL on
// failure; on success *out_size holds the decoded length.
static uint8_t *base64_decode(const char *text, size_t text_len, size_t *out_size) {
	if (text_len / 4 * 3 > ALBUMART_MAX_BYTES)
		return NULL;

	uint8_t *out = malloc(text_len / 4 * 3 + 3);
	if (!out)
		return NULL;

	size_t written = 0;
	uint32_t accum = 0;
	int bits = 0;

	for (size_t i = 0; i < text_len; i++) {
		int v = base64_value(text[i]);
		if (v < 0)
			continue;

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

	*out_size = written;
	return out;
}

// Parses the FLAC PICTURE structure that METADATA_BLOCK_PICTURE comments
// carry (same layout as the FLAC metadata block, all big endian).
static bool parse_flac_picture_block(const uint8_t *block, size_t size, albumart_t *out, uint8_t *out_type) {
	size_t pos = 0;
	if (size < 32)
		return false;

	uint32_t type = be32(block + pos);
	pos += 4;

	uint32_t mime_len = be32(block + pos);
	pos += 4;
	if (mime_len > size - pos)
		return false;
	pos += mime_len;

	if (pos + 4 > size)
		return false;
	uint32_t desc_len = be32(block + pos);
	pos += 4;
	if (desc_len > size - pos)
		return false;
	pos += desc_len;

	pos += 16; // width, height, colour depth, indexed colour count
	if (pos + 4 > size)
		return false;

	uint32_t data_len = be32(block + pos);
	pos += 4;
	if (data_len > size - pos)
		return false;

	*out_type = (uint8_t)type;
	return take_picture(out, block + pos, data_len);
}

// Examines one Vorbis comment for a cover picture. Kept separate because it
// serves two formats unchanged: an Opus file's tags ARE Vorbis comments, same
// shape and same keys as an .ogg. Returns true once a front cover is in hand
// and looking further cannot improve on it.
static bool consider_vorbis_picture(const char *entry, size_t entry_len, albumart_t *out, bool *found,
									uint8_t *best_type) {
	if (!entry || entry_len == 0)
		return false;

	const char *eq = memchr(entry, '=', entry_len);
	if (!eq)
		return false;

	size_t key_len = (size_t)(eq - entry);
	const char *value = eq + 1;
	size_t value_len = entry_len - key_len - 1;

	bool is_block = (key_len == 22 && strncasecmp(entry, "METADATA_BLOCK_PICTURE", 22) == 0);
	bool is_coverart = (key_len == 8 && strncasecmp(entry, "COVERART", 8) == 0);
	if (!is_block && !is_coverart)
		return false;

	size_t raw_size = 0;
	uint8_t *raw = base64_decode(value, value_len, &raw_size);
	if (!raw)
		return false;

	uint8_t type = 0xFF;
	bool ok = is_block ? parse_flac_picture_block(raw, raw_size, out, &type)
					   : take_picture(out, raw, raw_size); // COVERART holds the image directly
	free(raw);

	if (ok) {
		*found = true;
		*best_type = type;
	}
	return *found && *best_type == PIC_TYPE_FRONT_COVER;
}

static bool read_ogg_embedded(const char *filepath, albumart_t *out) {
	int error = 0;
	stb_vorbis *vorbis = stb_vorbis_open_filename(filepath, &error, NULL);
	if (!vorbis)
		return false;

	stb_vorbis_comment comment = stb_vorbis_get_comment(vorbis);
	bool found = false;
	uint8_t best_type = 0xFF;

	for (int i = 0; i < comment.comment_list_length; i++) {
		const char *entry = comment.comment_list[i];
		if (!entry)
			continue;
		if (consider_vorbis_picture(entry, strlen(entry), out, &found, &best_type))
			break;
	}

	stb_vorbis_close(vorbis);
	return found;
}

// Opus: the same Vorbis comments, taken from the OpusTags packet instead of a
// Vorbis stream header. The library hands over the lengths here, so not even an
// strlen is needed.
static bool read_opus_embedded(const char *filepath, albumart_t *out) {
	int err = 0;
	OggOpusFile *of = op_open_file(filepath, &err);
	if (!of)
		return false;

	bool found = false;
	uint8_t best_type = 0xFF;

	const OpusTags *tags = op_tags(of, -1);
	if (tags) {
		for (int i = 0; i < tags->comments; i++) {
			if (tags->comment_lengths[i] <= 0)
				continue;
			if (consider_vorbis_picture(tags->user_comments[i], (size_t)tags->comment_lengths[i], out, &found,
										&best_type))
				break;
		}
	}

	op_free(of);
	return found;
}

// WavPack: no base64 and no FLAC block. APEv2 holds the raw image inside a
// binary item, and wavpackdec_cover() has already stripped the file name that
// precedes it.
static bool read_wavpack_embedded(const char *filepath, albumart_t *out) {
	size_t size = 0;
	unsigned char *image = wavpackdec_cover(filepath, ALBUMART_MAX_BYTES, &size);
	if (!image)
		return false;

	bool ok = take_picture(out, image, size);
	free(image);
	return ok;
}

// ---------------------------------------------------------------------------
// Folder fallback: cover.jpg & friends sitting next to the music
// ---------------------------------------------------------------------------

// Preferred cover file names, best first. Matched case-insensitively, so
// "Folder.jpg" and "folder.JPG" both work.
static const char *const cover_basenames[] = {
	"cover", "folder", "front", "albumart", "albumartsmall", "album", "artwork", "thumb",
};

static bool is_image_name(const char *name) {
	return has_extension(name, ".jpg") || has_extension(name, ".jpeg") || has_extension(name, ".png");
}

// Ranks a file name against `cover_basenames`. Lower is better; -1 means the
// name isn't a recognised cover file at all.
static int cover_name_rank(const char *name) {
	if (!is_image_name(name))
		return -1;

	const char *dot = strrchr(name, '.');
	size_t stem_len = dot ? (size_t)(dot - name) : strlen(name);

	for (size_t i = 0; i < sizeof(cover_basenames) / sizeof(cover_basenames[0]); i++) {
		size_t len = strlen(cover_basenames[i]);
		if (stem_len == len && strncasecmp(name, cover_basenames[i], len) == 0)
			return (int)i;
	}
	return -1;
}

static bool read_whole_file(const char *path, albumart_t *out) {
	long size = get_file_size(path);
	if (size <= 0 || size > ALBUMART_MAX_BYTES)
		return false;

	FILE *f = fopen(path, "rb");
	if (!f)
		return false;

	uint8_t *buf = malloc((size_t)size);
	if (!buf) {
		fclose(f);
		return false;
	}

	size_t got = fread(buf, 1, (size_t)size, f);
	fclose(f);

	bool ok = (got == (size_t)size) && is_supported_image(buf, got);
	if (!ok) {
		free(buf);
		return false;
	}

	albumart_free(out);
	out->data = buf;
	out->size = got;
	return true;
}

// Whether an image file is named after `stem` -- the same name, a different
// extension.
static bool image_named_after(const char *name, const char *stem) {
	if (!stem || !stem[0])
		return false;

	const char *dot = strrchr(name, '.');
	size_t stem_len = dot ? (size_t)(dot - name) : strlen(name);
	return stem_len == strlen(stem) && strncasecmp(name, stem, stem_len) == 0;
}

// Looks for a cover image inside `dirpath`. `audio_stem` (may be NULL) is the
// file name of the track without its extension, so "01 - Song.mp3" can pick up
// a matching "01 - Song.jpg".
//
// Ranked, best first: the track's own name, then the folder's own name (rippers
// commonly write "Artist - Album.jpg" beside the tracks), then the generic
// names, and last the single image of a folder that holds exactly one.
#define COVER_RANK_TRACK_NAME 0
#define COVER_RANK_DIR_NAME 1
#define COVER_RANK_GENERIC 2
#define COVER_RANK_NONE 1000

static bool find_cover_file(const char *dirpath, const char *audio_stem, char *out_path, size_t out_size) {
	DIR *dir = opendir(dirpath);
	if (!dir)
		return false;

	// The folder's own name, without its path.
	const char *dir_slash = strrchr(dirpath, '/');
	const char *dir_stem = dir_slash ? dir_slash + 1 : dirpath;

	char best[256] = {0};
	int best_rank = COVER_RANK_NONE;

	char only_image[256] = {0};
	int image_count = 0;

	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.')
			continue;
		if (!is_image_name(de->d_name))
			continue;

		image_count++;
		if (image_count == 1) {
			snprintf(only_image, sizeof(only_image), "%s", de->d_name);
		}

		int rank = COVER_RANK_NONE;
		if (image_named_after(de->d_name, audio_stem)) {
			rank = COVER_RANK_TRACK_NAME;
		} else if (image_named_after(de->d_name, dir_stem)) {
			rank = COVER_RANK_DIR_NAME;
		} else {
			int generic = cover_name_rank(de->d_name);
			if (generic >= 0) {
				rank = COVER_RANK_GENERIC + generic;
			}
		}

		if (rank < best_rank) {
			best_rank = rank;
			snprintf(best, sizeof(best), "%s", de->d_name);
			if (rank == COVER_RANK_TRACK_NAME) {
				break; // nothing can beat it
			}
		}
	}
	closedir(dir);

	// Nothing matched by name, but if the folder holds exactly one image it's
	// almost certainly the cover.
	if (best[0] == '\0' && image_count == 1) {
		snprintf(best, sizeof(best), "%s", only_image);
	}

	if (best[0] == '\0')
		return false;

	snprintf(out_path, out_size, "%s/%s", dirpath, best);
	return true;
}

// ---------------------------------------------------------------------------
// MP4 / M4A / M4B: the `covr` atom
// ---------------------------------------------------------------------------

static bool read_mp4_embedded(const char *filepath, albumart_t *out) {
	mp4_file_t *m = mp4_open(filepath);
	if (!m) {
		return false;
	}

	uint64_t offset = 0;
	uint32_t size = 0;
	bool is_png = false;
	bool found = mp4_cover_art(m, &offset, &size, &is_png);
	mp4_close(m);

	if (!found || size == 0 || size > ALBUMART_MAX_BYTES) {
		return false;
	}

	FILE *f = fopen(filepath, "rb");
	if (!f) {
		return false;
	}
	uint8_t *data = malloc(size);
	if (!data || fseeko(f, (off_t)offset, SEEK_SET) != 0 || fread(data, 1, size, f) != size) {
		free(data);
		fclose(f);
		return false;
	}
	fclose(f);

	// Through take_picture like every other format: a `covr` atom may hold a
	// BMP, or the atom may simply be mis-sized, and handing those bytes on as
	// artwork costs a failed decode and, worse, a cached "no cover here".
	bool ok = take_picture(out, data, size);
	free(data);
	return ok;
}

// Reads the picture stored inside the file's own tags, if the format has a
// place for one.
static bool read_embedded(const char *filepath, albumart_t *out) {
	switch (decode_detect_format(filepath)) {
	case DECODE_FORMAT_MP3:
	case DECODE_FORMAT_AAC_ADTS:
		// Same reason as the tags: an .aac file's cover sits in an ID3v2 APIC
		// frame, exactly as in an mp3.
		return read_mp3_embedded(filepath, out);
	case DECODE_FORMAT_FLAC:
		return read_flac_embedded(filepath, out);
	case DECODE_FORMAT_OGG_VORBIS:
		return read_ogg_embedded(filepath, out);
	case DECODE_FORMAT_AAC_MP4:
	case DECODE_FORMAT_ALAC_MP4:
		// The cover lives in the container, not the audio stream, so the codec
		// is irrelevant. ALAC is listed explicitly because a file named .alac
		// is detected as its own format and would otherwise never be asked.
		return read_mp4_embedded(filepath, out);
	case DECODE_FORMAT_OPUS:
		return read_opus_embedded(filepath, out);
	case DECODE_FORMAT_WAVPACK:
		return read_wavpack_embedded(filepath, out);
	case DECODE_FORMAT_DSD:
		return has_extension(filepath, ".dsf") && read_dsf_embedded(filepath, out);
	case DECODE_FORMAT_SNDFILE:
		// Of what libsndfile plays here, only AIFF has somewhere to put a
		// picture.
		return (has_extension(filepath, ".aif") || has_extension(filepath, ".aiff") ||
				has_extension(filepath, ".aifc")) &&
			   read_aiff_embedded(filepath, out);
	default:
		return false; // WAV has no standard place to put a picture
	}
}

// Picks the alphabetically first track in the folder, so an album row always
// shows the same picture whatever order the filesystem returns entries in.
static bool find_first_track(const char *dirpath, char *out_name, size_t out_size) {
	DIR *dir = opendir(dirpath);
	if (!dir)
		return false;

	bool found = false;
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.')
			continue;
		if (decode_detect_format(de->d_name) == DECODE_FORMAT_UNKNOWN)
			continue; // only the formats that can carry a picture

		if (!found || strcasecmp(de->d_name, out_name) < 0) {
			snprintf(out_name, out_size, "%s", de->d_name);
			found = true;
		}
	}

	closedir(dir);
	return found;
}

bool albumart_load_for_dir(const char *dirpath, albumart_t *out) {
	for (int i = 0; i < ALBUMART_CANDIDATES; i++) {
		if (albumart_load_dir_candidate(dirpath, i, out))
			return true;
	}
	return false;
}

bool albumart_load_dir_candidate(const char *dirpath, int index, albumart_t *out) {
	memset(out, 0, sizeof(*out));

	if (index == 0) {
		char cover_path[1300];
		return find_cover_file(dirpath, NULL, cover_path, sizeof(cover_path)) && read_whole_file(cover_path, out);
	}
	if (index != 1)
		return false;

	// An album folder with no cover file usually still carries the artwork
	// inside its tracks -- borrow it from the first one.
	char track_name[256] = {0};
	if (!find_first_track(dirpath, track_name, sizeof(track_name)))
		return false;

	char track_path[1300];
	snprintf(track_path, sizeof(track_path), "%s/%s", dirpath, track_name);
	return read_embedded(track_path, out);
}

bool albumart_load_for_file(const char *filepath, albumart_t *out) {
	for (int i = 0; i < ALBUMART_CANDIDATES; i++) {
		if (albumart_load_candidate(filepath, i, out))
			return true;
	}
	return false;
}

bool albumart_load_candidate(const char *filepath, int index, albumart_t *out) {
	memset(out, 0, sizeof(*out));

	if (index < 0 || index >= ALBUMART_CANDIDATES)
		return false;

	// A track of a CUE sheet has no artwork of its own: the picture belongs to
	// the file the sheet cuts up, or sits in the folder beside it.
	//
	// The sheet is on the heap, not the stack: cue_sheet_t is 35 KB, the
	// compiler would reserve it on entry whether or not this branch is taken,
	// and this function recurses -- 70 KB of loader-thread stack for a case
	// that fires only on a `.cue?track=` path.
	char sheet_path[512];
	if (cue_split_path(filepath, sheet_path, sizeof(sheet_path)) > 0) {
		cue_sheet_t *cue = malloc(sizeof(*cue));
		if (!cue) {
			return false;
		}
		bool ok = cue_parse(sheet_path, cue) && albumart_load_candidate(cue->audio_path, index, out);
		free(cue);
		return ok;
	}

	// The path IS a picture. There is no track here to read tags from and no
	// folder to search: a radio station's downloaded icon is simply the
	// artwork, and the player asks for it by path like any other cover.
	if (has_extension(filepath, ".jpg") || has_extension(filepath, ".jpeg") || has_extension(filepath, ".png")) {
		return index == 0 && read_whole_file(filepath, out);
	}

	if (index == 0) {
		return read_embedded(filepath, out);
	}

	// A cover image file sitting next to the track.
	char dirpath[1024];
	strncpy(dirpath, filepath, sizeof(dirpath) - 1);
	dirpath[sizeof(dirpath) - 1] = '\0';

	char *slash = strrchr(dirpath, '/');
	const char *filename = slash ? slash + 1 : dirpath;

	char stem[256] = {0};
	const char *dot = strrchr(filename, '.');
	size_t stem_len = dot ? (size_t)(dot - filename) : strlen(filename);
	if (stem_len < sizeof(stem)) {
		memcpy(stem, filename, stem_len);
		stem[stem_len] = '\0';
	}

	if (slash) {
		*slash = '\0';
	} else {
		strncpy(dirpath, ".", sizeof(dirpath) - 1);
	}
	if (dirpath[0] == '\0')
		strncpy(dirpath, "/", sizeof(dirpath) - 1);

	char cover_path[1300];
	if (find_cover_file(dirpath, stem, cover_path, sizeof(cover_path)))
		return read_whole_file(cover_path, out);

	return false;
}

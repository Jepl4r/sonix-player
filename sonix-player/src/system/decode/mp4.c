#include "mp4.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> // strcasecmp, for the freeform tag names
#include <unistd.h>

// See mp4.h for why this exists and why the big tables stay in the file.

#define FOURCC(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

#define MAX_TRACKS 8
#define MAX_CHAPTERS 2000
#define MAX_RUNS 200000	   // ceiling on stsc/stts entries held in memory
#define SIZE_WINDOW 1024   // sample sizes cached at a time
#define MAX_SMALL_BOX (4 * 1024 * 1024)
#define MAX_DEPTH 12
#define MAX_FREEFORM 8

struct chunk_run {
	uint32_t first_chunk; // 1-based, as the file writes it
	uint32_t samples_per_chunk;
};

struct time_run {
	uint32_t count;
	uint32_t delta;
};

typedef struct {
	uint32_t id;
	uint32_t handler;
	uint32_t timescale;
	uint64_t duration;
	uint32_t sample_count;

	// sample sizes: either one number for all of them, or a table left in the
	// file at `size_table_off` with `size_field_bits` bits per entry.
	uint32_t uniform_size;
	uint64_t size_table_off;
	int size_field_bits;

	// chunk offsets, likewise left in the file
	uint64_t chunk_table_off;
	uint32_t chunk_count;
	bool chunk_64;

	struct chunk_run *stsc;
	uint32_t stsc_count;
	struct time_run *stts;
	uint32_t stts_count;

	// For AAC this is the esds AudioSpecificConfig, for ALAC the
	// ALACSpecificConfig of the `alac` box. Both are what the decoder must know
	// before the first frame, so they share one field: the reader already knows
	// which it is, from the codec.
	unsigned char asc[64];
	int asc_len;
	uint32_t sr;
	uint32_t ch;
	mp4_codec_t codec;

	// The declared bitrate, in bits per second, as the esds
	// DecoderConfigDescriptor states it. Zero when the file does not say, which
	// is legal: many encoders leave the field at 0.
	//
	// Used for the now-playing display: AAC has no bit depth of its own, and
	// showing "16/44.1" under a 96 kbps track describes the PCM leaving the
	// decoder, not what is being heard.
	uint32_t avg_bitrate;
	uint32_t max_bitrate;

	uint32_t chap_id; // the text track this one takes its chapters from
} track_t;

struct mp4_file {
	int fd;
	uint64_t file_size;

	track_t tracks[MAX_TRACKS];
	int track_count;
	int audio; // index into tracks, -1 when none

	mp4_chapter_t *chapters;
	int chapter_count;
	int chapter_capacity;

	char title[256];
	char artist[256];
	char album_artist[256];
	char album[256];
	char genre[128];
	int year;
	int track_number;
	int disc_number;

	uint64_t cover_off;
	uint32_t cover_size;
	bool cover_png;

	// The iTunes freeform tags worth keeping. Only a handful are ever wanted,
	// and holding every "----" atom a file might carry would mean an allocator
	// for something nobody reads.
	struct {
		char name[40];
		char value[40];
	} freeform[MAX_FREEFORM];
	int freeform_count;

	// the sample-size window
	uint32_t *size_cache;
	uint32_t cache_first;
	uint32_t cache_count;
	int cache_track; // which track the window belongs to

	// Where the next sample begins, when the next sample asked for is the one
	// after the last. See frame_location() for why this is not an optimisation
	// but the difference between playing a book and not.
	int seq_track;
	uint32_t seq_index;	 // the sample this prediction is for
	uint64_t seq_offset; // ...and where it starts
	uint32_t seq_chunk_end;
};

// ---------------------------------------------------------------------------
// raw reads
// ---------------------------------------------------------------------------

static bool read_at(const mp4_file_t *m, uint64_t off, void *buf, size_t len) {
	size_t done = 0;
	while (done < len) {
		ssize_t n = pread(m->fd, (char *)buf + done, len - done, (off_t)(off + done));
		if (n <= 0) {
			return false;
		}
		done += (size_t)n;
	}
	return true;
}

static uint32_t be32(const unsigned char *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint16_t be16(const unsigned char *p) { return (uint16_t)(((uint32_t)p[0] << 8) | p[1]); }
static uint64_t be64(const unsigned char *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

// ---------------------------------------------------------------------------
// the box walk
// ---------------------------------------------------------------------------

typedef struct {
	uint32_t type;
	uint64_t content;	// where the payload starts
	uint64_t next;		// where the following box starts
	uint64_t body_size; // payload length
} box_t;

static bool read_box(const mp4_file_t *m, uint64_t off, uint64_t limit, box_t *out) {
	if (off + 8 > limit) {
		return false;
	}
	unsigned char head[16];
	if (!read_at(m, off, head, 8)) {
		return false;
	}
	uint64_t size = be32(head);
	out->type = be32(head + 4);
	uint64_t content = off + 8;

	if (size == 1) {
		if (off + 16 > limit || !read_at(m, off + 8, head + 8, 8)) {
			return false;
		}
		size = be64(head + 8);
		content = off + 16;
	} else if (size == 0) {
		size = limit - off; // "to the end of the enclosing box"
	}

	if (size < content - off || off + size > limit) {
		return false;
	}

	out->content = content;
	out->body_size = size - (content - off);
	out->next = off + size;
	return true;
}

// Loads a whole box body. Only ever called for the small ones.
static unsigned char *load_box(const mp4_file_t *m, const box_t *box, size_t *len_out) {
	if (box->body_size == 0 || box->body_size > MAX_SMALL_BOX) {
		return NULL;
	}
	size_t len = (size_t)box->body_size;
	unsigned char *buf = malloc(len);
	if (!buf) {
		return NULL;
	}
	if (!read_at(m, box->content, buf, len)) {
		free(buf);
		return NULL;
	}
	*len_out = len;
	return buf;
}

// ---------------------------------------------------------------------------
// sample tables
// ---------------------------------------------------------------------------

static void parse_stts(track_t *t, const unsigned char *b, size_t len) {
	if (len < 8) {
		return;
	}
	uint32_t n = be32(b + 4);
	if (n == 0 || n > MAX_RUNS || 8 + (uint64_t)n * 8 > len) {
		n = (uint32_t)((len - 8) / 8);
		if (n == 0 || n > MAX_RUNS) {
			return;
		}
	}
	free(t->stts);
	t->stts = calloc(n, sizeof(*t->stts));
	if (!t->stts) {
		return;
	}
	for (uint32_t i = 0; i < n; i++) {
		t->stts[i].count = be32(b + 8 + i * 8);
		t->stts[i].delta = be32(b + 12 + i * 8);
	}
	t->stts_count = n;
}

static void parse_stsc(track_t *t, const unsigned char *b, size_t len) {
	if (len < 8) {
		return;
	}
	uint32_t n = be32(b + 4);
	if (n == 0 || n > MAX_RUNS || 8 + (uint64_t)n * 12 > len) {
		n = (uint32_t)((len - 8) / 12);
		if (n == 0 || n > MAX_RUNS) {
			return;
		}
	}
	free(t->stsc);
	t->stsc = calloc(n, sizeof(*t->stsc));
	if (!t->stsc) {
		return;
	}
	for (uint32_t i = 0; i < n; i++) {
		t->stsc[i].first_chunk = be32(b + 8 + i * 12);
		t->stsc[i].samples_per_chunk = be32(b + 12 + i * 12);
	}
	t->stsc_count = n;
}

// ---------------------------------------------------------------------------
// the sample description: which codec, and its configuration
// ---------------------------------------------------------------------------

// MPEG-4 descriptors carry their length in 7-bit groups, high bit meaning
// "another byte follows".
static uint32_t descriptor_len(const unsigned char *b, size_t len, size_t *pos) {
	uint32_t value = 0;
	for (int i = 0; i < 4 && *pos < len; i++) {
		unsigned char c = b[(*pos)++];
		value = (value << 7) | (c & 0x7F);
		if (!(c & 0x80)) {
			break;
		}
	}
	return value;
}

static void parse_esds(track_t *t, const unsigned char *b, size_t len) {
	size_t pos = 4; // version + flags
	while (pos < len) {
		unsigned char tag = b[pos++];
		uint32_t size = descriptor_len(b, len, &pos);
		if (pos + size > len) {
			size = (uint32_t)(len - pos);
		}
		switch (tag) {
		case 0x03: { // ES_Descriptor
			size_t p = pos;
			if (p + 3 > len) {
				return;
			}
			unsigned char flags = b[p + 2];
			p += 3;
			if (flags & 0x80) {
				p += 2; // dependsOn_ES_ID
			}
			if (flags & 0x40) { // URL
				if (p >= len) {
					return;
				}
				p += 1 + b[p];
			}
			if (flags & 0x20) {
				p += 2; // OCR_ES_Id
			}
			pos = p;
			continue; // descend: the next descriptor is inside this one
		}
		case 0x04: { // DecoderConfigDescriptor
			if (pos + 13 > len) {
				return;
			}
			// The thirteen bytes, in order: objectTypeIndication (1),
			// streamType/upStream/reserved (1), bufferSizeDB (3), maxBitrate
			// (4), avgBitrate (4). Both bitrates are in bits per second.
			t->max_bitrate = be32(b + pos + 5);
			t->avg_bitrate = be32(b + pos + 9);
			pos += 13;
			continue; // descend
		}
		case 0x05: // DecoderSpecificInfo: the AudioSpecificConfig itself
			if (size > 0 && size <= sizeof(t->asc)) {
				memcpy(t->asc, b + pos, size);
				t->asc_len = (int)size;
			}
			return;
		default:
			pos += size;
			break;
		}
	}
}

// The rate and channel count as the AudioSpecificConfig states them: the sample
// entry's own 16.16 rate field cannot hold 88200 and up, and lies about SBR.
static void parse_asc(track_t *t) {
	static const uint32_t RATES[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
									 22050, 16000, 12000, 11025, 8000,  7350};
	if (t->asc_len < 2) {
		return;
	}
	uint32_t bits = ((uint32_t)t->asc[0] << 24) | ((uint32_t)t->asc[1] << 16) |
					((uint32_t)(t->asc_len > 2 ? t->asc[2] : 0) << 8) | (t->asc_len > 3 ? t->asc[3] : 0);
	int used = 0;
	uint32_t aot = bits >> 27;
	used = 5;
	if (aot == 31) {
		aot = 32 + ((bits >> 21) & 0x3F);
		used = 11;
	}
	(void)aot;
	uint32_t index = (bits >> (32 - used - 4)) & 0x0F;
	used += 4;
	uint32_t rate;
	if (index == 0x0F) {
		if (t->asc_len < 5) {
			return;
		}
		// 24 explicit bits, which may straddle the four bytes loaded above
		uint64_t wide = 0;
		for (int i = 0; i < 8 && i < t->asc_len; i++) {
			wide = (wide << 8) | t->asc[i];
		}
		int total = (t->asc_len < 8 ? t->asc_len : 8) * 8;
		rate = (uint32_t)((wide >> (total - used - 24)) & 0xFFFFFF);
		used += 24;
		uint32_t ch = (uint32_t)((wide >> (total - used - 4)) & 0x0F);
		if (rate) {
			t->sr = rate;
		}
		if (ch >= 1 && ch <= 7) {
			t->ch = (ch == 7) ? 8 : ch;
		}
		return;
	}
	if (index < sizeof(RATES) / sizeof(RATES[0])) {
		t->sr = RATES[index];
	}
	uint32_t ch = (bits >> (32 - used - 4)) & 0x0F;
	if (ch >= 1 && ch <= 7) {
		t->ch = (ch == 7) ? 8 : ch;
	}
}

// The ALACSpecificConfig: the body of the `alac` box is four bytes of version
// and flags followed by twenty-four bytes of configuration. Some files append
// more (the channel layout), which is of no use here.
#define ALAC_CONFIG_SIZE 24

static void parse_alac_config(track_t *t, const unsigned char *b, size_t len) {
	if (len < 4 + ALAC_CONFIG_SIZE) {
		return;
	}
	memcpy(t->asc, b + 4, ALAC_CONFIG_SIZE);
	t->asc_len = ALAC_CONFIG_SIZE;
}

// Loads the configuration box and hands it to the right parser. Separate
// because the same box can sit in two places -- a direct child of the sample
// entry, or inside `wave`.
static void take_codec_config(mp4_file_t *m, track_t *t, const box_t *box, mp4_codec_t codec) {
	size_t len = 0;
	unsigned char *body = load_box(m, box, &len);
	if (!body) {
		return;
	}
	if (codec == MP4_CODEC_ALAC) {
		parse_alac_config(t, body, len);
	} else {
		parse_esds(t, body, len);
	}
	free(body);
}

static void parse_stsd(mp4_file_t *m, track_t *t, const box_t *stsd) {
	unsigned char head[8];
	if (stsd->body_size < 8 || !read_at(m, stsd->content, head, 8)) {
		return;
	}
	uint64_t pos = stsd->content + 8;
	uint64_t end = stsd->content + stsd->body_size;
	uint32_t entries = be32(head + 4);

	for (uint32_t e = 0; e < entries; e++) {
		box_t entry;
		if (!read_box(m, pos, end, &entry)) {
			return;
		}
		pos = entry.next;

		// The two playable sample entries. A .m4a can hold either and the file
		// name does not tell them apart: this is where it becomes known which
		// of the two decoders is needed.
		mp4_codec_t codec = MP4_CODEC_NONE;
		if (entry.type == FOURCC('m', 'p', '4', 'a')) {
			codec = MP4_CODEC_AAC;
		} else if (entry.type == FOURCC('a', 'l', 'a', 'c')) {
			codec = MP4_CODEC_ALAC;
		} else {
			continue;
		}

		// AudioSampleEntry: 6 reserved + 2 data ref + 8 version block +
		// 2 channels + 2 sample size + 4 pre-defined/reserved + 4 rate(16.16)
		unsigned char audio[28];
		if (entry.body_size < 28 || !read_at(m, entry.content, audio, 28)) {
			continue;
		}
		t->ch = be16(audio + 16);
		t->sr = be32(audio + 24) >> 16;
		t->codec = codec;

		// Which configuration box to look for depends on the codec: `esds` for
		// AAC, `alac` for ALAC -- the latter carrying the same name as the
		// sample entry that contains it, which is how Apple writes it.
		uint32_t want = (codec == MP4_CODEC_ALAC) ? FOURCC('a', 'l', 'a', 'c') : FOURCC('e', 's', 'd', 's');

		// Children of the sample entry: the box directly, or wrapped in `wave`
		// as QuickTime writes it.
		uint64_t child = entry.content + 28;
		uint64_t child_end = entry.content + entry.body_size;
		while (child < child_end) {
			box_t c;
			if (!read_box(m, child, child_end, &c)) {
				break;
			}
			if (c.type == want) {
				take_codec_config(m, t, &c, codec);
			} else if (c.type == FOURCC('w', 'a', 'v', 'e')) {
				uint64_t g = c.content;
				uint64_t g_end = c.content + c.body_size;
				while (g < g_end) {
					box_t inner;
					if (!read_box(m, g, g_end, &inner)) {
						break;
					}
					if (inner.type == want) {
						take_codec_config(m, t, &inner, codec);
					}
					g = inner.next;
				}
			}
			child = c.next;
		}

		// The AudioSpecificConfig corrects what the sample entry gets wrong
		// (SBR, parametric stereo). The ALACSpecificConfig is read by its own
		// decoder and there is nothing to extract from it here.
		if (codec == MP4_CODEC_AAC) {
			parse_asc(t);
		}
		return;
	}
}

// ---------------------------------------------------------------------------
// tags
// ---------------------------------------------------------------------------

static void copy_text(char *dst, size_t dst_size, const unsigned char *src, size_t len) {
	if (len >= dst_size) {
		len = dst_size - 1;
	}
	memcpy(dst, src, len);
	dst[len] = '\0';
}

static const char *const GENRES[] = {
	"Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge", "Hip-Hop", "Jazz", "Metal",
	"New Age", "Oldies", "Other", "Pop", "R&B", "Rap", "Reggae", "Rock", "Techno", "Industrial",
	"Alternative", "Ska", "Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop",
	"Vocal", "Jazz+Funk", "Fusion", "Trance", "Classical", "Instrumental", "Acid", "House", "Game",
	"Sound Clip", "Gospel", "Noise", "AlternRock", "Bass", "Soul", "Punk", "Space", "Meditative",
	"Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic", "Darkwave", "Techno-Industrial",
	"Electronic", "Pop-Folk", "Eurodance", "Dream", "Southern Rock", "Comedy", "Cult", "Gangsta",
	"Top 40", "Christian Rap", "Pop/Funk", "Jungle", "Native American", "Cabaret", "New Wave",
	"Psychadelic", "Rave", "Showtunes", "Trailer", "Lo-Fi", "Tribal", "Acid Punk", "Acid Jazz",
	"Polka", "Retro", "Musical", "Rock & Roll", "Hard Rock", "Audiobook",
};

static void parse_ilst(mp4_file_t *m, const box_t *ilst) {
	uint64_t pos = ilst->content;
	uint64_t end = ilst->content + ilst->body_size;

	while (pos < end) {
		box_t item;
		if (!read_box(m, pos, end, &item)) {
			return;
		}
		pos = item.next;

		// A freeform tag: `mean` (the vendor), `name` (the key) and `data`, as
		// three boxes rather than one. Walked rather than read positionally,
		// because taggers differ about whether `mean` comes first, and handled
		// before the ordinary-item path below, which requires the first child
		// to be a `data` box and would discard every one of these.
		if (item.type == FOURCC('-', '-', '-', '-')) {
			char name[40] = "";
			char value[40] = "";
			uint64_t child = item.content;
			uint64_t child_end = item.content + item.body_size;
			while (child < child_end) {
				box_t c;
				if (!read_box(m, child, child_end, &c)) {
					break;
				}
				if (c.type == FOURCC('n', 'a', 'm', 'e') && c.body_size > 4) {
					unsigned char buf[64];
					size_t len = (size_t)(c.body_size - 4);
					if (len > sizeof(buf)) {
						len = sizeof(buf);
					}
					if (read_at(m, c.content + 4, buf, len)) {
						copy_text(name, sizeof(name), buf, len);
					}
				} else if (c.type == FOURCC('d', 'a', 't', 'a') && c.body_size > 8) {
					unsigned char buf[64];
					size_t len = (size_t)(c.body_size - 8);
					if (len > sizeof(buf)) {
						len = sizeof(buf);
					}
					if (read_at(m, c.content + 8, buf, len)) {
						copy_text(value, sizeof(value), buf, len);
					}
				}
				child = c.next;
			}
			if (name[0] && value[0] && m->freeform_count < MAX_FREEFORM) {
				snprintf(m->freeform[m->freeform_count].name, sizeof(m->freeform[0].name), "%s", name);
				snprintf(m->freeform[m->freeform_count].value, sizeof(m->freeform[0].value), "%s", value);
				m->freeform_count++;
			}
			continue;
		}

		// Every item holds one `data` box: 4 bytes of type, 4 of locale, payload.
		box_t data;
		if (!read_box(m, item.content, item.content + item.body_size, &data)) {
			continue;
		}
		if (data.type != FOURCC('d', 'a', 't', 'a') || data.body_size < 8) {
			continue;
		}

		if (item.type == FOURCC('c', 'o', 'v', 'r')) {
			unsigned char head[8];
			if (read_at(m, data.content, head, 8)) {
				uint32_t kind = be32(head) & 0x00FFFFFF;
				m->cover_off = data.content + 8;
				m->cover_size = (uint32_t)(data.body_size - 8);
				m->cover_png = (kind == 14);
			}
			continue;
		}

		size_t len = 0;
		unsigned char *body = load_box(m, &data, &len);
		if (!body || len < 8) {
			free(body);
			continue;
		}
		const unsigned char *value = body + 8;
		size_t value_len = len - 8;

		switch (item.type) {
		case FOURCC(0xA9, 'n', 'a', 'm'):
			copy_text(m->title, sizeof(m->title), value, value_len);
			break;
		case FOURCC(0xA9, 'A', 'R', 'T'):
			copy_text(m->artist, sizeof(m->artist), value, value_len);
			break;
		case FOURCC('a', 'A', 'R', 'T'):
			copy_text(m->album_artist, sizeof(m->album_artist), value, value_len);
			break;
		case FOURCC(0xA9, 'a', 'l', 'b'):
			copy_text(m->album, sizeof(m->album), value, value_len);
			break;
		case FOURCC(0xA9, 'g', 'e', 'n'):
			copy_text(m->genre, sizeof(m->genre), value, value_len);
			break;
		case FOURCC('g', 'n', 'r', 'e'):
			if (value_len >= 2) {
				uint32_t index = be16(value);
				if (index >= 1 && index <= sizeof(GENRES) / sizeof(GENRES[0])) {
					snprintf(m->genre, sizeof(m->genre), "%s", GENRES[index - 1]);
				}
			}
			break;
		case FOURCC(0xA9, 'd', 'a', 'y'): {
			char buf[32];
			copy_text(buf, sizeof(buf), value, value_len);
			m->year = atoi(buf);
			break;
		}
		case FOURCC('t', 'r', 'k', 'n'):
			if (value_len >= 4) {
				m->track_number = be16(value + 2);
			}
			break;
		// The disc pair has the same layout as trkn: a 16-bit pad, the number,
		// then the total.
		case FOURCC('d', 'i', 's', 'k'):
			if (value_len >= 4) {
				m->disc_number = be16(value + 2);
			}
			break;
		default:
			break;
		}
		free(body);
	}
}

// ---------------------------------------------------------------------------
// chapters
// ---------------------------------------------------------------------------

static void add_chapter(mp4_file_t *m, double start, const char *title) {
	if (m->chapter_count >= MAX_CHAPTERS) {
		return;
	}
	// Grown in blocks rather than allocated at the cap: a chapter entry is
	// two hundred bytes, and most books have twenty of them, not two thousand.
	if (m->chapter_count >= m->chapter_capacity) {
		int want = m->chapter_capacity ? m->chapter_capacity * 2 : 32;
		mp4_chapter_t *grown = realloc(m->chapters, (size_t)want * sizeof(*grown));
		if (!grown) {
			return;
		}
		m->chapters = grown;
		m->chapter_capacity = want;
	}
	mp4_chapter_t *c = &m->chapters[m->chapter_count++];
	c->start = start < 0 ? 0 : start;
	snprintf(c->title, sizeof(c->title), "%s", title ? title : "");
}

// Nero's list, the one most conversion tools write.
static void parse_chpl(mp4_file_t *m, const box_t *box) {
	size_t len = 0;
	unsigned char *b = load_box(m, box, &len);
	if (!b) {
		return;
	}
	size_t pos = 0;
	if (len < 5) {
		free(b);
		return;
	}
	unsigned char version = b[0];
	pos = 4;
	if (version) {
		pos += 4; // a word nobody has ever documented
	}
	if (pos >= len) {
		free(b);
		return;
	}
	unsigned int count = b[pos++];

	for (unsigned int i = 0; i < count; i++) {
		if (pos + 9 > len) {
			break;
		}
		uint64_t start = be64(b + pos);
		pos += 8;
		unsigned int name_len = b[pos++];
		if (pos + name_len > len) {
			break;
		}
		char title[192];
		copy_text(title, sizeof(title), b + pos, name_len);
		pos += name_len;
		add_chapter(m, (double)start / 10000000.0, title); // 100-nanosecond units
	}
	free(b);
}

static bool frame_location(mp4_file_t *m, int track_index, uint32_t index, uint64_t *offset, uint32_t *size);

// QuickTime's way: a text track the audio track points at, one sample per
// chapter, each holding its own title.
static void parse_chapter_track(mp4_file_t *m, const track_t *text) {
	int index = (int)(text - m->tracks);
	uint32_t count = text->sample_count;
	if (count > MAX_CHAPTERS) {
		count = MAX_CHAPTERS;
	}

	uint64_t ticks = 0;
	uint32_t run = 0, in_run = 0;

	for (uint32_t i = 0; i < count; i++) {
		uint64_t offset = 0;
		uint32_t size = 0;
		if (!frame_location(m, index, i, &offset, &size)) {
			break;
		}

		char title[192] = "";
		if (size >= 2) {
			unsigned char head[2];
			if (read_at(m, offset, head, 2)) {
				uint32_t text_len = be16(head);
				if (text_len > size - 2) {
					text_len = size - 2;
				}
				if (text_len > sizeof(title) - 1) {
					text_len = sizeof(title) - 1;
				}
				unsigned char buf[192];
				if (text_len && read_at(m, offset + 2, buf, text_len)) {
					// A byte-order mark means UTF-16, which is not carried
					// into the interface; leaving the title empty is better
					// than showing mojibake.
					if (!(text_len >= 2 && ((buf[0] == 0xFE && buf[1] == 0xFF) || (buf[0] == 0xFF && buf[1] == 0xFE)))) {
						copy_text(title, sizeof(title), buf, text_len);
					}
				}
			}
		}

		// An unnamed chapter is handed over unnamed. Naming it is the job of
		// whoever shows it, which is the only place that knows the language.
		double when = text->timescale ? (double)ticks / (double)text->timescale : 0;
		add_chapter(m, when, title);

		// advance the clock by this sample's duration
		while (run < text->stts_count && in_run >= text->stts[run].count) {
			run++;
			in_run = 0;
		}
		if (run < text->stts_count) {
			ticks += text->stts[run].delta;
			in_run++;
		}
	}
}

static int chapter_compare(const void *a, const void *b) {
	const mp4_chapter_t *x = a, *y = b;
	if (x->start < y->start) {
		return -1;
	}
	if (x->start > y->start) {
		return 1;
	}
	return 0;
}

// ---------------------------------------------------------------------------
// the recursive parse
// ---------------------------------------------------------------------------

static void parse_boxes(mp4_file_t *m, uint64_t start, uint64_t end, track_t *current, int depth) {
	if (depth > MAX_DEPTH) {
		return;
	}
	uint64_t pos = start;

	while (pos < end) {
		box_t box;
		if (!read_box(m, pos, end, &box)) {
			return;
		}
		uint64_t next = box.next;

		switch (box.type) {
		case FOURCC('m', 'o', 'o', 'v'):
		case FOURCC('m', 'd', 'i', 'a'):
		case FOURCC('m', 'i', 'n', 'f'):
		case FOURCC('s', 't', 'b', 'l'):
		case FOURCC('u', 'd', 't', 'a'):
		case FOURCC('t', 'r', 'e', 'f'):
			parse_boxes(m, box.content, box.content + box.body_size, current, depth + 1);
			break;

		case FOURCC('t', 'r', 'a', 'k'):
			if (m->track_count < MAX_TRACKS) {
				track_t *t = &m->tracks[m->track_count++];
				memset(t, 0, sizeof(*t));
				parse_boxes(m, box.content, box.content + box.body_size, t, depth + 1);
			}
			break;

		case FOURCC('t', 'k', 'h', 'd'):
			if (current) {
				unsigned char b[24];
				if (read_at(m, box.content, b, 24)) {
					current->id = (b[0] == 1) ? be32(b + 20) : be32(b + 12);
				}
			}
			break;

		case FOURCC('m', 'd', 'h', 'd'):
			if (current) {
				unsigned char b[32];
				if (read_at(m, box.content, b, 32)) {
					if (b[0] == 1) {
						current->timescale = be32(b + 20);
						current->duration = be64(b + 24);
					} else {
						current->timescale = be32(b + 12);
						current->duration = be32(b + 16);
					}
				}
			}
			break;

		case FOURCC('h', 'd', 'l', 'r'):
			if (current) {
				unsigned char b[12];
				if (read_at(m, box.content, b, 12)) {
					current->handler = be32(b + 8);
				}
			}
			break;

		case FOURCC('c', 'h', 'a', 'p'):
			if (current && box.body_size >= 4) {
				unsigned char b[4];
				if (read_at(m, box.content, b, 4)) {
					current->chap_id = be32(b);
				}
			}
			break;

		case FOURCC('s', 't', 's', 'd'):
			if (current) {
				parse_stsd(m, current, &box);
			}
			break;

		case FOURCC('s', 't', 't', 's'):
			if (current) {
				size_t len = 0;
				unsigned char *b = load_box(m, &box, &len);
				if (b) {
					parse_stts(current, b, len);
					free(b);
				}
			}
			break;

		case FOURCC('s', 't', 's', 'c'):
			if (current) {
				size_t len = 0;
				unsigned char *b = load_box(m, &box, &len);
				if (b) {
					parse_stsc(current, b, len);
					free(b);
				}
			}
			break;

		case FOURCC('s', 't', 's', 'z'):
			if (current && box.body_size >= 12) {
				unsigned char b[12];
				if (read_at(m, box.content, b, 12)) {
					current->uniform_size = be32(b + 4);
					current->sample_count = be32(b + 8);
					current->size_table_off = box.content + 12;
					current->size_field_bits = 32;
				}
			}
			break;

		case FOURCC('s', 't', 'z', '2'):
			if (current && box.body_size >= 12) {
				unsigned char b[12];
				if (read_at(m, box.content, b, 12)) {
					current->uniform_size = 0;
					current->size_field_bits = b[7]; // 4, 8 or 16
					current->sample_count = be32(b + 8);
					current->size_table_off = box.content + 12;
				}
			}
			break;

		case FOURCC('s', 't', 'c', 'o'):
			if (current && box.body_size >= 8) {
				unsigned char b[8];
				if (read_at(m, box.content, b, 8)) {
					current->chunk_count = be32(b + 4);
					current->chunk_table_off = box.content + 8;
					current->chunk_64 = false;
				}
			}
			break;

		case FOURCC('c', 'o', '6', '4'):
			if (current && box.body_size >= 8) {
				unsigned char b[8];
				if (read_at(m, box.content, b, 8)) {
					current->chunk_count = be32(b + 4);
					current->chunk_table_off = box.content + 8;
					current->chunk_64 = true;
				}
			}
			break;

		case FOURCC('m', 'e', 't', 'a'): {
			// A FullBox everywhere except in files QuickTime wrote, where the
			// four version bytes are simply absent. Told apart by looking for
			// a box type where the first child should be.
			unsigned char probe[8];
			uint64_t child = box.content;
			if (box.body_size >= 8 && read_at(m, box.content, probe, 8)) {
				uint32_t maybe = be32(probe + 4);
				if (maybe != FOURCC('h', 'd', 'l', 'r') && maybe != FOURCC('i', 'l', 's', 't') &&
					maybe != FOURCC('k', 'e', 'y', 's')) {
					child += 4;
				}
			} else {
				child += 4;
			}
			parse_boxes(m, child, box.content + box.body_size, current, depth + 1);
			break;
		}

		case FOURCC('i', 'l', 's', 't'):
			parse_ilst(m, &box);
			break;

		case FOURCC('c', 'h', 'p', 'l'):
			parse_chpl(m, &box);
			break;

		default:
			break;
		}

		if (next <= pos) {
			return; // a zero-length box would spin forever
		}
		pos = next;
	}
}

// ---------------------------------------------------------------------------
// locating a sample
// ---------------------------------------------------------------------------

static bool size_at(mp4_file_t *m, int track_index, uint32_t index, uint32_t *out) {
	track_t *t = &m->tracks[track_index];

	if (t->uniform_size) {
		*out = t->uniform_size;
		return true;
	}
	if (index >= t->sample_count || t->size_field_bits <= 0) {
		return false;
	}

	if (m->cache_track != track_index || index < m->cache_first || index >= m->cache_first + m->cache_count) {
		if (!m->size_cache) {
			m->size_cache = malloc(SIZE_WINDOW * sizeof(uint32_t));
			if (!m->size_cache) {
				return false;
			}
		}
		uint32_t first = index;
		uint32_t want = t->sample_count - first;
		if (want > SIZE_WINDOW) {
			want = SIZE_WINDOW;
		}

		unsigned char raw[SIZE_WINDOW * 4];
		size_t bytes;
		uint64_t off;
		switch (t->size_field_bits) {
		case 32:
			bytes = (size_t)want * 4;
			off = t->size_table_off + (uint64_t)first * 4;
			break;
		case 16:
			bytes = (size_t)want * 2;
			off = t->size_table_off + (uint64_t)first * 2;
			break;
		case 8:
			bytes = want;
			off = t->size_table_off + first;
			break;
		case 4:
			// two per byte; start on a byte boundary and drop the odd first
			first &= ~1u;
			want = t->sample_count - first;
			if (want > SIZE_WINDOW) {
				want = SIZE_WINDOW;
			}
			bytes = (size_t)(want + 1) / 2;
			off = t->size_table_off + first / 2;
			break;
		default:
			return false;
		}
		if (!read_at(m, off, raw, bytes)) {
			return false;
		}
		for (uint32_t i = 0; i < want; i++) {
			switch (t->size_field_bits) {
			case 32:
				m->size_cache[i] = be32(raw + i * 4);
				break;
			case 16:
				m->size_cache[i] = be16(raw + i * 2);
				break;
			case 8:
				m->size_cache[i] = raw[i];
				break;
			default:
				m->size_cache[i] = (i & 1) ? (raw[i / 2] & 0x0F) : (raw[i / 2] >> 4);
				break;
			}
		}
		m->cache_first = first;
		m->cache_count = want;
		m->cache_track = track_index;
	}

	*out = m->size_cache[index - m->cache_first];
	return true;
}

static bool chunk_offset(const mp4_file_t *m, const track_t *t, uint32_t chunk /* 1-based */, uint64_t *out) {
	if (chunk == 0 || chunk > t->chunk_count) {
		return false;
	}
	unsigned char b[8];
	if (t->chunk_64) {
		if (!read_at(m, t->chunk_table_off + (uint64_t)(chunk - 1) * 8, b, 8)) {
			return false;
		}
		*out = be64(b);
	} else {
		if (!read_at(m, t->chunk_table_off + (uint64_t)(chunk - 1) * 4, b, 4)) {
			return false;
		}
		*out = be32(b);
	}
	return true;
}

// Where sample `index` of a track lives, and how long it is. The chunk map is
// walked run by run -- there are a handful of runs even in a long book.
//
// The fast path at the top is not a nicety. A sample's offset is its chunk's
// offset plus the sizes of every sample before it *in that chunk*, and ffmpeg
// writes audio chunks of five and a half thousand samples, so working that sum
// out from scratch for every frame re-reads kilobytes of the size table per
// twenty-three milliseconds of audio -- about a megabyte a second off the card.
// Playback asks for frame N+1 after frame N, so the answer is carried forward.
static bool frame_location(mp4_file_t *m, int track_index, uint32_t index, uint64_t *offset, uint32_t *size) {
	track_t *t = &m->tracks[track_index];
	if (index >= t->sample_count || t->stsc_count == 0 || t->chunk_count == 0) {
		return false;
	}

	if (track_index == m->seq_track && index == m->seq_index && index < m->seq_chunk_end) {
		if (!size_at(m, track_index, index, size)) {
			return false;
		}
		*offset = m->seq_offset;
		m->seq_index = index + 1;
		m->seq_offset += *size;
		return true;
	}

	uint32_t chunk = 0, chunk_first_sample = 0, chunk_last_sample = 0;
	uint64_t seen = 0;
	bool found = false;

	for (uint32_t r = 0; r < t->stsc_count; r++) {
		uint32_t first = t->stsc[r].first_chunk;
		uint32_t spc = t->stsc[r].samples_per_chunk;
		uint32_t last = (r + 1 < t->stsc_count) ? t->stsc[r + 1].first_chunk : t->chunk_count + 1;
		if (spc == 0 || last <= first) {
			continue;
		}
		uint64_t in_run = (uint64_t)(last - first) * spc;
		if ((uint64_t)index < seen + in_run) {
			uint32_t k = (uint32_t)(((uint64_t)index - seen) / spc);
			chunk = first + k;
			chunk_first_sample = (uint32_t)(seen + (uint64_t)k * spc);
			chunk_last_sample = chunk_first_sample + spc; // the first sample of the next chunk
			found = true;
			break;
		}
		seen += in_run;
	}
	if (!found) {
		return false;
	}

	uint64_t base;
	if (!chunk_offset(m, t, chunk, &base)) {
		return false;
	}

	for (uint32_t i = chunk_first_sample; i < index; i++) {
		uint32_t s = 0;
		if (!size_at(m, track_index, i, &s)) {
			return false;
		}
		base += s;
	}
	if (!size_at(m, track_index, index, size)) {
		return false;
	}
	*offset = base;

	// Hand the next sample its answer, so playing straight through a chunk
	// costs one walk rather than one per frame.
	m->seq_track = track_index;
	m->seq_index = index + 1;
	m->seq_offset = base + *size;
	m->seq_chunk_end = chunk_last_sample;
	return true;
}

// ---------------------------------------------------------------------------
// public interface
// ---------------------------------------------------------------------------

bool mp4_probe(const char *path) {
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		return false;
	}
	unsigned char head[8];
	bool ok = false;
	if (read(fd, head, 8) == 8) {
		uint32_t type = be32(head + 4);
		ok = type == FOURCC('f', 't', 'y', 'p') || type == FOURCC('m', 'o', 'o', 'v') ||
			 type == FOURCC('s', 'k', 'i', 'p') || type == FOURCC('w', 'i', 'd', 'e') ||
			 type == FOURCC('f', 'r', 'e', 'e') || type == FOURCC('m', 'd', 'a', 't');
	}
	close(fd);
	return ok;
}

mp4_file_t *mp4_open(const char *path) {
	mp4_file_t *m = calloc(1, sizeof(*m));
	if (!m) {
		return NULL;
	}
	m->audio = -1;
	m->cache_track = -1;

	m->fd = open(path, O_RDONLY);
	if (m->fd < 0) {
		free(m);
		return NULL;
	}
	off_t end = lseek(m->fd, 0, SEEK_END);
	if (end <= 0) {
		close(m->fd);
		free(m);
		return NULL;
	}
	m->file_size = (uint64_t)end;

	parse_boxes(m, 0, m->file_size, NULL, 0);

	for (int i = 0; i < m->track_count; i++) {
		track_t *t = &m->tracks[i];
		if (t->handler == FOURCC('s', 'o', 'u', 'n') && t->codec != MP4_CODEC_NONE && t->asc_len > 0 &&
			t->sample_count > 0) {
			m->audio = i;
			break;
		}
	}
	if (m->audio < 0) {
		mp4_close(m);
		return NULL;
	}

	// Chapters: the audio track's own text track if it names one, otherwise
	// whatever `chpl` already put in the list.
	uint32_t chap = m->tracks[m->audio].chap_id;
	if (chap && m->chapter_count == 0) {
		for (int i = 0; i < m->track_count; i++) {
			if (m->tracks[i].id == chap && m->tracks[i].sample_count > 0) {
				parse_chapter_track(m, &m->tracks[i]);
				break;
			}
		}
	}
	if (m->chapter_count > 1) {
		qsort(m->chapters, (size_t)m->chapter_count, sizeof(*m->chapters), chapter_compare);
	}
	// A mark past the end of the audio is not a chapter anybody can reach --
	// it happens when a chapter file was written for a longer edit of the same
	// book -- and leaving it in the list gives a row that seeks into silence.
	double length = mp4_duration_seconds(m);
	if (length > 0) {
		while (m->chapter_count > 0 && m->chapters[m->chapter_count - 1].start >= length) {
			m->chapter_count--;
		}
	}
	// A single mark at zero is not a chapter list, it is the whole book.
	if (m->chapter_count == 1 && m->chapters[0].start <= 0.001) {
		m->chapter_count = 0;
	}

	return m;
}

void mp4_close(mp4_file_t *m) {
	if (!m) {
		return;
	}
	for (int i = 0; i < m->track_count; i++) {
		free(m->tracks[i].stsc);
		free(m->tracks[i].stts);
	}
	free(m->chapters);
	free(m->size_cache);
	if (m->fd >= 0) {
		close(m->fd);
	}
	free(m);
}

mp4_codec_t mp4_audio_codec(const mp4_file_t *m) {
	if (!m || m->audio < 0) {
		return MP4_CODEC_NONE;
	}
	return m->tracks[m->audio].codec;
}

bool mp4_has_video(const mp4_file_t *m) {
	if (!m) {
		return false;
	}
	for (int i = 0; i < m->track_count; i++) {
		// A single frame is cover art written as a video track -- some
		// encoders do this -- and must not disqualify an album.
		if (m->tracks[i].handler == FOURCC('v', 'i', 'd', 'e') && m->tracks[i].sample_count > 1) {
			return true;
		}
	}
	return false;
}

bool mp4_file_has_video(const char *path) {
	mp4_file_t *m = mp4_open(path);
	if (!m) {
		// Not an MP4 this code can read: there is no video to keep out, and
		// the caller will not index it anyway.
		return false;
	}
	bool video = mp4_has_video(m);
	mp4_close(m);
	return video;
}

const unsigned char *mp4_audio_config(const mp4_file_t *m, int *len) {
	if (!m || m->audio < 0) {
		return NULL;
	}
	if (len) {
		*len = m->tracks[m->audio].asc_len;
	}
	return m->tracks[m->audio].asc;
}

int mp4_audio_channels(const mp4_file_t *m) {
	return (m && m->audio >= 0) ? (int)m->tracks[m->audio].ch : 0;
}

int mp4_audio_sample_rate(const mp4_file_t *m) {
	return (m && m->audio >= 0) ? (int)m->tracks[m->audio].sr : 0;
}

double mp4_duration_seconds(const mp4_file_t *m) {
	if (!m || m->audio < 0) {
		return 0;
	}
	const track_t *t = &m->tracks[m->audio];
	if (t->timescale == 0) {
		return 0;
	}
	if (t->duration) {
		return (double)t->duration / (double)t->timescale;
	}
	uint64_t ticks = 0;
	for (uint32_t i = 0; i < t->stts_count; i++) {
		ticks += (uint64_t)t->stts[i].count * t->stts[i].delta;
	}
	return (double)ticks / (double)t->timescale;
}

uint32_t mp4_audio_frame_count(const mp4_file_t *m) {
	return (m && m->audio >= 0) ? m->tracks[m->audio].sample_count : 0;
}

int mp4_audio_bitrate_kbps(const mp4_file_t *m) {
	if (!m || m->audio < 0) {
		return 0;
	}
	const track_t *t = &m->tracks[m->audio];
	// The average when present, the maximum when the average is zero: many
	// encoders fill in only the latter, and for a constant-bitrate stream --
	// which is what streaming services send -- the two are the same number.
	uint32_t bps = t->avg_bitrate ? t->avg_bitrate : t->max_bitrate;
	if (bps == 0) {
		return 0;
	}
	// Bits per second to kbps, rounded: 320000 -> 320, 319998 -> 320.
	return (int)((bps + 500) / 1000);
}

uint32_t mp4_max_frame_size(const mp4_file_t *m) {
	// Exact when every frame is the same length; otherwise unknown, because
	// finding the real maximum means reading the whole size table -- six
	// megabytes for a long book, for a number the grow-on-demand path in
	// mp4_read_audio_frame() supplies for free.
	return (m && m->audio >= 0) ? m->tracks[m->audio].uniform_size : 0;
}

int mp4_read_audio_frame(mp4_file_t *m, uint32_t index, unsigned char *buf, uint32_t buf_size) {
	if (!m || m->audio < 0) {
		return 0;
	}
	uint64_t offset = 0;
	uint32_t size = 0;
	if (!frame_location(m, m->audio, index, &offset, &size) || size == 0) {
		return 0;
	}
	if (size > buf_size) {
		return -(int)size;
	}
	if (!read_at(m, offset, buf, size)) {
		return 0;
	}
	return (int)size;
}

double mp4_frame_time(const mp4_file_t *m, uint32_t index) {
	if (!m || m->audio < 0) {
		return 0;
	}
	const track_t *t = &m->tracks[m->audio];
	if (t->timescale == 0) {
		return 0;
	}
	uint64_t ticks = 0, seen = 0;
	for (uint32_t r = 0; r < t->stts_count; r++) {
		uint64_t count = t->stts[r].count;
		if ((uint64_t)index < seen + count) {
			ticks += ((uint64_t)index - seen) * t->stts[r].delta;
			return (double)ticks / (double)t->timescale;
		}
		seen += count;
		ticks += count * t->stts[r].delta;
	}
	return (double)ticks / (double)t->timescale;
}

uint32_t mp4_frame_at_time(const mp4_file_t *m, double secs) {
	if (!m || m->audio < 0) {
		return 0;
	}
	const track_t *t = &m->tracks[m->audio];
	if (t->timescale == 0 || secs <= 0) {
		return 0;
	}
	uint64_t want = (uint64_t)(secs * (double)t->timescale);
	uint64_t ticks = 0, seen = 0;
	for (uint32_t r = 0; r < t->stts_count; r++) {
		uint64_t count = t->stts[r].count;
		uint64_t delta = t->stts[r].delta;
		if (delta == 0) {
			seen += count;
			continue;
		}
		uint64_t span = count * delta;
		if (want < ticks + span) {
			uint64_t index = seen + (want - ticks) / delta;
			return index >= t->sample_count ? t->sample_count - 1 : (uint32_t)index;
		}
		ticks += span;
		seen += count;
	}
	return t->sample_count ? t->sample_count - 1 : 0;
}

int mp4_chapter_count(const mp4_file_t *m) { return m ? m->chapter_count : 0; }

const mp4_chapter_t *mp4_chapter(const mp4_file_t *m, int index) {
	if (!m || index < 0 || index >= m->chapter_count) {
		return NULL;
	}
	return &m->chapters[index];
}

int mp4_chapter_at_time(const mp4_file_t *m, double secs) {
	if (!m || m->chapter_count == 0) {
		return -1;
	}
	int found = 0;
	for (int i = 0; i < m->chapter_count; i++) {
		if (m->chapters[i].start <= secs + 0.001) {
			found = i;
		} else {
			break;
		}
	}
	return found;
}

const char *mp4_tag_title(const mp4_file_t *m) { return m ? m->title : ""; }
const char *mp4_tag_artist(const mp4_file_t *m) { return m ? m->artist : ""; }
const char *mp4_tag_album_artist(const mp4_file_t *m) { return m ? m->album_artist : ""; }
const char *mp4_tag_album(const mp4_file_t *m) { return m ? m->album : ""; }
const char *mp4_tag_genre(const mp4_file_t *m) { return m ? m->genre : ""; }
int mp4_tag_year(const mp4_file_t *m) { return m ? m->year : 0; }

const char *mp4_tag_freeform(const mp4_file_t *m, const char *name) {
	if (!m || !name) {
		return NULL;
	}
	for (int i = 0; i < m->freeform_count; i++) {
		if (strcasecmp(m->freeform[i].name, name) == 0) {
			return m->freeform[i].value;
		}
	}
	return NULL;
}
int mp4_tag_track_number(const mp4_file_t *m) { return m ? m->track_number : 0; }
int mp4_tag_disc_number(const mp4_file_t *m) { return m ? m->disc_number : 0; }

bool mp4_cover_art(const mp4_file_t *m, uint64_t *offset, uint32_t *size, bool *is_png) {
	if (!m || m->cover_size == 0) {
		return false;
	}
	if (offset) {
		*offset = m->cover_off;
	}
	if (size) {
		*size = m->cover_size;
	}
	if (is_png) {
		*is_png = m->cover_png;
	}
	return true;
}

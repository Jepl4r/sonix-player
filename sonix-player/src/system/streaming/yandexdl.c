#define _GNU_SOURCE 1 // fopencookie

#include "yandexdl.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/gui/shell/gui.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/device/system.h"
#include "src/system/device/usb.h"
#include "src/system/library/library.h"
#include "src/system/library/mp4flac.h"
#include "src/system/net/http.h"
#include "src/system/streaming/podcastdl.h"
#include "src/system/streaming/yandexcache.h"

// See yandexdl.h.

#define LIBRARY_FOLDER "Yandex Music"
#define QUEUE_MAX 300 // about 400 KB at the very most, an album is ~20
#define COVER_LIMIT (4 * 1024 * 1024)
// A FLAC's own metadata blocks are kept (the seek table, an embedded
// picture...), held while the header is rewritten; a block past this is
// dropped rather than held.
#define KEEP_BLOCK_MAX (1024 * 1024)

typedef struct {
	bool album; // `track` holds only the album's id and title, expanded later
	yandex_track_t track;
} entry_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static entry_t *queue; // allocated with the first entry, freed when it empties
static int queue_len;
static bool worker_started;
static bool running; // an entry is being worked on
static char root[400];
static unsigned generation; // raised by every cancel: work from before it stops
static yandexdl_status_t status;
static void (*listener)(yandexdl_event_t, int, const char *);

void yandexdl_set_listener(void (*cb)(yandexdl_event_t event, int count, const char *text)) { listener = cb; }

static void tell(yandexdl_event_t event, int count, const char *text) {
	if (listener) {
		listener(event, count, text ? text : "");
	}
}

// ---------------------------------------------------------------------------
// where the files go
// ---------------------------------------------------------------------------

// The card is there and may be written: not pulled, not handed to a computer.
static bool card_usable(void) { return root[0] && storage_card_attached() && !usb_storage_active() && storage_sd_writable(); }

// <card>/<the first scan folder, else Music>.
static void music_root(char *out, size_t size) {
	int count = 0;
	char **names = library_scan_folders(&count);
	const char *folder = "Music";
	for (int i = 0; i < count; i++) {
		if (names[i] && names[i][0]) {
			folder = names[i];
			break;
		}
	}
	snprintf(out, size, "%s/%s", root, folder);
	library_scan_folders_free(names, count);
}

static bool make_dirs(const char *path) {
	char buf[1024];
	snprintf(buf, sizeof(buf), "%s", path);
	for (char *p = buf + strlen(root) + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
				return false;
			}
			*p = '/';
		}
	}
	return mkdir(buf, 0755) == 0 || errno == EEXIST;
}

// ---------------------------------------------------------------------------
// tags, written into the stream on its way to the card
//
// The file is not tagged afterwards -- that would be a second copy of thirty
// megabytes on the card for every FLAC -- but while it is written, through a
// FILE of our own (fopencookie) in front of the real one. Both download paths
// (a plain copy and the flac-mp4 unwrapper) write to a FILE and never seek, so
// neither has to know.
//
// FLAC: the metadata blocks at the head are read as they pass; the source's
// own Vorbis comment and padding are dropped, the rest kept in order
// (STREAMINFO first), and when the last block has gone by out goes "fLaC",
// the kept blocks, our Vorbis comment and a little padding for later edits.
// The audio frames then pass untouched.
//
// MP3: an ID3v2 tag at the head of the source, if any, is skipped and ours
// takes its place. Anything else passes untouched.
// ---------------------------------------------------------------------------

typedef struct {
	char title[YANDEX_TITLE_MAX];
	char artist[YANDEX_NAME_MAX];
	char album[YANDEX_TITLE_MAX];
	char album_artist[YANDEX_NAME_MAX];
	char genre[48];
	int track, tracks, disc, discs, year;
} tags_t;

enum { SINK_FLAC, SINK_MP3, SINK_MP4 };
enum { S_MAGIC, S_BLOCK_HEAD, S_BLOCK_BODY, S_ID3_HEAD, S_ID3_SKIP, S_PASS, S_BOX_HEAD, S_BOX_HOLD, S_BOX_PASS };

typedef struct {
	FILE *out;
	int kind;
	int state;
	const tags_t *tags;
	uint8_t head[16];
	size_t head_len;
	uint8_t *keep; // FLAC: the kept blocks, headers included; MP4: the box held
	size_t keep_len, keep_cap;
	bool keep_this, last;
	uint32_t left; // of the block body, or the ID3 tag, still to come
	bool failed;
	// MP4 (see mp4_sink_write)
	uint64_t pos;	   // bytes of the source taken so far
	uint64_t box_left; // of the box passing or being held
	uint32_t box_type;
	uint64_t moov_at; // where the source's moov began
	int64_t delta;	  // how much the rewritten moov grew by
	bool moov_done;
	bool *tagged; // the caller's: set once the moov carries our tags
} sink_t;

typedef struct {
	uint8_t *data;
	size_t len, cap;
	bool failed;
} buf_t;

static void buf_add(buf_t *b, const void *data, size_t len) {
	if (b->failed || len == 0) {
		return;
	}
	if (b->len + len > b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 1024;
		while (cap < b->len + len) {
			cap *= 2;
		}
		uint8_t *grown = realloc(b->data, cap);
		if (!grown) {
			b->failed = true;
			return;
		}
		b->data = grown;
		b->cap = cap;
	}
	memcpy(b->data + b->len, data, len);
	b->len += len;
}

static void le32(buf_t *b, uint32_t v) {
	uint8_t x[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
	buf_add(b, x, 4);
}

static void vorbis_field(buf_t *b, const char *name, const char *value) {
	if (!value || !value[0]) {
		return;
	}
	le32(b, (uint32_t)(strlen(name) + 1 + strlen(value)));
	buf_add(b, name, strlen(name));
	buf_add(b, "=", 1);
	buf_add(b, value, strlen(value));
}

static void vorbis_number(buf_t *b, const char *name, int value) {
	if (value > 0) {
		char text[16];
		snprintf(text, sizeof(text), "%d", value);
		vorbis_field(b, name, text);
	}
}

// fLaC + the kept blocks + VORBIS_COMMENT + PADDING (last).
static bool flac_header_out(sink_t *k) {
	static const char VENDOR[] = "Sonix Player";
	buf_t c = {0};
	le32(&c, sizeof(VENDOR) - 1);
	buf_add(&c, VENDOR, sizeof(VENDOR) - 1);
	size_t count_at = c.len;
	le32(&c, 0);
	size_t before = c.len;
	const tags_t *t = k->tags;
	vorbis_field(&c, "TITLE", t->title);
	vorbis_field(&c, "ARTIST", t->artist);
	vorbis_field(&c, "ALBUM", t->album);
	vorbis_field(&c, "ALBUMARTIST", t->album_artist);
	vorbis_number(&c, "TRACKNUMBER", t->track);
	vorbis_number(&c, "TRACKTOTAL", t->tracks);
	vorbis_number(&c, "DISCNUMBER", t->disc);
	vorbis_number(&c, "DISCTOTAL", t->discs);
	vorbis_number(&c, "DATE", t->year);
	vorbis_field(&c, "GENRE", t->genre);
	if (c.failed) {
		free(c.data);
		return false;
	}
	// The comment count, now that it is known: walk what was added.
	uint32_t fields = 0;
	for (size_t at = before; at + 4 <= c.len; fields++) {
		at += 4 + ((size_t)c.data[at] | (size_t)c.data[at + 1] << 8 | (size_t)c.data[at + 2] << 16 | (size_t)c.data[at + 3] << 24);
	}
	c.data[count_at] = (uint8_t)fields;
	c.data[count_at + 1] = (uint8_t)(fields >> 8);
	c.data[count_at + 2] = (uint8_t)(fields >> 16);
	c.data[count_at + 3] = (uint8_t)(fields >> 24);

	static const size_t PADDING = 1024;
	uint8_t comment_head[4] = {4, (uint8_t)(c.len >> 16), (uint8_t)(c.len >> 8), (uint8_t)c.len};
	uint8_t padding_head[4] = {0x80 | 1, (uint8_t)(PADDING >> 16), (uint8_t)(PADDING >> 8), (uint8_t)PADDING};
	uint8_t zeros[256] = {0};
	bool ok = fwrite("fLaC", 1, 4, k->out) == 4 && fwrite(k->keep, 1, k->keep_len, k->out) == k->keep_len && fwrite(comment_head, 1, 4, k->out) == 4 && fwrite(c.data, 1, c.len, k->out) == c.len && fwrite(padding_head, 1, 4, k->out) == 4;
	for (size_t done = 0; ok && done < PADDING; done += sizeof(zeros)) {
		ok = fwrite(zeros, 1, sizeof(zeros), k->out) == sizeof(zeros);
	}
	free(c.data);
	free(k->keep);
	k->keep = NULL;
	k->keep_len = k->keep_cap = 0;
	return ok;
}

static void id3_frame(buf_t *b, const char *id, const char *text) {
	if (!text || !text[0]) {
		return;
	}
	uint32_t size = (uint32_t)strlen(text) + 1; // the encoding byte
	uint8_t head[10] = {(uint8_t)id[0], (uint8_t)id[1], (uint8_t)id[2], (uint8_t)id[3], (uint8_t)((size >> 21) & 0x7f), (uint8_t)((size >> 14) & 0x7f), (uint8_t)((size >> 7) & 0x7f), (uint8_t)(size & 0x7f), 0, 0};
	buf_add(b, head, sizeof(head));
	buf_add(b, "\x03", 1); // UTF-8
	buf_add(b, text, strlen(text));
}

// ID3v2.4, UTF-8 text frames: what the player's reader and any other takes.
static bool id3_out(sink_t *k) {
	buf_t f = {0};
	const tags_t *t = k->tags;
	char num[32];
	id3_frame(&f, "TIT2", t->title);
	id3_frame(&f, "TPE1", t->artist);
	id3_frame(&f, "TALB", t->album);
	id3_frame(&f, "TPE2", t->album_artist);
	if (t->track > 0) {
		snprintf(num, sizeof(num), t->tracks > 0 ? "%d/%d" : "%d", t->track, t->tracks);
		id3_frame(&f, "TRCK", num);
	}
	if (t->disc > 0) {
		snprintf(num, sizeof(num), t->discs > 0 ? "%d/%d" : "%d", t->disc, t->discs);
		id3_frame(&f, "TPOS", num);
	}
	if (t->year > 0) {
		snprintf(num, sizeof(num), "%d", t->year);
		id3_frame(&f, "TDRC", num);
	}
	id3_frame(&f, "TCON", t->genre);
	if (f.failed) {
		free(f.data);
		return false;
	}
	uint32_t size = (uint32_t)f.len;
	uint8_t head[10] = {'I', 'D', '3', 4, 0, 0, (uint8_t)((size >> 21) & 0x7f), (uint8_t)((size >> 14) & 0x7f), (uint8_t)((size >> 7) & 0x7f), (uint8_t)(size & 0x7f)};
	bool ok = fwrite(head, 1, sizeof(head), k->out) == sizeof(head) && fwrite(f.data, 1, f.len, k->out) == f.len;
	free(f.data);
	return ok;
}

static bool keep_add(sink_t *k, const void *data, size_t len) {
	buf_t b = {k->keep, k->keep_len, k->keep_cap, false};
	buf_add(&b, data, len);
	k->keep = b.data;
	k->keep_len = b.len;
	k->keep_cap = b.cap;
	return !b.failed;
}

// ---------------------------------------------------------------------------
// MP4 (AAC in an .m4a): iTunes-style atoms in the movie box
//
// The tags of an MP4 live inside its "moov" box (moov/udta/meta/ilst), and
// the moov holds the absolute file offsets of the audio: the chunk offset
// tables (stco, co64) of a whole file, and in a fragmented one -- which is
// what Yandex serves -- the base offset of every fragment (tfhd) and the
// fragment index at the end (mfra/tfra). With the moov in front of the audio,
// as it is in Yandex's files, tags written into it push every one of those
// offsets along.
//
// So, while the file streams past (still never seeking, still one copy):
// top-level boxes go straight through, except
//   * the moov, held whole (it is small: no audio in it), written again
//     without its old udta and with ours, and every chunk offset past it
//     moved by what it grew;
//   * after that, each moof and the mfra, held (a few hundred bytes each) and
//     their absolute offsets moved the same way.
// A moov after the audio (moov at the end) needs no offset moved at all: the
// rule "move what points past the old moov" covers both. Anything that does
// not fit the bounds below -- a moov over 4 MB, a box running to the end of
// the file before the moov -- is written as it came, untagged, and the caller
// falls back to the sidecar of 1.2.4 (write_sidecar), so a file is never lost
// to its tags. The cover is not put in (covr): it would be held in memory
// beside the moov, and the album's cover.jpg next to the files is what the
// library and other players show anyway.
// ---------------------------------------------------------------------------

#define MP4_HOLD_MAX (4 * 1024 * 1024)
#define BOX(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) << 32 | rd32(p + 4); }
static void wr32(uint8_t *p, uint32_t v) {
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}
static void wr64(uint8_t *p, uint64_t v) {
	wr32(p, (uint32_t)(v >> 32));
	wr32(p + 4, (uint32_t)v);
}

static void be32_add(buf_t *b, uint32_t v) {
	uint8_t x[4];
	wr32(x, v);
	buf_add(b, x, 4);
}

// One ilst item: <type> { data { type indicator, locale, value } }.
static void ilst_item(buf_t *b, uint32_t type, uint32_t indicator, const void *value, size_t len) {
	be32_add(b, (uint32_t)(8 + 16 + len));
	be32_add(b, type);
	be32_add(b, (uint32_t)(16 + len));
	be32_add(b, BOX('d', 'a', 't', 'a'));
	be32_add(b, indicator);
	be32_add(b, 0); // locale
	buf_add(b, value, len);
}

static void ilst_text(buf_t *b, uint32_t type, const char *text) {
	if (text && text[0]) {
		ilst_item(b, type, 1, text, strlen(text)); // 1: UTF-8
	}
}

// trkn and disk: a pad, the number, the total (and trkn one more pad).
static void ilst_pair(buf_t *b, uint32_t type, int number, int total, size_t len) {
	if (number <= 0) {
		return;
	}
	uint8_t v[8] = {0, 0, (uint8_t)(number >> 8), (uint8_t)number, (uint8_t)(total > 0 ? total >> 8 : 0), (uint8_t)(total > 0 ? total : 0), 0, 0};
	ilst_item(b, type, 0, v, len);
}

// udta { meta { hdlr mdir, ilst {...} } } for the tags.
static void mp4_udta(buf_t *b, const tags_t *t) {
	buf_t ilst = {0};
	ilst_text(&ilst, BOX(0xA9, 'n', 'a', 'm'), t->title);
	ilst_text(&ilst, BOX(0xA9, 'A', 'R', 'T'), t->artist);
	ilst_text(&ilst, BOX('a', 'A', 'R', 'T'), t->album_artist);
	ilst_text(&ilst, BOX(0xA9, 'a', 'l', 'b'), t->album);
	ilst_pair(&ilst, BOX('t', 'r', 'k', 'n'), t->track, t->tracks, 8);
	ilst_pair(&ilst, BOX('d', 'i', 's', 'k'), t->disc, t->discs, 6);
	if (t->year > 0) {
		char year[16];
		snprintf(year, sizeof(year), "%d", t->year);
		ilst_text(&ilst, BOX(0xA9, 'd', 'a', 'y'), year);
	}
	ilst_text(&ilst, BOX(0xA9, 'g', 'e', 'n'), t->genre);
	ilst_text(&ilst, BOX(0xA9, 't', 'o', 'o'), "Sonix Player");
	if (ilst.failed) {
		b->failed = true;
		free(ilst.data);
		return;
	}
	// hdlr: version/flags, pre_defined, "mdir", reserved ("appl", 0, 0), an
	// empty name -- what iTunes writes and every reader expects.
	static const uint8_t HDLR[33] = {0, 0, 0, 33, 'h', 'd', 'l', 'r', 0, 0, 0, 0, 0, 0, 0, 0, 'm', 'd', 'i', 'r', 'a', 'p', 'p', 'l', 0, 0, 0, 0, 0, 0, 0, 0, 0};
	uint32_t meta_len = 8 + 4 + sizeof(HDLR) + 8 + (uint32_t)ilst.len;
	be32_add(b, 8 + meta_len);
	be32_add(b, BOX('u', 'd', 't', 'a'));
	be32_add(b, meta_len);
	be32_add(b, BOX('m', 'e', 't', 'a'));
	be32_add(b, 0); // meta is a full box
	buf_add(b, HDLR, sizeof(HDLR));
	be32_add(b, 8 + (uint32_t)ilst.len);
	be32_add(b, BOX('i', 'l', 's', 't'));
	buf_add(b, ilst.data, ilst.len);
	free(ilst.data);
}

// A box inside [at, end): its type, header length and whole size. False when
// it does not fit.
static bool box_at(const uint8_t *p, size_t at, size_t end, uint32_t *type, size_t *head, size_t *size) {
	if (at + 8 > end) {
		return false;
	}
	uint64_t len = rd32(p + at);
	*type = rd32(p + at + 4);
	*head = 8;
	if (len == 1) {
		if (at + 16 > end) {
			return false;
		}
		len = rd64(p + at + 8);
		*head = 16;
	} else if (len == 0) {
		len = end - at;
	}
	if (len < *head || len > end - at) {
		return false;
	}
	*size = (size_t)len;
	return true;
}

// Moves every chunk offset in [at, end) of a moov that points past `from`.
static bool shift_chunks(uint8_t *p, size_t at, size_t end, uint64_t from, int64_t delta) {
	uint32_t type;
	size_t head, size;
	for (; box_at(p, at, end, &type, &head, &size); at += size) {
		size_t body = at + head;
		switch (type) {
		case BOX('t', 'r', 'a', 'k'):
		case BOX('m', 'd', 'i', 'a'):
		case BOX('m', 'i', 'n', 'f'):
		case BOX('s', 't', 'b', 'l'):
			if (!shift_chunks(p, body, at + size, from, delta)) {
				return false;
			}
			break;
		case BOX('s', 't', 'c', 'o'):
		case BOX('c', 'o', '6', '4'): {
			size_t width = type == BOX('c', 'o', '6', '4') ? 8 : 4;
			if (body + 8 > at + size) {
				return false;
			}
			uint32_t count = rd32(p + body + 4);
			if ((uint64_t)count * width > at + size - body - 8) {
				return false;
			}
			for (uint32_t i = 0; i < count; i++) {
				uint8_t *e = p + body + 8 + (size_t)i * width;
				uint64_t v = width == 8 ? rd64(e) : rd32(e);
				if (v < from) {
					continue;
				}
				v += (uint64_t)delta;
				if (width == 4 && v > UINT32_MAX) {
					return false; // would need a co64: not worth it for a song
				}
				width == 8 ? wr64(e, v) : wr32(e, (uint32_t)v);
			}
			break;
		}
		default:
			break;
		}
	}
	return true;
}

// The moov held in k->keep, written out again with our udta in place of its
// own and the chunk offsets moved.
static bool mp4_moov_out(sink_t *k) {
	const uint8_t *src = k->keep;
	uint32_t type;
	size_t head, size;
	if (!box_at(src, 0, k->keep_len, &type, &head, &size)) {
		return false;
	}
	buf_t b = {0};
	be32_add(&b, 0); // the size, once known
	be32_add(&b, BOX('m', 'o', 'o', 'v'));
	size_t at = head;
	uint32_t child;
	size_t child_head, child_size;
	for (; box_at(src, at, size, &child, &child_head, &child_size); at += child_size) {
		if (child != BOX('u', 'd', 't', 'a')) {
			buf_add(&b, src + at, child_size);
		}
	}
	if (at != size) {
		free(b.data);
		return false; // a moov that does not add up: left alone
	}
	mp4_udta(&b, k->tags);
	if (b.failed || b.len > UINT32_MAX) {
		free(b.data);
		return false;
	}
	wr32(b.data, (uint32_t)b.len);
	k->delta = (int64_t)b.len - (int64_t)k->keep_len;
	if (!shift_chunks(b.data, 8, b.len, k->moov_at, k->delta)) {
		free(b.data);
		return false;
	}
	bool ok = fwrite(b.data, 1, b.len, k->out) == b.len;
	free(b.data);
	if (ok && k->tagged) {
		*k->tagged = true;
	}
	return ok;
}

// A moof or the mfra after a moov that grew: their absolute offsets follow.
static bool mp4_fragment_shift(sink_t *k) {
	uint8_t *p = k->keep;
	uint32_t type, child, inner;
	size_t head, size, child_head, child_size, inner_head, inner_size;
	if (!box_at(p, 0, k->keep_len, &type, &head, &size)) {
		return false;
	}
	for (size_t at = head; box_at(p, at, size, &child, &child_head, &child_size); at += child_size) {
		size_t body = at + child_head, end = at + child_size;
		if (type == BOX('m', 'o', 'o', 'f') && child == BOX('t', 'r', 'a', 'f')) {
			for (size_t in = body; box_at(p, in, end, &inner, &inner_head, &inner_size); in += inner_size) {
				size_t tf = in + inner_head;
				// tfhd: version and flags, the track, then -- with flag 1 --
				// the fragment's base offset in the file.
				if (inner == BOX('t', 'f', 'h', 'd') && tf + 16 <= in + inner_size && (rd32(p + tf) & 1)) {
					uint64_t base = rd64(p + tf + 8);
					if (base >= k->moov_at) {
						wr64(p + tf + 8, base + (uint64_t)k->delta);
					}
				}
			}
		} else if (type == BOX('m', 'f', 'r', 'a') && child == BOX('t', 'f', 'r', 'a') && body + 16 <= end) {
			// tfra: each entry a time and the offset of its moof, then three
			// numbers whose widths the header gives.
			bool v1 = p[body] == 1;
			uint32_t sizes = rd32(p + body + 8);
			uint32_t count = rd32(p + body + 12);
			size_t entry = (v1 ? 16 : 8) + ((sizes >> 4) & 3) + 1 + ((sizes >> 2) & 3) + 1 + (sizes & 3) + 1;
			size_t first = body + 16;
			if ((uint64_t)count * entry > end - first) {
				return false;
			}
			for (uint32_t i = 0; i < count; i++) {
				uint8_t *e = p + first + (size_t)i * entry + (v1 ? 8 : 4);
				uint64_t off = v1 ? rd64(e) : rd32(e);
				if (off < k->moov_at) {
					continue;
				}
				off += (uint64_t)k->delta;
				if (!v1 && off > UINT32_MAX) {
					return false;
				}
				v1 ? wr64(e, off) : wr32(e, (uint32_t)off);
			}
		}
	}
	return fwrite(p, 1, k->keep_len, k->out) == k->keep_len;
}

// The MP4 half of sink_write: top-level boxes, as above.
static void mp4_sink_write(sink_t *k, const uint8_t *p, size_t n) {
	while (n > 0 && !k->failed) {
		switch (k->state) {
		case S_BOX_HEAD: {
			size_t want = k->head_len >= 8 && rd32(k->head) == 1 ? 16 : 8;
			size_t take = want - k->head_len < n ? want - k->head_len : n;
			memcpy(k->head + k->head_len, p, take);
			k->head_len += take;
			p += take;
			n -= take;
			k->pos += take;
			if (k->head_len < want || (want == 8 && rd32(k->head) == 1)) {
				break; // a 64-bit size follows
			}
			uint64_t box_at_pos = k->pos - k->head_len;
			uint32_t len32 = rd32(k->head);
			uint64_t len = len32 == 1 ? rd64(k->head + 8) : len32;
			k->box_type = rd32(k->head + 4);
			size_t hl = k->head_len;
			k->head_len = 0;
			if (box_at_pos == 0 && k->box_type != BOX('f', 't', 'y', 'p') && k->box_type != BOX('m', 'o', 'o', 'v')) {
				// Not an MP4 that starts the usual way: written as it came,
				// untagged, rather than guessed at.
				k->failed = fwrite(k->head, 1, hl, k->out) != hl;
				k->box_left = UINT64_MAX;
				k->state = S_BOX_PASS;
				break;
			}
			if (len32 == 0) {
				// Runs to the end of the file: passed as it is. Before the
				// moov, that means the moov never comes, and no tags.
				k->failed = fwrite(k->head, 1, hl, k->out) != hl;
				k->box_left = UINT64_MAX;
				k->state = S_BOX_PASS;
				break;
			}
			if (len < hl) {
				k->failed = true; // not an MP4 after all
				break;
			}
			k->box_left = len - hl;
			bool moov = k->box_type == BOX('m', 'o', 'o', 'v') && !k->moov_done;
			bool shifted = k->moov_done && k->delta != 0 && (k->box_type == BOX('m', 'o', 'o', 'f') || k->box_type == BOX('m', 'f', 'r', 'a'));
			if ((moov || shifted) && len <= MP4_HOLD_MAX) {
				if (moov) {
					k->moov_at = box_at_pos;
				}
				k->keep_len = 0;
				k->failed = !keep_add(k, k->head, hl);
				k->state = S_BOX_HOLD;
			} else if (shifted) {
				k->failed = true; // a fragment too big to hold: offsets would be wrong
			} else {
				if (moov) {
					k->moov_done = true; // too big to hold: untagged
				}
				k->failed = fwrite(k->head, 1, hl, k->out) != hl;
				k->state = S_BOX_PASS;
			}
			if (!k->failed && k->box_left == 0 && k->state == S_BOX_PASS) {
				k->state = S_BOX_HEAD;
			}
			break;
		}
		case S_BOX_PASS: {
			size_t take = k->box_left < n ? (size_t)k->box_left : n;
			k->failed = fwrite(p, 1, take, k->out) != take;
			p += take;
			n -= take;
			k->pos += take;
			if (k->box_left != UINT64_MAX) {
				k->box_left -= take;
				if (k->box_left == 0) {
					k->state = S_BOX_HEAD;
				}
			}
			break;
		}
		case S_BOX_HOLD: {
			size_t take = k->box_left < n ? (size_t)k->box_left : n;
			k->failed = !keep_add(k, p, take);
			p += take;
			n -= take;
			k->pos += take;
			k->box_left -= take;
			if (k->failed || k->box_left > 0) {
				break;
			}
			if (k->box_type == BOX('m', 'o', 'o', 'v')) {
				k->moov_done = true;
				if (!mp4_moov_out(k)) {
					// Not ours to rewrite: as it came, and no offset moves.
					k->delta = 0;
					k->failed = fwrite(k->keep, 1, k->keep_len, k->out) != k->keep_len;
				}
			} else {
				k->failed = !mp4_fragment_shift(k);
			}
			k->keep_len = 0;
			k->state = S_BOX_HEAD;
			break;
		}
		default:
			k->failed = true;
			break;
		}
	}
}

static ssize_t sink_write(void *cookie, const char *data, size_t size) {
	sink_t *k = cookie;
	const uint8_t *p = (const uint8_t *)data;
	size_t n = size;
	if (k->kind == SINK_MP4) {
		mp4_sink_write(k, p, n);
		return k->failed ? -1 : (ssize_t)size;
	}
	while (n > 0 && !k->failed) {
		switch (k->state) {
		case S_PASS:
			if (fwrite(p, 1, n, k->out) != n) {
				k->failed = true;
			}
			n = 0;
			break;
		case S_MAGIC:
		case S_BLOCK_HEAD:
		case S_ID3_HEAD: {
			size_t want = k->state == S_MAGIC ? 4 : k->state == S_BLOCK_HEAD ? 4 : 10;
			size_t take = want - k->head_len < n ? want - k->head_len : n;
			memcpy(k->head + k->head_len, p, take);
			k->head_len += take;
			p += take;
			n -= take;
			if (k->head_len < want) {
				break;
			}
			k->head_len = 0;
			if (k->state == S_MAGIC) {
				// Not a FLAC after all: written as it came, untagged.
				if (memcmp(k->head, "fLaC", 4) != 0) {
					k->failed = fwrite(k->head, 1, 4, k->out) != 4;
					k->state = S_PASS;
				} else {
					k->state = S_BLOCK_HEAD;
				}
			} else if (k->state == S_BLOCK_HEAD) {
				uint8_t type = k->head[0] & 0x7f;
				k->last = (k->head[0] & 0x80) != 0;
				k->left = (uint32_t)k->head[1] << 16 | (uint32_t)k->head[2] << 8 | k->head[3];
				// The source's tags and padding go; ours replace them.
				k->keep_this = type != 1 && type != 4 && type != 127 && (type == 0 || k->left <= KEEP_BLOCK_MAX);
				if (k->keep_this) {
					uint8_t h[4] = {type, k->head[1], k->head[2], k->head[3]}; // never "last" now
					k->failed = !keep_add(k, h, 4);
				}
				k->state = S_BLOCK_BODY;
			} else {
				// ID3: the source's own tag is skipped, ours goes first.
				if (memcmp(k->head, "ID3", 3) == 0) {
					k->left = ((uint32_t)(k->head[6] & 0x7f) << 21 | (uint32_t)(k->head[7] & 0x7f) << 14 | (uint32_t)(k->head[8] & 0x7f) << 7 | (k->head[9] & 0x7f)) + ((k->head[5] & 0x10) ? 10 : 0);
					k->failed = !id3_out(k);
					k->state = S_ID3_SKIP;
				} else {
					k->failed = !id3_out(k) || fwrite(k->head, 1, 10, k->out) != 10;
					k->state = S_PASS;
				}
			}
			if (k->state == S_BLOCK_BODY && k->left == 0) {
				goto block_done;
			}
			if (k->state == S_ID3_SKIP && k->left == 0) {
				k->state = S_PASS;
			}
			break;
		}
		case S_BLOCK_BODY:
		case S_ID3_SKIP: {
			size_t take = k->left < n ? k->left : n;
			if (k->state == S_BLOCK_BODY && k->keep_this) {
				k->failed = !keep_add(k, p, take);
			}
			p += take;
			n -= take;
			k->left -= (uint32_t)take;
			if (k->left > 0) {
				break;
			}
			if (k->state == S_ID3_SKIP) {
				k->state = S_PASS;
				break;
			}
		block_done:
			if (k->last) {
				k->failed = !flac_header_out(k);
				k->state = S_PASS;
			} else {
				k->state = S_BLOCK_HEAD;
			}
			break;
		}
		}
	}
	return k->failed ? -1 : (ssize_t)size;
}

static int sink_close(void *cookie) {
	sink_t *k = cookie;
	// A source that ended inside its header is not a file; nor is an MP4 that
	// ended inside a box.
	bool ok = !k->failed && (k->state == S_PASS || (k->state == S_BOX_HEAD && k->head_len == 0) || (k->state == S_BOX_PASS && k->box_left == UINT64_MAX));
	free(k->keep);
	free(k);
	return ok ? 0 : -1;
}

// A FILE that tags what goes through it into `out`, which stays the caller's.
// NULL for an extension that is not tagged this way (the caller writes
// straight to `out` then). `tagged` (MP4 only, may be NULL) is set true once
// the tags are in: an MP4 left as it came needs the sidecar.
static FILE *sink_open(FILE *out, const char *ext, const tags_t *tags, bool *tagged) {
	int kind;
	if (strcmp(ext, "flac") == 0) {
		kind = SINK_FLAC;
	} else if (strcmp(ext, "mp3") == 0) {
		kind = SINK_MP3;
	} else if (strcmp(ext, "m4a") == 0) {
		kind = SINK_MP4;
	} else {
		return NULL;
	}
	sink_t *k = calloc(1, sizeof(*k));
	if (!k) {
		return NULL;
	}
	k->out = out;
	k->kind = kind;
	k->state = kind == SINK_FLAC ? S_MAGIC : kind == SINK_MP3 ? S_ID3_HEAD : S_BOX_HEAD;
	k->tags = tags;
	k->tagged = tagged;
	cookie_io_functions_t io = {.write = sink_write, .close = sink_close};
	FILE *f = fopencookie(k, "w", io);
	if (!f) {
		free(k);
		return NULL;
	}
	// The writes reach the sink in large pieces rather than one per fwrite.
	setvbuf(f, NULL, _IOFBF, 64 * 1024);
	return f;
}

// The m4a's tags beside it, where the player reads them: what 1.2.4 wrote for
// every .m4a, now only for one the MP4 sink could not tag (see above).
static void write_sidecar(const char *path, const tags_t *t) {
	char name[1350], tmp[1370];
	snprintf(name, sizeof(name), "%.1300s.tags", path);
	snprintf(tmp, sizeof(tmp), "%s.tmp", name);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		return;
	}
	fprintf(f, "title=%s\nartist=%s\nalbum=%s\nalbum_artist=%s\ntrack=%d\ndisc=%d\nyear=%d\ngenre=%s\n", t->title, t->artist, t->album, t->album_artist, t->track, t->disc, t->year, t->genre);
	if (fclose(f) != 0 || rename(tmp, name) != 0) {
		remove(tmp);
	}
}

// ---------------------------------------------------------------------------
// one track
// ---------------------------------------------------------------------------

// The album last asked about: an album's tracks come one after another, and
// it is one request for all of them.
static yandex_album_meta_t meta;
static bool meta_ok;

static void progress_cb(long written, long total, void *user) {
	unsigned asked = (unsigned)(uintptr_t)user;
	pthread_mutex_lock(&lock);
	status.percent = total > 0 ? (int)((long long)written * 100 / total) : -1;
	if (status.percent > 100) {
		status.percent = 100;
	}
	bool stale = asked != generation;
	pthread_mutex_unlock(&lock);
	if (stale || !card_usable()) {
		yandexcache_fetch_cancel();
	}
}

// Streams `src` into `out` (the cached copy of a track): 64 KB at a time.
static bool copy_file(const char *src, FILE *out, unsigned asked) {
	FILE *in = fopen(src, "rb");
	if (!in) {
		return false;
	}
	struct stat st;
	long total = fstat(fileno(in), &st) == 0 ? (long)st.st_size : 0;
	char *buf = malloc(64 * 1024);
	bool ok = buf != NULL;
	long done = 0;
	size_t n;
	while (ok && (n = fread(buf, 1, 64 * 1024, in)) > 0) {
		ok = fwrite(buf, 1, n, out) == n;
		done += (long)n;
		progress_cb(done, total, (void *)(uintptr_t)asked);
		pthread_mutex_lock(&lock);
		ok = ok && asked == generation;
		pthread_mutex_unlock(&lock);
	}
	ok = ok && !ferror(in);
	free(buf);
	fclose(in);
	return ok;
}

static bool exists(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 && st.st_size > 0;
}

static void fetch_cover(const char *folder, const char *url) {
	char path[1100], part[1100];
	snprintf(path, sizeof(path), "%.1024s/cover.jpg", folder);
	if (!url || !url[0] || exists(path)) {
		return;
	}
	char *body = NULL;
	size_t len = 0;
	if (http_get(url, &body, &len, COVER_LIMIT, 20) && len > 0) {
		snprintf(part, sizeof(part), "%.1024s/.cover.jpg.part", folder);
		FILE *f = fopen(part, "wb");
		bool ok = f && fwrite(body, 1, len, f) == len;
		if (f && fclose(f) != 0) {
			ok = false;
		}
		if (!ok || rename(part, path) != 0) {
			remove(part);
		}
	}
	free(body);
}

// Saves one track. True when the file is in the library afterwards (saved
// now or already there); false with `why` set otherwise.
static bool save_track(const yandex_track_t *t, unsigned asked, char *why, size_t why_size) {
	why[0] = '\0';
	if (!card_usable()) {
		snprintf(why, why_size, "%s", tr("download_no_card"));
		return false;
	}
	if (t->album_id[0] && (!meta_ok || strcmp(meta.id, t->album_id) != 0)) {
		meta_ok = yandex_album_meta(t->album_id, &meta);
		if (!meta_ok) {
			fprintf(stderr, "yandexdl: no album details for %s (%s), the track's own used\n", t->album_id, yandex_last_error());
		}
	}
	bool have_meta = meta_ok && strcmp(meta.id, t->album_id) == 0;

	tags_t tags;
	memset(&tags, 0, sizeof(tags));
	snprintf(tags.title, sizeof(tags.title), "%s", t->title);
	snprintf(tags.artist, sizeof(tags.artist), "%s", t->artist);
	snprintf(tags.album, sizeof(tags.album), "%s", have_meta ? meta.title : t->album);
	snprintf(tags.album_artist, sizeof(tags.album_artist), "%s", have_meta && meta.artist[0] ? meta.artist : t->artist);
	if (have_meta && meta.genre[0]) {
		// The service names genres by slug ("rusrock"): given a capital.
		snprintf(tags.genre, sizeof(tags.genre), "%s", meta.genre);
		tags.genre[0] = (char)toupper((unsigned char)tags.genre[0]);
	}
	tags.track = t->track_number;
	// The album's count is over all its discs: a per-disc total is not known,
	// so a record of several discs says none rather than a wrong one.
	tags.tracks = have_meta && meta.discs <= 1 ? meta.track_count : 0;
	tags.disc = t->disc_number;
	tags.discs = have_meta ? meta.discs : 0;
	tags.year = have_meta ? meta.year : 0;

	// <music>/Yandex Music/<album artist>/<album (year)>/<NN - title>
	char music[600], artist_dir[160], album_dir[200], album_name[220], base[220], title[200];
	music_root(music, sizeof(music));
	podcastdl_safe_name(tags.album_artist, "Unknown Artist", artist_dir, sizeof(artist_dir));
	if (tags.year > 0) {
		snprintf(album_name, sizeof(album_name), "%.180s (%d)", tags.album, tags.year);
	} else {
		snprintf(album_name, sizeof(album_name), "%.180s", tags.album);
	}
	podcastdl_safe_name(album_name, "Unknown Album", album_dir, sizeof(album_dir));
	podcastdl_safe_name(t->title, t->id, title, sizeof(title));
	if (t->track_number > 0 && tags.discs > 1 && t->disc_number > 0) {
		snprintf(base, sizeof(base), "%d-%02d - %.180s", t->disc_number, t->track_number, title);
	} else if (t->track_number > 0) {
		snprintf(base, sizeof(base), "%02d - %.180s", t->track_number, title);
	} else {
		snprintf(base, sizeof(base), "%s", title);
	}
	char folder[1024];
	snprintf(folder, sizeof(folder), "%s/%s/%s/%s", music, LIBRARY_FOLDER, artist_dir, album_dir);

	static const char *const EXTS[] = {"flac", "mp3", "m4a"};
	for (size_t i = 0; i < sizeof(EXTS) / sizeof(EXTS[0]); i++) {
		char existing[1300];
		snprintf(existing, sizeof(existing), "%.1024s/%.220s.%s", folder, base, EXTS[i]);
		if (exists(existing)) {
			printf("yandexdl: %s is already there\n", existing);
			return true;
		}
	}
	if (!make_dirs(folder)) {
		snprintf(why, why_size, "%s", strerror(errno));
		return false;
	}

	// The source: the cache's copy when it is good enough, else the service.
	char cached[512] = "";
	const char *ext = NULL;
	yandex_file_t file;
	memset(&file, 0, sizeof(file));
	if (yandexcache_find(t->id, cached, sizeof(cached))) {
		const char *dot = strrchr(cached, '.');
		ext = dot ? dot + 1 : "";
		if (yandex_get_quality() == YANDEX_QUALITY_LOSSLESS && strcmp(ext, "flac") != 0) {
			cached[0] = '\0'; // lossy in the cache, lossless asked for: fetch it
			ext = NULL;
		}
	}
	if (!cached[0]) {
		if (!yandex_file_info(t->id, &file)) {
			snprintf(why, why_size, "%s", yandex_last_error());
			return false;
		}
		ext = yandexcache_extension(file.codec);
	}

	char path[1300], part[1320];
	snprintf(path, sizeof(path), "%.1024s/%.220s.%.8s", folder, base, ext);
	snprintf(part, sizeof(part), "%.1024s/.%.220s.%.8s.part", folder, base, ext);
	pthread_mutex_lock(&lock);
	snprintf(status.title, sizeof(status.title), "%.95s", t->title);
	// Not half a letter at the end of a cut title (at worst a whole one goes).
	size_t cut = strlen(status.title);
	if (cut == sizeof(status.title) - 1) {
		while (cut > 0 && ((unsigned char)status.title[cut - 1] & 0xC0) == 0x80) {
			cut--;
		}
		if (cut > 0 && ((unsigned char)status.title[cut - 1] & 0x80)) {
			cut--;
		}
		status.title[cut] = '\0';
	}
	status.percent = 0;
	pthread_mutex_unlock(&lock);
	printf("yandexdl: %s -> %s (%s)\n", t->id, path, cached[0] ? "from the cache" : file.codec);

	FILE *out = fopen(part, "wb");
	if (!out) {
		snprintf(why, why_size, "%s", strerror(errno));
		return false;
	}
	bool tagged = false;
	FILE *sink = sink_open(out, ext, &tags, &tagged);
	FILE *target = sink ? sink : out;
	uint64_t samples = 0;
	bool ok;
	if (cached[0]) {
		ok = copy_file(cached, target, asked);
		if (!ok) {
			snprintf(why, why_size, "%s", tr("download_failed"));
		}
	} else {
		ok = yandexcache_fetch(t->id, &file, target, progress_cb, (void *)(uintptr_t)asked, &samples, why, why_size);
	}
	if (sink && fclose(sink) != 0) {
		ok = false;
		if (!why[0]) {
			snprintf(why, why_size, "%s", tr("yandex_unsupported_file"));
		}
	}
	if (fclose(out) != 0) {
		ok = false;
	}
	if (ok && samples) {
		mp4flac_patch_total_samples(part, samples);
	}
	if (ok && rename(part, path) != 0) {
		snprintf(why, why_size, "%s", strerror(errno));
		ok = false;
	}
	if (!ok) {
		remove(part);
		// The folders this file would have been the first in: rmdir only
		// takes them when they are empty.
		char parent[1024];
		snprintf(parent, sizeof(parent), "%s", folder);
		for (int up = 0; up < 3; up++) {
			if (rmdir(parent) != 0) {
				break;
			}
			char *slash = strrchr(parent, '/');
			if (!slash) {
				break;
			}
			*slash = '\0';
		}
		if (!why[0]) {
			snprintf(why, why_size, "%s", tr("download_failed"));
		}
		return false;
	}
	if (strcmp(ext, "m4a") == 0 && !tagged) {
		fprintf(stderr, "yandexdl: %s could not be tagged inside, the tags go beside it\n", path);
		write_sidecar(path, &tags);
	}
	fetch_cover(folder, have_meta && meta.cover[0] ? meta.cover : t->cover);
	printf("yandexdl: saved %s\n", path);
	return true;
}

// ---------------------------------------------------------------------------
// the worker
// ---------------------------------------------------------------------------

static void library_look(void *user) {
	(void)user;
	library_card_returned(storage_sd_root());
}

static void *worker_main(void *arg) {
	(void)arg;
	// Nice ten: a saved album is not urgent, but SCHED_IDLE would starve it
	// for as long as anything plays (see thread_be_low_priority).
	thread_be_low_priority("yandex save");
	for (;;) {
		pthread_mutex_lock(&lock);
		while (queue_len == 0) {
			running = false;
			pthread_cond_wait(&ready, &lock);
		}
		entry_t e = queue[0];
		memmove(queue, queue + 1, (size_t)(--queue_len) * sizeof(*queue));
		status.queued = queue_len;
		running = true;
		unsigned asked = generation;
		pthread_mutex_unlock(&lock);

		if (e.album) {
			// An album: its tracks go to the head of the queue, in order.
			yandex_track_t *tracks = calloc(150, sizeof(*tracks));
			int n = tracks ? yandex_album_tracks(e.track.album_id, tracks, 150) : -1;
			if (n < 0) {
				char text[300];
				snprintf(text, sizeof(text), "%s: %s", e.track.album, tracks ? yandex_last_error() : tr("out_of_memory"));
				pthread_mutex_lock(&lock);
				status.failed++;
				pthread_mutex_unlock(&lock);
				tell(YANDEXDL_FAILED, 1, text);
			}
			pthread_mutex_lock(&lock);
			int room = asked == generation && n > 0 ? QUEUE_MAX - queue_len : 0;
			int take = n < room ? n : room;
			entry_t *grown = take > 0 ? realloc(queue, (size_t)(queue_len + take) * sizeof(*queue)) : queue;
			if (grown) {
				queue = grown;
				memmove(queue + take, queue, (size_t)queue_len * sizeof(*queue));
				int k = 0;
				for (int i = 0; i < n && k < take; i++) {
					if (tracks[i].available) {
						queue[k].album = false;
						queue[k++].track = tracks[i];
					}
				}
				if (k < take) { // the unavailable ones left a gap
					memmove(queue + k, queue + take, (size_t)queue_len * sizeof(*queue));
				}
				queue_len += k;
				status.queued = queue_len;
				take = k;
			}
			pthread_mutex_unlock(&lock);
			free(tracks);
			printf("yandexdl: album %s, %d tracks to save\n", e.track.album_id, take);
		} else {
			char why[256];
			bool ok = save_track(&e.track, asked, why, sizeof(why));
			pthread_mutex_lock(&lock);
			bool stale = asked != generation;
			if (!stale) {
				ok ? status.done++ : status.failed++;
			}
			pthread_mutex_unlock(&lock);
			if (!ok && !stale) {
				fprintf(stderr, "yandexdl: %s not saved: %s\n", e.track.id, why);
				char text[420];
				snprintf(text, sizeof(text), "%.150s: %s", e.track.title, why);
				tell(YANDEXDL_FAILED, 1, text);
			}
		}

		pthread_mutex_lock(&lock);
		bool finished = queue_len == 0 && asked == generation;
		int saved = status.done;
		if (queue_len == 0) {
			free(queue);
			queue = NULL;
			status.active = false;
			status.title[0] = '\0';
			status.percent = -1;
		}
		pthread_mutex_unlock(&lock);
		if (finished) {
			char music[600];
			music_root(music, sizeof(music));
			char where[700];
			snprintf(where, sizeof(where), "%s/%s", strrchr(music, '/') ? strrchr(music, '/') + 1 : music, LIBRARY_FOLDER);
			printf("yandexdl: queue done, %d saved\n", saved);
			if (saved > 0) {
				// "Detect changes" finds the new folders: only those are read.
				gui_post(library_look, NULL);
			}
			tell(YANDEXDL_FINISHED, saved, where);
		}
	}
	return NULL;
}

static bool enqueue(const entry_t *e) {
	pthread_mutex_lock(&lock);
	if (!card_usable()) {
		pthread_mutex_unlock(&lock);
		return false;
	}
	if (queue_len >= QUEUE_MAX) {
		pthread_mutex_unlock(&lock);
		return false;
	}
	entry_t *grown = realloc(queue, (size_t)(queue_len + 1) * sizeof(*queue));
	if (!grown) {
		pthread_mutex_unlock(&lock);
		return false;
	}
	queue = grown;
	queue[queue_len++] = *e;
	if (!status.active) {
		status.active = true;
		status.done = status.failed = 0;
		status.percent = -1;
	}
	status.queued = queue_len;
	if (!worker_started) {
		pthread_t th;
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		pthread_attr_setstacksize(&attr, 256 * 1024); // TLS, as the other workers
		worker_started = pthread_create(&th, &attr, worker_main, NULL) == 0;
		pthread_attr_destroy(&attr);
	}
	pthread_cond_signal(&ready);
	pthread_mutex_unlock(&lock);
	return worker_started;
}

bool yandexdl_track(const yandex_track_t *track) {
	if (!track || !track->id[0]) {
		return false;
	}
	entry_t e = {.album = false, .track = *track};
	bool ok = enqueue(&e);
	if (ok) {
		tell(YANDEXDL_QUEUED, 1, track->title);
	}
	return ok;
}

bool yandexdl_album(const char *album_id, const char *title) {
	if (!album_id || !album_id[0]) {
		return false;
	}
	entry_t e;
	memset(&e, 0, sizeof(e));
	e.album = true;
	snprintf(e.track.album_id, sizeof(e.track.album_id), "%s", album_id);
	snprintf(e.track.album, sizeof(e.track.album), "%s", title ? title : "");
	bool ok = enqueue(&e);
	if (ok) {
		tell(YANDEXDL_QUEUED, 0, title);
	}
	return ok;
}

void yandexdl_status(yandexdl_status_t *out) {
	pthread_mutex_lock(&lock);
	*out = status;
	pthread_mutex_unlock(&lock);
}

bool yandexdl_busy(void) {
	pthread_mutex_lock(&lock);
	bool busy = status.active;
	pthread_mutex_unlock(&lock);
	return busy;
}

void yandexdl_cancel_all(void) {
	pthread_mutex_lock(&lock);
	bool had = status.active;
	generation++;
	queue_len = 0;
	status.queued = 0;
	pthread_mutex_unlock(&lock);
	yandexcache_fetch_cancel();
	if (had) {
		printf("yandexdl: queue dropped\n");
		tell(YANDEXDL_STOPPED, 0, tr("download_cancelled"));
	}
}

void yandexdl_set_root(const char *sd_root) {
	const char *next = sd_root ? sd_root : "";
	pthread_mutex_lock(&lock);
	bool changed = strcmp(root, next) != 0;
	pthread_mutex_unlock(&lock);
	if (changed) {
		yandexdl_cancel_all();
		pthread_mutex_lock(&lock);
		snprintf(root, sizeof(root), "%s", next);
		pthread_mutex_unlock(&lock);
	}
}

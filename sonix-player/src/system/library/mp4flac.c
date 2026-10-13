#include "mp4flac.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// MP4 boxes
//
// An MP4 is made of nested boxes, all of one shape:
//
//     [4 bytes: total length] [4 bytes: name] [payload...]
//
// The length is big-endian and includes the eight header bytes. Two special
// values: 0 means "to the end of the buffer", 1 means the real length is the
// next eight bytes (64-bit).
//
// Everything reaching this file comes from the network -- segments served by
// Tidal's CDN -- and a segment truncated by a Wi-Fi hiccup is just as likely to
// carry a nonsensical length as a deliberately malformed one. Hence three rules
// this file never breaks:
//
//   1. Lengths are compared as 64-bit values, never through pointer
//      arithmetic. `p + len > end` looks equivalent but is not: pointers are
//      32-bit on MIPS, so a declared length of 2^32 becomes `p + 0` and the
//      check passes. Then `len - 8` underflows a size_t and four billion bytes
//      get written to the card. A PC build's 64-bit pointers hide this, so it
//      shows up only on the device.
//
//   2. Every step advances. A zero-length box read as "zero bytes" loops
//      forever; the minimum length here is the header, so the pointer always
//      moves.
//
//   3. The descent has a floor. A box containing itself costs eight bytes per
//      level: an eight-megabyte segment holds a million levels of recursion,
//      which is the download thread's stack.
// ---------------------------------------------------------------------------

// How deep the descent goes. The real path -- moov/trak/mdia/minf/stbl/stsd/
// fLaC/dfLa -- uses eight; sixteen is double that, and a file needing more is
// not a file worth opening. Same idea as JSON_MAX_DEPTH.
#define MAX_DEPTH 16

static uint32_t be32(const uint8_t *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t be64(const uint8_t *p) { return ((uint64_t)be32(p) << 32) | (uint64_t)be32(p + 4); }

// A box found inside a buffer.
typedef struct {
	const uint8_t *payload;
	size_t payload_size;
} box_t;

// Walk over a sequence of boxes. Every length in this file is read here and
// nowhere else, so rule 1 above has exactly one place to hold.
typedef struct {
	const uint8_t *p;
	const uint8_t *end;
} walk_t;

static void walk_init(walk_t *w, const uint8_t *data, size_t size) {
	w->p = data;
	w->end = data + size;
}

// The next box. False when they run out or when what is there is not a box:
// either way the walk stops, because past an impossible length there is no
// telling where the cursor is.
static bool walk_next(walk_t *w, const uint8_t **name, const uint8_t **body, size_t *body_size) {
	if (w->p + 8 > w->end) {
		return false;
	}

	// Remaining space as a number, not a pointer: below this point no pointer
	// arithmetic is done with lengths that came off the network.
	uint64_t avail = (uint64_t)(w->end - w->p);

	uint64_t len = be32(w->p);
	uint64_t header = 8;

	if (len == 0) {
		len = avail; // "to the end"
	} else if (len == 1) {
		if (avail < 16) {
			return false;
		}
		len = be64(w->p + 8);
		header = 16;
	}

	if (len < header || len > avail) {
		return false;
	}

	*name = w->p + 4;
	*body = w->p + header;
	*body_size = (size_t)(len - header);
	w->p += (size_t)len; // len <= avail, so it fits and the step is >= 8
	return true;
}

// Which boxes have children. The descent needs to know: a data box read as a
// container would yield garbage lengths. The list holds exactly what is needed
// to get from `moov` to `dfLa`, and nothing else.
static bool is_container(const uint8_t *name) {
	static const char *const CONTAINERS[] = {"moov", "trak", "mdia", "minf", "stbl", "moof", "traf", "edts", "mvex"};
	for (size_t i = 0; i < sizeof(CONTAINERS) / sizeof(CONTAINERS[0]); i++) {
		if (memcmp(name, CONTAINERS[i], 4) == 0) {
			return true;
		}
	}
	return false;
}

static bool find_box(const uint8_t *data, size_t size, const char *name, box_t *out, int depth);

// `stsd` is not a plain container: four bytes of version/flags, then an entry
// count, and only then the entries, which are proper boxes. Same for the audio
// sample entry (`fLaC`), which carries twenty-eight bytes of fixed fields --
// channels, bits, sample rate -- before its child boxes. Skipping those is the
// only part of this file that depends on the format rather than the structure.
#define STSD_HEADER 8		  // 4 version+flags, 4 entry count
#define AUDIO_ENTRY_HEADER 28 // the fixed fields of an AudioSampleEntry

static bool find_in_stsd(const uint8_t *data, size_t size, const char *name, box_t *out, int depth) {
	if (size <= STSD_HEADER) {
		return false;
	}

	walk_t w;
	walk_init(&w, data + STSD_HEADER, size - STSD_HEADER);

	const uint8_t *entry_name;
	const uint8_t *body;
	size_t body_size;
	while (walk_next(&w, &entry_name, &body, &body_size)) {
		(void)entry_name;
		if (body_size > AUDIO_ENTRY_HEADER &&
			find_box(body + AUDIO_ENTRY_HEADER, body_size - AUDIO_ENTRY_HEADER, name, out, depth + 1)) {
			return true;
		}
	}
	return false;
}

// Looks for box `name` among the children of [data, data+size), descending into
// boxes that hold others.
static bool find_box(const uint8_t *data, size_t size, const char *name, box_t *out, int depth) {
	if (depth > MAX_DEPTH) {
		return false;
	}

	walk_t w;
	walk_init(&w, data, size);

	const uint8_t *box_name;
	const uint8_t *body;
	size_t body_size;
	while (walk_next(&w, &box_name, &body, &body_size)) {
		if (memcmp(box_name, name, 4) == 0) {
			out->payload = body;
			out->payload_size = body_size;
			return true;
		}
		if (memcmp(box_name, "stsd", 4) == 0) {
			if (find_in_stsd(body, body_size, name, out, depth + 1)) {
				return true;
			}
		} else if (is_container(box_name) && find_box(body, body_size, name, out, depth + 1)) {
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// The length
//
// A whole MP4 says how long its track is in `stts` (runs of samples of one
// duration). A fragmented one has an empty `stts` and says it fragment by
// fragment instead, in each `trun`: a count, and per sample a duration or,
// failing that, the default of the `tfhd` or, failing that, of the `trex`.
// The sum, in the `mdhd` timescale, is the length the STREAMINFO of a
// fragmented FLAC leaves at zero.
// ---------------------------------------------------------------------------

static void read_timing(mp4flac_t *m, const uint8_t *init, size_t init_size) {
	box_t box;
	if (find_box(init, init_size, "mdhd", &box, 0) && box.payload_size >= 24) {
		// version 0: flags 4, created 4, modified 4, timescale 4, duration 4;
		// version 1: the dates and the duration are 64-bit.
		m->timescale = be32(box.payload + (box.payload[0] == 1 ? 20 : 12));
	}
	if (find_box(init, init_size, "trex", &box, 0) && box.payload_size >= 24) {
		m->default_duration = be32(box.payload + 12); // flags, track, description, duration
	}
	if (find_box(init, init_size, "stts", &box, 0) && box.payload_size >= 8) {
		uint32_t entries = be32(box.payload + 4);
		for (uint64_t i = 0; i < entries && 8 + (i + 1) * 8 <= box.payload_size; i++) {
			const uint8_t *e = box.payload + 8 + i * 8;
			m->duration += (uint64_t)be32(e) * be32(e + 4);
		}
	}
}

static void count_traf(mp4flac_t *m, const uint8_t *data, size_t size) {
	uint32_t fallback = m->default_duration;
	walk_t w;
	walk_init(&w, data, size);
	const uint8_t *name;
	const uint8_t *body;
	size_t len;
	while (walk_next(&w, &name, &body, &len)) {
		if (memcmp(name, "tfhd", 4) == 0 && len >= 8) {
			uint32_t flags = be32(body) & 0xffffff;
			size_t at = 8 + ((flags & 0x01) ? 8 : 0) + ((flags & 0x02) ? 4 : 0);
			if ((flags & 0x08) && at + 4 <= len) {
				fallback = be32(body + at);
			}
		} else if (memcmp(name, "trun", 4) == 0 && len >= 8) {
			uint32_t flags = be32(body) & 0xffffff;
			uint64_t count = be32(body + 4);
			if (!(flags & 0x100)) {
				m->duration += count * fallback;
				continue;
			}
			size_t at = 8 + ((flags & 0x01) ? 4 : 0) + ((flags & 0x04) ? 4 : 0);
			size_t per = 4 + ((flags & 0x200) ? 4 : 0) + ((flags & 0x400) ? 4 : 0) + ((flags & 0x800) ? 4 : 0);
			for (uint64_t i = 0; i < count && at + 4 <= len; i++, at += per) {
				m->duration += be32(body + at);
			}
		}
	}
}

static void count_segment(mp4flac_t *m, const uint8_t *data, size_t size) {
	walk_t w;
	walk_init(&w, data, size);
	const uint8_t *name;
	const uint8_t *body;
	size_t len;
	while (walk_next(&w, &name, &body, &len)) {
		if (memcmp(name, "moof", 4) != 0) {
			continue;
		}
		walk_t inner;
		walk_init(&inner, body, len);
		const uint8_t *child;
		const uint8_t *cbody;
		size_t clen;
		while (walk_next(&inner, &child, &cbody, &clen)) {
			if (memcmp(child, "traf", 4) == 0) {
				count_traf(m, cbody, clen);
			}
		}
	}
}

uint64_t mp4flac_total_samples(const mp4flac_t *m) {
	if (!m || !m->duration) {
		return 0;
	}
	if (!m->timescale || !m->sample_rate || m->timescale == m->sample_rate) {
		return m->duration;
	}
	return m->duration * m->sample_rate / m->timescale;
}

bool mp4flac_patch_total_samples(const char *path, uint64_t samples) {
	if (!path || !samples || samples >= (1ULL << 36)) {
		return false;
	}
	FILE *f = fopen(path, "r+b");
	if (!f) {
		return false;
	}
	// "fLaC", the STREAMINFO block header, then its 34 bytes: the 36-bit total
	// sits in the low nibble of byte 13 and all of bytes 14-17.
	uint8_t head[8 + 18];
	bool ok = fread(head, 1, sizeof(head), f) == sizeof(head) && memcmp(head, "fLaC", 4) == 0 && (head[4] & 0x7f) == 0;
	uint8_t *si = head + 8;
	ok = ok && (si[13] & 0x0f) == 0 && !si[14] && !si[15] && !si[16] && !si[17];
	if (ok) {
		uint8_t patch[5] = {(uint8_t)((si[13] & 0xf0) | ((samples >> 32) & 0x0f)), (uint8_t)(samples >> 24), (uint8_t)(samples >> 16), (uint8_t)(samples >> 8), (uint8_t)samples};
		ok = fseek(f, 8 + 13, SEEK_SET) == 0 && fwrite(patch, 1, sizeof(patch), f) == sizeof(patch);
	}
	return fclose(f) == 0 && ok;
}

// ---------------------------------------------------------------------------

const char *mp4flac_extension_for(const char *codecs) {
	// Plain "flac", but also "fLaC" and the dotted forms: the manifest field is
	// not normalized, so its casing cannot be trusted.
	if (!codecs || !codecs[0]) {
		return "m4a";
	}
	if (strncmp(codecs, "flac", 4) == 0 || strncmp(codecs, "fLaC", 4) == 0 || strncmp(codecs, "FLAC", 4) == 0) {
		return "flac";
	}
	return "m4a";
}

static bool write_all(mp4flac_t *m, const void *data, size_t size) {
	if (size == 0) {
		return true;
	}
	if (fwrite(data, 1, size, m->out) != size) {
		fprintf(stderr, "mp4flac: write failed (card full?)\n");
		return false;
	}
	m->written += (long)size;
	return true;
}

bool mp4flac_begin(mp4flac_t *m, FILE *out, const char *codecs, const uint8_t *init, size_t init_size) {
	if (!m || !out || !init) {
		return false;
	}
	memset(m, 0, sizeof(*m));
	m->out = out;
	m->unwrap = strcmp(mp4flac_extension_for(codecs), "flac") == 0;

	if (!m->unwrap) {
		// Nothing to unwrap: the init segment is the start of the file.
		if (!write_all(m, init, init_size)) {
			return false;
		}
		m->header_bytes = m->written;
		return true;
	}

	box_t dfla;
	// The smallest useful `dfLa`: four bytes of version and flags, four of block
	// header, thirty-four of STREAMINFO. A looser bound lets through a file with
	// the "fLaC" signature and no STREAMINFO, which the decoder opens anyway and
	// plays as noise.
	if (!find_box(init, init_size, "dfLa", &dfla, 0) || dfla.payload_size < 4 + 4 + 34) {
		fprintf(stderr, "mp4flac: the header segment does not carry the STREAMINFO\n");
		return false;
	}

	// `dfLa` is a full box: four bytes of version and flags, then the FLAC
	// metadata blocks already carrying their own four-byte headers, which is
	// exactly what has to be written after the signature.
	const uint8_t *blocks = dfla.payload + 4;
	size_t blocks_size = dfla.payload_size - 4;
	// The STREAMINFO's 20-bit sample rate, bytes 10 to 12 of its body.
	m->sample_rate = ((uint32_t)blocks[4 + 10] << 12) | ((uint32_t)blocks[4 + 11] << 4) | (blocks[4 + 12] >> 4);
	read_timing(m, init, init_size);

	if (!write_all(m, "fLaC", 4)) {
		return false;
	}

	// The last-metadata-block bit. Blocks normally arrive with the right flag,
	// but if one did not the decoder would keep reading metadata into the first
	// audio frame. Checking costs one byte, and only the first block is checked:
	// STREAMINFO, the only one dfLa always contains.
	uint8_t first = blocks[0];
	size_t first_len = 4 + (((size_t)blocks[1] << 16) | ((size_t)blocks[2] << 8) | (size_t)blocks[3]);
	if (first_len >= blocks_size) {
		first |= 0x80;
	}
	if (!write_all(m, &first, 1) || !write_all(m, blocks + 1, blocks_size - 1)) {
		return false;
	}

	// The header size, so callers can tell whether any real audio followed it.
	// Without it, a track whose segments carry no `mdat` at all would end up as
	// a header-only file, marked complete and cached forever.
	m->header_bytes = m->written;
	return true;
}

bool mp4flac_segment(mp4flac_t *m, const uint8_t *data, size_t size) {
	if (!m || !m->out || !data) {
		return false;
	}

	if (!m->unwrap) {
		return write_all(m, data, size);
	}

	count_segment(m, data, size);

	// A segment can carry more than one `mdat` (several fragments in one
	// segment). They hold FLAC frames back to back and must be written in the
	// order they appear.
	walk_t w;
	walk_init(&w, data, size);

	const uint8_t *name;
	const uint8_t *body;
	size_t body_size;
	while (walk_next(&w, &name, &body, &body_size)) {
		if (memcmp(name, "mdat", 4) == 0 && !write_all(m, body, body_size)) {
			return false;
		}
	}

	// A segment without `mdat` is not fatal: it may be metadata only. It becomes
	// an error only if nothing ever arrives, which the caller detects by
	// comparing `written` with `header_bytes`.
	return true;
}

bool mp4flac_payload(mp4flac_t *m, const uint8_t *data, size_t size) { return m && m->out && data && write_all(m, data, size); }

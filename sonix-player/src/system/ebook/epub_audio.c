#include "src/system/ebook/epub_internal.h"
#include "src/system/ebook/epub_xml.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define AUDIO_MAX_CLIPS 8192u
#define AUDIO_MAX_SMIL (4u * 1024u * 1024u)

uint32_t ebook_audio_count(const ebook_t *book) { return book ? book->audio_count : 0; }
const ebook_audio_clip_t *ebook_audio_clip(const ebook_t *book, uint32_t index) { return book && index < book->audio_count ? &book->audio[index] : NULL; }
bool ebook_audio_unavailable(const ebook_t *book) { return book && book->audio_unavailable; }

const char *epub_audio_extension(const char *path, const char *mime, size_t mime_len) {
	static const struct {
		const char *mime, *extension;
	} formats[] = {{"audio/mpeg", "mp3"}, {"audio/mp3", "mp3"}, {"audio/mp4", "m4a"}, {"audio/aac", "aac"}, {"audio/aacp", "aac"}, {"audio/ogg", "ogg"}, {"audio/opus", "opus"}, {"audio/flac", "flac"}, {"audio/x-flac", "flac"}, {"audio/wav", "wav"}, {"audio/x-wav", "wav"}, {"audio/wave", "wav"}};
	for (size_t i = 0; mime && i < sizeof(formats) / sizeof(formats[0]); i++) {
		if (strlen(formats[i].mime) == mime_len && strncasecmp(mime, formats[i].mime, mime_len) == 0)
			return formats[i].extension;
	}
	const char *dot = path ? strrchr(path, '.') : NULL;
	static const char *const extensions[] = {"mp3", "m4a", "m4b", "mp4", "aac", "ogg", "opus", "flac", "wav", "wv"};
	for (size_t i = 0; dot && i < sizeof(extensions) / sizeof(extensions[0]); i++) {
		if (strcasecmp(dot + 1, extensions[i]) == 0)
			return extensions[i];
	}
	return NULL;
}

static int hex_digit(char c) {
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

bool epub_audio_path(char *out, size_t size, const char *base, const char *href, size_t len) {
	char xml[2048], decoded[2048];
	if (!href || !len || len >= sizeof(xml))
		return false;
	size_t n = epub_xml_unescape(xml, sizeof(xml), href, len);
	// Resource URIs are local to the archive. Do not fetch URLs or use their
	// names as filesystem paths. Strip URI syntax before percent decoding.
	if (!n || xml[0] == '/' || strchr(xml, ':') || strchr(xml, '\\'))
		return false;
	size_t used = 0;
	for (size_t i = 0; i < n && xml[i] != '#' && xml[i] != '?'; i++) {
		unsigned char c = (unsigned char)xml[i];
		if (c == '%') {
			if (i + 2 >= n || hex_digit(xml[i + 1]) < 0 || hex_digit(xml[i + 2]) < 0)
				return false;
			c = (unsigned char)((hex_digit(xml[i + 1]) << 4) | hex_digit(xml[i + 2]));
			i += 2;
		}
		if (!c || c == '\\' || c == ':' || c == '#' || c == '?' || used + 1 >= sizeof(decoded))
			return false;
		decoded[used++] = (char)c;
	}
	decoded[used] = '\0';
	if (!used || decoded[0] == '/')
		return false;
	return epub_resolve_path(out, size, base, decoded, used);
}

// SMIL clock values: full/partial clock and timecounts in h, min, s or ms.
static bool clock_seconds(const char *value, size_t len, double *out) {
	char text[128];
	if (!len || len >= sizeof(text))
		return false;
	memcpy(text, value, len);
	text[len] = '\0';
	char *p = text;
	if (strncmp(p, "npt=", 4) == 0)
		p += 4;
	unsigned colons = 0;
	for (const char *c = p; *c; c++)
		if (*c == ':')
			colons++;
	if (colons > 2 || !isdigit((unsigned char)*p))
		return false;
	double total = 0;
	for (unsigned part = 0; part <= colons; part++) {
		char *end;
		double number = strtod(p, &end);
		if (end == p || !isfinite(number) || number < 0 || (part && number >= 60))
			return false;
		if (part < colons) {
			if (*end != ':' || floor(number) != number)
				return false;
			total = (total + number) * 60;
			p = end + 1;
			if (!isdigit((unsigned char)*p))
				return false;
		} else {
			total += number;
			if (!colons) {
				if (strcmp(end, "ms") == 0)
					total /= 1000;
				else if (strcmp(end, "min") == 0)
					total *= 60;
				else if (strcmp(end, "h") == 0)
					total *= 3600;
				else if (*end && strcmp(end, "s") != 0)
					return false;
			} else if (*end)
				return false;
		}
	}
	if (!isfinite(total) || total > 365.0 * 86400.0)
		return false;
	*out = total;
	return true;
}

static bool add_clip(ebook_t *book, const epub_xml_t *x, const char *base, bool overlay) {
	const char *src, *mime = NULL, *value;
	size_t len, mime_len = 0;
	if (!epub_xml_attr(x, "src", &src, &len))
		return false;
	char path[1024];
	if (!epub_audio_path(path, sizeof(path), base, src, len)) {
		book->audio_unavailable = true;
		return false;
	}
	epub_xml_attr(x, "type", &mime, &mime_len);
	const char *extension = NULL;
	for (uint32_t i = 0; i < book->audio_resource_count; i++) {
		if (strcmp(path, book->audio_resources[i].path) == 0) {
			extension = book->audio_resources[i].extension;
			break;
		}
	}
	if (!extension)
		extension = epub_audio_extension(path, mime, mime_len);
	int index = epub_zip_find(&book->zip, path);
	if (!extension || index < 0 || !epub_zip_size(&book->zip, index)) {
		book->audio_unavailable = true;
		return false;
	}
	double begin = 0, end = -1;
	if (overlay) {
		if ((epub_xml_attr(x, "clipBegin", &value, &len) && !clock_seconds(value, len, &begin)) || (epub_xml_attr(x, "clipEnd", &value, &len) && !clock_seconds(value, len, &end)) || (end >= 0 && end <= begin)) {
			book->audio_unavailable = true;
			return false;
		}
	}
	// Continuous passages are the seek-slider unit. Authored sentence cues
	// are kept separately, so advancing a highlight never flushes the decoder.
	if (overlay && book->audio_count) {
		ebook_audio_clip_t *last = &book->audio[book->audio_count - 1];
		if (last->end >= 0 && fabs(last->end - begin) < 0.02 && strcmp(last->path, path) == 0) {
			last->end = end;
			return true;
		}
	}
	if (book->audio_count >= AUDIO_MAX_CLIPS) {
		book->audio_unavailable = true;
		return false;
	}
	// Grow in the chapter arena, just like text spans. Most chapters only
	// need a handful; the limit never becomes an up-front allocation.
	if (!(book->audio_count & (book->audio_count - 1)) && book->audio_count >= 16) {
		uint32_t capacity = book->audio_count * 2;
		ebook_audio_clip_t *grown = arena_calloc(&book->chapter_arena, capacity, sizeof(*grown));
		if (!grown)
			return false;
		memcpy(grown, book->audio, book->audio_count * sizeof(*grown));
		book->audio = grown;
	} else if (!book->audio) {
		book->audio = arena_calloc(&book->chapter_arena, 16, sizeof(*book->audio));
		if (!book->audio)
			return false;
	}
	const char *stored = NULL;
	// Every sentence normally names the same resource. Retain its path once.
	for (uint32_t i = 0; i < book->audio_count; i++)
		if (strcmp(book->audio[i].path, path) == 0) {
			stored = book->audio[i].path;
			break;
		}
	if (!stored)
		stored = arena_strdup(&book->chapter_arena, path);
	if (!stored)
		return false;
	book->audio[book->audio_count++] = (ebook_audio_clip_t){stored, extension, begin, end};
	return true;
}

uint32_t ebook_audio_cue_count(const ebook_t *book) { return book ? book->audio_cue_count : 0; }
const ebook_audio_cue_t *ebook_audio_cue(const ebook_t *book, uint32_t index) { return book && index < book->audio_cue_count ? &book->audio_cues[index] : NULL; }

uint32_t ebook_audio_cue_at(const ebook_t *book, uint32_t clip, double seconds) {
	if (!book || !book->audio_cue_count)
		return UINT32_MAX;
	// Passage index and start time are ordered, even when the source's clock
	// jumps backwards at a discontinuity. No chapter scan at each UI tick.
	uint32_t lo = 0, hi = book->audio_cue_count;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		const ebook_audio_cue_t *c = &book->audio_cues[mid];
		if (c->clip < clip || (c->clip == clip && c->begin <= seconds))
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo && book->audio_cues[lo - 1].clip == clip)
		return lo - 1;
	return lo < book->audio_cue_count && book->audio_cues[lo].clip == clip ? lo : UINT32_MAX;
}

uint32_t ebook_audio_cue_for_text(const ebook_t *book, uint32_t offset) {
	uint32_t after = UINT32_MAX, before = UINT32_MAX;
	for (uint32_t i = 0; book && i < book->audio_cue_count; i++) {
		const ebook_audio_cue_t *c = &book->audio_cues[i];
		if (c->text_begin == UINT32_MAX || c->text_end <= c->text_begin)
			continue;
		if (c->text_begin <= offset && offset < c->text_end)
			return i;
		if (c->text_begin >= offset && (after == UINT32_MAX || c->text_begin < book->audio_cues[after].text_begin))
			after = i;
		if (c->text_end <= offset && (before == UINT32_MAX || c->text_end > book->audio_cues[before].text_end))
			before = i;
	}
	return after != UINT32_MAX ? after : before;
}

uint32_t ebook_text_offset(const ebook_t *book, uint32_t block, uint32_t offset) {
	const ebook_block_t *b = ebook_block(book, block);
	if (!b)
		return book ? book->text_len : 0;
	if (!b->span_count) {
		for (uint32_t i = block + 1; i < ebook_block_count(book); i++)
			if (ebook_block(book, i)->span_count)
				return ebook_text_offset(book, i, 0);
		return book->text_len;
	}
	return ebook_span(book, b->first_span)->offset + offset;
}

bool ebook_text_position(const ebook_t *book, uint32_t text, uint32_t *block, uint32_t *offset) {
	uint32_t lo = 0, hi = ebook_block_count(book);
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		if (ebook_text_offset(book, mid, 0) <= text)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (!lo)
		return false;
	*block = lo - 1;
	*offset = text - ebook_text_offset(book, *block, 0);
	return true;
}

uint64_t epub_audio_id(const char *text, size_t length) {
	char decoded[512];
	if (!length || length >= sizeof(decoded))
		return 0;
	size_t n = epub_xml_unescape(decoded, sizeof(decoded), text, length);
	uint64_t id = 1469598103934665603ull;
	for (size_t i = 0; i < n; i++) {
		id ^= (unsigned char)decoded[i];
		id *= 1099511628211ull;
	}
	return id == 0 || id == UINT64_MAX ? 1 : id;
}

static int target_compare(const void *a, const void *b) {
	const epub_audio_target_t *x = a, *y = b;
	if (x->id != y->id)
		return x->id < y->id ? -1 : 1;
	return x->cue < y->cue ? -1 : x->cue != y->cue;
}

uint32_t epub_audio_target(const ebook_t *book, uint64_t id) {
	if (!id || !book->audio_targets)
		return UINT32_MAX;
	uint32_t lo = 0, hi = book->audio_cue_count;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		if (book->audio_targets[mid].id < id)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < book->audio_cue_count && book->audio_targets[lo].id == id ? lo : UINT32_MAX;
}

void epub_audio_resolve(ebook_t *book, uint32_t target, uint32_t begin, uint32_t end) {
	if (target >= book->audio_cue_count || !book->audio_targets)
		return;
	uint64_t id = book->audio_targets[target].id;
	for (; target < book->audio_cue_count && book->audio_targets[target].id == id; target++) {
		ebook_audio_cue_t *c = &book->audio_cues[book->audio_targets[target].cue];
		c->text_begin = begin;
		c->text_end = end;
	}
}

// SMIL text URLs must name this XHTML document. Fragments identify authored
// sentences; an absent fragment addresses the whole document. URI escapes are
// decoded before hashing, without retaining ID strings or building a DOM.
static bool text_target(ebook_t *book, uint32_t spine, const epub_xml_t *x, const char *base, uint64_t *id) {
	const char *src;
	size_t length;
	if (!epub_xml_attr(x, "src", &src, &length))
		return false;
	char path[1024], uri[2048];
	if (!epub_audio_path(path, sizeof(path), base, src, length) || strcmp(path, book->spine[spine].path) != 0)
		return false;
	epub_xml_unescape(uri, sizeof(uri), src, length);
	char *fragment = strchr(uri, '#');
	if (!fragment || !fragment[1]) {
		*id = UINT64_MAX;
		return true;
	}
	char decoded[512];
	size_t used = 0;
	for (char *p = fragment + 1; *p; p++) {
		unsigned char c = (unsigned char)*p;
		if (c == '%') {
			if (!p[1] || !p[2] || hex_digit(p[1]) < 0 || hex_digit(p[2]) < 0)
				return false;
			c = (unsigned char)((hex_digit(p[1]) << 4) | hex_digit(p[2]));
			p += 2;
		}
		if (!c || used + 1 >= sizeof(decoded))
			return false;
		decoded[used++] = (char)c;
	}
	// Already XML-decoded above; escape '&' would only matter in malformed IDs.
	uint64_t hash = 1469598103934665603ull;
	for (size_t i = 0; i < used; i++) {
		hash ^= (unsigned char)decoded[i];
		hash *= 1099511628211ull;
	}
	*id = hash == 0 || hash == UINT64_MAX ? 1 : hash;
	return true;
}

static void add_pair(ebook_t *book, const epub_xml_t *audio, const char *base, uint64_t target) {
	if (book->audio_cue_count >= AUDIO_MAX_CLIPS) {
		book->audio_unavailable = true;
		return;
	}
	if (!add_clip(book, audio, base, true))
		return;
	double begin = 0, end = -1;
	const char *value;
	size_t length;
	if (epub_xml_attr(audio, "clipBegin", &value, &length))
		clock_seconds(value, length, &begin);
	if (epub_xml_attr(audio, "clipEnd", &value, &length))
		clock_seconds(value, length, &end);
	uint32_t at = book->audio_cue_count++;
	book->audio_cues[at] = (ebook_audio_cue_t){begin, end, book->audio_count - 1, UINT32_MAX, UINT32_MAX};
	book->audio_targets[at] = (epub_audio_target_t){target, at};
}

void epub_read_audio(ebook_t *book, uint32_t spine, const char *xhtml) {
	const char *overlay = book->spine[spine].overlay;
	char base[1024];
	epub_arena_t scratch;
	arena_init(&scratch, "media overlay", 64u * 1024u);
	if (overlay) {
		int index = epub_zip_find(&book->zip, overlay);
		xhtml = epub_zip_extract(&book->zip, index, &scratch, AUDIO_MAX_SMIL, NULL);
		if (!xhtml) {
			book->audio_unavailable = true;
			goto done;
		}
		const char *slash = strrchr(overlay, '/');
		size_t len = slash ? (size_t)(slash - overlay + 1) : 0;
		if (len >= sizeof(base)) {
			book->audio_unavailable = true;
			goto done;
		}
		memcpy(base, overlay, len);
		base[len] = '\0';
		// Exact upper bound from a cheap scanner pass: no doubling waste for
		// sentence metadata, and never allocate the ceiling for a short chapter.
		epub_xml_t count;
		epub_xml_init(&count, xhtml);
		uint32_t capacity = 0;
		while (epub_xml_next(&count))
			if (count.event == EPUB_XML_OPEN && epub_xml_is(&count, "audio") && capacity < AUDIO_MAX_CLIPS)
				capacity++;
		if (!capacity)
			goto done;
		book->audio_cues = arena_calloc(&book->chapter_arena, capacity, sizeof(*book->audio_cues));
		book->audio_targets = calloc(capacity, sizeof(*book->audio_targets));
		if (!book->audio_cues || !book->audio_targets) {
			book->audio_unavailable = true;
			goto done;
		}
	} else
		snprintf(base, sizeof(base), "%s", book->chapter_dir);
	epub_xml_t x, pending_audio;
	epub_xml_init(&x, xhtml);
	bool in_audio = false, chosen = false, in_pair = false, have_audio = false, pair_local = true;
	uint64_t target = 0;
	unsigned skip_depth = 0;
	while (epub_xml_next(&x)) {
		if (overlay) {
			if (x.event == EPUB_XML_OPEN && epub_xml_is(&x, "par")) {
				in_pair = true;
				have_audio = false;
				target = 0;
				pair_local = true;
			} else if (x.event == EPUB_XML_OPEN && epub_xml_is(&x, "text") && in_pair) {
				pair_local = text_target(book, spine, &x, base, &target);
			} else if (x.event == EPUB_XML_OPEN && epub_xml_is(&x, "audio")) {
				if (in_pair) {
					pending_audio = x;
					have_audio = true;
				} else
					add_pair(book, &x, base, 0);
			} else if (x.event == EPUB_XML_CLOSE && epub_xml_is(&x, "par")) {
				if (in_pair && have_audio && pair_local)
					add_pair(book, &pending_audio, base, target);
				in_pair = have_audio = false;
			}
			continue;
		}
		if (x.event == EPUB_XML_OPEN && (epub_xml_is(&x, "head") || epub_xml_is(&x, "script") || epub_xml_is(&x, "template") || epub_xml_is(&x, "video"))) {
			if (!x.self_closing)
				skip_depth++;
			continue;
		}
		if (skip_depth) {
			if (x.event == EPUB_XML_OPEN && !x.self_closing)
				skip_depth++;
			else if (x.event == EPUB_XML_CLOSE)
				skip_depth--;
			continue;
		}
		if (x.event == EPUB_XML_OPEN && epub_xml_is(&x, "audio")) {
			chosen = add_clip(book, &x, base, false);
			in_audio = !x.self_closing;
		} else if (in_audio && !chosen && x.event == EPUB_XML_OPEN && epub_xml_is(&x, "source"))
			chosen = add_clip(book, &x, base, false);
		else if (x.event == EPUB_XML_CLOSE && epub_xml_is(&x, "audio"))
			in_audio = false;
	}
	if (book->audio_targets)
		qsort(book->audio_targets, book->audio_cue_count, sizeof(*book->audio_targets), target_compare);
done:
	arena_destroy(&scratch);
}

bool ebook_extract_audio(const char *book_path, const char *zip_path, const char *destination, bool (*cancelled)(void *), void *user) {
	if (!book_path || !zip_path || !destination)
		return false;
	epub_arena_t scratch;
	arena_init(&scratch, "audio directory", 64u * 1024u);
	epub_zip_t zip;
	bool ok = epub_zip_open(&zip, book_path, &scratch);
	if (ok)
		ok = epub_zip_extract_file(&zip, epub_zip_find(&zip, zip_path), destination, 1024u * 1024u * 1024u, cancelled, user);
	epub_zip_close(&zip);
	arena_destroy(&scratch);
	return ok;
}

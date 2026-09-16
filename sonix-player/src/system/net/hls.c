#include "hls.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "src/system/net/http.h"
#include "src/system/core/lang.h"

// Manifest size cap. A live playlist lists five or six segments, so a few
// hundred bytes; 256 KB is a ceiling no honest manifest comes near and one
// that stops a server answering with a film.
#define MANIFEST_MAX (256 * 1024)

// One segment. Ten seconds at 320 kbps is 400 KB: four megabytes leaves room
// for stations that cut long, and stops anything else.
#define SEGMENT_MAX (4 * 1024 * 1024)

#define FETCH_TIMEOUT_SECS 12

// Bitrate ceiling when picking a variant. This is a pocket player on Wi-Fi:
// 320 kbps is already more than a radio station sends, and the variants above
// it exist for televisions.
#define VARIANT_MAX_KBPS 320

// How many segments to hold ahead before playing. One means the first network
// hiccup is audible; more is roughly ten seconds of slack, which is what any
// HLS player keeps.
#define PREFETCH_SEGMENTS 2

// How many empty rounds before declaring a live stream that has stopped
// producing to be over. A round is half a segment duration, so with six-second
// segments this is about forty seconds.
#define HLS_IDLE_GIVE_UP 15

// How many segment URLs to remember, so the ones a manifest repeats on every
// reload are not fetched again.
#define SEEN_MAX 32
#define URL_MAX 1024

struct hls {
	char media_url[URL_MAX]; // the manifest listing the segments
	hls_codec_t codec;

	// The segments already fetched. A live manifest is reread every few seconds
	// and repeats the previous entries: without this the same segment would be
	// played over and over.
	char seen[SEEN_MAX][URL_MAX];
	int seen_count;
	int seen_next;

	// Elementary stream already stripped of its container and not yet read.
	unsigned char *pending;
	int pending_len;
	int pending_read;
	int pending_cap;

	// TS demux state, which has to survive from one segment to the next.
	int ts_pid;
	int ts_stream_type;

	double target_duration; // segment length, as declared by the manifest
	bool ended;				// the manifest carried #EXT-X-ENDLIST
	int idle_rounds;		// consecutive rounds with no new segments
	volatile bool aborted;
};

// ---------------------------------------------------------------------------
// URLs
// ---------------------------------------------------------------------------

// Relative URL to absolute, against `base`. Covers the three forms a manifest
// actually uses: absolute, site-root relative, and alongside the manifest.
static void resolve_url(const char *base, const char *ref, char *out, size_t out_size) {
	if (!ref || !*ref) {
		out[0] = '\0';
		return;
	}
	if (strncasecmp(ref, "http://", 7) == 0 || strncasecmp(ref, "https://", 8) == 0) {
		snprintf(out, out_size, "%s", ref);
		return;
	}

	// Where "scheme://host[:port]" ends.
	const char *after_scheme = strstr(base, "://");
	const char *root = after_scheme ? strchr(after_scheme + 3, '/') : NULL;

	if (ref[0] == '/') {
		if (root) {
			snprintf(out, out_size, "%.*s%s", (int)(root - base), base, ref);
		} else {
			snprintf(out, out_size, "%s%s", base, ref);
		}
		return;
	}

	// Alongside the manifest: cut off the file name, and with it anything after
	// ? or #, which is not part of the path.
	char dir[URL_MAX];
	snprintf(dir, sizeof(dir), "%s", base);
	char *cut = strpbrk(dir, "?#");
	if (cut) {
		*cut = '\0';
	}
	char *slash = strrchr(dir, '/');
	if (slash && (!root || slash >= dir + (root - base))) {
		*(slash + 1) = '\0';
	} else {
		snprintf(dir, sizeof(dir), "%s/", base);
	}
	snprintf(out, out_size, "%s%s", dir, ref);
}

// ---------------------------------------------------------------------------
// the manifest
// ---------------------------------------------------------------------------

bool hls_url_looks_like(const char *url) {
	if (!url) {
		return false;
	}
	const char *q = strpbrk(url, "?#");
	size_t len = q ? (size_t)(q - url) : strlen(url);
	return len >= 5 && strncasecmp(url + len - 5, ".m3u8", 5) == 0;
}

bool hls_text_looks_like(const char *text, size_t len) {
	if (!text || len < 7) {
		return false;
	}
	// A leading byte order mark happens, and a manifest that starts with one is
	// still a manifest.
	if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
		(unsigned char)text[2] == 0xBF) {
		text += 3;
		len -= 3;
	}
	while (len > 0 && (*text == '\r' || *text == '\n' || *text == ' ')) {
		text++;
		len--;
	}
	if (len < 7 || strncmp(text, "#EXTM3U", 7) != 0) {
		return false;
	}
	// #EXTM3U on its own is also an ordinary .m3u. What marks HLS is an HLS tag.
	//
	// Searched by hand rather than with memmem: that is a GNU extension, and
	// this file is also built by the cross toolchain, where a header with fewer
	// definitions is enough to get an implicit declaration returning int --
	// that is, a pointer truncated to 32 bits.
	for (size_t i = 0; i + 7 <= len; i++) {
		if (memcmp(text + i, "#EXT-X-", 7) == 0) {
			return true;
		}
	}
	return false;
}

// One attribute of an #EXT-X-STREAM-INF line, as an integer.
static long attr_int(const char *line, const char *name) {
	const char *p = strstr(line, name);
	if (!p) {
		return -1;
	}
	p += strlen(name);
	if (*p != '=') {
		return -1;
	}
	p++;
	return strtol(p, NULL, 10);
}

bool hls_pick_variant(const char *text, const char *base, int max_kbps, char *out, size_t out_size) {
	if (!text || !out || out_size == 0) {
		return false;
	}
	out[0] = '\0';

	long best_rate = -1;
	long fallback_rate = -1;
	char fallback[URL_MAX] = "";
	bool any = false;

	const char *p = text;
	while (*p) {
		const char *eol = strchr(p, '\n');
		size_t line_len = eol ? (size_t)(eol - p) : strlen(p);
		char line[1024];
		size_t copy = line_len < sizeof(line) - 1 ? line_len : sizeof(line) - 1;
		memcpy(line, p, copy);
		line[copy] = '\0';
		while (copy > 0 && (line[copy - 1] == '\r' || line[copy - 1] == ' ')) {
			line[--copy] = '\0';
		}

		if (strncmp(line, "#EXT-X-STREAM-INF", 17) == 0) {
			long rate = attr_int(line, "BANDWIDTH");
			long avg = attr_int(line, "AVERAGE-BANDWIDTH");
			if (avg > 0) {
				rate = avg; // more honest than the peak, when it is there
			}
			long kbps = rate > 0 ? rate / 1000 : 0;

			// The URL is on the next line, skipping comments.
			const char *next = eol ? eol + 1 : NULL;
			while (next && *next) {
				const char *neol = strchr(next, '\n');
				size_t nlen = neol ? (size_t)(neol - next) : strlen(next);
				char uri[URL_MAX];
				size_t ncopy = nlen < sizeof(uri) - 1 ? nlen : sizeof(uri) - 1;
				memcpy(uri, next, ncopy);
				uri[ncopy] = '\0';
				while (ncopy > 0 && (uri[ncopy - 1] == '\r' || uri[ncopy - 1] == ' ')) {
					uri[--ncopy] = '\0';
				}
				if (ncopy == 0) {
					next = neol ? neol + 1 : NULL;
					continue;
				}
				if (uri[0] == '#') {
					next = neol ? neol + 1 : NULL;
					continue;
				}

				any = true;
				char abs[URL_MAX];
				resolve_url(base, uri, abs, sizeof(abs));

				// The best one under the ceiling...
				if (kbps <= max_kbps && (long)kbps > best_rate) {
					best_rate = kbps;
					snprintf(out, out_size, "%s", abs);
				}
				// ...and the lowest of all, for when every variant is above it.
				if (fallback_rate < 0 || (long)kbps < fallback_rate) {
					fallback_rate = kbps;
					snprintf(fallback, sizeof(fallback), "%s", abs);
				}
				break;
			}
		}

		if (!eol) {
			break;
		}
		p = eol + 1;
	}

	if (!any) {
		return false;
	}
	if (!out[0]) {
		snprintf(out, out_size, "%s", fallback);
	}
	return out[0] != '\0';
}

// ---------------------------------------------------------------------------
// MPEG-TS
//
// 188-byte packets, each with a four-byte header. Only a fraction of the format
// is needed here: find which PID carries the audio (by reading PAT and PMT),
// then reassemble that PID's PES packets, discarding their headers.
// ---------------------------------------------------------------------------

#define TS_PACKET 188
#define TS_SYNC 0x47

// Which stream types yield something this player can decode.
static bool ts_type_supported(int stream_type) {
	return stream_type == 0x03 || stream_type == 0x04 || // MP3 (MPEG-1/2 audio)
		   stream_type == 0x0F || stream_type == 0x11;	// AAC in ADTS / LATM
}

// Program association table: gives the PID the program map sits on.
static int parse_pat(const unsigned char *payload, int len) {
	if (len < 12) {
		return -1;
	}
	int pointer = payload[0];
	const unsigned char *sec = payload + 1 + pointer;
	int avail = len - 1 - pointer;
	if (avail < 12 || sec[0] != 0x00) {
		return -1;
	}
	int section_len = ((sec[1] & 0x0F) << 8) | sec[2];
	if (section_len + 3 > avail) {
		section_len = avail - 3;
	}
	// 8 bytes of section header, 4 bytes of CRC at the end.
	for (int i = 8; i + 4 <= section_len + 3 - 4; i += 4) {
		int program = (sec[i] << 8) | sec[i + 1];
		int pid = ((sec[i + 2] & 0x1F) << 8) | sec[i + 3];
		if (program != 0) {
			return pid; // the first real program
		}
	}
	return -1;
}

// Program map table: gives the PID the audio sits on and its stream type.
static bool parse_pmt(const unsigned char *payload, int len, int *pid_out, int *type_out) {
	if (len < 16) {
		return false;
	}
	int pointer = payload[0];
	const unsigned char *sec = payload + 1 + pointer;
	int avail = len - 1 - pointer;
	if (avail < 16 || sec[0] != 0x02) {
		return false;
	}
	int section_len = ((sec[1] & 0x0F) << 8) | sec[2];
	if (section_len + 3 > avail) {
		section_len = avail - 3;
	}
	int info_len = ((sec[10] & 0x0F) << 8) | sec[11];
	int i = 12 + info_len;
	int end = section_len + 3 - 4;

	while (i + 5 <= end) {
		int stream_type = sec[i];
		int pid = ((sec[i + 1] & 0x1F) << 8) | sec[i + 2];
		int es_info = ((sec[i + 3] & 0x0F) << 8) | sec[i + 4];
		if (ts_type_supported(stream_type)) {
			*pid_out = pid;
			*type_out = stream_type;
			return true;
		}
		i += 5 + es_info;
	}
	return false;
}

int hls_ts_extract(const unsigned char *seg, int seg_len, unsigned char *out, int out_size, int *pid,
				   int *stream_type) {
	if (!seg || seg_len < TS_PACKET || !out || out_size <= 0 || !pid || !stream_type) {
		return -1;
	}

	// Alignment: an honest segment starts with 0x47, but a stray leading byte
	// happens. Look for the first point where the sync byte repeats three times
	// 188 bytes apart -- a single one turns up by chance.
	int start = -1;
	for (int i = 0; i < TS_PACKET && i + 2 * TS_PACKET < seg_len; i++) {
		if (seg[i] == TS_SYNC && seg[i + TS_PACKET] == TS_SYNC && seg[i + 2 * TS_PACKET] == TS_SYNC) {
			start = i;
			break;
		}
	}
	if (start < 0) {
		return -1; // not a TS
	}

	int pmt_pid = -1;
	int written = 0;

	for (int off = start; off + TS_PACKET <= seg_len; off += TS_PACKET) {
		const unsigned char *pkt = seg + off;
		if (pkt[0] != TS_SYNC) {
			continue; // one bent packet is no reason to give up on the segment
		}

		bool payload_start = (pkt[1] & 0x40) != 0;
		int this_pid = ((pkt[1] & 0x1F) << 8) | pkt[2];
		int adaptation = (pkt[3] >> 4) & 0x03;
		if ((adaptation & 0x01) == 0) {
			continue; // adaptation field only: no payload to take
		}

		int body = 4;
		if (adaptation & 0x02) {
			body += 1 + pkt[4];
			if (body >= TS_PACKET) {
				continue;
			}
		}
		const unsigned char *data = pkt + body;
		int data_len = TS_PACKET - body;

		if (this_pid == 0) {
			int found = parse_pat(data, data_len);
			if (found > 0) {
				pmt_pid = found;
			}
			continue;
		}
		if (pmt_pid > 0 && this_pid == pmt_pid) {
			int p = -1, t = 0;
			if (parse_pmt(data, data_len, &p, &t)) {
				*pid = p;
				*stream_type = t;
			}
			continue;
		}
		if (*pid < 0 || this_pid != *pid) {
			continue;
		}

		// The audio stream. Every PES packet starts with 00 00 01 <id> and
		// carries a header length to skip over; what remains is the elementary
		// stream, which is exactly what is wanted.
		if (payload_start) {
			if (data_len < 9 || data[0] != 0x00 || data[1] != 0x00 || data[2] != 0x01) {
				continue;
			}
			int header_len = data[8];
			int skip = 9 + header_len;
			if (skip >= data_len) {
				continue;
			}
			data += skip;
			data_len -= skip;
		}

		if (written + data_len > out_size) {
			data_len = out_size - written;
		}
		if (data_len <= 0) {
			break;
		}
		memcpy(out + written, data, (size_t)data_len);
		written += data_len;
	}

	return written;
}

// ---------------------------------------------------------------------------
// recognising what is inside a segment
// ---------------------------------------------------------------------------

// Skips a leading ID3v2 tag if there is one: plenty of stations put one in
// front of every segment to carry the track title.
static int skip_id3(const unsigned char *data, int len) {
	int off = 0;
	while (off + 10 <= len && data[off] == 'I' && data[off + 1] == 'D' && data[off + 2] == '3') {
		// The size is syncsafe: seven bits per byte.
		int size = ((data[off + 6] & 0x7F) << 21) | ((data[off + 7] & 0x7F) << 14) | ((data[off + 8] & 0x7F) << 7) |
				   (data[off + 9] & 0x7F);
		int step = 10 + size;
		if (step <= 0 || off + step > len) {
			break;
		}
		off += step;
	}
	return off;
}

static hls_codec_t sniff_codec(const unsigned char *data, int len) {
	int off = skip_id3(data, len);
	if (off + 2 > len) {
		return HLS_CODEC_UNKNOWN;
	}
	// ADTS: twelve sync bits, then the layer bits at zero. MP3 has a non-zero
	// layer, which is what tells the two apart.
	if (data[off] == 0xFF && (data[off + 1] & 0xF6) == 0xF0) {
		return HLS_CODEC_AAC;
	}
	if (data[off] == 0xFF && (data[off + 1] & 0xE0) == 0xE0) {
		return HLS_CODEC_MP3;
	}
	return HLS_CODEC_UNKNOWN;
}

// ---------------------------------------------------------------------------
// the segment loop
// ---------------------------------------------------------------------------

static bool already_seen(hls_t *h, const char *url) {
	for (int i = 0; i < h->seen_count; i++) {
		if (strcmp(h->seen[i], url) == 0) {
			return true;
		}
	}
	return false;
}

static void remember(hls_t *h, const char *url) {
	snprintf(h->seen[h->seen_next], URL_MAX, "%s", url);
	h->seen_next = (h->seen_next + 1) % SEEN_MAX;
	if (h->seen_count < SEEN_MAX) {
		h->seen_count++;
	}
}

// Takes the not-yet-seen segments from the manifest, strips their container and
// appends them to `pending`. Returns how many were taken.
static int fetch_new_segments(hls_t *h, int max_segments) {
	char *text = NULL;
	size_t text_len = 0;
	// The FINAL URL, not the requested one: some CDNs (msvdn.net) answer the
	// manifest with a 302 to a session-bound edge, and the segments' relative
	// names resolve against that edge, not against the original host.
	if (!http_get(h->media_url, &text, &text_len, MANIFEST_MAX, FETCH_TIMEOUT_SECS)) {
		return -1;
	}
	snprintf(h->media_url, sizeof(h->media_url), "%s", http_last_final_url());

	// A broadcast that ends says so. Without this check the manifest would be
	// reread forever even though it will never change again. The declared
	// segment duration below sets how long to wait between rereads.
	if (strstr(text, "#EXT-X-ENDLIST")) {
		h->ended = true;
	}

	const char *td = strstr(text, "#EXT-X-TARGETDURATION:");
	if (td) {
		double d = strtod(td + 21, NULL);
		if (d > 0.5 && d < 60.0) {
			h->target_duration = d;
		}
	}

	int taken = 0;
	const char *p = text;
	while (*p && taken < max_segments && !h->aborted) {
		const char *eol = strchr(p, '\n');
		size_t line_len = eol ? (size_t)(eol - p) : strlen(p);
		char line[URL_MAX];
		size_t copy = line_len < sizeof(line) - 1 ? line_len : sizeof(line) - 1;
		memcpy(line, p, copy);
		line[copy] = '\0';
		while (copy > 0 && (line[copy - 1] == '\r' || line[copy - 1] == ' ')) {
			line[--copy] = '\0';
		}
		p = eol ? eol + 1 : p + line_len;

		if (copy == 0 || line[0] == '#') {
			continue;
		}

		char abs[URL_MAX];
		resolve_url(h->media_url, line, abs, sizeof(abs));
		if (!abs[0] || already_seen(h, abs)) {
			continue;
		}
		remember(h, abs);

		char *body = NULL;
		size_t body_len = 0;
		if (!http_get(abs, &body, &body_len, SEGMENT_MAX, FETCH_TIMEOUT_SECS)) {
			fprintf(stderr, "hls: segment did not arrive: %s\n", http_last_error());
			continue;
		}
		if (body_len == 0) {
			free(body);
			continue;
		}

		// Strip the container. TS is recognised by its sync byte; anything else
		// is tried as bare audio.
		unsigned char *raw = (unsigned char *)body;
		int raw_len = (int)body_len;
		unsigned char *elementary = raw;
		int elementary_len = raw_len;
		unsigned char *scratch = NULL;

		if (raw_len >= 8 && (memcmp(raw + 4, "ftyp", 4) == 0 || memcmp(raw + 4, "styp", 4) == 0 ||
							 memcmp(raw + 4, "moof", 4) == 0)) {
			fprintf(stderr, "hls: this segment is fMP4, which we cannot unwrap\n");
			free(body);
			return taken > 0 ? taken : -2;
		}

		if (raw[0] == TS_SYNC || (raw_len > TS_PACKET && raw[TS_PACKET] == TS_SYNC)) {
			scratch = malloc((size_t)raw_len);
			if (!scratch) {
				free(body);
				continue;
			}
			int got = hls_ts_extract(raw, raw_len, scratch, raw_len, &h->ts_pid, &h->ts_stream_type);
			if (got <= 0) {
				free(scratch);
				free(body);
				continue;
			}
			elementary = scratch;
			elementary_len = got;
		} else {
			int skip = skip_id3(raw, raw_len);
			elementary = raw + skip;
			elementary_len = raw_len - skip;
		}

		if (h->codec == HLS_CODEC_UNKNOWN && elementary_len > 4) {
			h->codec = sniff_codec(elementary, elementary_len);
		}

		if (elementary_len > 0) {
			int keep = h->pending_len - h->pending_read;
			int want = keep + elementary_len;
			if (want > h->pending_cap) {
				int cap = h->pending_cap ? h->pending_cap : 65536;
				while (cap < want) {
					cap *= 2;
				}
				unsigned char *grown = realloc(h->pending, (size_t)cap);
				if (!grown) {
					free(scratch);
					free(body);
					continue;
				}
				h->pending = grown;
				h->pending_cap = cap;
			}
			// Compact what is still unread to the front, then append the new
			// data, so the buffer does not grow without bound.
			if (keep > 0 && h->pending_read > 0) {
				memmove(h->pending, h->pending + h->pending_read, (size_t)keep);
			}
			memcpy(h->pending + keep, elementary, (size_t)elementary_len);
			h->pending_len = keep + elementary_len;
			h->pending_read = 0;
			taken++;
		}

		free(scratch);
		free(body);
	}

	free(text);
	return taken;
}

// ---------------------------------------------------------------------------
// opening and reading
// ---------------------------------------------------------------------------

hls_t *hls_open(const char *url, char *why, size_t why_size) {
	if (why && why_size) {
		why[0] = '\0';
	}
	if (!url || !*url) {
		return NULL;
	}

	char *text = NULL;
	size_t text_len = 0;
	// The final URL matters here too: Radio Deejay's master manifest comes from
	// an edge other than the requested host, and the variant named inside it
	// has to be resolved against that edge.
	if (!http_get(url, &text, &text_len, MANIFEST_MAX, FETCH_TIMEOUT_SECS)) {
		if (why && why_size) {
			snprintf(why, why_size, "%s", http_last_error());
		}
		return NULL;
	}
	// Copy the request's final URL AT ONCE: the next http_get on this thread
	// overwrites it.
	char final[URL_MAX];
	snprintf(final, sizeof(final), "%s", http_last_final_url());

	if (!hls_text_looks_like(text, text_len)) {
		free(text);
		return NULL; // not a manifest: the ordinary path handles it
	}

	hls_t *h = calloc(1, sizeof(*h));
	if (!h) {
		free(text);
		return NULL;
	}
	h->ts_pid = -1;
	h->target_duration = 6.0;
	snprintf(h->media_url, sizeof(h->media_url), "%s", final);

	// Master manifest: pick a variant and reread that one. Two hops at most --
	// nothing nests deeper than that, and going further would just be a way to
	// loop forever on a misconfigured server.
	for (int hop = 0; hop < 2; hop++) {
		char variant[URL_MAX];
		if (!hls_pick_variant(text, h->media_url, VARIANT_MAX_KBPS, variant, sizeof(variant))) {
			break;
		}
		fprintf(stderr, "hls: picking variant %s\n", variant);
		snprintf(h->media_url, sizeof(h->media_url), "%s", variant);

		free(text);
		text = NULL;
		text_len = 0;
		if (!http_get(h->media_url, &text, &text_len, MANIFEST_MAX, FETCH_TIMEOUT_SECS)) {
			if (why && why_size) {
				snprintf(why, why_size, "%s", http_last_error());
			}
			free(h);
			return NULL;
		}
		snprintf(h->media_url, sizeof(h->media_url), "%s", http_last_final_url());
	}
	free(text);

	// Fetch the first segments now, so the first note does not wait for two
	// network round trips. If not even one arrives there is nothing to play.
	int got = fetch_new_segments(h, PREFETCH_SEGMENTS);
	if (got == -2) {
		if (why && why_size) {
			snprintf(why, why_size, "%s",
					 tr("hls_mp4_unsupported"));
		}
		hls_close(h);
		return NULL;
	}
	if (got <= 0) {
		if (why && why_size && !why[0]) {
			snprintf(why, why_size, "%s", tr("hls_nothing_playable"));
		}
		hls_close(h);
		return NULL;
	}
	if (h->codec == HLS_CODEC_UNKNOWN) {
		if (why && why_size) {
			snprintf(why, why_size, "%s", tr("hls_unknown_format"));
		}
		hls_close(h);
		return NULL;
	}

	return h;
}

hls_codec_t hls_codec(const hls_t *h) { return h ? h->codec : HLS_CODEC_UNKNOWN; }

void hls_abort(hls_t *h) {
	if (h) {
		h->aborted = true;
	}
}

int hls_read(hls_t *h, void *out, int len) {
	if (!h || !out || len <= 0) {
		return -1;
	}

	// Serve from `pending` while it still holds stripped data.
	while (h->pending_read >= h->pending_len) {
		if (h->aborted) {
			return 0;
		}
		int got = fetch_new_segments(h, PREFETCH_SEGMENTS);
		if (got < 0) {
			return got == -2 ? -1 : 0;
		}
		if (got > 0) {
			h->idle_rounds = 0;
			continue;
		}
		if (h->ended) {
			return 0; // the manifest said this was the end, and it is
		}
		// A live stream that stops producing never says so. After enough empty
		// rounds, give up and let the caller decide -- it knows how to
		// reconnect.
		if (++h->idle_rounds > HLS_IDLE_GIVE_UP) {
			fprintf(stderr, "hls: no new segments for a while, giving up\n");
			return 0;
		}
		{
			// No new segments in the manifest yet, which is normal for a live
			// stream. Wait half a segment duration, as the specification
			// advises, split into short sleeps so an abort is picked up fast.
			int wait_ms = (int)(h->target_duration * 500.0);
			if (wait_ms < 200) {
				wait_ms = 200;
			}
			if (wait_ms > 5000) {
				wait_ms = 5000;
			}
			for (int slept = 0; slept < wait_ms && !h->aborted; slept += 100) {
				usleep(100 * 1000);
			}
		}
	}

	int avail = h->pending_len - h->pending_read;
	int give = avail < len ? avail : len;
	memcpy(out, h->pending + h->pending_read, (size_t)give);
	h->pending_read += give;
	return give;
}

void hls_close(hls_t *h) {
	if (!h) {
		return;
	}
	free(h->pending);
	free(h);
}

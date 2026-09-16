#ifndef HLS_H
#define HLS_H

#include <stdbool.h>
#include <stddef.h>

// HLS: a radio that sends a list of segments instead of a stream.
//
// An ordinary radio opens a socket and never closes it: bytes arrive and get
// decoded. HLS works differently. The server publishes a text manifest (.m3u8)
// listing the URLs of the last few segments produced, each a few seconds long;
// the client downloads them one by one and rereads the manifest periodically to
// learn what has appeared since. It is how live streaming is done nearly
// everywhere, and more stations keep moving to it because it passes through CDN
// caches like any other file.
//
// From outside this module behaves like a socket: open, read, close. Inside,
// each read draws from the current segment and downloads the next one when it
// runs out, reloading the manifest when needed.
//
// What hls_read() returns
//
// Not the segment bytes: those are almost always wrapped. It returns the bare
// audio stream -- MP3 frames or AAC frames in ADTS -- already unwrapped, so
// the caller does exactly what it did with an ordinary radio. The three
// wrappings that actually occur:
//
//   * MPEG-TS, 188-byte packets. The historical and still most common
//     wrapping: a table inside says which stream is the audio, and the audio
//     is itself in PES packets. Unwrapped here.
//   * bare audio: the segment already is a piece of MP3 or ADTS, at most with
//     an ID3 tag in front. Nothing to do.
//   * fMP4. Not handled: it holds AAC frames without headers, and decoding
//     them needs the config carried in the initialisation segment. Detected
//     and reported rather than played as if it worked.
//
// What is deliberately missing
//
// No encryption (#EXT-X-KEY): a public radio does not use it, and those who do
// do not want to be listened to. No adaptive bitrate: the best variant this
// device's network can carry is chosen once and kept. No byte ranges within a
// segment.

typedef struct hls hls_t;

typedef enum {
	HLS_CODEC_UNKNOWN = 0,
	HLS_CODEC_MP3,
	HLS_CODEC_AAC, // ADTS
} hls_codec_t;

// Recognises an HLS URL by its name. Not reliable on its own -- plenty of
// stations serve a manifest from a URL with no extension -- but a true answer
// is always right.
bool hls_url_looks_like(const char *url);

// Recognises a manifest by its content: the first line of an .m3u8 is always
// #EXTM3U, and an HLS manifest carries at least one #EXT-X- tag. This is the
// check that counts, because it looks at what the server actually sent.
bool hls_text_looks_like(const char *text, size_t len);

// Opens. Downloads the manifest (following a master manifest down to the
// segment list if there is one) and prepares the first segment. NULL when the
// URL is not HLS, is unreachable, or is a format that cannot be unwrapped;
// `why` (when not NULL) is left holding a message to display.
hls_t *hls_open(const char *url, char *why, size_t why_size);

// What is inside. Only known after the first segment, so right after
// hls_open() this can still be HLS_CODEC_UNKNOWN; the first hls_read()
// settles it.
hls_codec_t hls_codec(const hls_t *h);

// Reads up to `len` bytes of bare audio stream. Blocks while a segment
// downloads. Returns 0 when the live stream has ended or been abandoned, -1 on
// an unrecoverable error.
int hls_read(hls_t *h, void *out, int len);

// From another thread: makes the read in progress give up. The equivalent of
// http_stream_wake() for an ordinary radio -- without it, changing station
// waits for the current segment download to finish.
void hls_abort(hls_t *h);

void hls_close(hls_t *h);

// ---------------------------------------------------------------------------
// The two self-contained parsing steps, exposed so they can be tested
// ---------------------------------------------------------------------------

// Picks a stream from a master manifest: returns the URL (absolute, resolved
// against `base`) of the highest-bitrate variant not above `max_kbps`, or the
// lowest one when they all exceed it. False when the text is not a master
// manifest.
bool hls_pick_variant(const char *text, const char *base, int max_kbps, char *out, size_t out_size);

// Unwraps an MPEG-TS segment: writes the audio elementary stream into `out`
// and returns how many bytes came out, or -1 when the input is not a TS. `pid`
// and `stream_type` carry demux state across segments (initialise to -1/0
// before the first call) -- the tables are usually but not always at the head
// of every segment.
int hls_ts_extract(const unsigned char *seg, int seg_len, unsigned char *out, int out_size, int *pid,
				   int *stream_type);

#endif /* HLS_H */

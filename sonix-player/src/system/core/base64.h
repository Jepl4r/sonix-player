#ifndef BASE64_H
#define BASE64_H

#include <stddef.h>
#include <stdint.h>

// Base64, because Tidal wraps the important things in it.
//
// The response that says where a track is does not carry the URL: it carries a
// "manifest" field in base64, holding either a JSON with the URL or a DASH MPD.
// Without decoding it nothing plays.
//
// albumart.c keeps a private copy for cover art embedded in Vorbis comments:
// its size cap is tied to images and it sits on a working path, so folding the
// two together would buy thirty lines at the cost of touching cover loading.

// Decodes `text_len` characters into a freshly allocated buffer the caller
// frees. NULL when nothing comes out, when the result would exceed
// `max_bytes`, or when memory runs out.
//
// Lenient as the use case requires: spaces, newlines and padding are skipped
// rather than failing the whole decode. The URL-safe alphabet ('-' and '_' for
// '+' and '/') is accepted too, since that is what OAuth tokens use and
// recognising it costs nothing.
//
// The buffer carries one extra zero byte past `*out_size`: a Tidal manifest is
// text (JSON or XML) and its recipient hands it to a parser that wants a
// terminated string, without having to copy it.
uint8_t *base64_decode(const char *text, size_t text_len, size_t max_bytes, size_t *out_size);

#endif /* BASE64_H */

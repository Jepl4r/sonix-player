#ifndef JSON_H
#define JSON_H

#include <stdbool.h>
#include <stddef.h>

// A small JSON parser for streaming-service responses.
//
// radio.c gets by without one: it scans for keys by hand in the raw text, and
// for radio-browser.info -- an array of flat objects, all strings and numbers
// -- that works in twenty lines.
//
// Qobuz and Tidal nest four or five levels deep
//
//     {"tracks":{"items":[{"album":{"image":{"small":"..."}},"performer":{...}}]}}
//
// where searching the raw text for "small" finds the first occurrence in
// whatever object it happens to sit in. A real parser is the only way to read
// the right field.
//
// One pass fills an array of tokens in document order, each with its span in
// the text, its child count and its parent. No string is copied until it is
// asked for -- a Qobuz response is a hundred kilobytes or so, and copying it
// twice on a device with 64 MB is not free.
//
// The document holds a pointer to the text, not a copy: the text must stay
// alive and unmodified for as long as the document is read.

typedef enum {
	JSON_UNDEFINED = 0,
	JSON_OBJECT,
	JSON_ARRAY,
	JSON_STRING,
	JSON_NUMBER,
	JSON_BOOL,
	JSON_NULL,
} json_type_t;

typedef struct {
	json_type_t type;
	int start; // first character of the value (for strings, inside the quotes)
	int end;   // one past the last
	int size;  // object: pair count; array: element count; otherwise 0
	int parent;
} json_tok_t;

typedef struct {
	const char *text;
	json_tok_t *toks;
	int count;
} json_doc_t;

// How deep nesting may go before the document is rejected. Without a limit a
// document of nothing but opening brackets takes down the stack. Qobuz and
// Tidal never pass the fifth level.
#define JSON_MAX_DEPTH 64

// Parses `text`. False on malformed JSON, out of memory, or nesting past the
// limit. On success the caller must call json_free().
bool json_parse(const char *text, json_doc_t *doc);
void json_free(json_doc_t *doc);

// The root token, or -1 when the document is empty.
int json_root(const json_doc_t *doc);

// The value of `key` in object `obj`, or -1. Compares the unescaped string:
// these services use plain ASCII keys where raw text would do, but a key
// carrying a backslash must not slip through unnoticed.
int json_get(const json_doc_t *doc, int obj, const char *key);

// Element `index` of array `arr`, or -1.
int json_at(const json_doc_t *doc, int arr, int index);

// Element count of an array (or pair count of an object); 0 for anything else.
int json_len(const json_doc_t *doc, int tok);

// Walks a dotted path: json_path(doc, root, "tracks.items") is two json_get
// calls. A number between dots indexes an array:
// "tracks.items.0.album.title". Returns -1 as soon as a step is missing.
int json_path(const json_doc_t *doc, int from, const char *path);

// ---------------------------------------------------------------------------
// Extraction. All of these are lenient: a -1 token, or one of the wrong type,
// yields the fallback rather than an error to check on every line. Remote
// responses have fields that are sometimes present and sometimes not, and
// checking twenty times per screen helps nobody.
// ---------------------------------------------------------------------------

// Copies the string with escapes resolved (\" \\ \/ \b \f \n \r \t and
// \uXXXX, surrogate pairs included, converted to UTF-8). Truncates to `size`.
// False when the token is not a string, leaving `out` empty.
bool json_str(const json_doc_t *doc, int tok, char *out, size_t size);

long json_long(const json_doc_t *doc, int tok, long fallback);

// The same at 64 bits. `long` is 32 bits here, so an id above 2147483647 does
// not fit, and strtol saturates rather than returning garbage: every such id
// comes back as exactly 2147483647. Podcast Index episode ids are eleven
// digits, so json_long collapsed them all onto one number, hence one cache
// file, and the player kept playing the same episode under another episode's
// title and duration. Use this for any id the service does not promise to keep
// small.
long long json_llong(const json_doc_t *doc, int tok, long long fallback);

double json_double(const json_doc_t *doc, int tok, double fallback);
bool json_bool(const json_doc_t *doc, int tok, bool fallback);

// Shorthands for the common case: read one field of an object.
bool json_obj_str(const json_doc_t *doc, int obj, const char *key, char *out, size_t size);
long json_obj_long(const json_doc_t *doc, int obj, const char *key, long fallback);
long long json_obj_llong(const json_doc_t *doc, int obj, const char *key, long long fallback);
bool json_obj_bool(const json_doc_t *doc, int obj, const char *key, bool fallback);

#endif /* JSON_H */

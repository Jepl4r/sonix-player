#include "onair.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "src/system/core/json.h"

#define FIELD_MAX 256

// ---------------------------------------------------------------------------
// one line
// ---------------------------------------------------------------------------

// Control characters become one space, runs of spaces collapse to one, and
// the ends are trimmed.
static void flatten(char *text) {
	char *w = text;
	bool space = true; // drops leading spaces too
	for (const char *r = text; *r; r++) {
		unsigned char c = (unsigned char)*r;
		if (c < 0x20 || c == 0x7F || c == ' ') {
			if (!space) {
				*w++ = ' ';
				space = true;
			}
			continue;
		}
		*w++ = (char)c;
		space = false;
	}
	while (w > text && w[-1] == ' ') {
		w--;
	}
	*w = '\0';
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

static const char *const ARTIST_KEYS[] = {"artist", "executor", "performer", "singer", NULL};
static const char *const TITLE_KEYS[] = {"title", "track", "song", NULL};

// Whether the key's name contains one of `parts` (NULL-terminated), ignoring
// case.
static bool key_has_any(const json_doc_t *doc, int key, const char *const *parts) {
	char name[48];
	if (!parts || !json_str(doc, key, name, sizeof(name))) {
		return false;
	}
	for (char *c = name; *c; c++) {
		*c = (char)tolower((unsigned char)*c);
	}
	for (int p = 0; parts[p]; p++) {
		if (strstr(name, parts[p])) {
			return true;
		}
	}
	return false;
}

// The first non-empty string, anywhere under `tok`, whose key contains one of
// `parts` and none of `not` ("titleExecutor" is an artist, not a title).
static bool json_find(const json_doc_t *doc, int tok, const char *const *parts, const char *const *not, char *out,
					  size_t size) {
	const json_tok_t *t = &doc->toks[tok];
	if (t->type != JSON_OBJECT && t->type != JSON_ARRAY) {
		return false;
	}
	for (int i = tok + 1; i < doc->count && doc->toks[i].start < t->end; i++) {
		if (doc->toks[i].parent != tok) {
			continue;
		}
		if (t->type == JSON_OBJECT && doc->toks[i].type == JSON_STRING && i + 1 < doc->count) {
			int value = i + 1;
			if (key_has_any(doc, i, parts) && !key_has_any(doc, i, not) && json_str(doc, value, out, size) &&
				out[0]) {
				return true;
			}
			if (json_find(doc, value, parts, not, out, size)) {
				return true;
			}
			i = value;
		} else if (json_find(doc, i, parts, not, out, size)) {
			return true;
		}
	}
	return false;
}

// Each complete top-level object in `text` is tried on its own; one cut short
// by the length of the ICY field is skipped. `out` gets "Artist - Title", the
// title alone, or "".
static void from_json(const char *text, char *out, size_t size) {
	out[0] = '\0';
	const char *p = text;
	while (*p && !out[0]) {
		while (*p && *p != '{') {
			p++;
		}
		if (!*p) {
			break;
		}
		const char *q = p;
		int depth = 0;
		bool in_string = false;
		for (; *q; q++) {
			if (in_string) {
				if (*q == '\\' && q[1]) {
					q++;
				} else if (*q == '"') {
					in_string = false;
				}
			} else if (*q == '"') {
				in_string = true;
			} else if (*q == '{') {
				depth++;
			} else if (*q == '}' && --depth == 0) {
				break;
			}
		}
		if (!*q) {
			break;
		}

		size_t len = (size_t)(q - p) + 1;
		char *object = malloc(len + 1);
		if (!object) {
			break;
		}
		memcpy(object, p, len);
		object[len] = '\0';

		json_doc_t doc;
		if (json_parse(object, &doc)) {
			int root = json_root(&doc);
			char artist[FIELD_MAX / 2] = "", title[FIELD_MAX / 2] = "";
			if (root >= 0) {
				json_find(&doc, root, ARTIST_KEYS, NULL, artist, sizeof(artist));
				json_find(&doc, root, TITLE_KEYS, ARTIST_KEYS, title, sizeof(title));
			}
			if (artist[0] && title[0]) {
				snprintf(out, size, "%s - %s", artist, title);
			} else if (title[0]) {
				snprintf(out, size, "%s", title);
			}
			json_free(&doc);
		}
		free(object);
		p = q + 1;
	}
}

// ---------------------------------------------------------------------------
// key="value" fields
// ---------------------------------------------------------------------------

// Finds the next `key="value"` or `key='value'` at or after `from`. The key is
// letters, digits and underscores, starting a word. Fills the spans and
// returns true.
static bool next_field(const char *from, const char **key, size_t *key_len, const char **value, size_t *value_len,
					   const char **after) {
	for (const char *p = from; *p; p++) {
		if (!isalpha((unsigned char)*p) || (p > from && (isalnum((unsigned char)p[-1]) || p[-1] == '_'))) {
			continue;
		}
		const char *k = p;
		while (isalnum((unsigned char)*p) || *p == '_') {
			p++;
		}
		if (*p != '=' || (p[1] != '"' && p[1] != '\'')) {
			p--;
			continue;
		}
		char quote = p[1];
		const char *v = p + 2;
		const char *end = strchr(v, quote);
		if (!end) {
			return false;
		}
		*key = k;
		*key_len = (size_t)(p - k);
		*value = v;
		*value_len = (size_t)(end - v);
		*after = end + 1;
		return true;
	}
	return false;
}

static bool key_is(const char *key, size_t len, const char *const *names) {
	for (int i = 0; names[i]; i++) {
		if (strlen(names[i]) == len && strncasecmp(key, names[i], len) == 0) {
			return true;
		}
	}
	return false;
}

static void copy_span(char *out, size_t size, const char *from, size_t len) {
	if (len >= size) {
		len = size - 1;
	}
	memcpy(out, from, len);
	out[len] = '\0';
}

// `Artist - text="Title" song_spot="M" MediaBaseId="..."` and
// `title="Title",artist="Artist",url="..."` alike. Returns false, leaving
// `text` alone, when there is no such field in it.
static bool from_fields(char *text) {
	static const char *const TITLE_NAMES[] = {"title", "text", "song", "track", "songtitle", NULL};
	static const char *const ARTIST_NAMES[] = {"artist", "performer", "singer", NULL};

	const char *key, *value, *after;
	size_t key_len, value_len;
	if (!next_field(text, &key, &key_len, &value, &value_len, &after)) {
		return false;
	}

	char prefix[FIELD_MAX] = "";
	copy_span(prefix, sizeof(prefix), text, (size_t)(key - text));
	char title[FIELD_MAX] = "", artist[FIELD_MAX] = "";

	const char *p = text;
	while (next_field(p, &key, &key_len, &value, &value_len, &after)) {
		if (!title[0] && key_is(key, key_len, TITLE_NAMES)) {
			copy_span(title, sizeof(title), value, value_len);
		} else if (!artist[0] && key_is(key, key_len, ARTIST_NAMES)) {
			copy_span(artist, sizeof(artist), value, value_len);
		}
		p = after;
	}

	// What stands before the first field: the artist in front of text="...",
	// or the whole of the song when no field names one.
	size_t n = strlen(prefix);
	while (n > 0 && (prefix[n - 1] == ' ' || prefix[n - 1] == '-' || prefix[n - 1] == ',' || prefix[n - 1] == ';')) {
		prefix[--n] = '\0';
	}
	if (!artist[0] && title[0]) {
		snprintf(artist, sizeof(artist), "%s", prefix);
	}

	char result[2 * FIELD_MAX + 4];
	if (artist[0] && title[0]) {
		snprintf(result, sizeof(result), "%s - %s", artist, title);
	} else if (title[0]) {
		snprintf(result, sizeof(result), "%s", title);
	} else {
		snprintf(result, sizeof(result), "%s", prefix);
	}
	size_t room = strlen(text);
	snprintf(text, room + 1, "%s", result);
	return true;
}

// ---------------------------------------------------------------------------
// codes among the words
// ---------------------------------------------------------------------------

// Whether a piece of the text is a code rather than words. Codes are:
//
//   anything without a letter          numbers, dates, times
//   a single character
//   identifiers of six or more characters with a digit and no space, made of
//     hex digits and dashes                        "a3f9c2e1", "3f2a-9c1b"
//     capitals, digits and underscores             "SPOT_12345"
//     capitals, digits and dashes, digits >= letters   "RDS-12345"
//   eight or more letters and digits, upper and lower case mixed   "aB3dE9fG2"
//
// A name with a number in it ("UB40", "Blink-182", "Maroon 5") is none of these.
static bool is_code(const char *f, size_t len) {
	if (len < 2) {
		return true;
	}
	int letters = 0, digits = 0;
	bool upper = false, lower = false, space = false, other = false;
	bool hex_only = true, upper_underscore = true, upper_dash = true;
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)f[i];
		if (isalpha(c) || c >= 0x80) {
			letters++;
		}
		digits += isdigit(c) != 0;
		upper |= isupper(c) != 0;
		lower |= islower(c) != 0;
		space |= c == ' ';
		other |= !isalnum(c);
		hex_only &= isxdigit(c) || c == '-';
		upper_underscore &= isupper(c) || isdigit(c) || c == '_';
		upper_dash &= isupper(c) || isdigit(c) || c == '-';
	}
	if (letters == 0) {
		return true;
	}
	if (space || digits == 0) {
		return false;
	}
	if (len >= 6 && (hex_only || upper_underscore || (upper_dash && digits >= letters))) {
		return true;
	}
	return len >= 8 && upper && lower && !other;
}

// Text split at `sep` (a tilde, or an asterisk): the first two pieces made of
// words, joined with " - ", and every code dropped. False, leaving `text`
// alone, when `sep` is not there.
static bool from_separated(char *text, size_t size, char sep) {
	if (!strchr(text, sep)) {
		return false;
	}

	char result[2 * FIELD_MAX + 4] = "";
	int kept = 0;
	const char *p = text;
	while (kept < 2) {
		const char *end = strchr(p, sep);
		size_t len = end ? (size_t)(end - p) : strlen(p);
		const char *f = p;
		while (len > 0 && *f == ' ') {
			f++;
			len--;
		}
		while (len > 0 && f[len - 1] == ' ') {
			len--;
		}
		if (!is_code(f, len)) {
			size_t used = strlen(result);
			snprintf(result + used, sizeof(result) - used, "%s%.*s", kept ? " - " : "", (int)len, f);
			kept++;
		}
		if (!end) {
			break;
		}
		p = end + 1;
	}

	snprintf(text, size, "%s", result);
	return true;
}

// The pieces between " - " that are codes go. A plain number goes only when
// there are three pieces or more: "Smashing Pumpkins - 1979" is a song.
static void drop_code_pieces(char *text) {
	int pieces = 1;
	for (const char *p = strstr(text, " - "); p; p = strstr(p + 3, " - ")) {
		pieces++;
	}
	if (pieces < 2) {
		return;
	}

	char result[2 * FIELD_MAX + 4] = "";
	const char *p = text;
	for (;;) {
		const char *end = strstr(p, " - ");
		size_t len = end ? (size_t)(end - p) : strlen(p);
		bool has_letter = false;
		for (size_t i = 0; i < len; i++) {
			has_letter |= isalpha((unsigned char)p[i]) || (unsigned char)p[i] >= 0x80;
		}
		bool drop = len == 0 || (has_letter ? is_code(p, len) : pieces >= 3);
		if (!drop) {
			size_t used = strlen(result);
			snprintf(result + used, sizeof(result) - used, "%s%.*s", used ? " - " : "", (int)len, p);
		}
		if (!end) {
			break;
		}
		p = end + 3;
	}
	snprintf(text, strlen(text) + 1, "%s", result);
}

// How many times `c` is in `text`.
static int count_of(const char *text, char c) {
	int n = 0;
	for (; *text; text++) {
		n += *text == c;
	}
	return n;
}

// ---------------------------------------------------------------------------
// the rest
// ---------------------------------------------------------------------------

// Web addresses go, word by word.
static void drop_urls(char *text) {
	char *w = text;
	const char *r = text;
	while (*r) {
		const char *word_end = strchr(r, ' ');
		size_t len = word_end ? (size_t)(word_end - r) : strlen(r);
		bool url = strncasecmp(r, "http://", 7) == 0 || strncasecmp(r, "https://", 8) == 0 ||
				   strncasecmp(r, "www.", 4) == 0;
		if (!url) {
			memmove(w, r, len);
			w += len;
			if (word_end) {
				*w++ = ' ';
			}
		}
		r += len;
		if (*r == ' ') {
			r++;
		}
	}
	*w = '\0';
}

// HTML tags go, and the common entities become their characters.
static void drop_html(char *text) {
	static const struct {
		const char *entity;
		char c;
	} ENTITIES[] = {{"&amp;", '&'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&#39;", '\''},
					{"&#039;", '\''}, {"&lt;", '<'}, {"&gt;", '>'},	 {"&nbsp;", ' '}};

	char *w = text;
	for (const char *r = text; *r;) {
		if (*r == '<' && (isalpha((unsigned char)r[1]) || r[1] == '/')) {
			const char *close = strchr(r, '>');
			if (close && close - r < 40) {
				*w++ = ' ';
				r = close + 1;
				continue;
			}
		}
		if (*r == '&') {
			bool done = false;
			for (size_t i = 0; i < sizeof(ENTITIES) / sizeof(ENTITIES[0]); i++) {
				size_t len = strlen(ENTITIES[i].entity);
				if (strncasecmp(r, ENTITIES[i].entity, len) == 0) {
					*w++ = ENTITIES[i].c;
					r += len;
					done = true;
					break;
				}
			}
			if (done) {
				continue;
			}
		}
		*w++ = *r++;
	}
	*w = '\0';
}

static bool is_separator(char c) { return c == ' ' || c == '-' || c == '|' || c == '/' || c == ':'; }

// Separators left dangling at either end, and " - - " in the middle where a
// field between them was empty.
static void trim_separators(char *text) {
	char *start = text;
	while (*start && is_separator(*start)) {
		start++;
	}
	if (start != text) {
		memmove(text, start, strlen(start) + 1);
	}
	size_t n = strlen(text);
	while (n > 0 && is_separator(text[n - 1])) {
		text[--n] = '\0';
	}

	char *hole;
	while ((hole = strstr(text, " - - ")) != NULL) {
		memmove(hole, hole + 2, strlen(hole + 2) + 1);
	}
}

static bool is_placeholder(const char *text) {
	static const char *const NAMES[] = {"unknown", "unknown - unknown", "n/a", "null", "undefined", NULL};
	for (int i = 0; NAMES[i]; i++) {
		if (strcasecmp(text, NAMES[i]) == 0) {
			return true;
		}
	}
	return false;
}

void onair_clean(char *text, size_t size) {
	if (!text || size == 0) {
		return;
	}
	flatten(text);

	if (text[0] == '{' || text[0] == '[') {
		char found[FIELD_MAX + 4];
		from_json(text, found, sizeof(found));
		snprintf(text, strlen(text) + 1, "%s", found);
	} else if (!from_separated(text, size, '~') &&
			   !(count_of(text, '*') >= 2 && from_separated(text, size, '*'))) {
		from_fields(text);
	}

	drop_html(text);
	drop_urls(text);
	flatten(text);
	drop_code_pieces(text);
	trim_separators(text);
	if (is_placeholder(text)) {
		text[0] = '\0';
	}
}

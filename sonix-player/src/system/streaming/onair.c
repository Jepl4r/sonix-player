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
// fields between tildes
// ---------------------------------------------------------------------------

// Whether a field between tildes is a code rather than words: no letter at all
// (numbers, dates, times), a single character, or an identifier -- six or more
// characters with a digit, no space, and either only hex digits and dashes or
// only capitals, digits and underscores ("a3f9c2e1", "SPOT_12345").
static bool tilde_field_is_code(const char *f, size_t len) {
	if (len < 2) {
		return true;
	}
	bool letter = false, digit = false, space = false, hex_only = true, upper_only = true;
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)f[i];
		letter |= isalpha(c) || c >= 0x80;
		digit |= isdigit(c) != 0;
		space |= c == ' ';
		hex_only &= isxdigit(c) || c == '-';
		upper_only &= isupper(c) || isdigit(c) || c == '_';
	}
	if (!letter) {
		return true;
	}
	return len >= 6 && digit && !space && (hex_only || upper_only);
}

// `Artist~Title~2024~0~a3f9c2e1~` and the like: the first two fields made of
// words, joined with " - ", and every code dropped. False, leaving `text`
// alone, when there is no tilde in it.
static bool from_tildes(char *text, size_t size) {
	if (!strchr(text, '~')) {
		return false;
	}

	char result[2 * FIELD_MAX + 4] = "";
	int kept = 0;
	const char *p = text;
	while (kept < 2) {
		const char *end = strchr(p, '~');
		size_t len = end ? (size_t)(end - p) : strlen(p);
		const char *f = p;
		while (len > 0 && *f == ' ') {
			f++;
			len--;
		}
		while (len > 0 && f[len - 1] == ' ') {
			len--;
		}
		if (!tilde_field_is_code(f, len)) {
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
	} else if (!from_tildes(text, size)) {
		from_fields(text);
	}

	drop_html(text);
	drop_urls(text);
	flatten(text);
	trim_separators(text);
	if (is_placeholder(text)) {
		text[0] = '\0';
	}
}

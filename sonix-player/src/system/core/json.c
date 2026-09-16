#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

typedef struct {
	const char *text;
	int pos;
	json_doc_t *doc;
	int capacity;
	int depth;
} parser_t;

static int tok_new(parser_t *p, json_type_t type, int start, int parent) {
	if (p->doc->count == p->capacity) {
		// Doubling, starting at 64: a Qobuz response runs to a few thousand
		// tokens, and starting at one would mean a dozen reallocs per request.
		int wanted = p->capacity ? p->capacity * 2 : 64;
		json_tok_t *grown = realloc(p->doc->toks, (size_t)wanted * sizeof(*grown));
		if (!grown) {
			return -1;
		}
		p->doc->toks = grown;
		p->capacity = wanted;
	}

	int index = p->doc->count++;
	json_tok_t *t = &p->doc->toks[index];
	t->type = type;
	t->start = start;
	t->end = start;
	t->size = 0;
	t->parent = parent;
	return index;
}

static void skip_space(parser_t *p) {
	while (p->text[p->pos] == ' ' || p->text[p->pos] == '\t' || p->text[p->pos] == '\n' || p->text[p->pos] == '\r') {
		p->pos++;
	}
}

// Skips a string body, leaving `pos` on the closing quote. Escapes are not
// decoded, only stepped over, since an escaped quote is not the end.
static bool skip_string(parser_t *p) {
	while (p->text[p->pos]) {
		char c = p->text[p->pos];
		if (c == '\\') {
			if (!p->text[p->pos + 1]) {
				return false;
			}
			p->pos += 2;
			continue;
		}
		if (c == '"') {
			return true;
		}
		p->pos++;
	}
	return false;
}

static bool parse_value(parser_t *p, int parent);

static bool parse_object(parser_t *p, int self) {
	p->pos++; // opening brace
	for (;;) {
		skip_space(p);
		if (p->text[p->pos] == '}') {
			p->pos++;
			p->doc->toks[self].end = p->pos;
			return true;
		}
		if (p->doc->toks[self].size > 0) {
			if (p->text[p->pos] != ',') {
				return false;
			}
			p->pos++;
			skip_space(p);
			// A trailing comma before the closing brace is malformed, not an
			// empty object: rejecting it here avoids counting a pair that is
			// not there.
			if (p->text[p->pos] == '}') {
				return false;
			}
		}

		if (p->text[p->pos] != '"') {
			return false; // a key is always a string
		}
		int key_start = ++p->pos;
		if (!skip_string(p)) {
			return false;
		}
		int key = tok_new(p, JSON_STRING, key_start, self);
		if (key < 0) {
			return false;
		}
		p->doc->toks[key].end = p->pos;
		p->pos++; // closing quote

		skip_space(p);
		if (p->text[p->pos] != ':') {
			return false;
		}
		p->pos++;

		// The value's parent is the key, not the object: that is how json_get()
		// tells keys from values while walking the object's children.
		if (!parse_value(p, key)) {
			return false;
		}
		p->doc->toks[self].size++;
	}
}

static bool parse_array(parser_t *p, int self) {
	p->pos++; // opening bracket
	for (;;) {
		skip_space(p);
		if (p->text[p->pos] == ']') {
			p->pos++;
			p->doc->toks[self].end = p->pos;
			return true;
		}
		if (p->doc->toks[self].size > 0) {
			if (p->text[p->pos] != ',') {
				return false;
			}
			p->pos++;
			skip_space(p);
			if (p->text[p->pos] == ']') {
				return false;
			}
		}
		if (!parse_value(p, self)) {
			return false;
		}
		p->doc->toks[self].size++;
	}
}

static bool is_number_char(char c) {
	return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E';
}

static bool parse_value(parser_t *p, int parent) {
	if (p->depth >= JSON_MAX_DEPTH) {
		return false;
	}

	skip_space(p);
	char c = p->text[p->pos];

	if (c == '{' || c == '[') {
		int self = tok_new(p, c == '{' ? JSON_OBJECT : JSON_ARRAY, p->pos, parent);
		if (self < 0) {
			return false;
		}
		p->depth++;
		bool ok = c == '{' ? parse_object(p, self) : parse_array(p, self);
		p->depth--;
		return ok;
	}

	if (c == '"') {
		int start = ++p->pos;
		if (!skip_string(p)) {
			return false;
		}
		int self = tok_new(p, JSON_STRING, start, parent);
		if (self < 0) {
			return false;
		}
		p->doc->toks[self].end = p->pos;
		p->pos++;
		return true;
	}

	if (strncmp(p->text + p->pos, "true", 4) == 0 || strncmp(p->text + p->pos, "false", 5) == 0) {
		int len = p->text[p->pos] == 't' ? 4 : 5;
		int self = tok_new(p, JSON_BOOL, p->pos, parent);
		if (self < 0) {
			return false;
		}
		p->pos += len;
		p->doc->toks[self].end = p->pos;
		return true;
	}

	if (strncmp(p->text + p->pos, "null", 4) == 0) {
		int self = tok_new(p, JSON_NULL, p->pos, parent);
		if (self < 0) {
			return false;
		}
		p->pos += 4;
		p->doc->toks[self].end = p->pos;
		return true;
	}

	if (is_number_char(c)) {
		int start = p->pos;
		while (is_number_char(p->text[p->pos])) {
			p->pos++;
		}
		int self = tok_new(p, JSON_NUMBER, start, parent);
		if (self < 0) {
			return false;
		}
		p->doc->toks[self].end = p->pos;
		return true;
	}

	return false;
}

bool json_parse(const char *text, json_doc_t *doc) {
	if (!text || !doc) {
		return false;
	}

	memset(doc, 0, sizeof(*doc));
	doc->text = text;

	parser_t p = {.text = text, .pos = 0, .doc = doc, .capacity = 0, .depth = 0};
	if (!parse_value(&p, -1)) {
		json_free(doc);
		return false;
	}

	// Only whitespace may follow the root value. A second document appended
	// behind it means the response arrived mangled, and silently reading just
	// the first part would be worse than failing.
	skip_space(&p);
	if (text[p.pos] != '\0') {
		json_free(doc);
		return false;
	}

	return doc->count > 0;
}

void json_free(json_doc_t *doc) {
	if (!doc) {
		return;
	}
	free(doc->toks);
	doc->toks = NULL;
	doc->count = 0;
	doc->text = NULL;
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

int json_root(const json_doc_t *doc) { return (doc && doc->count > 0) ? 0 : -1; }

static bool valid(const json_doc_t *doc, int tok) { return doc && doc->toks && tok >= 0 && tok < doc->count; }

// Compares a string token's raw text with `key`, unescaping only when an
// escape is present. Keys from these services never carry one, so the common
// case stays a memcmp.
static bool key_equals(const json_doc_t *doc, int tok, const char *key) {
	const json_tok_t *t = &doc->toks[tok];
	int len = t->end - t->start;
	if (memchr(doc->text + t->start, '\\', (size_t)len) == NULL) {
		return (int)strlen(key) == len && memcmp(doc->text + t->start, key, (size_t)len) == 0;
	}
	char buf[256];
	if (!json_str(doc, tok, buf, sizeof(buf))) {
		return false;
	}
	return strcmp(buf, key) == 0;
}

int json_get(const json_doc_t *doc, int obj, const char *key) {
	if (!valid(doc, obj) || !key || doc->toks[obj].type != JSON_OBJECT) {
		return -1;
	}

	int obj_end = doc->toks[obj].end;
	for (int i = obj + 1; i < doc->count && doc->toks[i].start < obj_end; i++) {
		// The object's direct children are its keys; a key's value has the key
		// as its parent.
		if (doc->toks[i].parent != obj) {
			continue;
		}
		if (doc->toks[i].type == JSON_STRING && key_equals(doc, i, key)) {
			for (int v = i + 1; v < doc->count; v++) {
				if (doc->toks[v].parent == i) {
					return v;
				}
			}
			return -1;
		}
	}
	return -1;
}

int json_at(const json_doc_t *doc, int arr, int index) {
	if (!valid(doc, arr) || index < 0 || doc->toks[arr].type != JSON_ARRAY) {
		return -1;
	}

	int arr_end = doc->toks[arr].end;
	int seen = 0;
	for (int i = arr + 1; i < doc->count && doc->toks[i].start < arr_end; i++) {
		if (doc->toks[i].parent != arr) {
			continue;
		}
		if (seen == index) {
			return i;
		}
		seen++;
	}
	return -1;
}

int json_len(const json_doc_t *doc, int tok) {
	if (!valid(doc, tok)) {
		return 0;
	}
	json_type_t type = doc->toks[tok].type;
	return (type == JSON_ARRAY || type == JSON_OBJECT) ? doc->toks[tok].size : 0;
}

int json_path(const json_doc_t *doc, int from, const char *path) {
	if (!valid(doc, from) || !path) {
		return -1;
	}

	int at = from;
	const char *p = path;
	while (*p && at >= 0) {
		char piece[128];
		size_t n = 0;
		while (*p && *p != '.' && n + 1 < sizeof(piece)) {
			piece[n++] = *p++;
		}
		piece[n] = '\0';
		while (*p && *p != '.') {
			p++; // segment longer than the buffer: discard the remainder
		}
		if (*p == '.') {
			p++;
		}
		if (n == 0) {
			return -1;
		}

		// All digits on an array token means the segment is an index.
		bool digits = true;
		for (size_t i = 0; i < n; i++) {
			if (piece[i] < '0' || piece[i] > '9') {
				digits = false;
				break;
			}
		}
		if (digits && doc->toks[at].type == JSON_ARRAY) {
			at = json_at(doc, at, atoi(piece));
		} else {
			at = json_get(doc, at, piece);
		}
	}
	return at;
}

// ---------------------------------------------------------------------------
// Extraction
// ---------------------------------------------------------------------------

// Encodes one code point as UTF-8. Returns the number of bytes written.
static size_t utf8_put(char *out, size_t room, unsigned int cp) {
	if (cp < 0x80) {
		if (room < 1) {
			return 0;
		}
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		if (room < 2) {
			return 0;
		}
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		if (room < 3) {
			return 0;
		}
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	if (room < 4) {
		return 0;
	}
	out[0] = (char)(0xF0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	out[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

static int hex4(const char *s) {
	int value = 0;
	for (int i = 0; i < 4; i++) {
		char c = s[i];
		int digit;
		if (c >= '0' && c <= '9') {
			digit = c - '0';
		} else if (c >= 'a' && c <= 'f') {
			digit = c - 'a' + 10;
		} else if (c >= 'A' && c <= 'F') {
			digit = c - 'A' + 10;
		} else {
			return -1;
		}
		value = value * 16 + digit;
	}
	return value;
}

bool json_str(const json_doc_t *doc, int tok, char *out, size_t size) {
	if (!out || size == 0) {
		return false;
	}
	out[0] = '\0';
	if (!valid(doc, tok) || doc->toks[tok].type != JSON_STRING) {
		return false;
	}

	const char *src = doc->text + doc->toks[tok].start;
	int len = doc->toks[tok].end - doc->toks[tok].start;
	size_t w = 0;

	for (int i = 0; i < len && w + 1 < size; i++) {
		if (src[i] != '\\') {
			out[w++] = src[i];
			continue;
		}
		if (i + 1 >= len) {
			break;
		}
		char esc = src[++i];
		switch (esc) {
		case '"':
			out[w++] = '"';
			break;
		case '\\':
			out[w++] = '\\';
			break;
		case '/':
			out[w++] = '/';
			break;
		case 'b':
			out[w++] = '\b';
			break;
		case 'f':
			out[w++] = '\f';
			break;
		case 'n':
			out[w++] = '\n';
			break;
		case 'r':
			out[w++] = '\r';
			break;
		case 't':
			out[w++] = '\t';
			break;
		case 'u': {
			if (i + 4 >= len) {
				i = len;
				break;
			}
			int cp = hex4(src + i + 1);
			if (cp < 0) {
				i = len;
				break;
			}
			i += 4;
			// Surrogate pair: Qobuz titles are full of characters outside the
			// basic plane (emoji in playlist names), and decoding one half at
			// a time produces mojibake.
			if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < len && src[i + 1] == '\\' && src[i + 2] == 'u') {
				int low = hex4(src + i + 3);
				if (low >= 0xDC00 && low <= 0xDFFF) {
					cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
					i += 6;
				}
			}
			w += utf8_put(out + w, size - 1 - w, (unsigned int)cp);
			break;
		}
		default:
			out[w++] = esc; // unknown escape: keep the character as it is
			break;
		}
	}

	out[w] = '\0';
	return true;
}

long json_long(const json_doc_t *doc, int tok, long fallback) {
	if (!valid(doc, tok)) {
		return fallback;
	}
	const json_tok_t *t = &doc->toks[tok];
	if (t->type == JSON_BOOL) {
		return doc->text[t->start] == 't' ? 1 : 0;
	}
	// Strings are accepted too: Qobuz sends ids sometimes as numbers and
	// sometimes quoted, within the same response.
	if (t->type != JSON_NUMBER && t->type != JSON_STRING) {
		return fallback;
	}

	char buf[32];
	int len = t->end - t->start;
	if (len <= 0 || len >= (int)sizeof(buf)) {
		return fallback;
	}
	memcpy(buf, doc->text + t->start, (size_t)len);
	buf[len] = '\0';

	char *stop = NULL;
	long value = strtol(buf, &stop, 10);
	return (stop && stop != buf) ? value : fallback;
}

long long json_llong(const json_doc_t *doc, int tok, long long fallback) {
	if (!valid(doc, tok)) {
		return fallback;
	}
	const json_tok_t *t = &doc->toks[tok];
	if (t->type == JSON_BOOL) {
		return doc->text[t->start] == 't' ? 1 : 0;
	}
	if (t->type != JSON_NUMBER && t->type != JSON_STRING) {
		return fallback;
	}

	char buf[32];
	int len = t->end - t->start;
	if (len <= 0 || len >= (int)sizeof(buf)) {
		return fallback;
	}
	memcpy(buf, doc->text + t->start, (size_t)len);
	buf[len] = '\0';

	char *stop = NULL;
	long long value = strtoll(buf, &stop, 10);
	return (stop && stop != buf) ? value : fallback;
}

double json_double(const json_doc_t *doc, int tok, double fallback) {
	if (!valid(doc, tok)) {
		return fallback;
	}
	const json_tok_t *t = &doc->toks[tok];
	if (t->type != JSON_NUMBER && t->type != JSON_STRING) {
		return fallback;
	}

	char buf[64];
	int len = t->end - t->start;
	if (len <= 0 || len >= (int)sizeof(buf)) {
		return fallback;
	}
	memcpy(buf, doc->text + t->start, (size_t)len);
	buf[len] = '\0';

	char *stop = NULL;
	double value = strtod(buf, &stop);
	return (stop && stop != buf) ? value : fallback;
}

bool json_bool(const json_doc_t *doc, int tok, bool fallback) {
	if (!valid(doc, tok)) {
		return fallback;
	}
	const json_tok_t *t = &doc->toks[tok];
	if (t->type == JSON_BOOL) {
		return doc->text[t->start] == 't';
	}
	// Qobuz sends its subscription flags as booleans in some responses and as
	// 0/1 in others.
	if (t->type == JSON_NUMBER) {
		return json_long(doc, tok, fallback ? 1 : 0) != 0;
	}
	return fallback;
}

bool json_obj_str(const json_doc_t *doc, int obj, const char *key, char *out, size_t size) {
	return json_str(doc, json_get(doc, obj, key), out, size);
}

long json_obj_long(const json_doc_t *doc, int obj, const char *key, long fallback) {
	return json_long(doc, json_get(doc, obj, key), fallback);
}

long long json_obj_llong(const json_doc_t *doc, int obj, const char *key, long long fallback) {
	return json_llong(doc, json_get(doc, obj, key), fallback);
}

bool json_obj_bool(const json_doc_t *doc, int obj, const char *key, bool fallback) {
	return json_bool(doc, json_get(doc, obj, key), fallback);
}

#include "epub_xml.h"

#include <stdlib.h>
#include <string.h>

static bool name_char(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
		   c == '.' || c == ':';
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

// The part of `name` after the last ':'. XHTML writes `p`, an OPF often writes
// `dc:title`, and the two are the same kind of thing to everything here.
static const char *strip_prefix(const char *name, size_t len, size_t *out_len) {
	for (size_t i = len; i > 0; i--) {
		if (name[i - 1] == ':') {
			*out_len = len - i;
			return name + i;
		}
	}
	*out_len = len;
	return name;
}

void epub_xml_init(epub_xml_t *x, const char *doc) {
	memset(x, 0, sizeof(*x));
	x->cursor = doc ? doc : "";
}

// Walks past the things that are markup but not elements. Returns where the
// next real character is.
static const char *skip_non_element(const char *p) {
	for (;;) {
		if (p[0] != '<') {
			return p;
		}
		if (p[1] == '?') { // <?xml ... ?>
			const char *end = strstr(p + 2, "?>");
			p = end ? end + 2 : p + strlen(p);
			continue;
		}
		if (p[1] == '!') {
			if (strncmp(p + 2, "--", 2) == 0) { // a comment
				const char *end = strstr(p + 4, "-->");
				p = end ? end + 3 : p + strlen(p);
				continue;
			}
			if (strncmp(p + 2, "[CDATA[", 7) == 0) {
				return p; // text, and the caller wants it
			}
			// <!DOCTYPE ...>, which can hold a bracketed internal subset.
			const char *q = p + 2;
			int depth = 0;
			while (*q && (depth > 0 || *q != '>')) {
				if (*q == '[') {
					depth++;
				} else if (*q == ']') {
					depth--;
				}
				q++;
			}
			p = *q ? q + 1 : q;
			continue;
		}
		return p;
	}
}

bool epub_xml_next(epub_xml_t *x) {
	const char *p = skip_non_element(x->cursor);
	if (!*p) {
		return false;
	}

	// A run of text: everything up to the next '<' that starts markup.
	if (*p != '<') {
		const char *start = p;
		while (*p && *p != '<') {
			p++;
		}
		x->event = EPUB_XML_TEXT;
		x->text = start;
		x->text_len = (size_t)(p - start);
		x->name = NULL;
		x->name_len = 0;
		x->attrs = NULL;
		x->attrs_len = 0;
		x->self_closing = false;
		x->cursor = p;
		return true;
	}

	// A CDATA section is text that happens to be spelled as markup.
	if (strncmp(p, "<![CDATA[", 9) == 0) {
		const char *start = p + 9;
		const char *end = strstr(start, "]]>");
		x->event = EPUB_XML_TEXT;
		x->text = start;
		x->text_len = end ? (size_t)(end - start) : strlen(start);
		x->name = NULL;
		x->name_len = 0;
		x->attrs = NULL;
		x->attrs_len = 0;
		x->self_closing = false;
		x->cursor = end ? end + 3 : start + x->text_len;
		return true;
	}

	bool closing = p[1] == '/';
	const char *name = p + (closing ? 2 : 1);
	const char *q = name;
	while (name_char(*q)) {
		q++;
	}
	if (q == name) {
		// A '<' that begins nothing -- an unescaped one in the text, which
		// converters do produce. Report it as text so it reaches the page.
		x->event = EPUB_XML_TEXT;
		x->text = p;
		x->text_len = 1;
		x->name = NULL;
		x->name_len = 0;
		x->cursor = p + 1;
		return true;
	}

	size_t raw_len = (size_t)(q - name);
	x->name = strip_prefix(name, raw_len, &x->name_len);

	// Everything from here to the '>' is attributes. Quoted values may hold a
	// '>' of their own, so this cannot just look for the next one.
	const char *attrs = q;
	bool in_quote = false;
	char quote = 0;
	while (*q && (in_quote || *q != '>')) {
		if (in_quote) {
			if (*q == quote) {
				in_quote = false;
			}
		} else if (*q == '"' || *q == '\'') {
			in_quote = true;
			quote = *q;
		}
		q++;
	}

	const char *attr_end = q;
	x->self_closing = !closing && attr_end > attrs && attr_end[-1] == '/';
	if (x->self_closing) {
		attr_end--;
	}
	x->attrs = attrs;
	x->attrs_len = (size_t)(attr_end - attrs);
	x->event = closing ? EPUB_XML_CLOSE : EPUB_XML_OPEN;
	x->text = NULL;
	x->text_len = 0;
	x->cursor = *q ? q + 1 : q;
	return true;
}

bool epub_xml_is(const epub_xml_t *x, const char *name) {
	size_t len = strlen(name);
	if (len != x->name_len) {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		if (lower(x->name[i]) != lower(name[i])) {
			return false;
		}
	}
	return true;
}

bool epub_xml_attr(const epub_xml_t *x, const char *name, const char **value, size_t *len) {
	size_t want_len = strlen(name);
	const char *p = x->attrs;
	const char *end = x->attrs + x->attrs_len;

	while (p < end) {
		while (p < end && !name_char(*p)) {
			p++;
		}
		const char *key = p;
		while (p < end && name_char(*p)) {
			p++;
		}
		if (p == key) {
			break;
		}
		size_t key_len = 0;
		const char *local = strip_prefix(key, (size_t)(p - key), &key_len);

		while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
			p++;
		}
		if (p >= end || *p != '=') {
			continue; // an attribute with no value: HTML allows it, skip it
		}
		p++;
		while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
			p++;
		}

		const char *val;
		size_t val_len;
		if (p < end && (*p == '"' || *p == '\'')) {
			char quote = *p++;
			val = p;
			while (p < end && *p != quote) {
				p++;
			}
			val_len = (size_t)(p - val);
			if (p < end) {
				p++;
			}
		} else {
			val = p;
			while (p < end && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
				p++;
			}
			val_len = (size_t)(p - val);
		}

		if (key_len == want_len) {
			bool same = true;
			for (size_t i = 0; i < key_len && same; i++) {
				same = lower(local[i]) == lower(name[i]);
			}
			if (same) {
				*value = val;
				*len = val_len;
				return true;
			}
		}
	}
	return false;
}

// One code point as UTF-8. Returns how many bytes it took.
static size_t put_utf8(char *out, size_t room, unsigned cp) {
	if (cp < 0x80 && room >= 1) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800 && room >= 2) {
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000 && room >= 3) {
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	if (cp < 0x110000 && room >= 4) {
		out[0] = (char)(0xF0 | (cp >> 18));
		out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
		out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[3] = (char)(0x80 | (cp & 0x3F));
		return 4;
	}
	return 0;
}

// The named entities worth knowing. The five XML ones are required; the rest
// are the handful that turn up constantly in books converted from Word and
// would otherwise be printed as "&nbsp;" in the middle of a sentence.
static const struct {
	const char *name;
	unsigned cp;
} ENTITIES[] = {
	{"amp", '&'},	 {"lt", '<'},	  {"gt", '>'},		{"quot", '"'},	 {"apos", '\''}, {"nbsp", 0x00A0},
	{"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026}, {"lsquo", 0x2018}, {"rsquo", 0x2019},
	{"ldquo", 0x201C}, {"rdquo", 0x201D}, {"laquo", 0x00AB},  {"raquo", 0x00BB}, {"eacute", 0x00E9},
	{"egrave", 0x00E8}, {"agrave", 0x00E0}, {"igrave", 0x00EC}, {"ograve", 0x00F2}, {"ugrave", 0x00F9},
	{"deg", 0x00B0},  {"copy", 0x00A9},  {"reg", 0x00AE},	{"trade", 0x2122}, {"bull", 0x2022},
	{"middot", 0x00B7}, {"euro", 0x20AC}, {"pound", 0x00A3},  {"sect", 0x00A7},
};

size_t epub_xml_unescape(char *out, size_t out_size, const char *text, size_t len) {
	if (!out || out_size == 0) {
		return 0;
	}
	size_t written = 0;
	for (size_t i = 0; i < len && written + 1 < out_size;) {
		if (text[i] != '&') {
			out[written++] = text[i++];
			continue;
		}

		// Find the ';'. A bare '&' in the text -- which happens -- has none
		// within reach, and is copied through as itself.
		size_t j = i + 1;
		size_t limit = i + 12 < len ? i + 12 : len;
		while (j < limit && text[j] != ';') {
			j++;
		}
		if (j >= limit || text[j] != ';') {
			out[written++] = text[i++];
			continue;
		}

		const char *body = text + i + 1;
		size_t body_len = j - i - 1;
		unsigned cp = 0;
		bool known = false;

		if (body_len >= 2 && body[0] == '#') {
			cp = (unsigned)strtoul(body[1] == 'x' || body[1] == 'X' ? body + 2 : body + 1, NULL,
								   body[1] == 'x' || body[1] == 'X' ? 16 : 10);
			known = cp != 0;
		} else {
			for (size_t k = 0; k < sizeof(ENTITIES) / sizeof(ENTITIES[0]); k++) {
				if (strlen(ENTITIES[k].name) == body_len && strncmp(ENTITIES[k].name, body, body_len) == 0) {
					cp = ENTITIES[k].cp;
					known = true;
					break;
				}
			}
		}

		if (!known) {
			out[written++] = text[i++]; // leave it as it was written
			continue;
		}

		size_t took = put_utf8(out + written, out_size - 1 - written, cp);
		if (!took) {
			break; // no room for this code point: stop cleanly
		}
		written += took;
		i = j + 1;
	}

	out[written] = '\0';
	return written;
}

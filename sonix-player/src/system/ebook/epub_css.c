#include "src/system/ebook/epub_css.h"

#include <string.h>

// See epub_css.h for what this reads and what it deliberately does not.

#define FIRST_RULES 32u

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

static bool name_equals(const char *a, size_t a_len, const char *b) {
	size_t b_len = strlen(b);
	if (a_len != b_len) {
		return false;
	}
	for (size_t i = 0; i < a_len; i++) {
		char ca = a[i], cb = b[i];
		if (ca >= 'A' && ca <= 'Z') {
			ca = (char)(ca + 32);
		}
		if (cb >= 'A' && cb <= 'Z') {
			cb = (char)(cb + 32);
		}
		if (ca != cb) {
			return false;
		}
	}
	return true;
}

static bool same_name(const char *a, size_t a_len, const char *b, size_t b_len) {
	if (a_len != b_len) {
		return false;
	}
	for (size_t i = 0; i < a_len; i++) {
		char ca = a[i], cb = b[i];
		if (ca >= 'A' && ca <= 'Z') {
			ca = (char)(ca + 32);
		}
		if (cb >= 'A' && cb <= 'Z') {
			cb = (char)(cb + 32);
		}
		if (ca != cb) {
			return false;
		}
	}
	return true;
}

void epub_css_init(epub_css_t *css, epub_arena_t *arena) {
	memset(css, 0, sizeof(*css));
	css->arena = arena;
}

uint32_t epub_css_rule_count(const epub_css_t *css) { return css ? css->count : 0; }

// The rule array starts at FIRST_RULES and doubles, in the arena, up to
// EPUB_CSS_MAX_RULES; past that further rules are dropped.
static bool grow(epub_css_t *css) {
	if (css->count < css->cap) {
		return true;
	}
	if (css->cap >= EPUB_CSS_MAX_RULES) {
		return false;
	}
	uint32_t want = css->cap ? css->cap * 2u : FIRST_RULES;
	if (want > EPUB_CSS_MAX_RULES) {
		want = EPUB_CSS_MAX_RULES;
	}
	epub_css_rule_t *bigger = arena_alloc(css->arena, want * sizeof(*bigger));
	if (!bigger) {
		return false;
	}
	if (css->count) {
		memcpy(bigger, css->rules, css->count * sizeof(*bigger));
	}
	css->rules = bigger;
	css->cap = want;
	return true;
}

// The declarations between the braces, as a set of flags. Anything not in the
// four this understands is skipped without comment -- a stylesheet is mostly
// margins and line heights, and none of that reaches the screen here.
static uint8_t read_declarations(const char *body, size_t len) {
	uint8_t flags = 0;
	size_t i = 0;

	while (i < len) {
		while (i < len && (is_space(body[i]) || body[i] == ';')) {
			i++;
		}
		size_t prop_start = i;
		while (i < len && body[i] != ':' && body[i] != ';' && body[i] != '}') {
			i++;
		}
		if (i >= len || body[i] != ':') {
			// A declaration with no colon: skip to the next semicolon rather
			// than giving up on the rest of the block.
			while (i < len && body[i] != ';') {
				i++;
			}
			continue;
		}
		size_t prop_end = i;
		while (prop_end > prop_start && is_space(body[prop_end - 1])) {
			prop_end--;
		}
		i++; // the colon

		while (i < len && is_space(body[i])) {
			i++;
		}
		size_t value_start = i;
		while (i < len && body[i] != ';') {
			i++;
		}
		size_t value_end = i;
		while (value_end > value_start && is_space(body[value_end - 1])) {
			value_end--;
		}

		const char *prop = body + prop_start;
		size_t prop_len = prop_end - prop_start;
		const char *value = body + value_start;
		size_t value_len = value_end - value_start;

		if (name_equals(prop, prop_len, "font-style")) {
			if (name_equals(value, value_len, "italic") || name_equals(value, value_len, "oblique")) {
				flags |= EPUB_CSS_ITALIC;
			} else if (name_equals(value, value_len, "normal")) {
				flags |= EPUB_CSS_NOT_ITALIC;
			}
		} else if (name_equals(prop, prop_len, "font-weight")) {
			// Numbers as well as names: a converter writes `font-weight: 700`
			// about as often as it writes `bold`.
			if (name_equals(value, value_len, "bold") || name_equals(value, value_len, "bolder") ||
				name_equals(value, value_len, "600") || name_equals(value, value_len, "700") ||
				name_equals(value, value_len, "800") || name_equals(value, value_len, "900")) {
				flags |= EPUB_CSS_BOLD;
			} else if (name_equals(value, value_len, "normal") || name_equals(value, value_len, "400")) {
				flags |= EPUB_CSS_NOT_BOLD;
			}
		} else if (name_equals(prop, prop_len, "text-align")) {
			if (name_equals(value, value_len, "center") || name_equals(value, value_len, "centre")) {
				flags |= EPUB_CSS_CENTRE;
			} else if (name_equals(value, value_len, "right")) {
				flags |= EPUB_CSS_RIGHT;
			}
		} else if (name_equals(prop, prop_len, "display")) {
			if (name_equals(value, value_len, "none")) {
				flags |= EPUB_CSS_HIDDEN;
			}
		}
	}
	return flags;
}

// One selector out of a comma-separated list. Returns false for anything with a
// combinator, an id or a pseudo-class, which this does not resolve and will not
// guess at.
static bool read_selector(const char *sel, size_t len, const char **tag, uint16_t *tag_len, const char **cls,
						  uint16_t *cls_len) {
	while (len > 0 && is_space(sel[0])) {
		sel++;
		len--;
	}
	while (len > 0 && is_space(sel[len - 1])) {
		len--;
	}
	if (!len) {
		return false;
	}

	for (size_t i = 0; i < len; i++) {
		char c = sel[i];
		if (is_space(c) || c == '>' || c == '+' || c == '~' || c == '#' || c == ':' || c == '[' || c == '*') {
			return false;
		}
	}

	const char *dot = memchr(sel, '.', len);
	if (!dot) {
		*tag = sel;
		*tag_len = (uint16_t)len;
		*cls = NULL;
		*cls_len = 0;
		return true;
	}
	size_t before = (size_t)(dot - sel);
	size_t after = len - before - 1;
	if (!after) {
		return false; // a lone dot
	}
	// A second dot would be two classes at once, which is a rule this cannot
	// answer with one class name.
	if (memchr(dot + 1, '.', after)) {
		return false;
	}
	*tag = before ? sel : NULL;
	*tag_len = (uint16_t)before;
	*cls = dot + 1;
	*cls_len = (uint16_t)after;
	return true;
}

void epub_css_add(epub_css_t *css, const char *text, size_t len) {
	if (!css || !css->arena || !text) {
		return;
	}

	size_t i = 0;
	while (i < len) {
		// Comments, which a stylesheet is full of and which can hold anything.
		if (i + 1 < len && text[i] == '/' && text[i + 1] == '*') {
			i += 2;
			while (i + 1 < len && !(text[i] == '*' && text[i + 1] == '/')) {
				i++;
			}
			i = i + 2 < len ? i + 2 : len;
			continue;
		}
		if (is_space(text[i])) {
			i++;
			continue;
		}

		// An at-rule: @media, @font-face, @import. Skipped whole, including a
		// braced body if it has one. @media in particular is a screen size this
		// device does not have, and applying its contents unconditionally would
		// be worse than ignoring it.
		if (text[i] == '@') {
			int depth = 0;
			while (i < len) {
				if (text[i] == '{') {
					depth++;
				} else if (text[i] == '}') {
					depth--;
					if (depth <= 0) {
						i++;
						break;
					}
				} else if (text[i] == ';' && depth == 0) {
					i++;
					break;
				}
				i++;
			}
			continue;
		}

		size_t sel_start = i;
		while (i < len && text[i] != '{') {
			i++;
		}
		if (i >= len) {
			break; // a selector with no block: the sheet is truncated
		}
		size_t sel_end = i;
		i++; // the brace

		size_t body_start = i;
		while (i < len && text[i] != '}') {
			i++;
		}
		size_t body_end = i;
		if (i < len) {
			i++;
		}

		uint8_t flags = read_declarations(text + body_start, body_end - body_start);
		if (!flags) {
			continue; // nothing here reaches the screen
		}

		// The comma-separated selectors in front of that block.
		size_t s = sel_start;
		while (s < sel_end) {
			size_t comma = s;
			while (comma < sel_end && text[comma] != ',') {
				comma++;
			}

			const char *tag = NULL, *cls = NULL;
			uint16_t tag_len = 0, cls_len = 0;
			if (read_selector(text + s, comma - s, &tag, &tag_len, &cls, &cls_len) && grow(css)) {
				epub_css_rule_t *rule = &css->rules[css->count++];
				rule->tag = tag;
				rule->tag_len = tag_len;
				rule->class_name = cls;
				rule->class_len = cls_len;
				rule->flags = flags;
			}
			s = comma + 1;
		}
	}
}

// Does `class_attr` -- which may be "calibre3 first-line" -- contain `name`?
static bool has_class(const char *class_attr, size_t class_len, const char *name, size_t name_len) {
	size_t i = 0;
	while (i < class_len) {
		while (i < class_len && is_space(class_attr[i])) {
			i++;
		}
		size_t start = i;
		while (i < class_len && !is_space(class_attr[i])) {
			i++;
		}
		if (i > start && same_name(class_attr + start, i - start, name, name_len)) {
			return true;
		}
	}
	return false;
}

uint8_t epub_css_lookup(const epub_css_t *css, const char *tag, size_t tag_len, const char *class_attr,
						size_t class_len) {
	if (!css || !css->count) {
		return 0;
	}

	uint8_t flags = 0;
	// In order, so a later rule wins: for rules of equal specificity that is
	// what the cascade does, and telling specificities apart would mean keeping
	// far more of CSS than this does.
	for (uint32_t i = 0; i < css->count; i++) {
		const epub_css_rule_t *rule = &css->rules[i];

		if (rule->tag && !same_name(rule->tag, rule->tag_len, tag, tag_len)) {
			continue;
		}
		if (rule->class_name) {
			if (!class_attr || !class_len) {
				continue;
			}
			if (!has_class(class_attr, class_len, rule->class_name, rule->class_len)) {
				continue;
			}
		}

		// A later rule that says "normal" clears what an earlier one set, which
		// is the whole reason the NOT_ bits exist.
		if (rule->flags & EPUB_CSS_NOT_BOLD) {
			flags &= (uint8_t)~EPUB_CSS_BOLD;
		}
		if (rule->flags & EPUB_CSS_NOT_ITALIC) {
			flags &= (uint8_t)~EPUB_CSS_ITALIC;
		}
		if (rule->flags & (EPUB_CSS_CENTRE | EPUB_CSS_RIGHT)) {
			flags &= (uint8_t)~(EPUB_CSS_CENTRE | EPUB_CSS_RIGHT);
		}
		flags |= (uint8_t)(rule->flags & ~(EPUB_CSS_NOT_BOLD | EPUB_CSS_NOT_ITALIC));
	}
	return flags;
}

#ifndef EPUB_CSS_H
#define EPUB_CSS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "src/system/ebook/epub_arena.h"

// ---------------------------------------------------------------------------
// As much CSS as a book actually needs
//
// Not a CSS engine. A real one means a cascade, specificity, inheritance, a box
// model and units -- and on a 480 px screen with one font family loaded from the
// card, almost none of that would change a single pixel. What it would change is
// how long a chapter takes to open on a 1 GHz MIPS.
//
// So this reads the four things that are visible when they are missing, and
// ignores the rest of the stylesheet:
//
//   font-style: italic          a converter that writes <span class="calibre3">
//   font-weight: bold           instead of <em> and <strong>, which is most of
//                               them
//   text-align: center|right    title pages, poems, the line under a heading
//   display: none               notes and markers meant not to be shown, which
//                               without this are read out in the middle of a
//                               sentence
//
// Selectors are the ones books use for those four: `p`, `.calibre3`,
// `p.calibre3`, and comma-separated lists of them. A selector with a descendant,
// a child, an id or a pseudo-class is skipped rather than half-applied -- half
// of a rule is worse than none, because it is wrong in a way nobody can see the
// cause of.
//
// Everything lives in the arena it is given, so a chapter's stylesheet goes away
// with the chapter.
// ---------------------------------------------------------------------------

#define EPUB_CSS_BOLD 0x01u
#define EPUB_CSS_ITALIC 0x02u
#define EPUB_CSS_CENTRE 0x04u
#define EPUB_CSS_RIGHT 0x08u
#define EPUB_CSS_HIDDEN 0x10u
// Set when a rule says the opposite, so a class that turns italic OFF beats an
// enclosing one that turned it on. Without these a `.noitalic` is a class this
// would read as "says nothing about italic".
#define EPUB_CSS_NOT_BOLD 0x20u
#define EPUB_CSS_NOT_ITALIC 0x40u

typedef struct epub_css epub_css_t;

// Starts empty. `arena` is where the rules go and has to outlive every lookup.
void epub_css_init(epub_css_t *css, epub_arena_t *arena);

// Reads one stylesheet. Called once per <style> block and once per stylesheet
// the chapter links to; later rules win over earlier ones, which is what the
// cascade does for rules of equal specificity.
void epub_css_add(epub_css_t *css, const char *text, size_t len);

// What the rules say about an element with this tag and this class attribute.
// `class_attr` may hold several names separated by spaces, and may be NULL.
uint8_t epub_css_lookup(const epub_css_t *css, const char *tag, size_t tag_len, const char *class_attr,
						size_t class_len);

// How many rules were kept.
uint32_t epub_css_rule_count(const epub_css_t *css);

// The struct is here rather than hidden so it can live on the parser's stack
// without an allocation. Nothing outside epub_css.c touches its fields.
typedef struct {
	const char *tag;   // NULL for a bare class selector
	uint16_t tag_len;
	const char *class_name; // NULL for a bare element selector
	uint16_t class_len;
	uint8_t flags;
} epub_css_rule_t;

#define EPUB_CSS_MAX_RULES 512u

struct epub_css {
	epub_arena_t *arena;
	epub_css_rule_t *rules;
	uint32_t count;
	uint32_t cap;
};

#endif /* EPUB_CSS_H */

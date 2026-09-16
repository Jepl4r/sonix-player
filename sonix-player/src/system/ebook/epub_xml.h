#ifndef EPUB_XML_H
#define EPUB_XML_H

#include <stdbool.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// The whole XML this reader has
//
// A scanner that walks a document in memory and reports one event at a time:
// a tag opened, a tag closed, a run of text. It builds nothing -- no tree, no
// attribute table, no copies -- and every string it reports points into the
// document itself. Three files use it: container.xml, the OPF, and the chapter
// XHTML, and none of them wants a DOM.
//
// What it does not do, on purpose: validate, resolve entities outside the small
// table it carries, track namespaces properly, or care
// about the difference between well-formed and merely readable. An EPUB in the
// wild is often the output of a converter, and a reader that refuses a book
// over a stray ampersand is a worse reader than one that shows the ampersand.
//
// Namespace prefixes are stripped from element names, so `dc:title` reports as
// `title` and `opf:package` as `package`. That is wrong in general and exactly
// right here: this reader knows which document it is reading, and no EPUB uses
// the same local name in two namespaces in a way that matters.
// ---------------------------------------------------------------------------

typedef enum {
	EPUB_XML_OPEN,	// <p ...> or <br/>
	EPUB_XML_CLOSE, // </p>
	EPUB_XML_TEXT,	// the run between two tags
} epub_xml_event_t;

typedef struct {
	const char *cursor; // where the next event starts

	epub_xml_event_t event;
	const char *name; // element name with the prefix stripped; not NUL-terminated
	size_t name_len;
	const char *text; // EPUB_XML_TEXT only, still escaped
	size_t text_len;
	const char *attrs; // the raw text between the name and the '>'
	size_t attrs_len;
	bool self_closing; // <br/>: an OPEN that is also its own CLOSE
} epub_xml_t;

// `doc` must be NUL-terminated, which is what epub_zip_extract() returns.
void epub_xml_init(epub_xml_t *x, const char *doc);

// The next event, or false at the end of the document.
bool epub_xml_next(epub_xml_t *x);

// True when the current element is `name` (case-insensitive: XHTML in the wild
// is not consistent about it).
bool epub_xml_is(const epub_xml_t *x, const char *name);

// The value of one attribute of the current element. The prefix is ignored on
// both sides, so `xlink:href` answers to `href`. False when it is absent.
bool epub_xml_attr(const epub_xml_t *x, const char *name, const char **value, size_t *len);

// Copies `len` bytes of escaped XML into `out` as plain UTF-8 text, resolving
// numeric references and a small table of named entities (see ENTITIES in
// epub_xml.c). Returns how many bytes were
// written, never more than `out_size - 1`, and always NUL-terminates.
size_t epub_xml_unescape(char *out, size_t out_size, const char *text, size_t len);

#endif /* EPUB_XML_H */

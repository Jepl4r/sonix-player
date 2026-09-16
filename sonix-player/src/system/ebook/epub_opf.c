#include "src/system/ebook/epub_internal.h"
#include "src/system/ebook/epub_xml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// What the book says it is
//
// Three documents, read once when the book opens and then never again:
//
//   META-INF/container.xml   says where the package document is
//   the package document     says what the book is called, what it is made of,
//                            and in what order those parts are read
//   the table of contents    the NCX of EPUB 2 or the nav document of EPUB 3
//
// Together they come to a few kilobytes of structure for a book of any size,
// and that is the whole of what stays in memory between chapters.
// ---------------------------------------------------------------------------

// One entry of the manifest, kept only while the OPF is being read: the spine
// refers to chapters by id, and this is what turns an id into a path. It is
// deliberately transient -- a malloc'd array freed before this file returns --
// because after the spine is resolved nobody needs it, and leaving it in the
// book's arena would keep every resource of the book listed for the whole
// session.
typedef struct {
	const char *id;
	size_t id_len;
	const char *href;
	size_t href_len;
	bool is_nav;	 // properties="nav": the EPUB 3 table of contents
	bool is_cover;	 // properties="cover-image"
	bool is_ncx;	 // media-type says it is the EPUB 2 table of contents
} manifest_item_t;

#define MANIFEST_MAX 8192

bool epub_resolve_path(char *out, size_t out_size, const char *base, const char *href, size_t href_len) {
	// A fragment names a place inside a document, and this reader opens whole
	// documents, so it is cut here rather than in three callers.
	for (size_t i = 0; i < href_len; i++) {
		if (href[i] == '#') {
			href_len = i;
			break;
		}
	}
	if (!href_len) {
		return false;
	}

	// An absolute path inside the archive ignores the base.
	if (href[0] == '/') {
		href++;
		href_len--;
		base = "";
	}

	char joined[1024];
	int n = snprintf(joined, sizeof(joined), "%s%.*s", base ? base : "", (int)href_len, href);
	if (n < 0 || (size_t)n >= sizeof(joined)) {
		return false;
	}

	// Collapse "." and ".." by walking the segments. A ZIP name never contains
	// either, so one has to be resolved here or the member is simply not found
	// -- and "../images/x.jpg" from a chapter in a subfolder is ordinary.
	struct {
		const char *at;
		size_t len;
	} segments[64];
	int depth = 0;

	const char *p = joined;
	while (*p) {
		const char *slash = strchr(p, '/');
		size_t len = slash ? (size_t)(slash - p) : strlen(p);
		if (len == 1 && p[0] == '.') {
			// the current folder: nothing to do
		} else if (len == 2 && p[0] == '.' && p[1] == '.') {
			if (depth > 0) {
				depth--;
			}
		} else if (len > 0) {
			if (depth >= (int)(sizeof(segments) / sizeof(segments[0]))) {
				return false;
			}
			segments[depth].at = p;
			segments[depth].len = len;
			depth++;
		}
		if (!slash) {
			break;
		}
		p = slash + 1;
	}

	size_t used = 0;
	for (int i = 0; i < depth; i++) {
		if (used + segments[i].len + 2 > out_size) {
			return false;
		}
		if (used) {
			out[used++] = '/';
		}
		memcpy(out + used, segments[i].at, segments[i].len);
		used += segments[i].len;
	}
	if (used >= out_size) {
		return false;
	}
	out[used] = '\0';
	return used > 0;
}

// The text between the current OPEN and its CLOSE, unescaped into the book's
// arena. For <dc:title> and friends, which are one short run of text.
static char *element_text(ebook_t *book, epub_xml_t *x) {
	char buffer[512];
	size_t used = 0;
	buffer[0] = '\0';

	int depth = 1;
	while (depth > 0 && epub_xml_next(x)) {
		if (x->event == EPUB_XML_OPEN && !x->self_closing) {
			depth++;
		} else if (x->event == EPUB_XML_CLOSE) {
			depth--;
		} else if (x->event == EPUB_XML_TEXT && used + 1 < sizeof(buffer)) {
			used += epub_xml_unescape(buffer + used, sizeof(buffer) - used, x->text, x->text_len);
		}
	}

	// Books written by converters often wrap the title in whitespace.
	char *start = buffer;
	while (*start == ' ' || *start == '\n' || *start == '\r' || *start == '\t') {
		start++;
	}
	size_t len = strlen(start);
	while (len && (start[len - 1] == ' ' || start[len - 1] == '\n' || start[len - 1] == '\r' ||
				   start[len - 1] == '\t')) {
		len--;
	}
	return len ? arena_strndup(&book->book_arena, start, len) : NULL;
}

static char *find_opf_path(ebook_t *book) {
	int index = epub_zip_find(&book->zip, "META-INF/container.xml");
	if (index < 0) {
		return NULL;
	}
	uint32_t size = 0;
	char *doc = epub_zip_extract(&book->zip, index, &book->book_arena, EPUB_MAX_OPF, &size);
	if (!doc) {
		return NULL;
	}

	epub_xml_t x;
	epub_xml_init(&x, doc);
	char *path = NULL;
	while (!path && epub_xml_next(&x)) {
		if (x.event != EPUB_XML_OPEN || !epub_xml_is(&x, "rootfile")) {
			continue;
		}
		const char *value;
		size_t len;
		if (epub_xml_attr(&x, "full-path", &value, &len)) {
			path = arena_strndup(&book->book_arena, value, len);
		}
	}
	return path;
}

// "OEBPS/content.opf" -> "OEBPS/". Everything the OPF points at is relative to
// this, which is why a book whose OPF sits in a folder does not resolve without
// it.
static char *folder_of(ebook_t *book, const char *path) {
	const char *slash = strrchr(path, '/');
	if (!slash) {
		return arena_strdup(&book->book_arena, "");
	}
	return arena_strndup(&book->book_arena, path, (size_t)(slash - path + 1));
}

static bool attr_has_word(const epub_xml_t *x, const char *attr, const char *word) {
	const char *value;
	size_t len;
	if (!epub_xml_attr(x, attr, &value, &len)) {
		return false;
	}
	size_t want = strlen(word);
	for (size_t i = 0; i + want <= len; i++) {
		if (strncmp(value + i, word, want) != 0) {
			continue;
		}
		bool left = i == 0 || value[i - 1] == ' ';
		bool right = i + want == len || value[i + want] == ' ';
		if (left && right) {
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// The table of contents
// ---------------------------------------------------------------------------

// Which spine entry a href lands in. The TOC points at documents, the reader
// moves in spine order, and a TOC entry whose document is not in the spine
// simply has nowhere to go.
static int spine_of_path(const ebook_t *book, const char *path) {
	for (uint32_t i = 0; i < book->spine_count; i++) {
		if (strcmp(book->spine[i].path, path) == 0) {
			return (int)i;
		}
	}
	return -1;
}

static void toc_add(ebook_t *book, const char *label, const char *path, int depth) {
	if (book->toc_count >= EPUB_MAX_TOC || !label || !label[0]) {
		return;
	}
	int spine = spine_of_path(book, path);
	if (spine < 0) {
		return;
	}
	ebook_toc_entry_t *e = &book->toc[book->toc_count++];
	e->label = label;
	e->spine = (uint32_t)spine;
	e->depth = (uint8_t)(depth < 0 ? 0 : (depth > 255 ? 255 : depth));
}

// EPUB 2: an NCX, which is navPoints nested inside navPoints.
static void read_ncx(ebook_t *book, const char *doc, const char *base) {
	epub_xml_t x;
	epub_xml_init(&x, doc);

	int depth = -1; // nesting level of navPoints
	char label[256];
	char target[1024];
	label[0] = '\0';
	target[0] = '\0';

	while (epub_xml_next(&x)) {
		if (x.event == EPUB_XML_OPEN && epub_xml_is(&x, "navpoint")) {
			depth++;
			label[0] = '\0';
			target[0] = '\0';
			continue;
		}
		if (x.event == EPUB_XML_CLOSE && epub_xml_is(&x, "navpoint")) {
			if (depth >= 0) {
				depth--;
			}
			continue;
		}
		if (x.event == EPUB_XML_OPEN && epub_xml_is(&x, "text")) {
			char *got = element_text(book, &x);
			if (got && !label[0]) {
				snprintf(label, sizeof(label), "%s", got);
			}
			continue;
		}
		if (x.event == EPUB_XML_OPEN && epub_xml_is(&x, "content")) {
			const char *value;
			size_t len;
			if (epub_xml_attr(&x, "src", &value, &len)) {
				epub_resolve_path(target, sizeof(target), base, value, len);
			}
			// The label comes before the content in an NCX, so this is where
			// both are known.
			if (label[0] && target[0]) {
				toc_add(book, arena_strdup(&book->book_arena, label), target, depth);
				label[0] = '\0';
				target[0] = '\0';
			}
		}
	}
}

// EPUB 3: a nav document, which is an ordinary XHTML <nav> of nested <ol>.
static void read_nav(ebook_t *book, const char *doc, const char *base) {
	epub_xml_t x;
	epub_xml_init(&x, doc);

	bool inside = false;
	int depth = -1;
	char target[1024];

	while (epub_xml_next(&x)) {
		if (x.event == EPUB_XML_OPEN && epub_xml_is(&x, "nav")) {
			// The book may have several: a table of contents, a landmarks list,
			// a page list. Only the first is wanted.
			inside = !book->toc_count;
			continue;
		}
		if (x.event == EPUB_XML_CLOSE && epub_xml_is(&x, "nav")) {
			inside = false;
			continue;
		}
		if (!inside) {
			continue;
		}
		if (x.event == EPUB_XML_OPEN && epub_xml_is(&x, "ol")) {
			depth++;
			continue;
		}
		if (x.event == EPUB_XML_CLOSE && epub_xml_is(&x, "ol")) {
			if (depth >= 0) {
				depth--;
			}
			continue;
		}
		if (x.event == EPUB_XML_OPEN && epub_xml_is(&x, "a")) {
			const char *value;
			size_t len;
			target[0] = '\0';
			if (epub_xml_attr(&x, "href", &value, &len)) {
				epub_resolve_path(target, sizeof(target), base, value, len);
			}
			char *label = element_text(book, &x); // consumes to </a>
			if (target[0]) {
				toc_add(book, label, target, depth);
			}
		}
	}
}

static void read_toc(ebook_t *book, const manifest_item_t *items, int item_count, const char *ncx_id) {
	book->toc = arena_calloc(&book->book_arena, EPUB_MAX_TOC, sizeof(ebook_toc_entry_t));
	if (!book->toc) {
		return;
	}

	// The nav document first: an EPUB 3 book that also carries an NCX for old
	// readers has the better list in the nav.
	char path[1024];
	for (int pass = 0; pass < 2 && !book->toc_count; pass++) {
		for (int i = 0; i < item_count; i++) {
			const manifest_item_t *it = &items[i];
			bool want = pass == 0 ? it->is_nav
								  : (it->is_ncx || (ncx_id && it->id_len == strlen(ncx_id) &&
													strncmp(it->id, ncx_id, it->id_len) == 0));
			if (!want) {
				continue;
			}
			if (!epub_resolve_path(path, sizeof(path), book->opf_dir, it->href, it->href_len)) {
				continue;
			}
			int index = epub_zip_find(&book->zip, path);
			if (index < 0) {
				continue;
			}
			// Read into the CHAPTER arena, not the book's: the document is
			// hundreds of kilobytes on a big book and nothing needs it once the
			// entries are copied out. The arena is emptied below.
			char *doc = epub_zip_extract(&book->zip, index, &book->chapter_arena, EPUB_MAX_OPF, NULL);
			if (!doc) {
				continue;
			}
			// The links inside a table of contents are relative to the table of
			// contents, not to the OPF. Those are the same folder only in a book
			// that keeps everything in one; a book with its text in text/ and its
			// nav document beside it resolves to nothing against the OPF folder.
			char base[1024];
			const char *slash = strrchr(path, '/');
			size_t base_len = slash ? (size_t)(slash - path) + 1u : 0u;
			if (base_len >= sizeof(base)) {
				base_len = sizeof(base) - 1u;
			}
			memcpy(base, path, base_len);
			base[base_len] = '\0';
			if (pass == 0) {
				read_nav(book, doc, base);
			} else {
				read_ncx(book, doc, base);
			}
			arena_destroy(&book->chapter_arena);
			break;
		}
	}
}

bool epub_read_structure(ebook_t *book) {
	char *opf_path = find_opf_path(book);
	if (!opf_path) {
		return false;
	}
	book->opf_dir = folder_of(book, opf_path);

	int opf_index = epub_zip_find(&book->zip, opf_path);
	if (opf_index < 0) {
		return false;
	}
	uint32_t opf_size = 0;
	char *opf = epub_zip_extract(&book->zip, opf_index, &book->book_arena, EPUB_MAX_OPF, &opf_size);
	if (!opf) {
		return false;
	}

	manifest_item_t *items = calloc(MANIFEST_MAX, sizeof(manifest_item_t));
	if (!items) {
		return false;
	}
	int item_count = 0;

	book->spine = arena_calloc(&book->book_arena, EPUB_MAX_SPINE, sizeof(epub_spine_entry_t));
	if (!book->spine) {
		free(items);
		return false;
	}

	// The id of the NCX, which the spine names as an attribute rather than the
	// manifest marking it. Copied because the OPF text outlives this loop but
	// the scanner's pointers do not stay meaningful once it moves on.
	char ncx_id[128];
	ncx_id[0] = '\0';

	// Which manifest ids the spine asks for, in order. Resolved to paths after
	// the whole OPF is read, because a spine may come before its manifest.
	const char **order = calloc(EPUB_MAX_SPINE, sizeof(char *));
	size_t *order_len = calloc(EPUB_MAX_SPINE, sizeof(size_t));
	uint32_t order_count = 0;
	if (!order || !order_len) {
		free(items);
		free(order);
		free(order_len);
		return false;
	}

	char *cover_id = NULL; // <meta name="cover" content="..."> of EPUB 2

	epub_xml_t x;
	epub_xml_init(&x, opf);
	while (epub_xml_next(&x)) {
		if (x.event != EPUB_XML_OPEN) {
			continue;
		}

		if (epub_xml_is(&x, "title") && !book->title) {
			book->title = element_text(book, &x);
			continue;
		}
		if (epub_xml_is(&x, "creator") && !book->author) {
			book->author = element_text(book, &x);
			continue;
		}
		if (epub_xml_is(&x, "meta")) {
			const char *name, *content;
			size_t name_len, content_len;
			if (epub_xml_attr(&x, "name", &name, &name_len) && name_len == 5 && strncmp(name, "cover", 5) == 0 &&
				epub_xml_attr(&x, "content", &content, &content_len)) {
				cover_id = arena_strndup(&book->book_arena, content, content_len);
			}
			continue;
		}
		if (epub_xml_is(&x, "item") && item_count < MANIFEST_MAX) {
			manifest_item_t *it = &items[item_count];
			const char *value;
			size_t len;
			if (!epub_xml_attr(&x, "id", &it->id, &it->id_len)) {
				continue;
			}
			if (!epub_xml_attr(&x, "href", &it->href, &it->href_len)) {
				continue;
			}
			it->is_nav = attr_has_word(&x, "properties", "nav");
			it->is_cover = attr_has_word(&x, "properties", "cover-image");
			static const char NCX_TYPE[] = "application/x-dtbncx+xml";
			it->is_ncx = epub_xml_attr(&x, "media-type", &value, &len) && len == sizeof(NCX_TYPE) - 1 &&
						 strncmp(value, NCX_TYPE, len) == 0;
			item_count++;
			continue;
		}
		if (epub_xml_is(&x, "spine")) {
			const char *value;
			size_t len;
			if (epub_xml_attr(&x, "toc", &value, &len) && len < sizeof(ncx_id)) {
				memcpy(ncx_id, value, len);
				ncx_id[len] = '\0';
			}
			continue;
		}
		if (epub_xml_is(&x, "itemref") && order_count < EPUB_MAX_SPINE) {
			const char *value;
			size_t len;
			if (epub_xml_attr(&x, "idref", &value, &len)) {
				order[order_count] = value;
				order_len[order_count] = len;
				order_count++;
			}
			continue;
		}
	}

	// Spine ids to ZIP paths.
	char resolved[1024];
	for (uint32_t i = 0; i < order_count; i++) {
		for (int k = 0; k < item_count; k++) {
			if (items[k].id_len != order_len[i] || strncmp(items[k].id, order[i], order_len[i]) != 0) {
				continue;
			}
			if (!epub_resolve_path(resolved, sizeof(resolved), book->opf_dir, items[k].href, items[k].href_len)) {
				break;
			}
			// A spine entry whose document is not actually in the archive is
			// dropped rather than kept as a chapter that cannot open.
			if (epub_zip_find(&book->zip, resolved) < 0) {
				break;
			}
			book->spine[book->spine_count].path = arena_strdup(&book->book_arena, resolved);
			if (book->spine[book->spine_count].path) {
				book->spine_count++;
			}
			break;
		}
	}

	// The cover: named by a manifest property in EPUB 3, by a <meta> pointing
	// at a manifest id in EPUB 2. Both are common, so both are read.
	for (int k = 0; k < item_count && !book->cover_path; k++) {
		bool named = items[k].is_cover;
		if (!named && cover_id) {
			named = items[k].id_len == strlen(cover_id) && strncmp(items[k].id, cover_id, items[k].id_len) == 0;
		}
		if (named && epub_resolve_path(resolved, sizeof(resolved), book->opf_dir, items[k].href, items[k].href_len)) {
			book->cover_path = arena_strdup(&book->book_arena, resolved);
		}
	}

	if (book->spine_count) {
		read_toc(book, items, item_count, ncx_id[0] ? ncx_id : NULL);
	}

	free(items);
	free(order);
	free(order_len);
	return book->spine_count > 0;
}

#include "ebookfonts.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "src/gui/fonts/fonts.h"
#include "src/system/core/respath.h"

// The firmware's own copy, then the card's.
#define EBOOK_FONT_DIR SONIX_RESOURCE_DIR "/fonts/ebook"
// The interface's Japanese faces, which a Japanese book is set in.
#define JAPANESE_FONT_DIR SONIX_RESOURCE_DIR "/fonts"

// The four files, in the order ebookfonts_face() falls back through.
typedef enum {
	FACE_REGULAR = 0,
	FACE_ITALIC,
	FACE_BOLD,
	FACE_BOLD_ITALIC,
	FACE_COUNT,
} face_t;

static const char *const FACE_FILES[FACE_COUNT] = {
	"Bookerly-Regular.ttf",
	"Bookerly-Italic.ttf",
	"Bookerly-Bold.ttf",
	"Bookerly-BoldItalic.ttf",
};

// Rodin has no italic: the slanted styles come back as missing and fall back
// to the upright ones.
static const char *const JAPANESE_FILES[FACE_COUNT] = {
	"japanese.otf",
	NULL,
	"japanese-bold.otf",
	NULL,
};

static const char *const *face_files = FACE_FILES;

static char font_dir[512];
// The page's set, at the page's size.
static lv_font_t *faces[FACE_COUNT];
// Which faces have already been looked for and not found, so a book full of
// italics does not try to open a missing file on every paragraph. It must be
// cleared whenever the faces are -- hence a file static rather than a static
// local inside face_or_load(), which close_set() could not reach.
static bool tried[FACE_COUNT];
static int current_size = 20;
static bool have_regular;
// The one-word view's set, at its own size, with the same bookkeeping.
static lv_font_t *large[FACE_COUNT];
static bool large_tried[FACE_COUNT];
static int large_size;

static lv_font_t *open_face(face_t which, int size) {
	if (!face_files[which]) {
		return NULL;
	}
	char path[640];
	snprintf(path, sizeof(path), "%s/%s", font_dir, face_files[which]);
	if (access(path, R_OK) != 0) {
		return NULL;
	}
	// The style is NORMAL for every face on purpose: the slant and the weight
	// are in the file, which is the whole point of shipping four of them. Asking
	// FreeType to slant Bookerly Regular would produce a fifth thing that is
	// neither the publisher's italic nor the designer's.
	lv_font_t *font = lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP, (uint32_t)size,
											  LV_FREETYPE_FONT_STYLE_NORMAL);
	// Rodin's line is two ems tall -- its hhea line gap is a whole em -- which
	// sets a book double-spaced. A line of one and a half ems, the em box (0.12
	// below the baseline to 0.88 above) in the middle of it, is how Japanese
	// prose is usually leaded; the spacing setting adds to it as it does to
	// Bookerly's.
	if (font && face_files == JAPANESE_FILES) {
		font->line_height = size * 3 / 2;
		font->base_line = size * 37 / 100;
	}
	return font;
}

static void close_set(lv_font_t *set[], bool tried_set[]) {
	for (int i = 0; i < FACE_COUNT; i++) {
		if (set[i]) {
			lv_freetype_font_delete(set[i]);
			set[i] = NULL;
		}
		tried_set[i] = false;
	}
}

static void close_faces(void) {
	close_set(faces, tried);
	close_set(large, large_tried);
	large_size = 0;
	have_regular = false;
}

bool ebookfonts_open(const char *sd_root, int size, bool japanese) {
	ebookfonts_close();

	current_size = size > 0 ? size : 20;
	face_files = japanese ? JAPANESE_FILES : FACE_FILES;

	snprintf(font_dir, sizeof(font_dir), "%s", japanese ? JAPANESE_FONT_DIR : EBOOK_FONT_DIR);
	faces[FACE_REGULAR] = open_face(FACE_REGULAR, current_size);
	if (!faces[FACE_REGULAR]) {
		snprintf(font_dir, sizeof(font_dir), "%s/.local/fonts", sd_root ? sd_root : "");
		faces[FACE_REGULAR] = open_face(FACE_REGULAR, current_size);
	}
	have_regular = faces[FACE_REGULAR] != NULL;
	if (!have_regular) {
		fprintf(stderr, "ebook: no %s in %s or %s; reading with the interface font\n", face_files[FACE_REGULAR],
				japanese ? JAPANESE_FONT_DIR : EBOOK_FONT_DIR, font_dir);
	}
	return have_regular;
}

void ebookfonts_close(void) {
	close_faces();
	font_dir[0] = '\0';
}

bool ebookfonts_set_size(int size) {
	if (size <= 0 || size == current_size) {
		return have_regular;
	}

	// Which faces were in use, so the same ones come back at the new size and
	// no more: a book with no bold does not acquire the bold face because the
	// reader changed size.
	bool wanted[FACE_COUNT];
	for (int i = 0; i < FACE_COUNT; i++) {
		wanted[i] = faces[i] != NULL;
	}

	close_faces();
	current_size = size;
	for (int i = 0; i < FACE_COUNT; i++) {
		if (wanted[i]) {
			faces[i] = open_face((face_t)i, current_size);
		}
	}
	have_regular = faces[FACE_REGULAR] != NULL;
	return have_regular;
}

int ebookfonts_size(void) { return current_size; }
bool ebookfonts_present(void) { return have_regular; }

// Loads a face of `set` the first time the book asks for it. `tried_set` is
// what stops a missing file being opened again on every paragraph;
// close_set() clears it.
static lv_font_t *face_or_load(lv_font_t *set[], bool tried_set[], face_t which, int size) {
	if (set[which]) {
		return set[which];
	}
	if (tried_set[which]) {
		return NULL;
	}
	tried_set[which] = true;
	set[which] = open_face(which, size);
	return set[which];
}

// The face of `set` for a style, falling back through the others in the order
// the markdown asks for: italic before bold, because losing the slant of a
// title is more noticeable than losing its weight. NULL only when not even the
// regular face can be opened.
static lv_font_t *pick(lv_font_t *set[], bool tried_set[], int size, bool bold, bool italic) {
	if (bold && italic) {
		lv_font_t *f = face_or_load(set, tried_set, FACE_BOLD_ITALIC, size);
		if (f) {
			return f;
		}
		f = face_or_load(set, tried_set, FACE_ITALIC, size);
		if (f) {
			return f;
		}
		f = face_or_load(set, tried_set, FACE_BOLD, size);
		if (f) {
			return f;
		}
		return face_or_load(set, tried_set, FACE_REGULAR, size);
	}
	if (italic) {
		lv_font_t *f = face_or_load(set, tried_set, FACE_ITALIC, size);
		if (f) {
			return f;
		}
	}
	if (bold) {
		lv_font_t *f = face_or_load(set, tried_set, FACE_BOLD, size);
		if (f) {
			return f;
		}
	}
	return face_or_load(set, tried_set, FACE_REGULAR, size);
}

const lv_font_t *ebookfonts_face(bool bold, bool italic) {
	if (!have_regular) {
		// No Bookerly at all: the interface font, which has no italic of its
		// own either. A book is still readable; it is simply not set the way
		// its publisher set it.
		return &font_ui_20;
	}
	lv_font_t *f = pick(faces, tried, current_size, bold, italic);
	return f ? f : &font_ui_20;
}

const lv_font_t *ebookfonts_large_face(bool bold, bool italic, int size) {
	if (!have_regular || size <= 0) {
		return &font_ui_32;
	}
	if (size != large_size) {
		close_set(large, large_tried);
		large_size = size;
	}
	lv_font_t *f = pick(large, large_tried, size, bold, italic);
	return f ? f : &font_ui_32;
}

#include "ebookfonts.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "src/gui/fonts/fonts.h"

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

static char font_dir[512];
static lv_font_t *faces[FACE_COUNT];
// Which faces have already been looked for and not found, so a book full of
// italics does not try to open a missing file on every paragraph. It must be
// cleared whenever the faces are -- hence a file static rather than a static
// local inside ebookfonts_face(), which close_faces() could not reach.
static bool tried[FACE_COUNT];
static int current_size = 20;
static bool have_regular;

static lv_font_t *open_face(face_t which, int size) {
	char path[640];
	snprintf(path, sizeof(path), "%s/%s", font_dir, FACE_FILES[which]);
	if (access(path, R_OK) != 0) {
		return NULL;
	}
	// The style is NORMAL for every face on purpose: the slant and the weight
	// are in the file, which is the whole point of shipping four of them. Asking
	// FreeType to slant Bookerly Regular would produce a fifth thing that is
	// neither the publisher's italic nor the designer's.
	return lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP, (uint32_t)size,
								   LV_FREETYPE_FONT_STYLE_NORMAL);
}

static void close_faces(void) {
	for (int i = 0; i < FACE_COUNT; i++) {
		if (faces[i]) {
			lv_freetype_font_delete(faces[i]);
			faces[i] = NULL;
		}
		tried[i] = false;
	}
	have_regular = false;
}

bool ebookfonts_open(const char *sd_root, int size) {
	ebookfonts_close();

	snprintf(font_dir, sizeof(font_dir), "%s/.local/fonts", sd_root ? sd_root : "");
	current_size = size > 0 ? size : 20;

	faces[FACE_REGULAR] = open_face(FACE_REGULAR, current_size);
	have_regular = faces[FACE_REGULAR] != NULL;
	if (!have_regular) {
		fprintf(stderr, "ebook: no Bookerly in %s; reading with the interface font\n", font_dir);
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

// Loads a face the first time the book asks for it. `tried` is what stops a
// missing file being opened again on every paragraph; close_faces() clears it.
static lv_font_t *face_or_load(face_t which) {
	if (faces[which]) {
		return faces[which];
	}
	if (tried[which]) {
		return NULL;
	}
	tried[which] = true;
	faces[which] = open_face(which, current_size);
	return faces[which];
}

const lv_font_t *ebookfonts_face(bool bold, bool italic) {
	if (!have_regular) {
		// No Bookerly at all: the interface font, which has no italic of its
		// own either. A book is still readable; it is simply not set the way
		// its publisher set it.
		return &font_ui_20;
	}

	if (bold && italic) {
		lv_font_t *f = face_or_load(FACE_BOLD_ITALIC);
		if (f) {
			return f;
		}
		// The order the markdown asks for: italic before bold, because losing
		// the slant of a title is more noticeable than losing its weight.
		f = face_or_load(FACE_ITALIC);
		if (f) {
			return f;
		}
		f = face_or_load(FACE_BOLD);
		if (f) {
			return f;
		}
		return faces[FACE_REGULAR];
	}
	if (italic) {
		lv_font_t *f = face_or_load(FACE_ITALIC);
		return f ? f : faces[FACE_REGULAR];
	}
	if (bold) {
		lv_font_t *f = face_or_load(FACE_BOLD);
		return f ? f : faces[FACE_REGULAR];
	}
	return faces[FACE_REGULAR];
}

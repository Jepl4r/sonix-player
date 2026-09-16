#include "fonts.h"

#include "src/system/core/respath.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "lvgl/lvgl.h"

#include "src/system/core/lang.h"

// All text is drawn through LVGL's FreeType binding, from the faces the
// firmware ships in FONT_DIR. One FT_Face per file serves however many sizes
// use it; glyphs are rasterised on demand into a shared LRU cache.
//
// The lv_font_t objects the interface refers to (&font_ui_24 and friends) are
// statically allocated here and filled in by copying the fonts FreeType
// creates. The copy is safe because LVGL's FreeType callbacks reach their
// state only through font->dsc, which the copy carries along. The heap
// objects are deliberately never freed: they own the descriptors the copies
// point at, and live styles hold the fonts for the life of the process.

#define FONT_DIR SONIX_RESOURCE_DIR "/fonts"

// Host fallback: the copies in the repository, when there is no resource tree.
#ifdef HOST_BUILD
#define FONT_DIR_FALLBACK "assets/fonts"
#else
#define FONT_DIR_FALLBACK FONT_DIR
#endif

// The same four faces ship as .ttf on some firmwares and .otf on others, and
// FreeType reads both. Each face is therefore a list of candidate names, tried
// in order, and the first one present wins. A default face that is not found
// means an interface with no text in it at all.
static const char *const FONT_DEFAULT_FILES[] = {FONT_DIR "/default.ttf", FONT_DIR "/default.otf",
												 FONT_DIR_FALLBACK "/default.ttf", FONT_DIR_FALLBACK "/default.otf",
												 NULL};
static const char *const FONT_BOLD_FILES[] = {FONT_DIR "/bold.ttf", FONT_DIR "/bold.otf",
											  FONT_DIR_FALLBACK "/bold.ttf", FONT_DIR_FALLBACK "/bold.otf", NULL};
static const char *const FONT_KOREAN_FILES[] = {FONT_DIR "/Korean.ttf", FONT_DIR "/Korean.otf",
												FONT_DIR_FALLBACK "/Korean.ttf", FONT_DIR_FALLBACK "/Korean.otf",
												NULL};
static const char *const FONT_THAI_FILES[] = {FONT_DIR "/Thai.ttf", FONT_DIR "/Thai.otf",
											  FONT_DIR_FALLBACK "/Thai.ttf", FONT_DIR_FALLBACK "/Thai.otf", NULL};

static const char *basename_of(const char *path) {
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

// The one that exists, or NULL.
static const char *first_present(const char *const *candidates) {
	for (int i = 0; candidates[i]; i++) {
		if (access(candidates[i], R_OK) == 0) {
			return candidates[i];
		}
	}
	return NULL;
}

lv_font_t font_ui_14;
lv_font_t font_ui_16;
lv_font_t font_ui_18;
lv_font_t font_ui_20;
lv_font_t font_ui_22;
lv_font_t font_ui_24;
lv_font_t font_ui_24_bold;
lv_font_t font_ui_26;
lv_font_t font_ui_28;
lv_font_t font_ui_32;
lv_font_t font_ui_36_bold;
lv_font_t font_ui_64_bold;
lv_font_t font_ui_72;

typedef struct {
	lv_font_t *font;
	int size;
	bool bold;
} ui_font_t;

static const ui_font_t ui_fonts[] = {
	{&font_ui_14, 14, false}, {&font_ui_16, 16, false}, {&font_ui_18, 18, false},
	{&font_ui_20, 20, false}, {&font_ui_22, 22, false}, {&font_ui_24, 24, false},
	{&font_ui_26, 26, false}, {&font_ui_28, 28, false}, {&font_ui_32, 32, false},
	{&font_ui_72, 72, false}, {&font_ui_24_bold, 24, true}, {&font_ui_36_bold, 36, true},
	{&font_ui_64_bold, 64, true},
};

static char summary[128] = "";

static lv_font_t *open_face(const char *path, int size) {
	if (access(path, R_OK) != 0) {
		return NULL;
	}
	return lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP, (uint32_t)size,
								   LV_FREETYPE_FONT_STYLE_NORMAL);
}

bool fonts_init(void) {
	const char *regular_file = first_present(FONT_DEFAULT_FILES);
	const char *bold_file = first_present(FONT_BOLD_FILES);
	const char *korean_file = first_present(FONT_KOREAN_FILES);
	const char *thai_file = first_present(FONT_THAI_FILES);

	if (!regular_file) {
		fprintf(stderr, "fonts: no default.ttf or default.otf in %s or %s\n", FONT_DIR, FONT_DIR_FALLBACK);
		return false;
	}

	bool have_korean = false;
	bool have_thai = false;
	int filled = 0;

	for (size_t i = 0; i < sizeof(ui_fonts) / sizeof(ui_fonts[0]); i++) {
		const ui_font_t *ui = &ui_fonts[i];

		// The face this size draws from. The bold heading font uses the bold
		// file when the firmware has one; the regular stands in otherwise,
		// which reads fine at heading sizes even if it is not actually heavier.
		lv_font_t *head = NULL;
		if (ui->bold && bold_file) {
			head = open_face(bold_file, ui->size);
		}
		if (!head) {
			head = open_face(regular_file, ui->size);
		}
		if (!head) {
			fprintf(stderr, "fonts: cannot open %s at %dpx\n", regular_file, ui->size);
			return false;
		}

		// Hangul and Thai live in their own files, chained behind the main
		// face as fallbacks so they only answer for what it lacks.
		lv_font_t *tail = head;
		lv_font_t *korean = korean_file ? open_face(korean_file, ui->size) : NULL;
		if (korean) {
			tail->fallback = korean;
			tail = korean;
			have_korean = true;
		}
		lv_font_t *thai = thai_file ? open_face(thai_file, ui->size) : NULL;
		if (thai) {
			tail->fallback = thai;
			have_thai = true;
		}

		// Fill the static object the interface points at. `head` itself is
		// left allocated on purpose: its dsc is what the copy draws through.
		*ui->font = *head;
		filled++;
	}

	// Records the actual file names, so the log shows which container this
	// firmware ships.
	snprintf(summary, sizeof(summary), "%s%s%s%s", basename_of(regular_file),
			 bold_file ? ", " : "", bold_file ? basename_of(bold_file) : "",
			 have_korean || have_thai ? (have_korean && have_thai ? ", Korean + Thai" : (have_korean ? ", Korean" : ", Thai")) : "");

	char from[256];
	snprintf(from, sizeof(from), "%.*s", (int)(basename_of(regular_file) - regular_file - 1), regular_file);
	fprintf(stderr, "fonts: %d sizes from %s (%s)\n", filled, from, summary);
	return true;
}

// Empty until the faces have been opened, which is before any page is built.
// The stand-in is resolved here rather than stored, so it follows the language
// the same way every other line on the page does.
const char *fonts_summary(void) { return summary[0] ? summary : tr("none"); }

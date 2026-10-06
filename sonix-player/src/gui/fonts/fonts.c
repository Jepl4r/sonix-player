#include "fonts.h"

#include "src/system/core/respath.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "lvgl/lvgl.h"
#include "lvgl/src/display/lv_display_private.h"

#include "src/gui/board_profile.h"
#include "src/system/core/config.h"
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

// The main faces ship as .ttf on some firmwares and .otf on others, and
// FreeType reads both. Each face is therefore a list of candidate names, tried
// in order, and the first one present wins. A default face that is not found
// means an interface with no text in it at all.
#define FACE_FILES(base)                                                                                               \
	{FONT_DIR "/" base ".ttf", FONT_DIR "/" base ".otf", FONT_DIR_FALLBACK "/" base ".ttf",                          \
	 FONT_DIR_FALLBACK "/" base ".otf", NULL}

static const char *const FONT_DEFAULT_FILES[] = FACE_FILES("default");
static const char *const FONT_BOLD_FILES[] = FACE_FILES("bold");

// The scripts MiSans has no letters for, one face each, chained behind the
// main face so they only answer for what it lacks. The bold file is optional:
// the regular one stands in at the bold sizes when it is not there.
typedef struct {
	const char *name; // for the summary
	const char *const regular_files[5];
	const char *const bold_files[5];
	// Letters drawn this much higher than the file places them, in
	// thousandths of the size. See ARABIC_RAISE.
	int raise_per_mille;
	const char *regular; // the files found, or NULL
	const char *bold;
	bool used;
} script_face_t;

// MiSans Arabic sits lower than MiSans. Its tails, and the dots under them,
// reach 0.39 em below the baseline, where a line here has room for 0.28 (the
// main face's descent), and a label cuts off whatever passes its bottom edge:
// the two dots of a final ya were not drawn at all. Raised by this much, the
// deepest of the common letters ends where the line does.
#define ARABIC_RAISE 105

// Rodin, for Japanese. Chained right behind the main face and silent unless
// the interface is in Japanese; see japanese_ui below.
static script_face_t japanese = {"Japanese", FACE_FILES("japanese"), FACE_FILES("japanese-bold"), 0, NULL, NULL,
								 false};

static script_face_t scripts[] = {
	{"Korean", FACE_FILES("korean"), FACE_FILES("korean-bold"), 0, NULL, NULL, false},
	{"Thai", FACE_FILES("thai"), FACE_FILES("thai-bold"), 0, NULL, NULL, false},
	{"Arabic", FACE_FILES("arabic"), FACE_FILES("arabic-bold"), ARABIC_RAISE, NULL, NULL, false},
};

#define SCRIPT_COUNT (sizeof(scripts) / sizeof(scripts[0]))

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
lv_font_t font_ui_20_bold;
lv_font_t font_ui_22;
lv_font_t font_ui_22_bold;
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
	{&font_ui_72, 72, false}, {&font_ui_20_bold, 20, true}, {&font_ui_22_bold, 22, true}, {&font_ui_24_bold, 24, true}, {&font_ui_36_bold, 36, true},
	{&font_ui_64_bold, 64, true},
};

#define UI_FONT_COUNT (sizeof(ui_fonts) / sizeof(ui_fonts[0]))

static char summary[160] = "";

// Settings -> Appearance -> Text size: Large. The small sizes -- the ones
// secondary lines, notes, clocks and counters are set in -- are drawn a step
// or more larger; headings, big numbers and the screensaver's clock stay as
// they are, since those were never hard to read. The objects keep their
// names: font_ui_22 is whatever 22 is drawn at, so every page follows without
// being told.
typedef struct {
	int size;
	int large;
} text_step_t;

static const text_step_t LARGE_STEPS[] = {
	{14, 17}, {16, 19}, {18, 21}, {20, 23}, {22, 25}, {24, 26},
};

static bool large_text;

// The named font objects are used throughout the interface.  Remapping them
// here is both safer and more complete than chasing hundreds of explicit
// &font_ui_24 references: every page keeps its typography hierarchy, while a
// "24" font no longer consumes a tenth of the V1's entire width per word.
static int tempotec_drawn_size(int size, bool large) {
	int compact;
	switch (size) {
	case 14: compact = 14; break;
	case 16: compact = 15; break;
	case 18: compact = 15; break;
	case 20: compact = 16; break;
	case 22: compact = 16; break;
	case 24: compact = 17; break;
	case 26: compact = 18; break;
	case 28: compact = 19; break;
	case 32: compact = 21; break;
	case 36: compact = 23; break;
	case 64: compact = 32; break;
	case 72: compact = 36; break;
	default: compact = size; break;
	}
	// Large text remains useful on the small display, but one restrained step
	// avoids recreating the overflows this profile exists to remove.
	if (large && size <= 36) {
		compact += 1;
	}
	return compact;
}

static int drawn_size(int size, bool large) {
	if (bp_is_tempotec_v1()) {
		return tempotec_drawn_size(size, large);
	}
	if (!large) {
		return size;
	}
	for (size_t i = 0; i < sizeof(LARGE_STEPS) / sizeof(LARGE_STEPS[0]); i++) {
		if (LARGE_STEPS[i].size == size) {
			return LARGE_STEPS[i].large;
		}
	}
	return size;
}

bool fonts_large_text(void) { return large_text; }

// The two sets of fonts, normal and large, each built the first time it is
// needed and then kept: switching back and forth copies one set into the
// objects above and opens nothing. A size the large set does not change is
// the normal set's font, not a second copy of it.
static lv_font_t sets[2][UI_FONT_COUNT];
static bool set_built[2];

static const char *regular_file;
static const char *bold_file;

static lv_font_t *open_face(const char *path, int size) {
	if (access(path, R_OK) != 0) {
		return NULL;
	}
	return lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP, (uint32_t)size,
								   LV_FREETYPE_FONT_STYLE_NORMAL);
}

// LVGL's FreeType lookup, which every face starts with. A raised face goes
// through it and then moves the glyph up by the pixels kept in its user_data.
static bool (*freetype_glyph_dsc)(const lv_font_t *, lv_font_glyph_dsc_t *, uint32_t, uint32_t);

static bool raised_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *dsc, uint32_t letter,
							 uint32_t letter_next) {
	if (!freetype_glyph_dsc(font, dsc, letter, letter_next)) {
		return false;
	}
	dsc->ofs_y += (int32_t)(intptr_t)font->user_data;
	return true;
}

// With the interface in Japanese, kana, kanji and the CJK punctuation and
// full-width forms are drawn from Rodin rather than MiSans, whose kanji are
// the Chinese shapes. The main face then passes on those letters, Rodin
// answers them, and a copy of the main face at the end of the chain covers the
// kanji Rodin does not have. In any other language Rodin passes on everything
// and the chain behaves as if it were not there.
static bool japanese_ui;

static bool is_cjk(uint32_t letter) {
	return (letter >= 0x3000 && letter <= 0x33FF) || // punctuation, kana, enclosed and compatibility forms
		   (letter >= 0x3400 && letter <= 0x4DBF) || // extension A
		   (letter >= 0x4E00 && letter <= 0x9FFF) || // unified ideographs
		   (letter >= 0xF900 && letter <= 0xFAFF) || // compatibility ideographs
		   (letter >= 0xFF00 && letter <= 0xFFEF);	 // full-width and half-width forms
}

static bool main_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *dsc, uint32_t letter, uint32_t letter_next) {
	if (japanese_ui && is_cjk(letter)) {
		return false;
	}
	return freetype_glyph_dsc(font, dsc, letter, letter_next);
}

static bool japanese_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *dsc, uint32_t letter,
							   uint32_t letter_next) {
	if (!japanese_ui) {
		return false;
	}
	return freetype_glyph_dsc(font, dsc, letter, letter_next);
}

static void raise_face(lv_font_t *face, int size, int per_mille) {
	int pixels = (size * per_mille + 500) / 1000;
	if (pixels <= 0) {
		return;
	}
	freetype_glyph_dsc = face->get_glyph_dsc;
	face->user_data = (void *)(intptr_t)pixels;
	face->get_glyph_dsc = raised_glyph_dsc;
}

// One size as the interface draws it: the main face with the other scripts
// chained behind. NULL when the main face cannot be opened.
static lv_font_t *open_chain(const ui_font_t *ui, int size) {
	// The face this size draws from. The bold heading font uses the bold
	// file when the firmware has one; the regular stands in otherwise,
	// which reads fine at heading sizes even if it is not actually heavier.
	lv_font_t *head = NULL;
	if (ui->bold && bold_file) {
		head = open_face(bold_file, size);
	}
	if (!head) {
		head = open_face(regular_file, size);
	}
	if (!head) {
		fprintf(stderr, "fonts: cannot open %s at %dpx\n", regular_file, size);
		return NULL;
	}

	freetype_glyph_dsc = head->get_glyph_dsc;
	lv_font_t *tail = head;

	lv_font_t *rodin = NULL;
	if (ui->bold && japanese.bold) {
		rodin = open_face(japanese.bold, size);
	}
	if (!rodin && japanese.regular) {
		rodin = open_face(japanese.regular, size);
	}
	if (rodin) {
		head->get_glyph_dsc = main_glyph_dsc;
		rodin->get_glyph_dsc = japanese_glyph_dsc;
		tail->fallback = rodin;
		tail = rodin;
		japanese.used = true;
	}

	for (size_t i = 0; i < SCRIPT_COUNT; i++) {
		script_face_t *script = &scripts[i];
		lv_font_t *face = NULL;
		if (ui->bold && script->bold) {
			face = open_face(script->bold, size);
		}
		if (!face && script->regular) {
			face = open_face(script->regular, size);
		}
		if (face) {
			if (script->raise_per_mille) {
				raise_face(face, size, script->raise_per_mille);
			}
			tail->fallback = face;
			tail = face;
			script->used = true;
		}
	}

	// The main face again, answering everything: the kanji Rodin lacks.
	if (rodin) {
		lv_font_t *again = NULL;
		if (ui->bold && bold_file) {
			again = open_face(bold_file, size);
		}
		if (!again) {
			again = open_face(regular_file, size);
		}
		if (again) {
			tail->fallback = again;
		}
	}
	return head;
}

// Fills sets[large]. The heap fonts FreeType creates are kept on purpose:
// their dsc is what the copies draw through.
static bool build_set(bool large) {
	if (set_built[large]) {
		return true;
	}
	for (size_t i = 0; i < UI_FONT_COUNT; i++) {
		const ui_font_t *ui = &ui_fonts[i];
		int size = drawn_size(ui->size, large);
		if (size == ui->size && set_built[!large] && drawn_size(ui->size, !large) == size) {
			sets[large][i] = sets[!large][i];
			continue;
		}
		lv_font_t *head = open_chain(ui, size);
		if (!head) {
			return false;
		}
		sets[large][i] = *head;
	}
	set_built[large] = true;
	return true;
}

bool fonts_init(void) {
	regular_file = first_present(FONT_DEFAULT_FILES);
	bold_file = first_present(FONT_BOLD_FILES);
	for (size_t i = 0; i < SCRIPT_COUNT; i++) {
		scripts[i].regular = first_present(scripts[i].regular_files);
		scripts[i].bold = first_present(scripts[i].bold_files);
	}
	japanese.regular = first_present(japanese.regular_files);
	japanese.bold = first_present(japanese.bold_files);
	japanese_ui = strcmp(lang_current(), FONTS_JAPANESE_LANGUAGE) == 0;

	if (!regular_file) {
		fprintf(stderr, "fonts: no default.ttf or default.otf in %s or %s\n", FONT_DIR, FONT_DIR_FALLBACK);
		return false;
	}

	large_text = config_get_int("ui", "text_size", FONTS_TEXT_NORMAL) == FONTS_TEXT_LARGE;
	if (!build_set(large_text)) {
		return false;
	}
	// Fill the static objects the interface points at.
	for (size_t i = 0; i < UI_FONT_COUNT; i++) {
		*ui_fonts[i].font = sets[large_text][i];
	}

	// Records the actual file names, so the log shows which container this
	// firmware ships.
	int len = snprintf(summary, sizeof(summary), "%s%s%s", basename_of(regular_file), bold_file ? ", " : "",
					   bold_file ? basename_of(bold_file) : "");
	const char *sep = ", ";
	if (japanese.used && len > 0 && (size_t)len < sizeof(summary)) {
		len += snprintf(summary + len, sizeof(summary) - (size_t)len, "%s%s", sep, japanese.name);
		sep = " + ";
	}
	for (size_t i = 0; i < SCRIPT_COUNT && len > 0 && (size_t)len < sizeof(summary); i++) {
		if (scripts[i].used) {
			len += snprintf(summary + len, sizeof(summary) - (size_t)len, "%s%s", sep, scripts[i].name);
			sep = " + ";
		}
	}

	char from[256];
	snprintf(from, sizeof(from), "%.*s", (int)(basename_of(regular_file) - regular_file - 1), regular_file);
	fprintf(stderr, "fonts: %d sizes from %s (%s)%s\n", (int)UI_FONT_COUNT, from, summary,
			large_text ? ", large text" : "");
	return true;
}

// ---------------------------------------------------------------------------
// Changing the size on a running interface
//
// Every label draws through the objects above, so copying the other set into
// them is most of the change: after that each object is told its style has
// changed, and LVGL measures the text again and lays the pages out around it.
//
// What LVGL cannot know about is a height a page worked out from a font when
// it was built -- a label pinned to one line, or capped at two, so that
// LV_LABEL_LONG_DOT cuts it there. Those are found by their value: a label
// whose height, or maximum height, is a whole number of its own font's lines
// (with the line spacing between them) is a label sized that way, and gets the
// same number of lines at the new size. Anything else the pages worked out
// from a font is theirs to redo, from a callback registered below.
// ---------------------------------------------------------------------------

#define FONTS_CHANGE_CALLBACKS 8
static void (*change_callbacks[FONTS_CHANGE_CALLBACKS])(void);
static int change_callback_count;

void fonts_register_change(void (*cb)(void)) {
	if (cb && change_callback_count < FONTS_CHANGE_CALLBACKS) {
		change_callbacks[change_callback_count++] = cb;
	}
}

// `value` again for the new line height, when it is one to three lines of the
// old one. Unchanged otherwise.
static int32_t rescale_lines(int32_t value, int32_t old_line, int32_t new_line, int32_t gap) {
	if (!LV_COORD_IS_PX(value) || old_line <= 0) {
		return value;
	}
	for (int32_t lines = 1; lines <= 3; lines++) {
		if (value == lines * old_line + (lines - 1) * gap) {
			return lines * new_line + (lines - 1) * gap;
		}
	}
	return value;
}

static void rescale_labels(lv_obj_t *obj, const int32_t *old_line) {
	if (lv_obj_check_type(obj, &lv_label_class)) {
		const lv_font_t *font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
		for (size_t i = 0; i < UI_FONT_COUNT; i++) {
			if (ui_fonts[i].font != font) {
				continue;
			}
			int32_t new_line = lv_font_get_line_height(font);
			if (new_line == old_line[i]) {
				break;
			}
			int32_t gap = lv_obj_get_style_text_line_space(obj, LV_PART_MAIN);
			int32_t height = lv_obj_get_style_height(obj, LV_PART_MAIN);
			int32_t scaled = rescale_lines(height, old_line[i], new_line, gap);
			if (scaled != height) {
				lv_obj_set_height(obj, scaled);
			}
			int32_t max_height = lv_obj_get_style_max_height(obj, LV_PART_MAIN);
			scaled = rescale_lines(max_height, old_line[i], new_line, gap);
			if (scaled != max_height) {
				lv_obj_set_style_max_height(obj, scaled, 0);
			}
			break;
		}
	}
	uint32_t count = lv_obj_get_child_count(obj);
	for (uint32_t i = 0; i < count; i++) {
		rescale_labels(lv_obj_get_child(obj, (int32_t)i), old_line);
	}
}

bool fonts_set_large_text(bool large) {
	if (large == large_text) {
		return true;
	}
	if (!build_set(large)) {
		return false;
	}

	int32_t old_line[UI_FONT_COUNT];
	for (size_t i = 0; i < UI_FONT_COUNT; i++) {
		old_line[i] = lv_font_get_line_height(ui_fonts[i].font);
		*ui_fonts[i].font = sets[large][i];
	}
	large_text = large;

	// Every screen the display holds, the top and system layers included:
	// LVGL keeps those in the same list.
	for (lv_display_t *disp = lv_display_get_next(NULL); disp; disp = lv_display_get_next(disp)) {
		for (uint32_t i = 0; i < disp->screen_cnt; i++) {
			rescale_labels(disp->screens[i], old_line);
			lv_obj_refresh_style(disp->screens[i], LV_PART_ANY, LV_STYLE_PROP_ANY);
		}
	}
	for (int i = 0; i < change_callback_count; i++) {
		change_callbacks[i]();
	}
	fprintf(stderr, "fonts: %s text\n", large ? "large" : "normal");
	return true;
}

void fonts_set_language(const char *name) {
	bool japanese_now = name && strcmp(name, FONTS_JAPANESE_LANGUAGE) == 0;
	if (japanese_now == japanese_ui) {
		return;
	}
	japanese_ui = japanese_now;

	// The kanji and kana already on screen change face: every label is
	// measured again.
	for (lv_display_t *disp = lv_display_get_next(NULL); disp; disp = lv_display_get_next(disp)) {
		for (uint32_t i = 0; i < disp->screen_cnt; i++) {
			lv_obj_refresh_style(disp->screens[i], LV_PART_ANY, LV_STYLE_PROP_ANY);
		}
	}
}

// Empty until the faces have been opened, which is before any page is built.
// The stand-in is resolved here rather than stored, so it follows the language
// the same way every other line on the page does.
const char *fonts_summary(void) { return summary[0] ? summary : tr("none"); }

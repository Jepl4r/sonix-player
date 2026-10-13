#ifndef FONTS_H
#define FONTS_H

#include <stdbool.h>

#include "lvgl/lvgl.h"

// The UI fonts -- every size the interface draws with.
//
// There are no fonts inside the binary. Everything is rendered through
// FreeType from the faces shipped in /usr/resource/sonix/fonts:
//
//   default.otf   MiSans Regular -- Latin, Greek, Cyrillic, kana, full CJK
//   bold.otf      optional bold weight for the headings; regular stands in
//                 when it is not there
//   korean.otf    Hangul, and the few Japanese marks MiSans lacks (the
//                 middle dot, the wave dash): a subset of Pretendard
//                 matched to MiSans
//   thai.otf      MiSans Thai
//   arabic.otf    MiSans Arabic
//   japanese.otf  FOT-Rodin, for kana, kanji and CJK punctuation when the
//                 interface is in Japanese
//
// The last four each have a -bold file for the bold sizes, optional like
// bold.otf. fonts_init() opens them once and builds one fallback chain per
// size: default answers first, Korean, Thai and Arabic fill in what it lacks.
// In Japanese, Rodin answers the CJK letters ahead of default (see fonts.c).
// FreeType maps the files -- nothing is decoded up front -- and rendered
// glyphs live in the shared FTC cache (LV_FREETYPE_CACHE_FT_GLYPH_CNT), so
// memory stays flat no matter how much text is on screen.
//
// Arabic is drawn right to left and in its joined forms by LVGL itself
// (LV_USE_BIDI and LV_USE_ARABIC_PERSIAN_CHARS in lv_conf.h); the face only
// has to carry the presentation forms, which MiSans Arabic does.
//
// These are real objects, not pointers, so `&font_ui_24` is a valid
// lv_font_t * at every call site.

extern lv_font_t font_ui_14;
extern lv_font_t font_ui_16;
extern lv_font_t font_ui_17; // 2.0: the mono-ish card subtitle (codec, network)
extern lv_font_t font_ui_18;
extern lv_font_t font_ui_20;
extern lv_font_t font_ui_20_bold; // bold text in the release notes of the update card
extern lv_font_t font_ui_21; // 2.0: a card's subtitle, a sheet's
extern lv_font_t font_ui_22;
extern lv_font_t font_ui_22_bold; // headings in the release notes of the update card
extern lv_font_t font_ui_24;
extern lv_font_t font_ui_24_bold;
extern lv_font_t font_ui_26;
extern lv_font_t font_ui_26_bold; // 2.0: a sheet's title
extern lv_font_t font_ui_28;
extern lv_font_t font_ui_28_bold; // 2.0: the player's and an album page's title
extern lv_font_t font_ui_30_bold; // 2.0: a page title longer than 12 letters
extern lv_font_t font_ui_32;
extern lv_font_t font_ui_34_bold; // 2.0: a page title
// The A and B letters on the Gearboy buttons, nothing else: on a 110 pixel
// circle the regular weight is lost.
extern lv_font_t font_ui_36_bold;
// The letter shown in the middle of the screen while the A-Z list index is in
// use: one letter at a time, and it has to read from a distance.
extern lv_font_t font_ui_64_bold;
extern lv_font_t font_ui_72; // the screensaver's clock, nothing else is this big

// Loads the faces and fills the objects above. Must run after lv_init() (it
// uses LVGL's FreeType binding) and before the first widget is created.
// Returns false when the default face could not be opened at all -- nothing
// can be drawn without it, so the caller should treat that as fatal.
bool fonts_init(void);

// Settings -> Appearance -> Text size, stored as [ui] text_size and read by
// fonts_init().
#define FONTS_TEXT_NORMAL 0
#define FONTS_TEXT_LARGE 1

// Whether the small sizes are drawn larger (see fonts.c).
bool fonts_large_text(void);

// Switches the size on the running interface: the objects above are drawn at
// the other size from the next frame, and every page is measured and laid out
// again. Does not touch the config. False, changing nothing, when the faces
// cannot be opened at the new sizes.
bool fonts_set_large_text(bool large);

// Called after every such switch, for a page that worked something out from a
// font when it was built and has to work it out again.
void fonts_register_change(void (*cb)(void));

// The language file whose interface draws CJK letters from japanese.otf.
#define FONTS_JAPANESE_LANGUAGE "Japanese"

// The interface language is about to become `name` (a language file's name,
// as lang_set() takes it). Call before lang_set(), so the relabelled pages are
// measured with the right faces.
void fonts_set_language(const char *name);

// For the developer options page: which font files are in use, e.g.
// "default.otf, bold.otf, Korean + Thai + Arabic". Never NULL.
const char *fonts_summary(void);

#endif // FONTS_H

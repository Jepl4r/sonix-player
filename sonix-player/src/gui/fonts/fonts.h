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
//   Korean.ttf    Hangul
//   Thai.ttf      Thai
//
// fonts_init() opens them once and builds one fallback chain per size:
// default answers first, Korean and Thai fill in what it lacks. FreeType maps
// the files -- nothing is decoded up front -- and rendered glyphs live in the
// shared FTC cache (LV_FREETYPE_CACHE_FT_GLYPH_CNT), so memory stays flat no
// matter how much text is on screen.
//
// These are real objects, not pointers, so `&font_ui_24` is a valid
// lv_font_t * at every call site.

extern lv_font_t font_ui_14;
extern lv_font_t font_ui_16;
extern lv_font_t font_ui_18;
extern lv_font_t font_ui_20;
extern lv_font_t font_ui_20_bold; // bold text in the release notes of the update card
extern lv_font_t font_ui_22;
extern lv_font_t font_ui_22_bold; // headings in the release notes of the update card
extern lv_font_t font_ui_24;
extern lv_font_t font_ui_24_bold;
extern lv_font_t font_ui_26;
extern lv_font_t font_ui_28;
extern lv_font_t font_ui_32;
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

// For the developer options page: which font files are in use, e.g.
// "default.otf, bold.otf, Korean + Thai". Never NULL.
const char *fonts_summary(void);

#endif // FONTS_H

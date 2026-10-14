#ifndef GUI_SX_PALETTE_H
#define GUI_SX_PALETTE_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

#include "src/gui/shell/sx_tokens.h"

// ---------------------------------------------------------------------------
// Sonix UI 2.0: the palette
//
// The whole interface is coloured from one hue: the cover's of the track
// playing, or, on an album page, that album's. From the hue, the theme (dark
// or light) and two choices on the Appearance page -- the accent ("from the
// cover" or one of six presets) and "Tint from cover" -- this computes the
// twenty-odd colours of the screen, in OKLCH (sx_tokens.h gives each one's
// lightness and chroma), straight into RGB565.
//
// theme.c owns the three choices and pushes the result into the shared styles
// (theme_style_*), which is what restyles every page at once; theme() keeps
// returning the familiar theme_palette_t, now filled from here. Code written
// for 2.0 reads sx_pal() for the colours theme_palette_t has no field for.
//
// The cost: one recompute is about 25 OKLCH conversions (a few cbrt-free
// multiplies and three powf each), tens of microseconds on the device, done
// when the hue actually changes -- a new album, not every track of the same
// one -- and when an album page opens or closes. Nothing runs in between.
//
// UI thread only, like everything that touches LVGL. The tone provider is
// called on the UI thread too and must not block: it is a cache lookup, not a
// decode.
// ---------------------------------------------------------------------------

typedef struct {
	bool dark;	 // the theme
	bool tinted; // "Tint from cover" is on: the neutrals take the hue
	bool accent_from_cover;
	float hue;		// the page's hue: an album page's own while one is pushed
	float play_hue; // the playing track's, which the always-dark surfaces use

	// The page: follows the theme, and the hue while the tint is on.
	lv_color_t bg;
	lv_color_t surface;
	lv_color_t press;
	lv_color_t text;
	lv_color_t text2;
	lv_color_t accent;
	lv_color_t on_accent;  // text and glyphs on an accent fill
	lv_color_t active;	   // 16 % accent into surface: the "on now" plaque (sx_card active)
	lv_color_t swipe_end;  // the underlay of a row pulled left ("add to the end")
	lv_color_t cover_tone; // the plaque a row draws until its thumbnail exists, at the page hue
	lv_color_t star_on;	   // a set star on this theme
	lv_color_t hairline;   // the 1 px line over a sheet or the mini player...
	lv_opa_t hairline_opa; // ...and how strong

	// Always dark, whatever the theme, at the playing track's hue: the player,
	// the mini player.
	lv_color_t np_accent;
	lv_color_t np_ink; // the glyph on the white play disc
	lv_color_t mini_bg;
	lv_color_t mini_cover_tone;
	lv_color_t np_grad_top;
	lv_color_t np_grad_bot;
	lv_color_t np_panel_top;
	lv_color_t np_panel_bot;

	// The album page, at the page hue: its gradient at 0, 380 and 900 px.
	lv_color_t album_grad[3];
} sx_palette_t;

// The colours in force. Never NULL (valid from theme_init() on).
const sx_palette_t *sx_pal(void);

// ---------------------------------------------------------------------------
// Where the hue comes from
// ---------------------------------------------------------------------------

// Answers the hue (0..360) and chroma (OKLCH C, about 0..0.3) of the cover of
// `track_path`, or false when it does not know. Must be quick: called on the
// UI thread at every track change. The cover pipeline plugs its cached tone in
// here; until one is set, or when it says false, the hue is a hash of the
// album's title (or the folder's name), as the handoff asks for covers that
// have no tone yet. NULL restores that default.
typedef bool (*sx_tone_provider_t)(const char *track_path, float *hue_deg, float *chroma);
void sx_palette_set_tone_provider(sx_tone_provider_t provider);

// The playing track changed: work out its hue and, when it moved, recolour.
// `track_path` is the file (NULL for a stream); `name` is what to hash when no
// tone is known -- the album title for a file (NULL: the album tag of the
// loaded track, else the folder's name), the station's name for a stream.
void sx_palette_track_changed(const char *track_path, const char *name);

// Nothing loaded any more: back to the neutral hue the interface starts with.
void sx_palette_track_cleared(void);

// The hue a name hashes to, for a page that needs a tone and has no cover:
// an album without art, an artist's letter plaque, a genre.
float sx_palette_hue_of_name(const char *name);

// An album page colours itself after its own album rather than after the
// track playing: push its hue when it opens and pop when it closes. One level
// deep -- a push replaces an earlier one -- since album pages do not stack on
// top of each other. `chroma` < 0 means "the default".
void sx_palette_push_hue(float hue_deg, float chroma);
void sx_palette_pop_hue(void);

// ---------------------------------------------------------------------------
// Colour arithmetic
// ---------------------------------------------------------------------------

// OKLCH to RGB565 (Bjoern Ottosson's OKLab), gamut-clipped per channel.
uint16_t sx_oklch_to_565(float L, float C, float h_deg);

// The same as an lv_color_t (whose 8-bit channels are the RGB565 ones widened,
// so nothing is lost on the way to the panel).
lv_color_t sx_oklch(float L, float C, float h_deg);

// A token at a hue, with the cover's chroma scale applied (see sx_palette.c).
lv_color_t sx_tone(sx_lc_t lc, float h_deg);

// The accent "from the cover" would be now, whichever accent is chosen: for
// the swatch that offers it.
lv_color_t sx_palette_cover_accent(void);
lv_color_t sx_palette_cover_on_accent(void);

// `pct` percent of `a` over `b`.
lv_color_t sx_mix(lv_color_t a, lv_color_t b, uint8_t pct);

// A hex token (0xRRGGBB) as a colour.
static inline lv_color_t sx_hex(uint32_t rgb) { return lv_color_hex(rgb); }

// ---------------------------------------------------------------------------
// The dithered vertical gradient
//
// The one gradient the platform affords (handoff, "Платформенные
// ограничения"): vertical, on a static page only, rendered once per track into
// a narrow strip and tiled across the width as the page's background image:
//
//     lv_obj_set_style_bg_image_src(page, strip, 0);
//     lv_obj_set_style_bg_image_tiled(page, true, 0);
//
// RGB565 with 4 x 4 ordered dithering, so the steps of a dark gradient do not
// band. The handoff's 1 x H works (w = 1 dithers along the column only); a few
// pixels of width give the dither its second dimension for 2 x w x h bytes --
// 4 x 800 is 6.4 KB -- and cost nothing more to draw than w = 1 would after
// the first row of tiles. `stops` are colours at y positions, ascending; the
// colour is held past the first and the last. NULL when out of memory.
// ---------------------------------------------------------------------------

typedef struct {
	lv_color_t color;
	int32_t y;
} sx_grad_stop_t;

lv_draw_buf_t *sx_gradient_create(int32_t w, int32_t h, const sx_grad_stop_t *stops, int count);
void sx_gradient_destroy(lv_draw_buf_t *strip);

// The strip on an object with rounded corners. Not as its background image:
// LVGL draws a tiled image under a corner radius line by line through a
// radius mask, over every tile of the WHOLE object for every frame that
// touches any of it -- the player's panel spent 10,000 masked lines on each
// step of the title scrolling across 18,000 of its pixels, four times what
// the title itself cost. This draws the strip from the object's own draw
// event, only over the part of the object being redrawn, through the mask
// only in the bands of the corners and with plain copies everywhere else.
// `slot` points at the caller's strip pointer, read at every draw, so a new
// strip needs no call here, only lv_obj_invalidate(obj); a NULL strip draws
// nothing, and so does a transparent bg_opa or bg_image_opa. Set the object's
// bg_color as usual (what the strip is drawn on). UI thread only.
void sx_gradient_paint(lv_obj_t *obj, lv_draw_buf_t *const *slot);

// ---------------------------------------------------------------------------
// For theme.c only
// ---------------------------------------------------------------------------

// The three Appearance choices. `accent` is a preset index (0..5) or -1 for
// "from the cover". Recomputes; does not notify.
void sx_palette_configure(bool dark, int accent, lv_color_t preset, bool tint);

// Called after the palette moved because the hue did (a track change, an
// album page): theme.c pushes it into the shared styles and repaints.
void sx_palette_set_change_hook(void (*hook)(void));

#endif /* GUI_SX_PALETTE_H */

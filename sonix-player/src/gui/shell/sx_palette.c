#include "sx_palette.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "src/system/playback/device_state.h"

// The hue the interface wears before anything has played, and after the queue
// is gone: a cool blue, the nearest thing in the cover palette to the blue
// accent the player had before 2.0, so a first boot still looks like Sonix.
#define DEFAULT_HUE 255.0f

// The chroma the tokens were drawn for. A cover's own chroma scales them: a
// black-and-white sleeve gives a nearly grey page instead of an invented
// colour, a loud one does not shout louder than the design allows.
#define REFERENCE_CHROMA 0.10f
#define MIN_CHROMA_SCALE 0.20f

static sx_palette_t pal;

// The Appearance choices, as theme.c last handed them over.
static bool cfg_dark = true;
static int cfg_accent = -1; // -1: from the cover
static lv_color_t cfg_preset;
static bool cfg_tint = true;

// The playing track's tone, and an album page's while it is pushed.
static float play_hue = DEFAULT_HUE;
static float play_chroma = REFERENCE_CHROMA;
static bool pushed;
static float pushed_hue;
static float pushed_chroma;

// The chroma scale of the hue being drawn, applied by sx_tone().
static float chroma_scale = 1.0f;

static sx_tone_provider_t tone_provider;
static void (*change_hook)(void);

const sx_palette_t *sx_pal(void) { return &pal; }

// ---------------------------------------------------------------------------
// OKLCH
// ---------------------------------------------------------------------------

static float srgb_gamma(float x) {
	if (x <= 0.0031308f) {
		return 12.92f * x;
	}
	return 1.055f * powf(x, 1.0f / 2.4f) - 0.055f;
}

static float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

// OKLCH to gamma-encoded sRGB in 0..1, clipped channel by channel as the
// handoff does: the tokens stay inside the gamut for every hue except the
// palest accents, where a clipped channel is invisible.
static void oklch_to_srgb(float L, float C, float h_deg, float rgb[3]) {
	float h = h_deg * 0.01745329252f;
	float a = C * cosf(h);
	float b = C * sinf(h);

	float l_ = L + 0.3963377774f * a + 0.2158037573f * b;
	float m_ = L - 0.1055613458f * a - 0.0638541728f * b;
	float s_ = L - 0.0894841775f * a - 1.2914855480f * b;

	float l = l_ * l_ * l_;
	float m = m_ * m_ * m_;
	float s = s_ * s_ * s_;

	rgb[0] = clamp01(srgb_gamma(4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s));
	rgb[1] = clamp01(srgb_gamma(-1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s));
	rgb[2] = clamp01(srgb_gamma(-0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s));
}

uint16_t sx_oklch_to_565(float L, float C, float h_deg) {
	float rgb[3];
	oklch_to_srgb(L, C, h_deg, rgb);
	return (uint16_t)(((uint16_t)(rgb[0] * 31.0f + 0.5f) << 11) | ((uint16_t)(rgb[1] * 63.0f + 0.5f) << 5) |
					  (uint16_t)(rgb[2] * 31.0f + 0.5f));
}

// Widened from RGB565 rather than rounded from the float: what is on screen is
// the 565 value, and two colours that look the same on the panel then compare
// equal here too.
lv_color_t sx_oklch(float L, float C, float h_deg) {
	uint16_t c = sx_oklch_to_565(L, C, h_deg);
	uint8_t r5 = (uint8_t)(c >> 11), g6 = (uint8_t)((c >> 5) & 0x3f), b5 = (uint8_t)(c & 0x1f);
	return lv_color_make((uint8_t)((r5 << 3) | (r5 >> 2)), (uint8_t)((g6 << 2) | (g6 >> 4)),
						 (uint8_t)((b5 << 3) | (b5 >> 2)));
}

lv_color_t sx_tone(sx_lc_t lc, float h_deg) { return sx_oklch(lc.L, lc.C * chroma_scale, h_deg); }

lv_color_t sx_palette_cover_accent(void) { return sx_tone(cfg_dark ? SX_T_D_ACCENT : SX_T_L_ACCENT, pal.hue); }

lv_color_t sx_palette_cover_on_accent(void) {
	return cfg_dark ? sx_tone(SX_T_D_ON_ACC, pal.hue) : lv_color_white();
}

lv_color_t sx_mix(lv_color_t a, lv_color_t b, uint8_t pct) {
	if (pct > 100) {
		pct = 100;
	}
	return lv_color_mix(a, b, (uint8_t)((pct * 255 + 50) / 100));
}

// ---------------------------------------------------------------------------
// The palette
// ---------------------------------------------------------------------------

static float scale_for(float chroma) {
	if (chroma < 0.0f) {
		return 1.0f;
	}
	float s = chroma / REFERENCE_CHROMA;
	if (s < MIN_CHROMA_SCALE) {
		s = MIN_CHROMA_SCALE;
	}
	return s > 1.0f ? 1.0f : s;
}

static void recompute(void) {
	float hue = pushed ? pushed_hue : play_hue;
	float page_scale = scale_for(pushed ? pushed_chroma : play_chroma);
	float play_scale = scale_for(play_chroma);

	pal.dark = cfg_dark;
	pal.tinted = cfg_tint;
	pal.accent_from_cover = cfg_accent < 0;
	pal.hue = hue;
	pal.play_hue = play_hue;

	chroma_scale = page_scale;

	// The neutrals: the cover's hue at the token's lightness, or Adwaita's
	// greys with the tint off.
	if (cfg_tint) {
		if (cfg_dark) {
			pal.bg = sx_tone(SX_T_D_BG, hue);
			pal.surface = sx_tone(SX_T_D_SURFACE, hue);
			pal.press = sx_tone(SX_T_D_PRESS, hue);
			pal.text = sx_hex(SX_TEXT_ON_DARK);
			pal.text2 = sx_tone(SX_T_D_TEXT2, hue);
		} else {
			pal.bg = sx_tone(SX_T_L_BG, hue);
			pal.surface = sx_hex(SX_L_SURFACE);
			pal.press = sx_tone(SX_T_L_PRESS, hue);
			pal.text = sx_hex(SX_TEXT_ON_LIGHT);
			pal.text2 = sx_tone(SX_T_L_TEXT2, hue);
		}
	} else if (cfg_dark) {
		pal.bg = sx_hex(SX_D_BG);
		pal.surface = sx_hex(SX_D_SURFACE);
		pal.press = sx_hex(SX_D_PRESS);
		pal.text = sx_hex(SX_D_TEXT);
		pal.text2 = sx_hex(SX_D_TEXT2);
	} else {
		pal.bg = sx_hex(SX_L_BG);
		pal.surface = sx_hex(SX_L_SURFACE);
		pal.press = sx_hex(SX_L_PRESS);
		pal.text = sx_hex(SX_L_TEXT);
		pal.text2 = sx_hex(SX_L_TEXT2);
	}

	// The accent comes from the cover whether or not the neutrals do: the
	// tint is about the backgrounds, the accent choice about the highlights.
	if (cfg_accent < 0) {
		pal.accent = sx_tone(cfg_dark ? SX_T_D_ACCENT : SX_T_L_ACCENT, hue);
		pal.on_accent = cfg_dark ? sx_tone(SX_T_D_ON_ACC, hue) : lv_color_white();
	} else {
		pal.accent = cfg_preset;
		// Yellow is the one preset white text does not read on.
		pal.on_accent = lv_color_eq(cfg_preset, sx_hex(SX_ACC_YELLOW)) ? sx_hex(SX_ACC_YELLOW_INK) : lv_color_white();
	}

	pal.active = sx_mix(pal.accent, pal.surface, 16);
	pal.swipe_end = sx_tone(cfg_dark ? SX_T_D_SWIPE_L : SX_T_L_SWIPE_L, hue);
	pal.cover_tone = sx_tone(SX_T_COVER_TONE, hue);
	pal.star_on = sx_hex(cfg_dark ? SX_STAR_ON_DARK : SX_STAR_ON_LIGHT);
	pal.hairline = cfg_dark ? lv_color_white() : lv_color_black();
	pal.hairline_opa = cfg_dark ? SX_HAIRLINE_DARK_OPA : SX_HAIRLINE_LIGHT_OPA;

	pal.album_grad[0] = sx_tone(SX_T_AL_GRAD_0, hue);
	pal.album_grad[1] = sx_tone(SX_T_AL_GRAD_1, hue);
	pal.album_grad[2] = sx_tone(SX_T_AL_GRAD_2, hue);

	// The always-dark surfaces belong to the track playing, not to the page:
	// the mini player over an album page still wears the song it plays.
	chroma_scale = play_scale;
	pal.np_accent = cfg_accent < 0 ? sx_tone(SX_T_NP_ACCENT, play_hue) : cfg_preset;
	pal.np_ink = sx_tone(SX_T_NP_INK, play_hue);
	pal.mini_bg = sx_tone(SX_T_MINI_BG, play_hue);
	pal.mini_cover_tone = sx_tone(SX_T_COVER_TONE, play_hue);
	pal.np_grad_top = sx_tone(SX_T_NP_GRAD_TOP, play_hue);
	pal.np_grad_bot = sx_tone(SX_T_NP_GRAD_BOT, play_hue);
	pal.np_panel_top = sx_tone(SX_T_NP_PANEL_T, play_hue);
	pal.np_panel_bot = sx_tone(SX_T_NP_PANEL_B, play_hue);

	// sx_tone() as called from outside works at the page's scale.
	chroma_scale = page_scale;
}

void sx_palette_configure(bool dark, int accent, lv_color_t preset, bool tint) {
	cfg_dark = dark;
	cfg_accent = accent;
	cfg_preset = preset;
	cfg_tint = tint;
	recompute();
}

void sx_palette_set_change_hook(void (*hook)(void)) { change_hook = hook; }

// Recomputes and tells theme.c, but only when what is drawn would change: a
// hue a fraction of a degree away is the same 565 colours, and every repaint
// costs a frame.
static void hue_moved(void) {
	sx_palette_t before = pal;
	recompute();
	if (memcmp(&before, &pal, sizeof(pal)) == 0) {
		return;
	}
	if (change_hook) {
		change_hook();
	}
}

// ---------------------------------------------------------------------------
// The hue of a track
// ---------------------------------------------------------------------------

// FNV-1a: stable across builds and platforms, so an album keeps its colour
// from one firmware to the next and on the device as in the emulator.
float sx_palette_hue_of_name(const char *name) {
	uint32_t h = 2166136261u;
	for (const unsigned char *p = (const unsigned char *)(name ? name : ""); *p; p++) {
		h ^= *p;
		h *= 16777619u;
	}
	// The low bits of FNV are its weakest; fold the high half in first.
	h ^= h >> 16;
	return (float)(h % 360u);
}

// The default: no cover tone known, so the album's title is hashed -- the
// album tag of the loaded track when this is it, the folder's name otherwise
// (the folder is the album on a card laid out the usual way).
static bool default_tone(const char *track_path, float *hue, float *chroma) {
	if (!track_path || !track_path[0]) {
		return false;
	}
	const char *loaded = NULL;
	const song_metadata_t *m = device_state_loaded_metadata(&loaded);
	if (m && loaded && strcmp(loaded, track_path) == 0 && m->album[0]) {
		*hue = sx_palette_hue_of_name(m->album);
		*chroma = REFERENCE_CHROMA;
		return true;
	}

	const char *slash = strrchr(track_path, '/');
	if (!slash || slash == track_path) {
		*hue = sx_palette_hue_of_name(track_path);
	} else {
		char folder[256];
		size_t len = (size_t)(slash - track_path);
		const char *start = track_path;
		const char *prev = NULL;
		for (const char *p = track_path; p < slash; p++) {
			if (*p == '/') {
				prev = p;
			}
		}
		if (prev) {
			start = prev + 1;
			len = (size_t)(slash - start);
		}
		if (len >= sizeof(folder)) {
			len = sizeof(folder) - 1;
		}
		memcpy(folder, start, len);
		folder[len] = '\0';
		*hue = sx_palette_hue_of_name(folder);
	}
	*chroma = REFERENCE_CHROMA;
	return true;
}

void sx_palette_set_tone_provider(sx_tone_provider_t provider) { tone_provider = provider; }

void sx_palette_track_changed(const char *track_path, const char *name) {
	float hue = DEFAULT_HUE;
	float chroma = REFERENCE_CHROMA;
	bool known = false;

	if (track_path && track_path[0] && tone_provider) {
		known = tone_provider(track_path, &hue, &chroma);
	}
	if (!known && name && name[0]) {
		hue = sx_palette_hue_of_name(name);
		chroma = REFERENCE_CHROMA;
		known = true;
	}
	if (!known) {
		known = default_tone(track_path, &hue, &chroma);
	}
	if (!known) {
		hue = DEFAULT_HUE;
		chroma = REFERENCE_CHROMA;
	}

	if (hue < 0.0f || hue >= 360.0f) {
		hue = fmodf(fmodf(hue, 360.0f) + 360.0f, 360.0f);
	}
	play_hue = hue;
	play_chroma = chroma;
	hue_moved();
}

void sx_palette_track_cleared(void) {
	play_hue = DEFAULT_HUE;
	play_chroma = REFERENCE_CHROMA;
	hue_moved();
}

void sx_palette_push_hue(float hue_deg, float chroma) {
	pushed = true;
	pushed_hue = fmodf(fmodf(hue_deg, 360.0f) + 360.0f, 360.0f);
	pushed_chroma = chroma < 0.0f ? REFERENCE_CHROMA : chroma;
	hue_moved();
}

void sx_palette_pop_hue(void) {
	if (!pushed) {
		return;
	}
	pushed = false;
	hue_moved();
}

// ---------------------------------------------------------------------------
// The dithered gradient
// ---------------------------------------------------------------------------

// The 4 x 4 Bayer matrix, as thresholds in sixteenths.
static const uint8_t BAYER4[4][4] = {
	{0, 8, 2, 10},
	{12, 4, 14, 6},
	{3, 11, 1, 9},
	{15, 7, 13, 5},
};

// One channel, from 0..255 to `levels - 1` steps, the fraction pushed over the
// next step where the matrix says so.
static inline uint16_t dither_channel(float v, int levels_minus_one, uint8_t threshold) {
	float scaled = v * (float)levels_minus_one / 255.0f;
	int base = (int)scaled;
	float frac = scaled - (float)base;
	if (frac * 16.0f > (float)threshold + 0.5f && base < levels_minus_one) {
		base++;
	}
	return (uint16_t)base;
}

lv_draw_buf_t *sx_gradient_create(int32_t w, int32_t h, const sx_grad_stop_t *stops, int count) {
	if (w <= 0 || h <= 0 || !stops || count <= 0) {
		return NULL;
	}
	lv_draw_buf_t *buf = lv_draw_buf_create((uint32_t)w, (uint32_t)h, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
	if (!buf) {
		return NULL;
	}

	int stop = 0;
	for (int32_t y = 0; y < h; y++) {
		// The two stops around this row.
		while (stop + 1 < count && stops[stop + 1].y <= y) {
			stop++;
		}
		float rgb[3];
		const sx_grad_stop_t *a = &stops[stop];
		if (y <= stops[0].y || stop + 1 >= count) {
			const sx_grad_stop_t *only = y <= stops[0].y ? &stops[0] : a;
			rgb[0] = only->color.red;
			rgb[1] = only->color.green;
			rgb[2] = only->color.blue;
		} else {
			const sx_grad_stop_t *b = &stops[stop + 1];
			float t = (float)(y - a->y) / (float)(b->y - a->y);
			rgb[0] = a->color.red + (b->color.red - a->color.red) * t;
			rgb[1] = a->color.green + (b->color.green - a->color.green) * t;
			rgb[2] = a->color.blue + (b->color.blue - a->color.blue) * t;
		}

		uint16_t *row = (uint16_t *)(buf->data + (size_t)y * buf->header.stride);
		for (int32_t x = 0; x < w; x++) {
			uint8_t th = BAYER4[y & 3][x & 3];
			uint16_t r = dither_channel(rgb[0], 31, th);
			uint16_t g = dither_channel(rgb[1], 63, th);
			uint16_t b = dither_channel(rgb[2], 31, th);
			row[x] = (uint16_t)((r << 11) | (g << 5) | b);
		}
	}
	return buf;
}

void sx_gradient_destroy(lv_draw_buf_t *strip) {
	if (strip) {
		lv_draw_buf_destroy(strip);
	}
}

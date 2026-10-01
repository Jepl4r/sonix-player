#include "flappybird.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl/lvgl.h"
#include "lvgl/src/misc/cache/instance/lv_image_cache.h"

#include "src/gui/flappybird/flappysound.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/topbar.h"
#include "src/gui/shell/uiscale.h"
#include "src/gui/shell/volume_overlay.h"
#include "src/system/core/config.h"
#include "src/system/core/respath.h"
#include "src/system/device/power.h"
#include "src/system/image/stb_image_decl.h"

lv_obj_t *flappybird_screen;

// ---------------------------------------------------------------------------
// the world
// ---------------------------------------------------------------------------
//
// The game runs in the artwork's own pixels, 60 steps a second, and every
// drawing is shown at `scale` times its size -- four on a 480-wide panel,
// three on the original R3's 360 -- drawn straight into `frame`. The
// background and the ground are kept already widened `scale` times
// (`bg_wide`, `ground_wide`), so a row of either is one or two memcpy.

#define ART_DIR SONIX_RESOURCE_DIR "/gui/flappybird"
#define ART_MAX_BYTES (256 * 1024)

// ui_px(4), set by flappybird_init: 4 at 1:1, 3 on the R3 (three
// quarters). Never more than MAX_SCALE, which sizes the buffers.
#define MAX_SCALE 4
static int scale = MAX_SCALE;

// A length laid out for four times, at `scale` times.
static int art_px(int v) { return (v * scale + 2) / 4; }

#define STEP_US 16667
#define MAX_STEPS_PER_TICK 4
// A gap between two ticks from which the steps it held are made up.
#define CATCH_UP_US 50000
// The tick's own period. Each tick runs the steps its time is worth, rounded
// to the nearest, and puts the frame on the panel before returning.
#define TICK_MS 16
// A gap between two ticks this long means the panel was dark: a game in
// progress waits for a tap instead of carrying on.
#define STALL_US 250000

#define BG_W 144
#define BG_H 256
#define GROUND_W 168
#define GROUND_H 56

// The game itself runs in units of half an artwork pixel -- the 288x512
// coordinates of the original, whose physics this reproduces -- and a unit is
// scale / 2 panel pixels (u_px), one and a half on the R3.

// Units to panel pixels, rounded down, so a pipe moves the same number of
// pixels every step on either side of the left edge.
static int u_px(int u) {
	int v = u * scale;
	return v >= 0 ? v / 2 : -((1 - v) / 2);
}

static float u_pxf(float u) { return u * scale / 2.0f; }

static int px_u(int px) { return px * 2 / scale; }

// Units a step for the ground and the pipes; panel pixels a step for the
// background, a quarter of the ground's speed.
#define WORLD_SPEED 2
#define BG_PX_PER_STEP 1

#define PIPE_W 26 // artwork pixels
#define PIPE_H 160
#define PIPE_W_U (PIPE_W * 2)
#define PIPE_H_U (PIPE_H * 2)
#define PIPE_GAP_U 96
#define PIPE_SPACING_U 157
#define PIPE_COUNT 3
#define FIRST_PIPE_AHEAD_U 120
// The opening stays this far from the top of the screen and from the ground,
// and moves at least PIPE_MIN_CHANGE_U from one pipe to the next.
#define PIPE_MARGIN_U 32
#define PIPE_MIN_CHANGE_U 24

// The bird is a BIRD_BOX_U square hit box with its left edge at BIRD_LEFT_U,
// and the drawing centred on it.
#define BIRD_W 17 // artwork pixels
#define BIRD_H 12
#define BIRD_BOX_U 20
#define BIRD_LEFT_U 62

// Per step, in units. The height is a whole number moved by the speed
// truncated toward zero, and a flap takes only with the top of the box on the
// screen. With the whole drawing above the screen the bird is lost.
#define GRAVITY 0.3f
#define FLAP_VELOCITY -5.0f
#define MAX_FALL 8.0f
#define LOST_ABOVE_U (-(BIRD_BOX_U / 2 + BIRD_H))

// Degrees, positive with the beak down. A flap sets the turning speed to
// ROT_FLAP_RATE, which grows by ROT_ACCEL a step; the angle stays within
// ROT_MIN..ROT_MAX.
#define ROT_FLAP_RATE -10.0f
#define ROT_ACCEL 0.4f
#define ROT_MIN -20.0f
#define ROT_MAX 90.0f

// The wings go 1-2-3-2 on an animation clock of ANIM_MS_PER_STEP a step: one
// drawing every IDLE_FRAME_MS while waiting, and after a flap FLAP_FRAMES
// drawings of FLAP_FRAME_MS each, then the middle drawing until the next flap.
#define ANIM_MS_PER_STEP 15.0f
#define IDLE_FRAME_MS 100.0f
#define FLAP_FRAME_MS (1000.0f / 30)
#define FLAP_FRAMES 12

// While waiting the bird bobs BOB_U units up and down, BOB_DEG_STEP degrees of
// a sine a step.
#define BOB_U 4.0f
#define BOB_DEG_STEP 8

#define MEDAL_BRONZE_AT 10
#define MEDAL_SILVER_AT 20
#define MEDAL_GOLD_AT 30

// The white flash of a hit, in steps. Then, counted from the bird coming to
// rest on the ground: Game Over, the board rising, the score counting up.
#define FLASH_STEPS 12
#define FLASH_OPA 220
#define OVER_LABEL_AT 24
#define OVER_LABEL_STEPS 12
#define OVER_BOARD_AT 54
#define OVER_BOARD_STEPS 24
#define OVER_COUNT_AT (OVER_BOARD_AT + OVER_BOARD_STEPS)
#define OVER_COUNT_STEPS 45
#define OVER_DONE_AT (OVER_COUNT_AT + OVER_COUNT_STEPS)

// Half of a fade through black between two screens of the game.
#define FADE_STEPS 10

// Screen positions, in panel pixels for the 720-row panel at four times, and
// scaled with the artwork (art_px) rather than the interface: the sprites only
// take whole multiples, so the artwork's scale need not be the interface's
// (both are three quarters on the R3). On a taller
// panel the menus and the labels move down by half the extra rows, on a
// shorter one up by three quarters of the missing rows (layout_shift): half
// leaves Get Ready over the bird on the R3, all of them puts Game Over above
// the top.
#define LAYOUT_H art_px(720)
#define TITLE_LOGO_Y art_px(96)
#define TITLE_PLAY_ABOVE_GROUND art_px(20)
#define CORNER_MARGIN art_px(24)
#define SCORE_Y art_px(40)
#define READY_LABEL_Y art_px(100)
#define HOW_TO_ABOVE_GROUND art_px(16)
#define READY_BIRD_ABOVE_HOW_TO art_px(34)
#define OVER_LABEL_Y art_px(60)
#define OVER_BOARD_Y art_px(170)
#define OVER_BUTTONS_GAP art_px(24)
#define BUTTON_PRESS_SHIFT scale

// Artwork pixels between two digits: the running score's overlap by one, the
// board's stand one apart.
#define DIGIT_GAP_BIG -1
#define DIGIT_GAP_SMALL 1

// Where things sit on the score board, in its own pixels.
#define BOARD_MEDAL_X 12
#define BOARD_MEDAL_Y 21
#define BOARD_DIGITS_RIGHT 103
#define BOARD_SCORE_Y 17
#define BOARD_BEST_Y 38
#define BOARD_BEST_LABEL_X 85 // the printed BEST: left edge, top row, height
#define BOARD_BEST_LABEL_Y 30
#define BOARD_BEST_LABEL_H 6
#define BOARD_NEW_GAP 3

// ---------------------------------------------------------------------------
// the artwork
// ---------------------------------------------------------------------------

// RGB565 pixels and a mask: every PNG in the set is either fully opaque or
// fully transparent per pixel. `mask` is NULL for a drawing with no hole.
// `wide`, for the drawings put on every frame, holds the rows already
// widened scale times (sprite_keep_wide).
typedef struct {
	int w, h;
	uint16_t *rgb;
	uint8_t *mask;
	uint16_t *wide;
} sprite_t;

enum { BIRD_YELLOW, BIRD_RED, BIRD_BLUE, BIRD_GREEN, BIRD_PEACH, BIRD_PURPLE, BIRD_COW, BIRD_RARE, BIRD_COLOURS };
enum { MEDAL_BRONZE, MEDAL_SILVER, MEDAL_GOLD, MEDALS };

static const char *const BIRD_NAMES[BIRD_COLOURS] = {
	"yellow", "red", "blue", "green", "peach", "purple", "cow", "rare",
};

// How often each bird comes out, out of the sum: the cow is rare, the rare
// one rarer.
static const int BIRD_WEIGHTS[BIRD_COLOURS] = {10, 10, 10, 10, 10, 10, 3, 1};

static const char *const MEDAL_NAMES[MEDALS] = {"bronze", "silver", "gold"};

static sprite_t art_bg[2]; // day, night
static sprite_t art_ground;
static sprite_t art_pipe_upper; // pipe-up.png: hangs from the top, mouth down
static sprite_t art_pipe_lower; // pipe-down.png: stands on the ground, mouth up
static sprite_t art_bird[BIRD_COLOURS][3];
static sprite_t art_digit_big[10];	 // score-game-N: the score while playing
static sprite_t art_digit_small[10]; // score-N: the score board
static sprite_t art_medal[MEDALS];
static sprite_t art_logo, art_get_ready, art_how_to, art_game_over, art_board, art_new;
static sprite_t art_play, art_menu, art_exit, art_sound_on, art_sound_off;

static uint16_t pack565(const uint8_t *px) {
	return (uint16_t)(((px[0] & 0xF8) << 8) | ((px[1] & 0xFC) << 3) | (px[2] >> 3));
}

static void sprite_free(sprite_t *s) {
	free(s->rgb);
	free(s->mask);
	free(s->wide);
	memset(s, 0, sizeof(*s));
}

static bool sprite_load(sprite_t *s, const char *name) {
	char path[256];
	snprintf(path, sizeof(path), ART_DIR "/%s.png", name);

	FILE *f = fopen(path, "rb");
	if (!f) {
		printf("flappybird: %s: %s\n", path, strerror(errno));
		return false;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *file = size > 0 && size <= ART_MAX_BYTES ? malloc((size_t)size) : NULL;
	if (!file || fread(file, 1, (size_t)size, f) != (size_t)size) {
		printf("flappybird: %s cannot be read\n", path);
		free(file);
		fclose(f);
		return false;
	}
	fclose(f);

	int w = 0, h = 0, channels = 0;
	uint8_t *rgba = stbi_load_from_memory(file, (int)size, &w, &h, &channels, 4);
	free(file);
	if (!rgba || w <= 0 || h <= 0) {
		printf("flappybird: %s cannot be decoded\n", path);
		stbi_image_free(rgba);
		return false;
	}

	size_t n = (size_t)w * (size_t)h;
	s->rgb = malloc(n * sizeof(uint16_t));
	s->mask = malloc(n);
	if (!s->rgb || !s->mask) {
		stbi_image_free(rgba);
		sprite_free(s);
		return false;
	}
	bool opaque = true;
	for (size_t i = 0; i < n; i++) {
		s->rgb[i] = pack565(rgba + i * 4);
		s->mask[i] = rgba[i * 4 + 3] >= 128;
		opaque = opaque && s->mask[i];
	}
	stbi_image_free(rgba);
	if (opaque) {
		free(s->mask);
		s->mask = NULL;
	}
	s->w = w;
	s->h = h;
	return true;
}

// At four, pixel pairs, so each artwork pixel goes in as two 32-bit stores.
static void widen_row(uint16_t *dst, const uint16_t *src, int n) {
	if (scale == 4) {
		uint32_t *d = (uint32_t *)dst;
		for (int x = 0; x < n; x++) {
			uint32_t pair = (uint32_t)src[x] | ((uint32_t)src[x] << 16);
			d[2 * x] = pair;
			d[2 * x + 1] = pair;
		}
		return;
	}
	for (int x = 0; x < n; x++) {
		for (int k = 0; k < scale; k++) {
			*dst++ = src[x];
		}
	}
}

static bool sprite_keep_wide(sprite_t *s) {
	s->wide = malloc((size_t)s->w * s->h * scale * sizeof(uint16_t));
	if (!s->wide) {
		return false;
	}
	for (int y = 0; y < s->h; y++) {
		widen_row(s->wide + (size_t)y * s->w * scale, s->rgb + (size_t)y * s->w, s->w);
	}
	return true;
}

static void art_free(void) {
	for (int i = 0; i < 2; i++) {
		sprite_free(&art_bg[i]);
	}
	sprite_free(&art_ground);
	sprite_free(&art_pipe_upper);
	sprite_free(&art_pipe_lower);
	for (int c = 0; c < BIRD_COLOURS; c++) {
		for (int i = 0; i < 3; i++) {
			sprite_free(&art_bird[c][i]);
		}
	}
	for (int i = 0; i < 10; i++) {
		sprite_free(&art_digit_big[i]);
		sprite_free(&art_digit_small[i]);
	}
	for (int i = 0; i < MEDALS; i++) {
		sprite_free(&art_medal[i]);
	}
	sprite_free(&art_logo);
	sprite_free(&art_get_ready);
	sprite_free(&art_how_to);
	sprite_free(&art_game_over);
	sprite_free(&art_board);
	sprite_free(&art_new);
	sprite_free(&art_play);
	sprite_free(&art_menu);
	sprite_free(&art_exit);
	sprite_free(&art_sound_on);
	sprite_free(&art_sound_off);
}

// Every file, or false at the first one missing. The drawing code relies on
// the sizes of the background, the ground, the pipes and the bird, so those
// are checked too.
static bool art_load(void) {
	char name[48];
	bool ok = sprite_load(&art_bg[0], "BG-day") && sprite_load(&art_bg[1], "BG-night") &&
			  sprite_load(&art_ground, "ground") && sprite_load(&art_pipe_upper, "pipe-up") &&
			  sprite_load(&art_pipe_lower, "pipe-down") && sprite_load(&art_logo, "logo") &&
			  sprite_load(&art_get_ready, "get-ready") && sprite_load(&art_how_to, "how-to") &&
			  sprite_load(&art_game_over, "game-over") && sprite_load(&art_board, "score-board") &&
			  sprite_load(&art_new, "new-record-label") && sprite_load(&art_play, "play-button") &&
			  sprite_load(&art_menu, "menu") && sprite_load(&art_exit, "exit-button") &&
			  sprite_load(&art_sound_on, "sound-on") && sprite_load(&art_sound_off, "sound-off");

	for (int c = 0; ok && c < BIRD_COLOURS; c++) {
		for (int i = 0; ok && i < 3; i++) {
			snprintf(name, sizeof(name), "bird-%s-%02d", BIRD_NAMES[c], i + 1);
			ok = sprite_load(&art_bird[c][i], name) && art_bird[c][i].w == BIRD_W && art_bird[c][i].h == BIRD_H;
		}
	}
	for (int i = 0; ok && i < 10; i++) {
		snprintf(name, sizeof(name), "score-game-%d", i);
		ok = sprite_load(&art_digit_big[i], name);
		snprintf(name, sizeof(name), "score-%d", i);
		ok = ok && sprite_load(&art_digit_small[i], name);
	}
	for (int i = 0; ok && i < MEDALS; i++) {
		snprintf(name, sizeof(name), "medal-%s", MEDAL_NAMES[i]);
		ok = sprite_load(&art_medal[i], name);
	}

	if (ok) {
		ok = sprite_keep_wide(&art_pipe_upper) && sprite_keep_wide(&art_pipe_lower) &&
			 sprite_keep_wide(&art_get_ready) && sprite_keep_wide(&art_how_to);
		for (int i = 0; ok && i < 10; i++) {
			ok = sprite_keep_wide(&art_digit_big[i]);
		}
	}
	if (ok) {
		ok = art_bg[0].w == BG_W && art_bg[0].h == BG_H && art_bg[1].w == BG_W && art_bg[1].h == BG_H &&
			 art_ground.w == GROUND_W && art_ground.h == GROUND_H && art_pipe_upper.w == PIPE_W &&
			 art_pipe_upper.h == PIPE_H && art_pipe_lower.w == PIPE_W && art_pipe_lower.h == PIPE_H;
		if (!ok) {
			printf("flappybird: the background, ground, pipe or bird drawings are not the expected size\n");
		}
	}
	return ok;
}

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------

typedef enum {
	ST_TITLE,  // logo, play and exit
	ST_READY,  // Get Ready: the first tap starts the game
	ST_PLAY,
	ST_PAUSED, // the panel went dark mid-game: frozen until the next tap
	ST_DYING,  // hit a pipe, falling to the ground
	ST_OVER,
} game_state_t;

typedef struct {
	int x; // left edge, units
	int gap_top; // units
	bool scored;
} pipe_t;

typedef enum { WINGS_IDLE, WINGS_FLAP, WINGS_REST } wings_t;

typedef enum { BTN_NONE, BTN_PLAY, BTN_MENU, BTN_EXIT, BTN_SOUND } button_t;

static lv_obj_t *frame_image;
static lv_timer_t *tick_timer;
static lv_image_dsc_t frame_dsc;

static int panel_w, panel_h;
static int view_h;		  // the panel's height in artwork pixels
static int ground_top;	  // artwork pixels
static int bg_row0;		  // the background's row at the top of the panel
static int layout_shift;  // panel pixels
static uint16_t *frame;	  // panel_w x panel_h
static uint16_t *bg_wide; // ground_top rows of BG_W * scale, from bg_row0
static uint16_t *ground_wide; // GROUND_H rows of GROUND_W * scale
// Per row of bg_wide and ground_wide: its colour when the row is one colour,
// otherwise -1. Below ground_varied_rows every row of the ground is one colour.
static int32_t bg_flat[BG_H];
static int32_t ground_flat[GROUND_H];
static int ground_varied_rows;
// Panel rows above `band_top` are background of one colour per row: while
// the bird is in the air only the moving things in it change. Below it, down
// to the ground's last changing row, every row changes as the world scrolls.
static int band_top;

// `frame` and both pages of the panel hold the last frame whole, so the next
// one can be drawn in part (see render).
static bool frame_whole;

#define MAX_MOVING 8
// Where the moving things were drawn above band_top in the last frame, and
// the areas the last render changed, for the invalidation.
static lv_area_t moved[MAX_MOVING];
static int moved_n;
static lv_area_t changed[2 * MAX_MOVING + 1];
static int changed_n;

static bool active;
static bool dirty;
static int64_t last_us;

// How the frames went, for one line in the log when the game is left.
static struct {
	unsigned ticks, frames, late, slow;
	int64_t since, longest_gap, longest_work, work;
} stats;

static game_state_t state;
static pipe_t pipes[PIPE_COUNT];
static int bird_c; // top of the hit box, units
static float bird_v, bird_g;
static float rot_q, rot_r;
static int bob_deg;
static wings_t wings;
static float wing_ms;
static int wing_i;
static int bird_colour;
static int bg_index;
// The look was chosen for the title screen and the game that follows keeps it.
static bool look_kept;
static int world_px; // how far ground and pipes have moved, panel pixels
static int bg_px;
static int score, best, best_before;
static bool new_best;
static bool best_unsaved;
static int over_t;
static int flash_t = -1;
static int fade_t = -1;
static void (*fade_action)(void);
static button_t pressed_button = BTN_NONE;
// The game's own sounds, [flappybird] sound. Off leaves the music playing.
static bool sound_on;

static int64_t now_us(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

// ---------------------------------------------------------------------------
// background and ground
// ---------------------------------------------------------------------------

// A panel row from a horizontally repeating row `src_w` wide, `offset`
// pixels scrolled.
static void wrap_row(uint16_t *dst, const uint16_t *src, int src_w, int offset) {
	int x = 0;
	while (x < panel_w) {
		int n = src_w - offset;
		if (n > panel_w - x) {
			n = panel_w - x;
		}
		memcpy(dst + x, src + offset, (size_t)n * sizeof(uint16_t));
		x += n;
		offset = 0;
	}
}

// `rows` rows of `s` from `row0`, each pixel repeated scale times across.
static void widen(uint16_t *dst, const sprite_t *s, int row0, int rows) {
	for (int y = 0; y < rows; y++) {
		widen_row(dst + (size_t)y * s->w * scale, s->rgb + (size_t)(row0 + y) * s->w, s->w);
	}
}

// For each of `rows` rows of `s` from `row0`: its colour when the whole row is
// one colour, otherwise -1.
static void find_flat_rows(const sprite_t *s, int row0, int rows, int32_t *out) {
	for (int y = 0; y < rows; y++) {
		const uint16_t *src = s->rgb + (size_t)(row0 + y) * s->w;
		out[y] = src[0];
		for (int x = 1; x < s->w; x++) {
			if (src[x] != src[0]) {
				out[y] = -1;
				break;
			}
		}
	}
}

// Columns x1..x2 of a row, in pixel pairs where they line up.
static void fill_span(uint16_t *row, int x1, int x2, uint16_t colour) {
	if (x1 & 1) {
		row[x1++] = colour;
	}
	uint32_t pair = (uint32_t)colour | ((uint32_t)colour << 16);
	uint32_t *d = (uint32_t *)(row + x1);
	int pairs = (x2 - x1 + 1) / 2;
	for (int i = 0; i < pairs; i++) {
		d[i] = pair;
	}
	if (x1 + pairs * 2 <= x2) {
		row[x2] = colour;
	}
}

static void fill_row(uint16_t *dst, uint16_t colour) { fill_span(dst, 0, panel_w - 1, colour); }

// The background from panel row `from` down to the ground.
static void draw_background(int from) {
	int w = BG_W * scale;
	for (int y = from; y < ground_top * scale; y++) {
		uint16_t *row = frame + (size_t)y * panel_w;
		int32_t flat = bg_flat[y / scale];
		if (flat >= 0) {
			fill_row(row, (uint16_t)flat);
		} else {
			wrap_row(row, bg_wide + (size_t)(y / scale) * w, w, bg_px % w);
		}
	}
}

// The ground down to the last row that changes as it scrolls, or all of it.
static void draw_ground(bool whole) {
	int w = GROUND_W * scale;
	int top = ground_top * scale;
	int end = whole ? panel_h : top + ground_varied_rows * scale;
	for (int y = top; y < end; y++) {
		int sy = (y - top) / scale;
		if (sy >= GROUND_H) {
			sy = GROUND_H - 1;
		}
		uint16_t *row = frame + (size_t)y * panel_w;
		if (ground_flat[sy] >= 0) {
			fill_row(row, (uint16_t)ground_flat[sy]);
		} else {
			wrap_row(row, ground_wide + (size_t)sy * w, w, world_px % w);
		}
	}
}

// ---------------------------------------------------------------------------
// sprites
// ---------------------------------------------------------------------------

// `opa` 0..255, rounded to 256ths.
static uint16_t blend565(uint16_t fg, uint16_t bg, unsigned opa) {
	unsigned a = opa + (opa >> 7);
	unsigned inv = 256 - a;
	unsigned r = (((fg >> 11) & 31) * a + ((bg >> 11) & 31) * inv) >> 8;
	unsigned g = (((fg >> 5) & 63) * a + ((bg >> 5) & 63) * inv) >> 8;
	unsigned b = ((fg & 31) * a + (bg & 31) * inv) >> 8;
	return (uint16_t)((r << 11) | (g << 5) | b);
}

// `s` at `scale` times its size with its top left at (x, y) on the panel, down
// to row `y_end` (exclusive). Opaque runs go in with memcpy.
static void blit4_clipped(const sprite_t *s, int x, int y, int y_end, unsigned opa) {
	if (opa == 0) {
		return;
	}
	if (y_end > panel_h) {
		y_end = panel_h;
	}
	uint32_t pairs[BG_W * MAX_SCALE / 2];
	for (int j = 0; j < s->h; j++) {
		int top = y + j * scale;
		if (top >= y_end) {
			break;
		}
		if (top + scale <= 0) {
			continue;
		}
		const uint16_t *src = s->rgb + j * s->w;
		const uint8_t *m = s->mask ? s->mask + j * s->w : NULL;
		int i = 0;
		while (i < s->w) {
			if (m && !m[i]) {
				i++;
				continue;
			}
			int run = i;
			while (run < s->w && (!m || m[run])) {
				run++;
			}
			// Pixels i..run-1 are opaque: widened (or taken widened), then
			// written into each of the scale rows, clipped left and right.
			int x0 = x + i * scale, x1 = x + run * scale;
			int cut = x0 < 0 ? -x0 : 0;
			if (x1 > panel_w) {
				x1 = panel_w;
			}
			if (x0 + cut < x1) {
				const uint16_t *wide;
				if (s->wide) {
					wide = s->wide + ((size_t)j * s->w + i) * scale;
				} else {
					widen_row((uint16_t *)pairs, src + i, run - i);
					wide = (const uint16_t *)pairs;
				}
				for (int r = 0; r < scale; r++) {
					int py = top + r;
					if (py < 0 || py >= y_end) {
						continue;
					}
					uint16_t *row = frame + (size_t)py * panel_w;
					if (opa >= 255) {
						memcpy(row + x0 + cut, wide + cut, (size_t)(x1 - x0 - cut) * sizeof(uint16_t));
					} else {
						for (int px = x0 + cut; px < x1; px++) {
							row[px] = blend565(wide[px - x0], row[px], opa);
						}
					}
				}
			}
			i = run;
		}
	}
}

static void blit4(const sprite_t *s, int x, int y, unsigned opa) { blit4_clipped(s, x, y, panel_h, opa); }

// `s` at `scale` times its size, centred on (cx, cy) and turned by `deg`,
// positive anticlockwise. Nearest neighbour, so the pixels stay square.
static void blit4_turned(const sprite_t *s, float cx, float cy, float deg) {
	if (fabsf(deg) < 0.5f) {
		blit4(s, (int)lroundf(cx - s->w * scale / 2.0f), (int)lroundf(cy - s->h * scale / 2.0f), 255);
		return;
	}
	float rad = deg * (float)M_PI / 180.0f;
	float cs = cosf(rad) / scale, sn = sinf(rad) / scale;
	int reach = (int)ceilf(sqrtf((float)(s->w * s->w + s->h * s->h)) * scale / 2.0f) + 1;
	int x0 = (int)floorf(cx) - reach, y0 = (int)floorf(cy) - reach;
	for (int py = y0; py <= y0 + 2 * reach; py++) {
		if (py < 0 || py >= panel_h) {
			continue;
		}
		uint16_t *row = frame + (size_t)py * panel_w;
		// The source position of the first pixel of the row, then one step
		// along the turned x axis per pixel.
		float dx = (float)x0 + 0.5f - cx;
		float dy = (float)py + 0.5f - cy;
		float u = cs * dx - sn * dy + s->w / 2.0f;
		float v = sn * dx + cs * dy + s->h / 2.0f;
		for (int px = x0; px <= x0 + 2 * reach; px++, u += cs, v += sn) {
			if (px < 0 || px >= panel_w || u < 0 || v < 0) {
				continue;
			}
			int iu = (int)u, iv = (int)v;
			if (iu >= s->w || iv >= s->h) {
				continue;
			}
			int k = iv * s->w + iu;
			if (!s->mask || s->mask[k]) {
				row[px] = s->rgb[k];
			}
		}
	}
}

// The whole frame tinted with `colour` at `opa`.
static void wash(uint16_t colour, unsigned opa) {
	if (opa == 0) {
		return;
	}
	size_t n = (size_t)panel_w * panel_h;
	if (opa >= 255) {
		for (size_t i = 0; i < n; i++) {
			frame[i] = colour;
		}
		return;
	}
	unsigned a = opa + (opa >> 7);
	unsigned inv = 256 - a;
	unsigned cr = ((colour >> 11) & 31) * a, cg = ((colour >> 5) & 63) * a, cb = (colour & 31) * a;
	for (size_t i = 0; i < n; i++) {
		uint16_t p = frame[i];
		unsigned r = (cr + ((p >> 11) & 31) * inv) >> 8;
		unsigned g = (cg + ((p >> 5) & 63) * inv) >> 8;
		unsigned b = (cb + (p & 31) * inv) >> 8;
		frame[i] = (uint16_t)((r << 11) | (g << 5) | b);
	}
}

static int number_width(const sprite_t *digits, int gap, int value) {
	char text[16];
	int len = snprintf(text, sizeof(text), "%d", value);
	int w = 0;
	for (int i = 0; i < len; i++) {
		w += digits[text[i] - '0'].w;
	}
	return (w + (len - 1) * gap) * scale;
}

static void draw_number(const sprite_t *digits, int gap, int value, int x, int y) {
	char text[16];
	int len = snprintf(text, sizeof(text), "%d", value);
	for (int i = 0; i < len; i++) {
		const sprite_t *d = &digits[text[i] - '0'];
		blit4(d, x, y, 255);
		x += (d->w + gap) * scale;
	}
}

// ---------------------------------------------------------------------------
// layout
// ---------------------------------------------------------------------------

static int ground_top_px(void) { return ground_top * scale; }

static int how_to_y(void) { return ground_top_px() - art_how_to.h * scale - HOW_TO_ABOVE_GROUND; }

static int ready_bird_y_px(void) { return how_to_y() - READY_BIRD_ABOVE_HOW_TO; }

// Clear of the exit button in the corner: on a short panel the logo would
// otherwise reach up under it.
static int title_logo_y(void) {
	int y = TITLE_LOGO_Y + layout_shift;
	int below_exit = CORNER_MARGIN + art_exit.h * scale + CORNER_MARGIN / 2;
	int logo_right = (panel_w + art_logo.w * scale) / 2;
	int exit_left = panel_w - art_exit.w * scale - CORNER_MARGIN;
	return logo_right > exit_left && y < below_exit ? below_exit : y;
}

static int title_play_y(void);

// Halfway up the sky, or when the bob would take it into the logo, halfway
// between the logo and Play.
static float title_bird_y(void) {
	float y = ground_top_px() / 2.0f;
	float reach = art_bird[0][0].h * scale / 2.0f + u_pxf(BOB_U);
	int logo_bottom = title_logo_y() + art_logo.h * scale;
	if (y - reach < logo_bottom) {
		y = (logo_bottom + title_play_y()) / 2.0f;
	}
	return y;
}

static bool buttons_shown(void) {
	return state == ST_TITLE || (state == ST_OVER && over_t >= OVER_COUNT_AT);
}

// The rectangle of `b` on the panel, before any press shift. False when the
// button is not on screen now.
static bool button_rect(button_t b, lv_area_t *a) {
	if (!buttons_shown()) {
		return false;
	}
	const sprite_t *s = NULL;
	int x = 0, y = 0;
	if (b == BTN_SOUND) {
		// Bottom right, on the title screen and after a game.
		s = &art_sound_on;
		x = panel_w - s->w * scale - CORNER_MARGIN;
		y = panel_h - s->h * scale - CORNER_MARGIN;
	} else if (state == ST_TITLE) {
		if (b == BTN_PLAY) {
			s = &art_play;
			x = (panel_w - s->w * scale) / 2;
			y = title_play_y();
		} else if (b == BTN_EXIT) {
			s = &art_exit;
			x = panel_w - s->w * scale - CORNER_MARGIN;
			y = CORNER_MARGIN;
		}
	} else {
		// Play under the board, Menu under Play.
		int play_y = OVER_BOARD_Y + layout_shift + art_board.h * scale + OVER_BUTTONS_GAP;
		if (b == BTN_PLAY) {
			s = &art_play;
			y = play_y;
		} else if (b == BTN_MENU) {
			s = &art_menu;
			y = play_y + art_play.h * scale + OVER_BUTTONS_GAP;
		}
		if (s) {
			x = (panel_w - s->w * scale) / 2;
		}
	}
	if (!s) {
		return false;
	}
	a->x1 = x;
	a->y1 = y;
	a->x2 = x + s->w * scale - 1;
	a->y2 = y + s->h * scale - 1;
	return true;
}

static int title_play_y(void) { return ground_top_px() - art_play.h * scale - TITLE_PLAY_ABOVE_GROUND; }

static void draw_button(button_t b, const sprite_t *s) {
	lv_area_t a;
	if (button_rect(b, &a)) {
		blit4(s, a.x1, a.y1 + (pressed_button == b ? BUTTON_PRESS_SHIFT : 0), 255);
	}
}

static button_t button_at(const lv_point_t *p) {
	static const button_t ALL[] = {BTN_PLAY, BTN_MENU, BTN_EXIT, BTN_SOUND};
	for (size_t i = 0; i < sizeof(ALL) / sizeof(ALL[0]); i++) {
		lv_area_t a;
		if (button_rect(ALL[i], &a) && p->x >= a.x1 && p->x <= a.x2 && p->y >= a.y1 && p->y <= a.y2) {
			return ALL[i];
		}
	}
	return BTN_NONE;
}

// ---------------------------------------------------------------------------
// the game
// ---------------------------------------------------------------------------

static int ground_u(void) { return ground_top * 2; }


static float bob(void) { return sinf((float)bob_deg * (float)M_PI / 180.0f) * BOB_U; }

static int wing_frame(void) {
	static const int SEQUENCE[4] = {0, 1, 2, 1};
	return wings == WINGS_REST ? 1 : SEQUENCE[wing_i % 4];
}

static void wings_set(wings_t w) {
	wings = w;
	wing_ms = 0;
	wing_i = 0;
}

static void wings_tick(void) {
	if (wings == WINGS_REST) {
		return;
	}
	wing_ms += ANIM_MS_PER_STEP;
	if (wing_ms >= (wings == WINGS_FLAP ? FLAP_FRAME_MS : IDLE_FRAME_MS)) {
		wing_ms = 0;
		wing_i++;
		if (wings == WINGS_FLAP && wing_i >= FLAP_FRAMES) {
			wings = WINGS_REST;
		}
	}
}

// The top of an opening, at least PIPE_MIN_CHANGE_U away from `previous`
// (negative for none) when the range allows it.
static int random_gap_top(int previous) {
	int lo = PIPE_MARGIN_U;
	int hi = ground_u() - PIPE_MARGIN_U - PIPE_GAP_U;
	if (hi <= lo) {
		return lo;
	}
	int top = lo;
	for (int tries = 0; tries < 8; tries++) {
		top = lo + rand() % (hi - lo + 1);
		if (previous < 0 || abs(top - previous) >= PIPE_MIN_CHANGE_U) {
			break;
		}
	}
	return top;
}

static void pipes_reset(void) {
	for (int i = 0; i < PIPE_COUNT; i++) {
		pipes[i].x = px_u(panel_w) + FIRST_PIPE_AHEAD_U + i * PIPE_SPACING_U;
		pipes[i].gap_top = random_gap_top(i > 0 ? pipes[i - 1].gap_top : -1);
		pipes[i].scored = false;
	}
}

// A point as the left edge of a pipe reaches the left edge of the hit box.
static void pipes_advance(void) {
	for (int i = 0; i < PIPE_COUNT; i++) {
		pipes[i].x -= WORLD_SPEED;
	}
	for (int i = 0; i < PIPE_COUNT; i++) {
		pipe_t *p = &pipes[i];
		if (p->x + PIPE_W_U < 0) {
			int last = 0;
			for (int k = 1; k < PIPE_COUNT; k++) {
				if (pipes[k].x > pipes[last].x) {
					last = k;
				}
			}
			p->x = pipes[last].x + PIPE_SPACING_U;
			p->gap_top = random_gap_top(pipes[last].gap_top);
			p->scored = false;
		}
		if (!p->scored && p->x <= BIRD_LEFT_U) {
			p->scored = true;
			score++;
			flappysound_play(SFX_POINT);
		}
	}
}

static void randomise_look(void) {
	int total = 0;
	for (int i = 0; i < BIRD_COLOURS; i++) {
		total += BIRD_WEIGHTS[i];
	}
	int pick = rand() % total;
	bird_colour = 0;
	while (pick >= BIRD_WEIGHTS[bird_colour]) {
		pick -= BIRD_WEIGHTS[bird_colour];
		bird_colour++;
	}
	bg_index = rand() % 2;
	widen(bg_wide, &art_bg[bg_index], bg_row0, ground_top);
	find_flat_rows(&art_bg[bg_index], bg_row0, ground_top, bg_flat);
	band_top = 0;
	while (band_top < ground_top && bg_flat[band_top] >= 0) {
		band_top++;
	}
	band_top *= scale;
}

static void bird_reset(void) {
	bird_v = 0;
	bird_g = 0;
	rot_q = 0;
	rot_r = 0;
	bob_deg = 0;
	wings_set(WINGS_IDLE);
}

static void go_title(void) {
	randomise_look();
	look_kept = true;
	state = ST_TITLE;
	bird_reset();
	flash_t = -1;
}

static void go_ready(void) {
	if (!look_kept) {
		randomise_look();
	}
	look_kept = false;
	state = ST_READY;
	score = 0;
	new_best = false;
	bird_reset();
	bird_c = px_u(ready_bird_y_px()) - BIRD_BOX_U / 2;
	flash_t = -1;
	over_t = 0;
	pipes_reset();
}

static void fade_to(void (*action)(void)) {
	fade_t = 0;
	fade_action = action;
}

static void flap(void) {
	if (bird_c >= 0) {
		bird_v = FLAP_VELOCITY;
		bird_g = GRAVITY;
		rot_r = ROT_FLAP_RATE;
		wings_set(WINGS_FLAP);
		flappysound_play(SFX_WING);
	}
}

// The bob is where the bird is when the game starts.
static void start_play(void) {
	state = ST_PLAY;
	bird_c = px_u(ready_bird_y_px()) - BIRD_BOX_U / 2 + (int)lroundf(bob());
	flap();
}

static int bird_floor(void) { return ground_u() - BIRD_BOX_U; }

static bool bird_on_ground(void) { return bird_c >= bird_floor(); }

static void hit(void) {
	flash_t = 0;
	over_t = 0;
	best_before = best;
	if (score > best) {
		best = score;
		new_best = true;
		// Saved once the board has settled: a card write is too slow for the
		// middle of the fall.
		config_set_int("flappybird", "best", best);
		best_unsaved = true;
	}
	state = bird_on_ground() ? ST_OVER : ST_DYING;
	flappysound_play(SFX_HIT);
	if (state == ST_DYING) {
		flappysound_play(SFX_DIE);
	}
}

static void bird_fall(void) {
	bird_v += bird_g;
	if (bird_v > MAX_FALL) {
		bird_v = MAX_FALL;
	}
	bird_c = (int)((float)bird_c + bird_v);
	if (bird_c > bird_floor()) {
		bird_c = bird_floor();
		bird_v = 0;
		bird_g = 0;
	}
	rot_q += rot_r;
	rot_r += ROT_ACCEL;
	if (rot_q < ROT_MIN) {
		rot_q = ROT_MIN;
	}
	if (rot_q > ROT_MAX) {
		rot_q = ROT_MAX;
	}
}

// Edges touching count.
static bool boxes_touch(int x, int y, int w, int h, int px, int py, int pw, int ph) {
	return x + w >= px && x <= px + pw && y + h >= py && y <= py + ph;
}

static bool bird_hits_pipe(void) {
	for (int i = 0; i < PIPE_COUNT; i++) {
		const pipe_t *p = &pipes[i];
		if (boxes_touch(BIRD_LEFT_U, bird_c, BIRD_BOX_U, BIRD_BOX_U, p->x, p->gap_top - PIPE_H_U, PIPE_W_U, PIPE_H_U) ||
			boxes_touch(BIRD_LEFT_U, bird_c, BIRD_BOX_U, BIRD_BOX_U, p->x, p->gap_top + PIPE_GAP_U, PIPE_W_U,
						PIPE_H_U)) {
			return true;
		}
	}
	return false;
}

static void step(void) {
	bool moving = state == ST_TITLE || state == ST_READY || state == ST_PLAY;
	if (moving) {
		world_px = (world_px + u_px(WORLD_SPEED)) % (GROUND_W * scale);
		bg_px = (bg_px + BG_PX_PER_STEP) % (BG_W * scale);
	}
	if (state == ST_TITLE || state == ST_READY) {
		bob_deg = (bob_deg + BOB_DEG_STEP) % 360;
	}
	if (state != ST_PAUSED && state != ST_OVER) {
		wings_tick();
	}

	switch (state) {
	case ST_PLAY:
		pipes_advance();
		bird_fall();
		if (bird_on_ground() || bird_hits_pipe() || bird_c < LOST_ABOVE_U) {
			hit();
		}
		break;
	case ST_DYING:
		bird_fall();
		if (bird_on_ground()) {
			state = ST_OVER;
		}
		break;
	default:
		break;
	}

	if (state == ST_OVER) {
		over_t++;
		if (over_t == OVER_BOARD_AT) {
			flappysound_play(SFX_SWOOSH);
		}
	}
	if (flash_t >= 0 && ++flash_t > FLASH_STEPS) {
		flash_t = -1;
	}
	if (fade_t >= 0) {
		fade_t++;
		if (fade_t == FADE_STEPS && fade_action) {
			fade_action();
			fade_action = NULL;
		}
		if (fade_t >= 2 * FADE_STEPS) {
			fade_t = -1;
		}
	}

	if (state == ST_OVER && over_t == OVER_DONE_AT && best_unsaved) {
		best_unsaved = false;
		config_save();
	}

	bool still = (state == ST_OVER && over_t > OVER_DONE_AT) || state == ST_PAUSED;
	if (!still || flash_t >= 0 || fade_t >= 0) {
		dirty = true;
	}
}

// ---------------------------------------------------------------------------
// the picture
// ---------------------------------------------------------------------------

static float ease_out(float p) {
	if (p <= 0) {
		return 0;
	}
	if (p >= 1) {
		return 1;
	}
	float q = 1 - p;
	return 1 - q * q * q;
}

static void draw_board(void) {
	float p = (float)(over_t - OVER_BOARD_AT) / OVER_BOARD_STEPS;
	if (p <= 0) {
		return;
	}
	int target = OVER_BOARD_Y + layout_shift;
	int bx = (panel_w - art_board.w * scale) / 2;
	int by = panel_h + (int)lroundf((target - panel_h) * ease_out(p));
	blit4(&art_board, bx, by, 255);

	int shown = score;
	int c = over_t - OVER_COUNT_AT;
	if (c < OVER_COUNT_STEPS) {
		shown = c <= 0 ? 0 : score * c / OVER_COUNT_STEPS;
	}
	int best_shown = new_best ? (shown > best_before ? shown : best_before) : best;
	int right = bx + BOARD_DIGITS_RIGHT * scale;

	draw_number(art_digit_small, DIGIT_GAP_SMALL, shown, right - number_width(art_digit_small, DIGIT_GAP_SMALL, shown),
				by + BOARD_SCORE_Y * scale);
	draw_number(art_digit_small, DIGIT_GAP_SMALL, best_shown,
				right - number_width(art_digit_small, DIGIT_GAP_SMALL, best_shown), by + BOARD_BEST_Y * scale);

	if (over_t >= OVER_DONE_AT) {
		int medal = score >= MEDAL_GOLD_AT	   ? MEDAL_GOLD
					: score >= MEDAL_SILVER_AT ? MEDAL_SILVER
					: score >= MEDAL_BRONZE_AT ? MEDAL_BRONZE
											   : -1;
		if (medal >= 0) {
			blit4(&art_medal[medal], bx + BOARD_MEDAL_X * scale, by + BOARD_MEDAL_Y * scale, 255);
		}
		if (new_best) {
			// Left of the printed BEST, on its bottom row.
			blit4(&art_new, bx + (BOARD_BEST_LABEL_X - BOARD_NEW_GAP - art_new.w) * scale,
				  by + (BOARD_BEST_LABEL_Y + BOARD_BEST_LABEL_H - art_new.h) * scale, 255);
		}
	}
}

static void draw_score(void) {
	int w = number_width(art_digit_big, DIGIT_GAP_BIG, score);
	draw_number(art_digit_big, DIGIT_GAP_BIG, score, (panel_w - w) / 2, SCORE_Y);
}

static float bird_cx(void) { return u_pxf(BIRD_LEFT_U + BIRD_BOX_U / 2); }

static float bird_cy(void) {
	return state == ST_READY ? (float)ready_bird_y_px() + u_pxf(bob())
							 : u_pxf((float)(bird_c + BIRD_BOX_U / 2));
}

static bool score_shown(void) { return (state == ST_PLAY || state == ST_DYING) && over_t < OVER_LABEL_AT; }

static bool pipes_shown(void) {
	return state == ST_PLAY || state == ST_PAUSED || state == ST_DYING || state == ST_OVER;
}

// Adds `a`, cut to the panel above band_top, when anything is left of it.
static void add_area(lv_area_t *list, int *n, int x1, int y1, int x2, int y2) {
	if (x1 < 0) {
		x1 = 0;
	}
	if (y1 < 0) {
		y1 = 0;
	}
	if (x2 > panel_w - 1) {
		x2 = panel_w - 1;
	}
	if (y2 > band_top - 1) {
		y2 = band_top - 1;
	}
	if (x1 <= x2 && y1 <= y2) {
		list[*n] = (lv_area_t){x1, y1, x2, y2};
		(*n)++;
	}
}

// The bird, the pipes and the running score, as far as they reach above
// band_top.
static int moving_areas(lv_area_t *out) {
	int n = 0;
	int reach = (int)ceilf(sqrtf((float)(BIRD_W * BIRD_W + BIRD_H * BIRD_H)) * scale / 2.0f) + 2;
	int cx = (int)bird_cx(), cy = (int)bird_cy();
	add_area(out, &n, cx - reach, cy - reach, cx + reach, cy + reach);
	if (score_shown()) {
		int w = number_width(art_digit_big, DIGIT_GAP_BIG, score);
		int x = (panel_w - w) / 2;
		add_area(out, &n, x, SCORE_Y, x + w - 1, SCORE_Y + art_digit_big[0].h * scale - 1);
	}
	if (pipes_shown()) {
		for (int i = 0; i < PIPE_COUNT; i++) {
			int x = u_px(pipes[i].x);
			add_area(out, &n, x, 0, x + PIPE_W * scale - 1, band_top - 1);
		}
	}
	return n;
}

// A whole frame, or with `whole` false one where the background above
// band_top is painted back only where the moving things were and are, and the
// one-colour bottom of the ground is left as it is. Everything else is drawn
// as always: what does not move comes out the same.
static void render(bool whole) {
	lv_area_t now[MAX_MOVING];
	int now_n = moving_areas(now);

	changed_n = 0;
	if (whole) {
		changed[changed_n++] = (lv_area_t){0, 0, panel_w - 1, panel_h - 1};
		draw_background(0);
	} else {
		for (int i = 0; i < moved_n + now_n; i++) {
			const lv_area_t *a = i < moved_n ? &moved[i] : &now[i - moved_n];
			for (int y = a->y1; y <= a->y2; y++) {
				fill_span(frame + (size_t)y * panel_w, a->x1, a->x2, (uint16_t)bg_flat[y / scale]);
			}
			changed[changed_n++] = *a;
		}
		changed[changed_n++] =
			(lv_area_t){0, band_top, panel_w - 1, (ground_top + ground_varied_rows) * scale - 1};
		draw_background(band_top);
	}
	memcpy(moved, now, sizeof(now));
	moved_n = now_n;

	if (pipes_shown()) {
		int ground = ground_top_px();
		for (int i = 0; i < PIPE_COUNT; i++) {
			const pipe_t *p = &pipes[i];
			blit4_clipped(&art_pipe_upper, u_px(p->x), u_px(p->gap_top - PIPE_H_U), ground, 255);
			blit4_clipped(&art_pipe_lower, u_px(p->x), u_px(p->gap_top + PIPE_GAP_U), ground, 255);
		}
	}
	draw_ground(whole);

	const sprite_t *bird = &art_bird[bird_colour][wing_frame()];
	// Whole degrees, as the original draws it; anticlockwise here.
	float bird_turn = (float)-(int)rot_q;

	switch (state) {
	case ST_TITLE:
		blit4(&art_logo, (panel_w - art_logo.w * scale) / 2, title_logo_y(), 255);
		blit4_turned(bird, panel_w / 2.0f, title_bird_y() + u_pxf(bob()), 0);
		draw_button(BTN_PLAY, &art_play);
		draw_button(BTN_EXIT, &art_exit);
		draw_button(BTN_SOUND, sound_on ? &art_sound_on : &art_sound_off);
		break;
	case ST_READY:
		blit4(&art_get_ready, (panel_w - art_get_ready.w * scale) / 2, READY_LABEL_Y + layout_shift, 255);
		blit4(&art_how_to, (panel_w - art_how_to.w * scale) / 2, how_to_y(), 255);
		blit4_turned(bird, bird_cx(), bird_cy(), 0);
		break;
	case ST_PLAY:
	case ST_PAUSED:
	case ST_DYING:
	case ST_OVER:
		blit4_turned(bird, bird_cx(), bird_cy(), bird_turn);
		if (state == ST_PAUSED) {
			blit4(&art_get_ready, (panel_w - art_get_ready.w * scale) / 2, READY_LABEL_Y + layout_shift, 255);
		} else if (over_t < OVER_LABEL_AT) {
			draw_score();
		}
		if (over_t >= OVER_LABEL_AT) {
			float p = (float)(over_t - OVER_LABEL_AT) / OVER_LABEL_STEPS;
			if (p > 1) {
				p = 1;
			}
			int y = OVER_LABEL_Y + layout_shift - (int)lroundf((1 - ease_out(p)) * 6 * scale);
			blit4(&art_game_over, (panel_w - art_game_over.w * scale) / 2, y, (unsigned)lroundf(255 * p));
		}
		if (state == ST_OVER) {
			draw_board();
			draw_button(BTN_PLAY, &art_play);
			draw_button(BTN_MENU, &art_menu);
			draw_button(BTN_SOUND, sound_on ? &art_sound_on : &art_sound_off);
		}
		break;
	}

	if (flash_t >= 0) {
		wash(0xFFFF, (unsigned)(FLASH_OPA * (FLASH_STEPS - flash_t) / FLASH_STEPS));
	}
	if (fade_t >= 0) {
		int t = fade_t <= FADE_STEPS ? fade_t : 2 * FADE_STEPS - fade_t;
		wash(0x0000, (unsigned)(255 * t / FADE_STEPS));
	}
}

// ---------------------------------------------------------------------------
// the page
// ---------------------------------------------------------------------------

static void tick_cb(lv_timer_t *timer) {
	if (!active) {
		lv_timer_pause(timer); // woken by power.c at the end of a standby
		return;
	}

	int64_t now = now_us();
	int64_t elapsed = now - last_us;
	last_us = now;

	// One step a tick, so the world moves by the same amount between every
	// two frames on the panel. A tick late by a frame or two, as when the
	// music's decoder has had the processor, still moves one step: the game
	// slows for that moment rather than jumping. Only a longer gap is made up.
	int steps = 1;
	if (elapsed > STALL_US) {
		steps = 0;
		if (state == ST_PLAY) {
			state = ST_PAUSED;
			dirty = true;
		}
	} else if (elapsed * 2 > STEP_US * 3) {
		stats.late++;
		if (elapsed >= CATCH_UP_US) {
			steps = (int)((elapsed + STEP_US / 2) / STEP_US);
			if (steps > MAX_STEPS_PER_TICK) {
				steps = MAX_STEPS_PER_TICK;
			}
		}
	}
	for (int i = 0; i < steps; i++) {
		step();
	}
	stats.ticks++;
	if (elapsed <= STALL_US && elapsed > stats.longest_gap) {
		stats.longest_gap = elapsed;
	}

	if (dirty) {
		dirty = false;
		// While the bird is in the air, after one whole frame, the frames are
		// drawn in part.
		// A change of state takes things off the screen (Get Ready, the
		// how-to) or puts them on: that frame is drawn whole.
		static game_state_t drawn_state;
		bool in_air = (state == ST_READY || state == ST_PLAY || state == ST_DYING) && flash_t < 0 && fade_t < 0;
		if (state != drawn_state) {
			frame_whole = false;
			drawn_state = state;
		}
		render(!(in_air && frame_whole));
		frame_whole = in_air;
		for (int i = 0; i < changed_n; i++) {
			lv_obj_invalidate_area(frame_image, &changed[i]);
		}
		// On the panel now, in step with the frame just drawn, rather than at
		// the display timer's own phase.
		lv_refr_now(NULL);
		int64_t work = now_us() - now;
		if (work > stats.longest_work) {
			stats.longest_work = work;
		}
		if (work > STEP_US) {
			stats.slow++;
		}
		stats.work += work;
		stats.frames++;
	}
}

static void press_cb(lv_event_t *e) {
	(void)e;
	if (!active || fade_t >= 0) {
		return;
	}
	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);

	switch (state) {
	case ST_TITLE:
	case ST_OVER:
		pressed_button = button_at(&p);
		break;
	case ST_READY:
		start_play();
		break;
	case ST_PAUSED:
		state = ST_PLAY;
		flap();
		break;
	case ST_PLAY:
		flap();
		break;
	default:
		break;
	}
	dirty = true;
}

static void release_cb(lv_event_t *e) {
	if (!active || pressed_button == BTN_NONE) {
		return;
	}
	button_t pressed = pressed_button;
	pressed_button = BTN_NONE;
	dirty = true;

	if (lv_event_get_code(e) != LV_EVENT_RELEASED || fade_t >= 0) {
		return;
	}
	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);
	if (button_at(&p) != pressed) {
		return;
	}
	switch (pressed) {
	case BTN_PLAY:
		flappysound_play(SFX_SWOOSH);
		fade_to(go_ready);
		break;
	case BTN_MENU:
		flappysound_play(SFX_SWOOSH);
		fade_to(go_title);
		break;
	case BTN_SOUND:
		sound_on = !sound_on;
		config_set_bool("flappybird", "sound", sound_on);
		config_save();
		if (sound_on) {
			flappysound_start();
		} else {
			flappysound_stop();
		}
		break;
	case BTN_EXIT:
		back_btn_cb(NULL); // unloading the page frees everything
		break;
	default:
		break;
	}
}

static void release_all(void) {
	flappysound_stop();
	if (frame_image) {
		lv_image_set_src(frame_image, NULL);
	}
	if (frame) {
		// LVGL's image cache is keyed by the descriptor's address.
		lv_image_cache_drop(&frame_dsc);
	}
	memset(&frame_dsc, 0, sizeof(frame_dsc));
	free(frame);
	frame = NULL;
	free(bg_wide);
	bg_wide = NULL;
	free(ground_wide);
	ground_wide = NULL;
	art_free();
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	topbar_set_hidden(true);
	back_btn_force_hidden(true);
	volume_overlay_allow_outside_player(true);

	active = true;
	pressed_button = BTN_NONE;
	frame_whole = false;
	moved_n = 0;
	last_us = now_us();
	memset(&stats, 0, sizeof(stats));
	stats.since = last_us;
	dirty = true;
	lv_timer_resume(tick_timer);
	lv_timer_ready(tick_timer);
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	active = false;
	if (stats.frames > 0) {
		printf("flappybird: %u frames in %.1f s, %u late ticks, longest gap %.1f ms; frames %.1f ms on average, %u over "
			   "a step, longest %.1f ms\n",
			   stats.frames, (double)(now_us() - stats.since) / 1e6, stats.late, (double)stats.longest_gap / 1000.0,
			   (double)stats.work / stats.frames / 1000.0, stats.slow, (double)stats.longest_work / 1000.0);
	}
	if (best_unsaved) {
		best_unsaved = false;
		config_save();
	}
	lv_timer_pause(tick_timer);
	release_all();

	volume_overlay_allow_outside_player(false);
	back_btn_force_hidden(false);
	topbar_set_hidden(false);
}

void flappybird_open(void) {
	release_all();

	frame = malloc((size_t)panel_w * panel_h * sizeof(uint16_t));
	bg_wide = malloc((size_t)ground_top * BG_W * scale * sizeof(uint16_t));
	ground_wide = malloc((size_t)GROUND_H * GROUND_W * scale * sizeof(uint16_t));
	if (!frame || !bg_wide || !ground_wide || !art_load()) {
		release_all();
		gui_notify_popup("flappybird_missing_files");
		return;
	}

	// Without sound the game still runs.
	sound_on = config_get_bool("flappybird", "sound", true);
	if (sound_on) {
		flappysound_start();
	}

	best = (int)config_get_int("flappybird", "best", 0);
	srand((unsigned)now_us());
	widen(ground_wide, &art_ground, 0, GROUND_H);
	find_flat_rows(&art_ground, 0, GROUND_H, ground_flat);
	ground_varied_rows = 0;
	for (int y = 0; y < GROUND_H; y++) {
		if (ground_flat[y] < 0) {
			ground_varied_rows = y + 1;
		}
	}
	world_px = bg_px = 0;
	go_title();
	// Opens out of black.
	fade_t = FADE_STEPS;
	fade_action = NULL;
	render(true);

	frame_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	frame_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
	frame_dsc.header.w = (uint32_t)panel_w;
	frame_dsc.header.h = (uint32_t)panel_h;
	frame_dsc.header.stride = (uint32_t)panel_w * 2;
	frame_dsc.data_size = (uint32_t)panel_w * panel_h * 2;
	frame_dsc.data = (const uint8_t *)frame;
	lv_image_set_src(frame_image, &frame_dsc);

	switch_screen(flappybird_screen);
}

void flappybird_init(gui_config_t *cfg) {
	scale = ui_px(4);
	if (scale < 2) {
		scale = 2;
	} else if (scale > MAX_SCALE) {
		scale = MAX_SCALE;
	}
	panel_w = (int)cfg->screen_width;
	panel_h = (int)cfg->screen_height;
	view_h = panel_h / scale;
	ground_top = view_h - GROUND_H;
	layout_shift = panel_h >= LAYOUT_H ? (panel_h - LAYOUT_H) / 2 : (panel_h - LAYOUT_H) * 3 / 4;
	// The background stands on the bottom of the panel.
	bg_row0 = BG_H - view_h;

	lv_obj_set_style_bg_color(flappybird_screen, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(flappybird_screen, LV_OPA_COVER, 0);
	lv_obj_remove_flag(flappybird_screen, LV_OBJ_FLAG_SCROLLABLE);

	frame_image = lv_image_create(flappybird_screen);
	lv_obj_set_pos(frame_image, 0, 0);
	lv_obj_set_size(frame_image, panel_w, panel_h);
	lv_obj_remove_flag(frame_image, LV_OBJ_FLAG_CLICKABLE);

	lv_obj_add_event_cb(flappybird_screen, press_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(flappybird_screen, release_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(flappybird_screen, release_cb, LV_EVENT_PRESS_LOST, NULL);
	lv_obj_add_event_cb(flappybird_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(flappybird_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

	tick_timer = lv_timer_create(tick_cb, TICK_MS, NULL);
	lv_timer_pause(tick_timer);
	power_pause_in_standby(tick_timer);
}

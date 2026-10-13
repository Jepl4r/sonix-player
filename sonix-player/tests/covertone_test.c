// Desktop check of the cover pipeline's pixel work (src/system/image/albumtone.c):
// the hue picked out of synthetic sleeves -- one colour, two colours, a grey
// one, a grey one with a speck, a photograph-like one -- the title hash, the
// square thumbnails, and what decoding and analysing a real JPEG costs at the
// reductions the pipeline asks for.
//
//   python3 tests/covertone_jpegs.py /tmp/covertone        # makes the JPEGs
//   cc -O2 -Wall -Wextra -I. tests/covertone_test.c src/system/image/albumtone.c
//      src/system/image/jpeg_scaled.c src/system/image/png_scaled.c src/system/image/stb_image_impl.c
//      -lm -lpthread -o /tmp/covertone_test           (one line)
//   /tmp/covertone_test /tmp/covertone
//
// Without a directory only the synthetic checks run. The timings are for this
// machine; the commit message and the report scale them to the device with the
// 400x400 full decode, which the device log times at about 70 ms.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/system/image/albumtone.h"
#include "src/system/image/jpeg_planes.h"
#include "src/system/image/jpeg_scaled.h"
#include "src/system/image/png_scaled.h"

static int failures;

#define CHECK(cond, ...)                                                                                              \
	do {                                                                                                              \
		if (!(cond)) {                                                                                                \
			failures++;                                                                                               \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                               \
			printf(__VA_ARGS__);                                                                                      \
			printf("\n");                                                                                             \
		}                                                                                                             \
	} while (0)

static double now_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

// The OKLab hue of one sRGB colour, for the expectations.
static float hue_of(uint8_t r, uint8_t g, uint8_t b) {
	float L, a, bb;
	albumtone_oklab(r, g, b, &L, &a, &bb);
	float h = atan2f(bb, a) * 180.0f / (float)M_PI;
	return h < 0 ? h + 360.0f : h;
}

static float hue_distance(float a, float b) {
	float d = fabsf(a - b);
	return d > 180.0f ? 360.0f - d : d;
}

typedef struct {
	uint8_t *px;
	int w, h;
} image_t;

static image_t image_new(int w, int h) {
	image_t im = {calloc((size_t)w * h * 3, 1), w, h};
	return im;
}

static void fill_rect(image_t *im, int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b) {
	for (int y = y0; y < y1; y++) {
		for (int x = x0; x < x1; x++) {
			uint8_t *p = im->px + ((size_t)y * im->w + x) * 3;
			p[0] = r;
			p[1] = g;
			p[2] = b;
		}
	}
}

// A little deterministic noise, as a JPEG and a photograph both have.
static void add_noise(image_t *im, int amplitude) {
	unsigned seed = 12345;
	for (size_t i = 0; i < (size_t)im->w * im->h * 3; i++) {
		seed = seed * 1103515245u + 12345u;
		int v = im->px[i] + (int)((seed >> 16) % (2 * amplitude + 1)) - amplitude;
		im->px[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
	}
}

static void expect_hue(const char *what, image_t *im, float want, float tolerance) {
	albumtone_t t;
	bool ok = albumtone_extract(im->px, im->w, im->h, &t);
	printf("%-28s coloured=%d hue=%6.1f C=%.3f L=%.3f share=%.2f (want %.1f)\n", what, t.coloured, t.hue, t.chroma,
		   t.light, t.share, want);
	CHECK(ok && t.coloured, "%s: no hue found", what);
	CHECK(hue_distance(t.hue, want) <= tolerance, "%s: hue %.1f, wanted %.1f +- %.1f", what, t.hue, want, tolerance);
}

static void expect_grey(const char *what, image_t *im) {
	albumtone_t t;
	bool ok = albumtone_extract(im->px, im->w, im->h, &t);
	printf("%-28s coloured=%d hue=%6.1f C=%.3f L=%.3f (want grey)\n", what, t.coloured, t.hue, t.chroma, t.light);
	CHECK(ok && !t.coloured, "%s: a hue where there is none (%.1f)", what, t.hue);
}

static void synthetic(void) {
	printf("--- hue of synthetic sleeves\n");
	image_t im = image_new(75, 75); // what a 600x600 cover decodes to at 1/8

	fill_rect(&im, 0, 0, 75, 75, 220, 30, 40);
	add_noise(&im, 6);
	expect_hue("red", &im, hue_of(220, 30, 40), 4);

	fill_rect(&im, 0, 0, 75, 75, 30, 60, 220);
	add_noise(&im, 6);
	expect_hue("blue", &im, hue_of(30, 60, 220), 4);

	// Half and half: one of the two, never the purple between them.
	fill_rect(&im, 0, 0, 75, 75, 220, 30, 40);
	fill_rect(&im, 0, 37, 75, 75, 30, 60, 220);
	{
		albumtone_t t;
		albumtone_extract(im.px, im.w, im.h, &t);
		float dr = hue_distance(t.hue, hue_of(220, 30, 40)), db = hue_distance(t.hue, hue_of(30, 60, 220));
		printf("%-28s coloured=%d hue=%6.1f C=%.3f L=%.3f share=%.2f (want red or blue, not between)\n",
			   "red+blue halves", t.coloured, t.hue, t.chroma, t.light, t.share);
		CHECK(t.coloured && (dr < 5 || db < 5), "red+blue: hue %.1f is neither", t.hue);
	}

	// A dull teal over most of the sleeve and a vivid orange over a third:
	// the vivid one is the sleeve's colour.
	fill_rect(&im, 0, 0, 75, 75, 90, 130, 125);
	fill_rect(&im, 0, 50, 75, 75, 250, 120, 20);
	add_noise(&im, 4);
	expect_hue("dull teal + vivid orange", &im, hue_of(250, 120, 20), 6);

	// Grey: a ramp from black to white, with noise.
	for (int y = 0; y < 75; y++) {
		uint8_t v = (uint8_t)(y * 255 / 74);
		fill_rect(&im, 0, y, 75, y + 1, v, v, v);
	}
	add_noise(&im, 5);
	expect_grey("greyscale ramp", &im);

	// A grey photograph with a red speck of 1%: still grey.
	fill_rect(&im, 0, 0, 75, 75, 128, 128, 128);
	add_noise(&im, 8);
	fill_rect(&im, 30, 30, 37, 38, 230, 20, 30); // 56 px of 5625
	expect_grey("grey + 1% red speck", &im);

	// The same with a red patch of 6%: now it is noticed, and it wins.
	fill_rect(&im, 20, 20, 41, 37, 230, 20, 30);
	expect_hue("grey + 6% red patch", &im, hue_of(230, 20, 30), 5);

	// Sepia, the near-grey that is almost a colour: weakly coloured either way,
	// but never a confident blue or green.
	fill_rect(&im, 0, 0, 75, 75, 140, 120, 95);
	add_noise(&im, 5);
	{
		albumtone_t t;
		albumtone_extract(im.px, im.w, im.h, &t);
		printf("%-28s coloured=%d hue=%6.1f C=%.3f L=%.3f\n", "sepia", t.coloured, t.hue, t.chroma, t.light);
		CHECK(!t.coloured || hue_distance(t.hue, 70) < 30, "sepia: hue %.1f", t.hue);
	}
	free(im.px);

	// Photograph-like, at the size a 1500x1500 cover decodes to at 1/8: a pale
	// sky over half, a green field under it, a red figure, shading and noise.
	image_t ph = image_new(187, 187);
	for (int y = 0; y < 94; y++) {
		fill_rect(&ph, 0, y, 187, y + 1, (uint8_t)(150 + y / 4), (uint8_t)(180 + y / 5), (uint8_t)(215 - y / 6));
	}
	for (int y = 94; y < 187; y++) {
		fill_rect(&ph, 0, y, 187, y + 1, (uint8_t)(70 - (y - 94) / 4), (uint8_t)(130 - (y - 94) / 3), 40);
	}
	fill_rect(&ph, 80, 60, 105, 150, 210, 35, 35); // ~6.4%
	add_noise(&ph, 10);
	expect_hue("photo: sky, field, figure", &ph, hue_of(60, 115, 40), 12);
	free(ph.px);

	printf("--- title hash\n");
	uint16_t h1 = albumtone_hash_hue("The Dark Side of the Moon");
	uint16_t h2 = albumtone_hash_hue("THE DARK SIDE OF THE MOON");
	uint16_t h3 = albumtone_hash_hue("The Wall");
	printf("hash hues: %u %u %u, empty %u, Кино %u\n", h1, h2, h3, albumtone_hash_hue(""),
		   albumtone_hash_hue("Группа крови"));
	CHECK(h1 == h2, "case folding");
	CHECK(h1 < 360 && h3 < 360, "range");
	CHECK(h1 != h3, "two titles, one hue (unlucky but possible: check the hash)");
	// Spread: a thousand titles should land in most of 36 sectors.
	int sectors[36] = {0};
	char title[32];
	for (int i = 0; i < 1000; i++) {
		snprintf(title, sizeof(title), "Album %d", i);
		sectors[albumtone_hash_hue(title) / 10]++;
	}
	int empty = 0, most = 0;
	for (int i = 0; i < 36; i++) {
		empty += sectors[i] == 0;
		most = sectors[i] > most ? sectors[i] : most;
	}
	printf("1000 titles over 36 sectors: %d empty, fullest %d (even would be 28)\n", empty, most);
	CHECK(empty == 0 && most < 60, "hash spread");

	printf("--- square thumbnails\n");
	image_t sq = image_new(300, 200);
	fill_rect(&sq, 0, 0, 300, 200, 255, 0, 0);
	fill_rect(&sq, 0, 0, 50, 200, 0, 0, 255); // cropped away: the centre square is x 50..250
	fill_rect(&sq, 250, 0, 300, 200, 0, 0, 255);
	uint16_t out[240 * 240];
	CHECK(albumtone_square565(sq.px, sq.w, sq.h, 56, out), "56 from 300x200");
	int wrong = 0;
	for (int i = 0; i < 56 * 56; i++) {
		wrong += out[i] != 0xF800;
	}
	CHECK(wrong == 0, "centre crop of red: %d pixels not red", wrong);
	// Upscaling path: 40x40 checkerboard of black/white 2x2 blocks to 240.
	image_t small = image_new(40, 40);
	for (int y = 0; y < 40; y++) {
		for (int x = 0; x < 40; x++) {
			uint8_t v = ((x / 2 + y / 2) & 1) ? 255 : 0;
			fill_rect(&small, x, y, x + 1, y + 1, v, v, v);
		}
	}
	CHECK(albumtone_square565(small.px, 40, 40, 240, out), "240 from 40x40");
	// Output (20, 8) samples between source pixels 2 and 3 of row 0-1: both white.
	CHECK(out[0] == 0x0000 && out[8 * 240 + 20] == 0xFFFF, "upscale keeps the blocks (%04x %04x)", out[0],
		  out[8 * 240 + 20]);
	// Downscale of the same checkerboard to 10 is a flat mid grey.
	CHECK(albumtone_square565(small.px, 40, 40, 10, out), "10 from 40x40");
	CHECK(out[55] == 0x8410 || out[55] == 0x7BEF, "checkerboard averages to grey (%04x)", out[55]);
	free(sq.px);
	free(small.px);
}

static uint8_t *read_file(const char *path, size_t *size) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *data = malloc((size_t)n);
	if (data && fread(data, 1, (size_t)n, f) != (size_t)n) {
		free(data);
		data = NULL;
	}
	fclose(f);
	*size = (size_t)n;
	return data;
}

// Decodes `name` the way covertone does at scan time (tone + 56) and in the
// background (240), `rounds` times each, and prints the cost of each step.
static void time_file(const char *dir, const char *name, int rounds) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	size_t size = 0;
	uint8_t *data = read_file(path, &size);
	if (!data) {
		printf("%s: missing\n", path);
		failures++;
		return;
	}
	bool progressive = false;
	int w = 0, h = 0, sw = 0, sh = 0;
	uint8_t *rgb = jpeg_scaled_decode(data, size, 28, &w, &h, &sw, &sh);
	if (!rgb) {
		progressive = true;
		size_t peak = 0;
		rgb = jpeg_planes_decode(data, size, 112, 4 * 1024 * 1024, &w, &h, &peak);
		printf("%s: progressive, planes peak %zu KB\n", name, peak / 1024);
	}
	CHECK(rgb != NULL, "%s does not decode", name);
	if (!rgb) {
		free(data);
		return;
	}
	free(rgb);

	double t_decode = 0, t_tone = 0, t_56 = 0, t_decode240 = 0, t_240 = 0;
	uint16_t *thumb = malloc(240 * 240 * 2);
	albumtone_t tone;
	int w240 = 0, h240 = 0;
	for (int i = 0; i < rounds; i++) {
		double t0 = now_ms();
		rgb = progressive ? jpeg_planes_decode(data, size, 112, 4 * 1024 * 1024, &w, &h, NULL)
						  : jpeg_scaled_decode(data, size, 28, &w, &h, NULL, NULL);
		double t1 = now_ms();
		albumtone_extract(rgb, w, h, &tone);
		double t2 = now_ms();
		albumtone_square565(rgb, w, h, 56, thumb);
		double t3 = now_ms();
		free(rgb);
		rgb = progressive ? jpeg_planes_decode(data, size, 480, 4 * 1024 * 1024, &w240, &h240, NULL)
						  : jpeg_scaled_decode(data, size, 120, &w240, &h240, NULL, NULL);
		double t4 = now_ms();
		albumtone_square565(rgb, w240, h240, 240, thumb);
		double t5 = now_ms();
		free(rgb);
		t_decode += t1 - t0;
		t_tone += t2 - t1;
		t_56 += t3 - t2;
		t_decode240 += t4 - t3;
		t_240 += t5 - t4;
	}
	free(thumb);
	free(data);
	printf("%-16s %4dx%-4d %6zu KB -> %3dx%-3d: decode %6.2f ms, tone %5.2f ms, 56 px %5.2f ms | -> %3dx%-3d: decode "
		   "%6.2f ms, 240 px %5.2f ms | hue %.0f%s\n",
		   name, sw, sh, size / 1024, w, h, t_decode / rounds, t_tone / rounds, t_56 / rounds, w240, h240,
		   t_decode240 / rounds, t_240 / rounds, tone.hue, tone.coloured ? "" : " (grey)");
}

// The calibration point: a 400x400 baseline JPEG at 1:1, which the device log
// shows taking about 70 ms (09:55:39.069 play requested -> .143 decoded).
static void time_calibration(const char *dir) {
	char path[512];
	snprintf(path, sizeof(path), "%s/cal400.jpg", dir);
	size_t size = 0;
	uint8_t *data = read_file(path, &size);
	if (!data) {
		return;
	}
	double t = 0;
	int rounds = 50, w, h;
	for (int i = 0; i < rounds; i++) {
		double t0 = now_ms();
		uint8_t *rgb = jpeg_scaled_decode(data, size, 400, &w, &h, NULL, NULL);
		t += now_ms() - t0;
		free(rgb);
	}
	free(data);
	printf("calibration: 400x400 at 1:1 (%dx%d) %.2f ms here\n", w, h, t / rounds);
}

int main(int argc, char **argv) {
	synthetic();
	if (argc > 1) {
		printf("--- decode and analysis cost (mean of 20)\n");
		time_calibration(argv[1]);
		const char *names[] = {"photo600.jpg", "photo1500.jpg", "photo3000.jpg", "flat600.jpg", "prog1200.jpg"};
		for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
			time_file(argv[1], names[i], 20);
		}
	}
	printf(failures ? "%d FAILED\n" : "all passed\n", failures);
	return failures ? 1 : 0;
}

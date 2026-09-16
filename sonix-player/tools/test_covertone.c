// Il colore del bagliore sotto la copertina in Cover Flow.
//
// Il banco esiste per un difetto preciso: le copertine in bianco e nero
// accendevano un bagliore verde. La causa stava nell'espansione di un pixel
// RGB565 -- spostando i campi da cinque e sei bit in su di tre e di due, il
// verde resta un gradino avanti a rosso e blu su ogni grigio che esista, e la
// saturazione applicata dopo prende quel gradino e lo porta a fondo scala.
//
// Quindi qui si prova soprattutto il grigio, in tutte le forme in cui arriva da
// una copertina vera: neutro, con il rumore di compressione addosso, chiaro,
// scuro. E si prova che una copertina a colori conserva la sua tinta.
//
// Uso: run_covertone_bench.sh
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Quel tanto di cover_image_t che dominant_tone() guarda.
typedef struct {
	struct {
		struct {
			uint32_t w, h, stride;
		} header;
	} dsc;
	const uint8_t *pixels;
} cover_image_t;

#include "covertone_extracted.h"

static int checks, failures;

static void ok(bool condition, const char *what) {
	checks++;
	if (condition) {
		printf("  ok       %s\n", what);
	} else {
		failures++;
		printf("  FALLITO  %s\n", what);
	}
}

#define IMG_W 64
#define IMG_H 64

static uint16_t buffer[IMG_W * IMG_H];

static uint16_t pack(unsigned r, unsigned g, unsigned b) {
	return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static cover_image_t image_of(void) {
	cover_image_t img;
	img.dsc.header.w = IMG_W;
	img.dsc.header.h = IMG_H;
	img.dsc.header.stride = IMG_W * 2;
	img.pixels = (const uint8_t *)buffer;
	return img;
}

// Un grigio uniforme, con `noise` unita' di scarto per canale a scacchiera --
// che e' quello che il JPEG lascia su una fotografia in bianco e nero.
static void fill_grey(unsigned v, int noise) {
	for (int y = 0; y < IMG_H; y++) {
		for (int x = 0; x < IMG_W; x++) {
			int n = ((x + y) % 3) - 1; // -1, 0, +1
			int r = (int)v + n * noise;
			int g = (int)v - n * noise;
			int b = (int)v + n * noise;
			r = r < 0 ? 0 : (r > 255 ? 255 : r);
			g = g < 0 ? 0 : (g > 255 ? 255 : g);
			b = b < 0 ? 0 : (b > 255 ? 255 : b);
			buffer[y * IMG_W + x] = pack((unsigned)r, (unsigned)g, (unsigned)b);
		}
	}
}

// Una copertina a colori: una tinta su meta' e grigio sull'altra, come una
// fotografia con molto sfondo neutro.
static void fill_tinted(unsigned r, unsigned g, unsigned b, unsigned grey) {
	for (int y = 0; y < IMG_H; y++) {
		for (int x = 0; x < IMG_W; x++) {
			bool tint = (x < IMG_W / 2);
			buffer[y * IMG_W + x] = tint ? pack(r, g, b) : pack(grey, grey, grey);
		}
	}
}

static unsigned chan(uint32_t tone, int shift) { return (tone >> shift) & 0xFF; }

static int spread_of(uint32_t tone) {
	unsigned r = chan(tone, 16), g = chan(tone, 8), b = chan(tone, 0);
	unsigned hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
	unsigned lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
	return (int)(hi - lo);
}

static bool is_mono(uint32_t tone) { return tone == TONE_MONO; }

static void grey_case(unsigned v, int noise, const char *label) {
	fill_grey(v, noise);
	cover_image_t img = image_of();
	uint32_t tone = cover_dominant_tone(&img);
	char what[160];
	snprintf(what, sizeof(what), "%s -> %06x, niente colore inventato", label, (unsigned)tone);
	ok(is_mono(tone), what);
}

int main(void) {
	setvbuf(stdout, NULL, _IOLBF, 0);

	printf("\n-- grigi puri: nessuno deve tirare fuori una tinta\n");
	grey_case(255, 0, "bianco");
	grey_case(204, 0, "grigio chiaro");
	grey_case(153, 0, "grigio medio");
	grey_case(102, 0, "grigio scuro");
	grey_case(51, 0, "quasi nero");

	printf("\n-- grigi con il rumore della compressione addosso\n");
	grey_case(200, 2, "grigio chiaro, +/-2");
	grey_case(128, 3, "grigio medio, +/-3");
	grey_case(90, 4, "grigio scuro, +/-4");

	printf("\n-- una copertina a colori tiene la sua tinta\n");
	struct {
		unsigned r, g, b;
		const char *name;
		int dominant; // 16 rosso, 8 verde, 0 blu
	} tints[] = {
		{200, 40, 40, "rossa", 16},
		{40, 160, 60, "verde", 8},
		{40, 60, 200, "blu", 0},
		{220, 160, 40, "ambra", 16},
	};
	for (size_t i = 0; i < sizeof(tints) / sizeof(tints[0]); i++) {
		fill_tinted(tints[i].r, tints[i].g, tints[i].b, 128);
		cover_image_t img = image_of();
		uint32_t tone = cover_dominant_tone(&img);
		unsigned r = chan(tone, 16), g = chan(tone, 8), b = chan(tone, 0);
		unsigned want = chan(tone, tints[i].dominant);
		bool wins = want >= r && want >= g && want >= b;
		char what[160];
		snprintf(what, sizeof(what), "copertina %s -> %06x, il canale giusto e' il piu' forte", tints[i].name,
				 (unsigned)tone);
		ok(wins && !is_mono(tone), what);

		snprintf(what, sizeof(what), "copertina %s -> il bagliore e' acceso (scarto %d)", tints[i].name,
				 spread_of(tone));
		ok(spread_of(tone) >= 60, what);
	}

	printf("\n-- casi limite\n");
	{
		cover_image_t img = image_of();
		img.pixels = NULL;
		ok(cover_dominant_tone(&img) == 0, "senza pixel non risponde niente");
	}
	{
		fill_grey(128, 0);
		cover_image_t img = image_of();
		img.dsc.header.w = 0;
		ok(cover_dominant_tone(&img) == 0, "larghezza zero non risponde niente");
	}

	printf("\n%d controlli, %d falliti\n", checks, failures);
	return failures ? 1 : 0;
}

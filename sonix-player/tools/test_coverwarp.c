// Banco prova della proiezione prospettica delle cover.
//
// Misura il codice vero: tools/extract_coverwarp.py tira fuori da
// src/gui/coverflow.c le costanti, height_to_far() e warp_square() e le
// mette in un header che questo file include. Se cambia la geometria nel
// player, cambia anche quella misurata qui.
//
// Cosa si vuole dimostrare, che sono i tre difetti che il lettore ha rifiutato
// uno dopo l'altro:
//
//   NON e' uno schiacciamento. Uno schiacciamento e' una scala affine -- bordi
//   paralleli, righe della sorgente distribuite in parti uguali. Una proiezione
//   no: il bordo lontano e' piu' stretto, la larghezza scende in modo lineare
//   fra i due (i lati di un quadrato proiettato restano rette), e la meta'
//   vicina dello schermo mostra MENO della meta' della figura perche' e'
//   ingrandita;
//
//   il verso e' quello giusto. Il disco SOTTO a quello centrale ha il bordo
//   largo in BASSO -- quello lontano da chi guarda e' il bordo che punta al
//   centro, cosi' i bordi interni convergono. E la figura non e' capovolta;
//
//   NON e' a scatti. Fra dritto e completamente girato non ci sono gradini: si
//   deforma a ogni altezza intera, e da un'altezza alla successiva niente
//   salta.
//
// Misura anche quanto costa, perche' e' l'unico numero che decide se questa
// pagina si puo' fare: due dischi per fotogramma, di altezze che sommano sempre
// allo stesso valore.
//
// Scrive /tmp/coverwarp_<altezza>_<sopra|sotto>.ppm per guardarle.
//
// Uso: tools/run_coverwarp_bench.sh

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "coverwarp_extracted.h"

static int checks;
static int failures;

static void ok(bool cond, const char *fmt, ...) {
	va_list ap;
	checks++;
	if (!cond) {
		failures++;
	}
	printf(cond ? "  ok   " : "  FAIL ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

// Una sorgente quadrata con una griglia fitta: le righe orizzontali servono a
// vedere dove finiscono, che e' tutto il punto della prospettiva. La meta' in
// alto e' verde e quella in basso blu, cosi' si vede subito se e' capovolta.
static uint16_t *make_source(void) {
	uint16_t *px = malloc((size_t)CF_MID_W * CF_MID_W * 2);
	for (int y = 0; y < CF_MID_W; y++) {
		for (int x = 0; x < CF_MID_W; x++) {
			bool line = (y % 10) == 0 || (x % 10) == 0;
			px[y * CF_MID_W + x] = line ? 0xFFFF : (y < CF_MID_W / 2 ? 0x07E0 : 0x001F);
		}
	}
	return px;
}

static const uint8_t *alpha_row(const uint8_t *buf, int height, int y) {
	return buf + (size_t)CF_MID_W * height * 2 + (size_t)y * CF_MID_W;
}

// Quanto e' larga la riga, in pixel interi.
//
// Non "quanti pixel non sono trasparenti": i due bordi obliqui sono sfumati, e
// il pixel di bordo vale la frazione di se stesso che il trapezio copre. La
// somma delle alfa e' quindi la larghezza vera, con i decimi.
static int opaque_width(const uint8_t *buf, int height, int y, int *first) {
	const uint8_t *alpha = alpha_row(buf, height, y);
	int coverage = 0;
	int f = -1;
	for (int x = 0; x < CF_MID_W; x++) {
		coverage += alpha[x];
		if (alpha[x] == 0xFF && f < 0) {
			f = x;
		}
	}
	if (first) {
		*first = f;
	}
	return (coverage + 127) / 255;
}

// La stessa larghezza, in sedicesimi: serve al controllo della continuita', dove
// arrotondare a pixel interi trasformerebbe un passo vero di sette decimi in un
// salto misurato di due.
static int width16(const uint8_t *buf, int height, int y) {
	const uint8_t *alpha = alpha_row(buf, height, y);
	int coverage = 0;
	for (int x = 0; x < CF_MID_W; x++) {
		coverage += alpha[x];
	}
	return coverage * 16 / 255;
}

// Quante righe hanno un bordo sfumato invece che netto.
static int soft_edges(const uint8_t *buf, int height) {
	int soft = 0;
	for (int y = 0; y < height; y++) {
		const uint8_t *alpha = alpha_row(buf, height, y);
		for (int x = 0; x < CF_MID_W; x++) {
			if (alpha[x] != 0 && alpha[x] != 0xFF) {
				soft++;
				break;
			}
		}
	}
	return soft;
}

static void dump_ppm(const uint8_t *buf, int height, const char *path) {
	FILE *f = fopen(path, "wb");
	if (!f) {
		return;
	}
	fprintf(f, "P6\n%d %d\n255\n", CF_MID_W, height);
	const uint16_t *rgb = (const uint16_t *)buf;
	const uint8_t *alpha = buf + (size_t)CF_MID_W * height * 2;
	for (int i = 0; i < CF_MID_W * height; i++) {
		uint16_t c = rgb[i];
		unsigned r = ((c >> 11) & 0x1F) * 255 / 31;
		unsigned g = ((c >> 5) & 0x3F) * 255 / 63;
		unsigned b = (c & 0x1F) * 255 / 31;
		if (!alpha[i]) {
			r = g = b = 40; // il fondo della pagina, per vedere il ritaglio
		}
		fputc((int)r, f);
		fputc((int)g, f);
		fputc((int)b, f);
	}
	fclose(f);
}

int main(void) {
	uint16_t *src = make_source();
	uint8_t *buf = malloc((size_t)CF_MID_W * CF_MID_W * 3);
	uint8_t *buf2 = malloc((size_t)CF_MID_W * CF_MID_W * 3);
	const int FULL = CF_LEAN_H; // altezza del disco completamente girato

	printf("l'angolo, che viene tutto dall'altezza\n");
	ok(height_to_far(CF_MID_W) == 65536, "un disco alto quanto e' largo e' dritto");
	ok((height_to_far(CF_LEAN_H) * 100 + 32768) / 65536 == CF_FAR_PCT,
	   "alla minima altezza il bordo lontano e' al %d%%", CF_FAR_PCT);
	bool monotone_angle = true;
	int distinct = 0;
	for (int h = CF_LEAN_H + 1; h <= CF_MID_W; h++) {
		if (height_to_far(h) < height_to_far(h - 1)) {
			monotone_angle = false;
		}
		if (height_to_far(h) != height_to_far(h - 1)) {
			distinct++;
		}
	}
	ok(monotone_angle, "l'angolo si apre senza mai tornare indietro");
	ok(distinct == CF_MID_W - CF_LEAN_H, "e ha un valore diverso per ognuna delle %d altezze", distinct);

	printf("\nil disco dritto: nessun trucco, e nessuna divisione per zero\n");
	warp_square(src, CF_MID_W, false, buf);
	int flat_top = opaque_width(buf, CF_MID_W, 0, NULL);
	int flat_bottom = opaque_width(buf, CF_MID_W, CF_MID_W - 1, NULL);
	ok(flat_top == CF_MID_W && flat_bottom == CF_MID_W, "e' un quadrato pieno, %d px per riga", flat_top);
	{
		// Il piano non e' inclinato, quindi non ha un bordo lontano verso cui
		// spegnersi: il colore resta quello della sorgente.
		const uint16_t *out = (const uint16_t *)buf;
		int same = 0;
		for (int i = 0; i < CF_MID_W * CF_MID_W; i++) {
			if (out[i] == src[i]) {
				same++;
			}
		}
		ok(same > CF_MID_W * CF_MID_W * 99 / 100, "e la figura passa intatta (%d%% dei pixel identici)",
		   same * 100 / (CF_MID_W * CF_MID_W));
	}

	printf("\nil verso: il disco SOTTO ha il bordo largo in basso\n");
	warp_square(src, FULL, true, buf);   // sotto al centro
	warp_square(src, FULL, false, buf2); // sopra al centro
	int below_top = opaque_width(buf, FULL, 0, NULL);
	int below_bottom = opaque_width(buf, FULL, FULL - 1, NULL);
	int want_far = CF_MID_W * CF_FAR_PCT / 100;
	ok(below_bottom > below_top, "sotto: %d px in basso, %d in alto", below_bottom, below_top);
	ok(below_bottom >= CF_MID_W - 2, "il bordo vicino e' largo quanto il disco centrale (%d)", below_bottom);
	ok(below_top <= want_far + 2 && below_top >= want_far - 2, "il bordo lontano e' largo %d, atteso %d",
	   below_top, want_far);
	ok(opaque_width(buf2, FULL, 0, NULL) > opaque_width(buf2, FULL, FULL - 1, NULL),
	   "sopra e' il contrario, come deve essere");

	printf("\nla figura sta per il verso giusto (sorgente verde sopra, blu sotto)\n");
	for (int pass = 0; pass < 2; pass++) {
		const uint16_t *rgb = (const uint16_t *)(pass ? buf2 : buf);
		uint16_t hi = rgb[(size_t)(FULL / 8) * CF_MID_W + CF_MID_W / 2];
		uint16_t lo = rgb[(size_t)(FULL - 1 - FULL / 8) * CF_MID_W + CF_MID_W / 2];
		bool hi_green = ((hi >> 5) & 0x3F) > ((hi & 0x1F) * 2);
		bool lo_blue = (lo & 0x1F) > (((lo >> 5) & 0x3F) / 2);
		ok(hi_green && lo_blue, "%s: meta' di sopra verde, meta' di sotto blu", pass ? "sopra" : "sotto");
	}

	printf("\nnon e' uno schiacciamento\n");
	bool constant = true;
	for (int y = 1; y < FULL; y++) {
		if (opaque_width(buf, FULL, y, NULL) != below_bottom) {
			constant = false;
			break;
		}
	}
	ok(!constant, "le righe NON sono tutte larghe uguali");

	bool monotone = true;
	bool centred = true;
	int max_bend = 0;
	int prev = -1;
	for (int y = 0; y < FULL; y++) {
		int f;
		int w = opaque_width(buf, FULL, y, &f);
		if (prev >= 0 && w < prev) {
			monotone = false;
		}
		prev = w;
		if (f * 2 + w < CF_MID_W - 4 || f * 2 + w > CF_MID_W + 4) {
			centred = false;
		}
		int want = below_top + (below_bottom - below_top) * y / (FULL - 1);
		int bend = w > want ? w - want : want - w;
		if (bend > max_bend) {
			max_bend = bend;
		}
	}
	ok(monotone, "la larghezza cresce sempre andando verso il bordo vicino");
	ok(centred, "ogni riga e' centrata sull'asse del disco");
	ok(max_bend <= 2, "i lati sono rette: scarto massimo %d px", max_bend);
	{
		int soft = soft_edges(buf, FULL);
		ok(soft >= FULL * 3 / 4, "e sono sfumati, non a gradini: %d righe su %d hanno un bordo parziale",
		   soft, FULL);
	}

	printf("\nla prospettiva: la meta' vicina e' ingrandita, la lontana compressa\n");
	{
		// Dove finisce la riga di mezzo della figura, contata dal bordo lontano.
		// Con uno schiacciamento sarebbe esattamente a meta'.
		// Dove cade il confine: v(t) = 0.5 si risolve in t = 1/(1+s). Con uno
		// schiacciamento sarebbe a meta' esatta.
		double s = height_to_far(FULL) / 65536.0;
		double want = 1.0 / (1.0 + s);
		// La si trova cercando dove passa il confine verde/blu della sorgente.
		int boundary = -1;
		const uint16_t *rgb = (const uint16_t *)buf2; // sopra: bordo vicino in alto
		for (int y = 1; y < FULL; y++) {
			uint16_t c = rgb[(size_t)y * CF_MID_W + CF_MID_W / 2];
			if ((c & 0x1F) > (((c >> 5) & 0x3F) / 2)) {
				boundary = y;
				break;
			}
		}
		double frac = boundary / (double)FULL;
		ok(boundary > 0, "il confine fra le due meta' della figura si trova, alla riga %d di %d", boundary,
		   FULL);
		ok(frac > want - 0.03 && frac < want + 0.03,
		   "e cade al %.0f%% dell'altezza e non al 50%%, perche' la meta' vicina e' ingrandita (atteso %.0f%%)",
		   frac * 100, want * 100);
	}

	printf("\nl'ombra\n");
	{
		// Il bianco della griglia, letto vicino ai due bordi.
		const uint16_t *rgb = (const uint16_t *)buf2;
		unsigned near_max = 0, far_max = 0;
		for (int x = CF_MID_W / 3; x < 2 * CF_MID_W / 3; x++) {
			unsigned n = rgb[(size_t)1 * CF_MID_W + x] & 0x1F;
			unsigned f = rgb[(size_t)(FULL - 2) * CF_MID_W + x] & 0x1F;
			if (n > near_max) near_max = n;
			if (f > far_max) far_max = f;
		}
		ok(near_max > far_max, "il bordo lontano e' piu' scuro del vicino (%u contro %u)", far_max, near_max);
	}

	printf("\nnon e' a scatti: si deforma a ogni altezza\n");
	{
		int worst = 0;
		int worst_h = 0;
		int prev_far = -1;
		bool shrinking = true;
		for (int h = CF_MID_W; h >= CF_LEAN_H; h--) {
			warp_square(src, h, true, buf);
			int far = width16(buf, h, 0);
			if (prev_far >= 0) {
				if (far > prev_far) {
					shrinking = false;
				}
				int jump = prev_far - far;
				if (jump > worst) {
					worst = jump;
					worst_h = h;
				}
			}
			prev_far = far;
		}
		ok(shrinking, "da %d a %d px di altezza il bordo lontano si stringe sempre", CF_MID_W, CF_LEAN_H);
		// 120 altezze per 80 px di bordo: due terzi di pixel a passo. Misurato in
		// sedicesimi, perche' e' li' che si vede che il bordo si muove davvero a
		// ogni altezza e non a scatti di uno.
		ok(worst <= 16, "e il salto piu' grande fra due altezze e' %d/16 di px (a %d)", worst, worst_h);
	}

	printf("\nquanto costa un fotogramma\n");
	{
		// Solo due dischi sono a meta' della loro rotazione: quello sopra il
		// centro e quello sotto. Le loro altezze sommano sempre a CF_MID_W +
		// CF_LEAN_H, quindi il lavoro per fotogramma e' una costante.
		int n = 400;
		struct timespec a, b;
		clock_gettime(CLOCK_MONOTONIC, &a);
		for (int i = 0; i < n; i++) {
			int h1 = CF_LEAN_H + (i * (CF_MID_W - CF_LEAN_H)) / n;
			int h2 = CF_MID_W + CF_LEAN_H - h1;
			warp_square(src, h1, false, buf);
			warp_square(src, h2, true, buf2);
		}
		clock_gettime(CLOCK_MONOTONIC, &b);
		double ms = ((b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6) / n;
		long px = (long)CF_MID_W * (CF_MID_W + CF_LEAN_H);
		printf("       %ld px per fotogramma, %.3f ms su questa macchina (%.0f Mpx/s)\n", px, ms,
			   px / ms / 1000.0);
		ok(ms < 1.0, "sotto il millisecondo qui, quindi un ordine di grandezza di margine sul lettore");
	}

	printf("\nil ritaglio\n");
	warp_square(src, FULL, true, buf);
	ok(alpha_row(buf, FULL, 0)[0] == 0, "l'angolo fuori dal trapezio e' trasparente");
	ok(alpha_row(buf, FULL, FULL / 2)[CF_MID_W / 2] == 0xFF, "il centro e' pieno");

	for (int h = CF_LEAN_H; h <= CF_MID_W; h += (CF_MID_W - CF_LEAN_H) / 3) {
		char path[64];
		warp_square(src, h, true, buf);
		snprintf(path, sizeof(path), "/tmp/coverwarp_%d_sotto.ppm", h);
		dump_ppm(buf, h, path);
		warp_square(src, h, false, buf);
		snprintf(path, sizeof(path), "/tmp/coverwarp_%d_sopra.ppm", h);
		dump_ppm(buf, h, path);
	}

	free(src);
	free(buf);
	free(buf2);

	printf("\n%d controlli, %d falliti\n", checks, failures);
	return failures ? 1 : 0;
}

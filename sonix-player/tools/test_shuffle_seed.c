// Che la scelta casuale del brano da cui parte un mescolamento cambi davvero
// da un avvio all'altro.
//
// Il difetto che ha fatto nascere questo banco: lv_init() semina il generatore
// di LVGL, ma con una costante scritta nel sorgente (0x1234ABCD). Il seme e'
// quindi lo stesso a ogni accensione, e lv_rand() e' deterministico: la prima
// estrazione dopo un avvio da' sempre lo stesso numero. E' lv_rand() a scegliere
// il brano da cui parte "riproduci in ordine casuale", quindi su una lista di
// una data lunghezza il mescolamento partiva sempre dalla stessa posizione --
// casuale una volta, e mai piu'. La coda mescola per conto suo con srand() di
// libc (playlist.c, seed_once), quindi solo il punto di partenza era prevedibile.
//
// Il banco fa due cose: dimostra il difetto (dopo lv_init il generatore e' in
// uno stato noto e riproducibile) e controlla che il prodotto lo eviti (main.c
// risemina dall'orologio, dopo lv_init e non prima).
//
//   tools/run_shuffle_seed_bench.sh

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

// lv_conf.h dell'albero punta il font di default a questo simbolo, che il player
// riempie da FreeType. Qui non si disegna niente: basta che esista, altrimenti
// non si linka nemmeno.
lv_font_t font_ui_14;

// Il seme che lv_init() scrive (lvgl/src/lv_init.c). Se LVGL lo cambia, il
// primo controllo fallisce, ed e' giusto cosi': vuol dire che questo banco
// stava dicendo una cosa non piu' vera.
#define LV_INIT_SEED 0x1234ABCD

#define DRAWS 200
#define RANGE 12 // una playlist corta: il caso in cui il difetto si vede

static int failures;
static int checks;

static void ok(bool condition, const char *what) {
	checks++;
	if (!condition) {
		failures++;
		printf("  FALLITO  %s\n", what);
	} else {
		printf("  ok       %s\n", what);
	}
}

// Quanti valori distinti escono in DRAWS estrazioni su 0..RANGE-1.
static int distinct_draws(void) {
	bool seen[RANGE];
	memset(seen, 0, sizeof(seen));
	for (int i = 0; i < DRAWS; i++) {
		uint32_t v = lv_rand(0, RANGE - 1);
		if (v < RANGE) {
			seen[v] = true;
		}
	}
	int n = 0;
	for (int i = 0; i < RANGE; i++) {
		n += seen[i] ? 1 : 0;
	}
	return n;
}

int main(void) {
	setvbuf(stdout, NULL, _IOLBF, 0);

	lv_init();

	printf("\n-- il difetto: dopo lv_init il generatore parte da un seme fisso\n");
	uint32_t fresh[8];
	for (int i = 0; i < 8; i++) {
		fresh[i] = lv_rand(0, 999);
	}
	// La costante e' quella di lv_init(): se la sequenza si ripete identica
	// partendo da li', allora lv_init non fa altro che questo, e ogni avvio
	// comincia dallo stesso punto.
	lv_rand_set_seed(LV_INIT_SEED);
	bool fixed = true;
	for (int i = 0; i < 8; i++) {
		fixed = fixed && lv_rand(0, 999) == fresh[i];
	}
	ok(fixed, "lv_init lascia sempre lo stesso stato (seme costante)");

	printf("\n-- il generatore comunque funziona: copre l'intervallo\n");
	lv_rand_set_seed(LV_INIT_SEED);
	int spread = distinct_draws();
	printf("      %d valori distinti su %d in %d estrazioni\n", spread, RANGE, DRAWS);
	ok(spread >= RANGE - 1, "non e' bloccato su un valore solo");

	printf("\n-- due semi diversi non danno la stessa sequenza\n");
	lv_rand_set_seed(1);
	uint32_t a[8];
	for (int i = 0; i < 8; i++) {
		a[i] = lv_rand(0, 999);
	}
	lv_rand_set_seed(2);
	bool same = true;
	for (int i = 0; i < 8; i++) {
		same = same && lv_rand(0, 999) == a[i];
	}
	ok(!same, "sequenze diverse");

	printf("\n-- lo stesso seme la ridà uguale (e' un generatore, non rumore)\n");
	lv_rand_set_seed(1);
	bool repeats = true;
	for (int i = 0; i < 8; i++) {
		repeats = repeats && lv_rand(0, 999) == a[i];
	}
	ok(repeats, "stesso seme, stessa sequenza");

	printf("\n-- e il prodotto lo semina davvero\n");
	FILE *f = fopen("src/main.c", "r");
	bool seeds = false;
	bool seeds_before_lv_init = false;
	bool seen_lv_init = false;
	if (f) {
		char line[512];
		while (fgets(line, sizeof(line), f)) {
			if (strstr(line, "lv_init()")) {
				seen_lv_init = true;
			}
			if (strstr(line, "lv_rand_set_seed(") && !strstr(line, "//")) {
				seeds = true;
				seeds_before_lv_init = !seen_lv_init;
			}
		}
		fclose(f);
	}
	ok(f != NULL, "src/main.c letto");
	ok(seeds, "main.c chiama lv_rand_set_seed");
	ok(!seeds_before_lv_init, "e lo fa dopo lv_init(), che altrimenti lo sovrascrive");

	printf("\n%d controlli, %d falliti\n", checks, failures);
	return failures ? 1 : 0;
}

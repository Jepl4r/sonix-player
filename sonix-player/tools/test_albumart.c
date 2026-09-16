// Il lettore delle copertine dentro i tag, provato sotto AddressSanitizer.
//
// Perche' esiste: il bench delle copertine (tools/test_covers.c) sostituisce
// albumart con un finto che consegna i byte gia' pronti, perche' li' quello che
// si prova e' l'aritmetica dei buffer. Cosi' pero' il pezzo che legge il file
// audio -- un camminatore di byte scritto a mano sopra dati che arrivano dal
// file -- non lo prova nessuno, ed e' la stessa classe di codice per cui
// id3chap.c ha avuto bisogno di ventidue asserzioni prima di leggere ogni frame
// per intero.
//
// Gira contro src/system/albumart.c vero, su file costruiti da
// tools/make_art_zoo.py byte per byte. Non e' solo ASan: ogni immagine che
// torna viene riconosciuta dai suoi primi byte, cosi' si vede anche quando il
// lettore tiene la figura sbagliata invece di uscire dal buffer.
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "src/system/library/albumart.h"

static int checks;
static int failures;

static void ok(bool condition, const char *fmt, ...) {
	checks++;
	va_list args;
	va_start(args, fmt);
	printf(condition ? "  ok   " : "  NO   ");
	vprintf(fmt, args);
	printf("\n");
	va_end(args);
	if (!condition) {
		failures++;
	}
}

// JPEG, PNG o nient'altro, letto dai byte che sono tornati. Il tipo basta a
// dire QUALE immagine e' stata scelta quando un tag ne contiene piu' di una,
// perche' lo zoo mette una figura di tipo diverso in ognuna.
static const char *kind_of(const albumart_t *art) {
	if (!art->data || art->size < 12) {
		return "niente";
	}
	if (art->data[0] == 0xFF && art->data[1] == 0xD8 && art->data[2] == 0xFF) {
		return "jpeg";
	}
	static const uint8_t png[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
	if (memcmp(art->data, png, sizeof(png)) == 0) {
		return "png";
	}
	return "altro";
}

// Legge ogni byte di quello che e' tornato. Una dimensione piu' grande del
// buffer non si vede finche' qualcuno non ci legge dentro, e chi ci legge nel
// player e' il decodificatore.
static void touch(const albumart_t *art) {
	volatile uint32_t sum = 0;
	for (size_t i = 0; i < art->size; i++) {
		sum += art->data[i];
	}
	(void)sum;
}

static const char *zoo;

static void path_of(char *out, size_t size, const char *sub, const char *name) {
	snprintf(out, size, "%s/%s/%s", zoo, sub, name);
}

// ---------------------------------------------------------------------------
// i byte guastati a caso
// ---------------------------------------------------------------------------

#define FUZZ_ROUNDS 1500

static const struct {
	const char *file;
	unsigned seed;
} FUZZ_SEEDS[] = {
	{"v23.mp3", 1}, {"v24.mp3", 2},	  {"v22.mp3", 3},		{"unsync_tag.mp3", 4},
	{"esteso.mp3", 5}, {"tre_immagini.mp3", 6}, {"con_tag.dsf", 7}, {"con_tag.aiff", 8},
	{"con_copertina.flac", 9},
};

// Un generatore scritto qui invece di rand(): il banco deve fare esattamente le
// stesse mutazioni a ogni giro e su ogni macchina, e rand() non lo promette.
static uint32_t rng_state;

static uint32_t rng_next(void) {
	rng_state = rng_state * 1103515245u + 12345u;
	return rng_state >> 8;
}

// Guasta da uno a otto byte di una copia del file e la rida' al lettore. Torna
// quante volte l'ha fatto. Niente asserzioni sul risultato: quale immagine
// esca da un tag rotto non e' interessante, e nemmeno definito. Quello che si
// vuole sapere e' se il lettore resta dentro i suoi buffer, e a quello
// risponde AddressSanitizer.
static int fuzz(const char *name, unsigned seed, int rounds) {
	char source[1024];
	path_of(source, sizeof(source), "tags", name);

	FILE *f = fopen(source, "rb");
	if (!f) {
		printf("  NO   il fuzz non trova %s\n", name);
		failures++;
		return 0;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *whole = malloc((size_t)size);
	if (!whole || fread(whole, 1, (size_t)size, f) != (size_t)size) {
		free(whole);
		fclose(f);
		return 0;
	}
	fclose(f);

	char victim[1024];
	snprintf(victim, sizeof(victim), "%s/guasto%s", zoo, strrchr(name, '.'));

	uint8_t *copy = malloc((size_t)size);
	rng_state = seed;
	int done = 0;

	for (int i = 0; i < rounds && copy; i++) {
		memcpy(copy, whole, (size_t)size);

		// Fra uno e otto byte, ovunque nel file: le dimensioni dei frame stanno
		// sparse dappertutto e sono proprio quelle che interessa rovinare.
		int hits = 1 + (int)(rng_next() % 8);
		for (int h = 0; h < hits; h++) {
			size_t at = rng_next() % (size_t)size;
			copy[at] = (uint8_t)(rng_next() & 0xFF);
		}

		FILE *out = fopen(victim, "wb");
		if (!out) {
			break;
		}
		fwrite(copy, 1, (size_t)size, out);
		fclose(out);

		albumart_t art;
		if (albumart_load_for_file(victim, &art)) {
			touch(&art);
		}
		albumart_free(&art);
		done++;
	}

	free(copy);
	free(whole);
	remove(victim);
	return done;
}

// Una prova su un file: cosa deve tornare, e di che tipo.
static void expect(const char *name, const char *want, const char *why) {
	char path[1024];
	path_of(path, sizeof(path), "tags", name);

	albumart_t art;
	bool found = albumart_load_for_file(path, &art);
	const char *got = found ? kind_of(&art) : "niente";
	if (found) {
		touch(&art);
	}
	ok(strcmp(got, want) == 0, "%-26s %s (%s)", name, got, why);
	albumart_free(&art);
	albumart_free(&art); // due volte di seguito: deve restare innocuo
}

// La stessa cosa chiedendo i candidati uno per uno, che e' come li chiede il
// caricatore vero quando una sorgente ha i byte ma non si lascia decodificare.
static void expect_candidate(const char *name, int index, const char *want) {
	char path[1024];
	path_of(path, sizeof(path), "tags", name);

	albumart_t art;
	bool found = albumart_load_candidate(path, index, &art);
	const char *got = found ? kind_of(&art) : "niente";
	if (found) {
		touch(&art);
	}
	ok(strcmp(got, want) == 0, "%-26s candidato %d: %s", name, index, got);
	albumart_free(&art);
}

static void expect_dir(const char *sub, const char *want, const char *why) {
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", zoo, sub);

	albumart_t art;
	bool found = albumart_load_for_dir(path, &art);
	const char *got = found ? kind_of(&art) : "niente";
	if (found) {
		touch(&art);
	}
	ok(strcmp(got, want) == 0, "%-26s %s (%s)", sub, got, why);
	albumart_free(&art);
}

int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc != 2) {
		fprintf(stderr, "uso: test_albumart <cartella-dello-zoo>\n");
		return 2;
	}
	zoo = argv[1];

	printf("\n-- il caso normale, nelle tre versioni del tag\n");
	expect("v23.mp3", "jpeg", "ID3v2.3, dimensioni big endian");
	expect("v24.mp3", "jpeg", "ID3v2.4, dimensioni sincsafe");
	expect("v22.mp3", "jpeg", "ID3v2.2, PIC con il formato a tre lettere");
	expect("png.mp3", "png", "un PNG invece di un JPEG");

	printf("\n-- la 2.4 scritta come una 2.3, che e' meta' dei tagger\n");
	// Letta secondo le regole della 2.4 la dimensione torna circa un settimo di
	// quella vera, il passo successivo cade dentro i byte dell'immagine e la
	// copertina non si trova mai. Il lettore se ne accorge guardando se l'altra
	// lettura atterra su un frame vero.
	expect("v24_dimensioni_normali.mp3", "jpeg", "riconosce la dimensione non sincsafe");

	printf("\n-- unsynchronisation\n");
	expect("unsync_tag.mp3", "jpeg", "sull'intero tag, come fa la 2.3");
	expect("unsync_frame.mp3", "jpeg", "sul singolo frame, come fa la 2.4");

	printf("\n-- intestazione estesa\n");
	expect("esteso.mp3", "jpeg", "saltata, la copertina si trova lo stesso");
	// Una lunghezza assurda nell'intestazione estesa porta il passo oltre la
	// fine del tag. Non si legge niente fuori dal buffer, ma la copertina si
	// perde: il tag e' rotto, e nessuna lettura puo' indovinare dove ricomincia.
	expect("esteso_bugiardo.mp3", "niente", "lunghezza assurda: si ferma");

	printf("\n-- i casi storti\n");
	expect("frame_bugiardo.mp3", "niente", "un frame piu' grande di tutto il tag");
	expect("tag_troncato.mp3", "niente", "il tag dichiara piu' byte del file");
	// I byte dell'immagine finiscono a meta': l'intestazione JPEG c'e' ancora,
	// quindi viene tenuta ed e' il decodificatore a rifiutarla piu' avanti. Qui
	// conta che non si legga oltre quello che c'e'.
	expect("apic_troncato.mp3", "jpeg", "APIC tagliato: tiene quello che c'e'");
	expect("non_immagine.mp3", "niente", "un GIF non e' ne' JPEG ne' PNG");

	printf("\n-- descrizioni che non finiscono mai\n");
	// Tutte e quattro le codifiche: quelle UTF-16 finiscono con due zeri, non
	// uno, ed e' li' che un lettore distratto esce dal frame.
	expect("desc_latin1.mp3", "niente", "latin-1 senza terminatore");
	expect("desc_utf16bom.mp3", "niente", "UTF-16 con BOM senza terminatore");
	expect("desc_utf16be.mp3", "niente", "UTF-16BE senza terminatore");
	expect("desc_utf8.mp3", "niente", "UTF-8 senza terminatore");

	printf("\n-- piu' immagini nello stesso tag\n");
	// Nello zoo la copertina e' il JPEG e le altre due sono PNG, quindi il tipo
	// dice da solo quale delle tre e' stata tenuta.
	expect("tre_immagini.mp3", "jpeg", "il front cover vince anche se arriva per ultimo");
	expect("front_per_primo.mp3", "jpeg", "e niente lo sostituisce se arriva per primo");

	printf("\n-- quello che una copertina non ce l'ha\n");
	expect("senza_immagini.mp3", "niente", "un tag con il solo titolo");
	expect("solo_riempimento.mp3", "niente", "un tag di soli zeri");
	expect("tag_minuscolo.mp3", "niente", "un tag troppo piccolo per contenere qualcosa");
	expect("nudo.mp3", "niente", "nessun tag");
	expect("vuoto.mp3", "niente", "un file di zero byte");
	expect("non_esiste.mp3", "niente", "un file che non c'e'");

	printf("\n-- lo stesso tag dove lo mettono gli altri contenitori\n");
	expect("con_tag.dsf", "jpeg", ".dsf: il tag all'offset scritto nell'intestazione");
	expect("con_tag.aiff", "jpeg", "AIFF: il tag dentro un chunk ID3");

	printf("\n-- FLAC, dove la copertina e' un blocco PICTURE\n");
	expect("con_copertina.flac", "jpeg", "un blocco PICTURE normale");
	expect("copertina_png.flac", "png", "lo stesso con un PNG");
	expect("senza_copertina.flac", "niente", "nessun blocco PICTURE");
	// Un blocco che dichiara un'immagine piu' lunga di se stesso: qualunque
	// cosa ne faccia dr_flac, non deve leggere oltre.
	{
		char path[1024];
		path_of(path, sizeof(path), "tags", "copertina_bugiarda.flac");
		albumart_t art;
		if (albumart_load_for_file(path, &art)) {
			touch(&art);
		}
		albumart_free(&art);
		ok(true, "%-26s non legge oltre il blocco", "copertina_bugiarda.flac");
	}

	printf("\n-- i candidati, uno per volta\n");
	// 0 e' l'immagine dentro il file, 1 e' quella nella cartella. Nello zoo dei
	// tag non ci sono immagini sciolte, quindi il secondo candidato non trova
	// mai niente -- ed e' proprio quello che deve fare senza inventarselo.
	expect_candidate("v23.mp3", 0, "jpeg");
	expect_candidate("v23.mp3", 1, "niente");
	expect_candidate("nudo.mp3", 0, "niente");
	expect_candidate("nudo.mp3", 1, "niente");
	expect_candidate("v23.mp3", -1, "niente");
	expect_candidate("v23.mp3", 99, "niente");

	printf("\n-- la strada di ripiego sulla cartella\n");
	expect_dir("con_cover", "jpeg", "cover.jpg batte front.png");
	expect_dir("una_immagine_sola", "jpeg", "una sola immagine vale come copertina");
	expect_dir("solo_nei_tag", "jpeg", "nessuna immagine: la prende dal primo brano");
	expect_dir("niente", "niente", "una cartella senza niente da mostrare");
	expect_dir("non_esiste", "niente", "una cartella che non c'e'");

	printf("\n-- un'immagine chiesta per percorso, che e' quello che fa la radio\n");
	{
		char path[1024];
		snprintf(path, sizeof(path), "%s/con_cover/cover.jpg", zoo);
		albumart_t art;
		bool found = albumart_load_for_file(path, &art);
		if (found) {
			touch(&art);
		}
		ok(found && strcmp(kind_of(&art), "jpeg") == 0, "un .jpg e' la copertina di se stesso");
		albumart_free(&art);
	}

	printf("\n-- i byte guastati a caso\n");
	// I casi qui sopra sono quelli a cui ho pensato, e quelli a cui si pensa
	// sono quelli che il codice gia' regge. Questo invece prende un tag buono e
	// gli rovina byte a caso, migliaia di volte, con un seme fisso perche' un
	// banco che cambia risposta a ogni giro non e' un banco.
	//
	// Le lunghezze dentro un tag si rovinano da sole: guastare un byte di una
	// dimensione sincsafe la fa diventare enorme o minuscola, e da li' in poi
	// il camminatore si muove su offset che non corrispondono a niente. E' il
	// modo piu' diretto di chiedergli se si ferma ai bordi del buffer.
	int mutations = 0;
	for (size_t i = 0; i < sizeof(FUZZ_SEEDS) / sizeof(FUZZ_SEEDS[0]); i++) {
		mutations += fuzz(FUZZ_SEEDS[i].file, FUZZ_SEEDS[i].seed, FUZZ_ROUNDS);
	}
	ok(mutations > 0, "%d mutazioni, nessuna e' uscita dal buffer", mutations);

	printf("\n%d controlli, %d falliti\n", checks, failures);
	return failures ? 1 : 0;
}

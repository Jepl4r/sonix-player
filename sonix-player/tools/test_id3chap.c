// I capitoli dentro un MP3, provati senza schermo.
//
// Gira contro src/system/id3chap.c vero, su file costruiti da
// tools/make_id3_chapters.py: il tag lo scrive Python byte per byte, cosi' il
// lettore si misura contro la forma vera del formato e non contro qualcosa
// costruito con le stesse assunzioni che dovrebbe provare.
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/system/library/id3chap.h"

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

static bool close_to(double a, double b) { return (a - b) < 0.002 && (b - a) < 0.002; }

int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc != 2) {
		fprintf(stderr, "uso: test_id3chap <cartella-con-gli-mp3>\n");
		return 2;
	}
	const char *dir = argv[1];
	char path[512];
	id3chap_t chapters[8];

	printf("\n-- il caso normale: ID3v2.4 con tre capitoli\n");
	snprintf(path, sizeof(path), "%s/tre_capitoli.mp3", dir);
	int n = id3chap_read(path, chapters, 8);
	ok(n == 3, "ne trova %d", n);
	if (n == 3) {
		ok(close_to(chapters[0].start, 0.0), "il primo parte da %.3f", chapters[0].start);
		ok(close_to(chapters[1].start, 60.0), "il secondo da %.3f", chapters[1].start);
		// I millisecondi devono sopravvivere: 180500 ms sono 180,5 s, e un
		// lettore che divide fra interi li perde.
		ok(close_to(chapters[2].start, 180.5), "il terzo da %.3f, coi millisecondi", chapters[2].start);
		ok(strcmp(chapters[0].title, "Il primo capitolo") == 0, "e si chiama '%s'", chapters[0].title);
		ok(strcmp(chapters[2].title, "Il terzo") == 0, "l'ultimo si chiama '%s'", chapters[2].title);
	}

	printf("\n-- ID3v2.3, dove le dimensioni non sono sincsafe\n");
	snprintf(path, sizeof(path), "%s/v23.mp3", dir);
	n = id3chap_read(path, chapters, 8);
	// Letto con la regola della 2.4, la dimensione del primo frame torna circa
	// la meta' e il secondo capitolo non si trova mai.
	ok(n == 2, "ne trova %d (con la regola sbagliata ne troverebbe 1)", n);
	if (n == 2) {
		ok(strcmp(chapters[1].title, "Due") == 0, "e il secondo si chiama '%s'", chapters[1].title);
	}

	printf("\n-- i titoli non in latin-1\n");
	snprintf(path, sizeof(path), "%s/utf16.mp3", dir);
	n = id3chap_read(path, chapters, 8);
	ok(n == 1 && chapters[0].title[0] != '\0', "UTF-16 con BOM: '%s'", n ? chapters[0].title : "");
	ok(n == 1 && close_to(chapters[0].start, 1.5), "e il tempo e' giusto lo stesso");
	snprintf(path, sizeof(path), "%s/utf8.mp3", dir);
	n = id3chap_read(path, chapters, 8);
	ok(n == 1 && chapters[0].title[0] != '\0', "UTF-8: '%s'", n ? chapters[0].title : "");

	printf("\n-- i casi storti\n");
	snprintf(path, sizeof(path), "%s/senza_nome.mp3", dir);
	n = id3chap_read(path, chapters, 8);
	ok(n == 1, "un capitolo senza titolo conta lo stesso");
	ok(n == 1 && chapters[0].title[0] == '\0', "e il titolo resta vuoto");

	snprintf(path, sizeof(path), "%s/esteso.mp3", dir);
	n = id3chap_read(path, chapters, 8);
	ok(n == 1, "l'intestazione estesa viene saltata");

	snprintf(path, sizeof(path), "%s/bugiardo.mp3", dir);
	n = id3chap_read(path, chapters, 8);
	ok(n == 0, "un frame che dichiara di essere piu' grande del tag non legge oltre la fine");

	printf("\n-- quello che NON e' un audiolibro\n");
	snprintf(path, sizeof(path), "%s/niente_capitoli.mp3", dir);
	ok(!id3chap_present(path), "un MP3 con il tag ma senza capitoli non ne ha");
	snprintf(path, sizeof(path), "%s/nudo.mp3", dir);
	ok(!id3chap_present(path), "e nemmeno uno senza tag");
	snprintf(path, sizeof(path), "%s/non_esiste.mp3", dir);
	ok(!id3chap_present(path), "ne' un file che non c'e'");

	printf("\n-- chiedere solo se ce ne sono\n");
	snprintf(path, sizeof(path), "%s/tre_capitoli.mp3", dir);
	ok(id3chap_present(path), "il libro con tre capitoli ne ha");
	ok(id3chap_read(path, NULL, 0) == 1, "e chiedendo senza spazio si ferma al primo");

	printf("\n-- lo spazio che il chiamante offre\n");
	n = id3chap_read(path, chapters, 2);
	ok(n == 3, "torna quanti ce ne sono (%d) anche con spazio per due", n);
	ok(strcmp(chapters[1].title, "Il secondo capitolo") == 0, "e riempie i due che ci stanno");

	printf("\n%d controlli, %d falliti\n", checks, failures);
	return failures ? 1 : 0;
}

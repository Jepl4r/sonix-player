// Il lettore di EPUB, provato senza schermo.
//
// I libri se li costruisce il banco (tools/make_test_epub.py), quindi il
// formato lo produce la libreria standard di Python e non io: se questo
// lettore sbaglia, sbaglia contro uno zip vero e un OPF vero.
//
// La prova che conta non e' nessuna di quelle sul contenuto. E' l'ultima:
// VmRSS prima di aprire il libro, VmRSS con il libro aperto, VmRSS dopo averlo
// chiuso. Il markdown dice che se quella proprieta' non e' nell'architettura
// dall'inizio poi non ci si mette piu', ed e' l'unica ragione per cui tutto
// qui dentro passa da mmap invece che da malloc.
//
// Uso: run_ebook_bench.sh
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/system/ebook/ebook.h"

static int checks, failures;

static void ok(bool condition, const char *fmt, ...) {
	va_list ap;
	checks++;
	printf(condition ? "  ok   " : "  FAIL ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	if (!condition) {
		failures++;
	}
}

// Quanta memoria ha davvero questo processo, secondo il kernel. Non quanta ne
// ha chiesta: quanta gliene e' rimasta assegnata.
static long vmrss_kb(void) {
	FILE *f = fopen("/proc/self/status", "r");
	if (!f) {
		return -1;
	}
	char line[256];
	long kb = -1;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "VmRSS:", 6) == 0) {
			kb = strtol(line + 6, NULL, 10);
			break;
		}
	}
	fclose(f);
	return kb;
}

// Tutto il testo di un capitolo, come lo vedrebbe chi legge.
static size_t chapter_chars(const ebook_t *book) {
	size_t total = 0;
	for (uint32_t i = 0; i < ebook_block_count(book); i++) {
		const ebook_block_t *b = ebook_block(book, i);
		for (uint32_t s = 0; s < b->span_count; s++) {
			total += ebook_span(book, b->first_span + s)->length;
		}
	}
	return total;
}

static bool chapter_contains(const ebook_t *book, const char *needle) {
	const char *text = ebook_chapter_text(book);
	for (uint32_t i = 0; i < ebook_block_count(book); i++) {
		const ebook_block_t *b = ebook_block(book, i);
		for (uint32_t s = 0; s < b->span_count; s++) {
			const ebook_span_t *sp = ebook_span(book, b->first_span + s);
			if (sp->length >= strlen(needle) && memmem(text + sp->offset, sp->length, needle, strlen(needle))) {
				return true;
			}
		}
	}
	return false;
}

static const ebook_block_t *find_block(const ebook_t *book, uint8_t type, uint8_t level) {
	for (uint32_t i = 0; i < ebook_block_count(book); i++) {
		const ebook_block_t *b = ebook_block(book, i);
		if (b->type == type && (level == 0 || b->level == level)) {
			return b;
		}
	}
	return NULL;
}

static bool block_has_style(const ebook_t *book, const ebook_block_t *b, uint8_t style, const char *word) {
	const char *text = ebook_chapter_text(book);
	for (uint32_t s = 0; s < b->span_count; s++) {
		const ebook_span_t *sp = ebook_span(book, b->first_span + s);
		if ((sp->style & style) == style && sp->length >= strlen(word) &&
			memmem(text + sp->offset, sp->length, word, strlen(word))) {
			return true;
		}
	}
	return false;
}

// Il blocco che contiene `needle` ha questo allineamento?
static bool block_with_text_align(ebook_t *book, const char *needle, uint8_t align) {
	const char *pool = ebook_chapter_text(book);
	for (uint32_t i = 0; i < ebook_block_count(book); i++) {
		const ebook_block_t *b = ebook_block(book, i);
		for (uint32_t s = 0; s < b->span_count; s++) {
			const ebook_span_t *sp = ebook_span(book, b->first_span + s);
			if (sp->length && memmem(pool + sp->offset, sp->length, needle, strlen(needle))) {
				return b->align == align;
			}
		}
	}
	return false;
}

// Lo span che contiene esattamente `needle` ha questo stile?
static bool span_with_text_has_style(ebook_t *book, const char *needle, uint8_t style) {
	const char *pool = ebook_chapter_text(book);
	size_t n = strlen(needle);
	for (uint32_t i = 0; i < ebook_span_count(book); i++) {
		const ebook_span_t *sp = ebook_span(book, i);
		if (sp->length >= n && memmem(pool + sp->offset, sp->length, needle, n)) {
			return (sp->style & style) != 0;
		}
	}
	return false;
}

int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc != 2) {
		fprintf(stderr, "uso: test_ebook <cartella-con-i-libri>\n");
		return 2;
	}
	const char *dir = argv[1];
	char path[512];

	printf("\n-- un EPUB 3 normale\n");
	snprintf(path, sizeof(path), "%s/epub3.epub", dir);
	ebook_t *book = ebook_open(path);
	ok(book != NULL, "il libro si apre");
	if (!book) {
		printf("\n%d controlli, %d falliti\n", checks, failures + 1);
		return 1;
	}

	ok(strcmp(ebook_title(book), "Il libro di prova") == 0, "il titolo e' '%s'", ebook_title(book));
	ok(strcmp(ebook_author(book), "Autore Tale") == 0, "l'autore e' '%s'", ebook_author(book));
	ok(ebook_spine_count(book) == 4, "ci sono %u capitoli nella spine", ebook_spine_count(book));
	ok(ebook_toc_count(book) == 4, "e %u voci nell'indice", ebook_toc_count(book));
	if (ebook_toc_count(book) >= 2) {
		ok(strcmp(ebook_toc(book, 1)->label, "Secondo capitolo") == 0, "la seconda voce e' '%s'",
		   ebook_toc(book, 1)->label);
		ok(ebook_toc(book, 1)->spine == 1, "e punta al capitolo giusto");
	}

	// La riga di avanzamento della striscia in fondo alla pagina puo' misurare
	// il libro invece del capitolo, e per farlo pesa i capitoli con la
	// dimensione del loro file invece di contarli uguali. Qui il terzo e'
	// enorme apposta e il quarto e' una riga: se i pesi tornassero uguali, la
	// riga direbbe "meta' libro" alla fine della prefazione.
	printf("\n-- quanto pesa ogni capitolo\n");
	{
		uint64_t total = ebook_book_weight(book);
		ok(total > 0, "il libro pesa %llu byte", (unsigned long long)total);

		uint64_t sum = 0;
		for (uint32_t i = 0; i < ebook_spine_count(book); i++) {
			sum += ebook_chapter_weight(book, i);
		}
		ok(sum == total, "la somma dei capitoli fa il totale (%llu)", (unsigned long long)sum);

		ok(ebook_weight_before(book, 0) == 0, "prima del primo capitolo non c'e' niente");
		ok(ebook_weight_before(book, ebook_spine_count(book)) == total,
		   "prima della fine c'e' tutto il libro");

		bool rises = true;
		for (uint32_t i = 1; i <= ebook_spine_count(book); i++) {
			if (ebook_weight_before(book, i) < ebook_weight_before(book, i - 1)) {
				rises = false;
			}
		}
		ok(rises, "e in mezzo non torna mai indietro");

		uint32_t big = ebook_chapter_weight(book, 2);	// il capitolo lungo
		uint32_t small = ebook_chapter_weight(book, 3);	// quello di una riga
		ok(big > small * 10, "il capitolo lungo (%u) pesa piu' di dieci volte quello corto (%u)", big, small);

		// La prova vera: contando i capitoli uguali, quello lungo varrebbe un
		// quarto del libro. Pesato, vale molto di piu'.
		int counted = 100 / (int)ebook_spine_count(book);
		int weighed = total ? (int)((uint64_t)big * 100u / total) : 0;
		ok(weighed > counted * 2, "vale il %d%% del libro e non il %d%% che darebbe contarli", weighed,
		   counted);
	}

	printf("\n-- il capitolo, e cosa ne esce\n");
	ok(ebook_open_chapter(book, 0), "il primo capitolo si carica");
	ok(ebook_block_count(book) > 0, "ha %u blocchi", ebook_block_count(book));
	ok(chapter_contains(book, "Frase di prova"), "il testo c'e'");

	const ebook_block_t *h1 = find_block(book, EBOOK_BLOCK_HEADING, 1);
	ok(h1 != NULL, "il titolo del capitolo e' un heading di livello 1");

	const ebook_block_t *para = find_block(book, EBOOK_BLOCK_PARAGRAPH, 0);
	ok(para != NULL, "e c'e' almeno un paragrafo");
	if (para) {
		ok(block_has_style(book, para, EBOOK_STYLE_BOLD, "grassetto"), "il grassetto e' segnato");
		ok(block_has_style(book, para, EBOOK_STYLE_ITALIC, "corsivo"), "e il corsivo anche");
	}

	printf("\n-- quello che non va letto\n");
	ok(!chapter_contains(book, "NONDEVEUSCIRE"), "il contenuto di <script> non finisce nella pagina");
	ok(!chapter_contains(book, "COLOREROSSO"), "e nemmeno quello di <style>");
	ok(chapter_contains(book, "dentro un tag sconosciuto"),
	   "il testo dentro un elemento sconosciuto si legge lo stesso");

	printf("\n-- liste e citazioni\n");
	ok(find_block(book, EBOOK_BLOCK_LIST_ITEM, 0) != NULL, "le voci di lista sono blocchi loro");
	ok(find_block(book, EBOOK_BLOCK_QUOTE, 0) != NULL, "e la citazione pure");

	printf("\n-- cambiare capitolo non accumula\n");
	{
		size_t first = 0, largest = 0;
		for (uint32_t i = 0; i < ebook_spine_count(book); i++) {
			ok(ebook_open_chapter(book, i), "il capitolo %u si carica (%zu caratteri)", i,
			   chapter_chars(book));
			size_t now = ebook_chapter_bytes(book);
			if (!first) {
				first = now;
			}
			if (now > largest) {
				largest = now;
			}
		}
		// Il terzo capitolo e' grande apposta. Quello che si controlla qui non
		// e' che l'arena resti piccola -- deve crescere per contenerlo -- ma
		// che tornare a un capitolo piccolo la faccia tornare piccola, cioe'
		// che il capitolo precedente sia stato davvero buttato.
		ok(ebook_open_chapter(book, 0), "si torna al primo");
		ok(ebook_chapter_bytes(book) <= first,
		   "e l'arena del capitolo e' tornata a %zu KB dopo aver toccato %zu KB",
		   ebook_chapter_bytes(book) / 1024u, largest / 1024u);
	}

	printf("\n-- chiudere il capitolo restituisce tutto\n");
	ebook_close_chapter(book);
	ok(ebook_chapter_bytes(book) == 0, "l'arena del capitolo e' a zero");
	ok(ebook_book_bytes(book) > 0, "quella del libro e' ancora la' (%zu KB)", ebook_book_bytes(book) / 1024u);
	ok(ebook_book_bytes(book) <= 256u * 1024u, "ed e' piccola: %zu KB", ebook_book_bytes(book) / 1024u);

	ebook_close(book);
	book = NULL;

	printf("\n-- un EPUB 2 con l'indice NCX\n");
	snprintf(path, sizeof(path), "%s/epub2.epub", dir);
	ebook_t *old = ebook_open(path);
	ok(old != NULL, "si apre anche il formato vecchio");
	if (old) {
		ok(ebook_spine_count(old) == 2, "%u capitoli", ebook_spine_count(old));
		ok(ebook_toc_count(old) == 2, "%u voci d'indice, lette dall'NCX", ebook_toc_count(old));
		if (ebook_toc_count(old) >= 1) {
			ok(strcmp(ebook_toc(old, 0)->label, "Capitolo uno") == 0, "la prima e' '%s'", ebook_toc(old, 0)->label);
		}
		ok(ebook_open_chapter(old, 1), "e il secondo capitolo si legge");
		// Il libro sta in una sottocartella e l'indice ci arriva con "../":
		// se il percorso non si risolvesse, la spine sarebbe vuota.
		ok(chapter_contains(old, "sottocartella"), "i percorsi relativi con .. si risolvono");
		ebook_close(old);
	}

	printf("\n-- un file che non e' un EPUB\n");
	{
		snprintf(path, sizeof(path), "%s/rotto.epub", dir);
		ebook_t *bad = ebook_open(path);
		ok(bad == NULL, "un file spazzatura non si apre");
		snprintf(path, sizeof(path), "%s/vuoto.epub", dir);
		bad = ebook_open(path);
		ok(bad == NULL, "e nemmeno uno zip senza container.xml");
		bad = ebook_open("/questo/non/esiste.epub");
		ok(bad == NULL, "ne' un percorso che non c'e'");
	}

	// -----------------------------------------------------------------------
	// La prova che conta
	// -----------------------------------------------------------------------
	printf("\n-- la memoria torna al kernel\n");
	{
		long before = vmrss_kb();
		ok(before > 0, "VmRSS si legge (%ld KB prima di aprire)", before);

		// Lo stesso libro grande, letto per intero DUE volte.
		//
		// Due volte perche' e' l'unico modo di distinguere le due cose che
		// alzano VmRSS e che si somigliano guardando una sola misura: il
		// residuo dell'allocatore -- glibc tiene il suo heap cresciuto dopo la
		// prima raffica di malloc, e non lo restituisce -- e una perdita vera,
		// che cresce a ogni libro. Il primo e' un gradino che si paga una volta.
		// La seconda si vedrebbe qui come un secondo gradino uguale al primo.
		snprintf(path, sizeof(path), "%s/grosso.epub", dir);

		long peak = before;
		long after[2] = {0, 0};
		for (int pass = 0; pass < 2; pass++) {
			ebook_t *big = ebook_open(path);
			ok(big != NULL, "il libro grande si apre (giro %d)", pass + 1);
			if (big) {
				for (uint32_t i = 0; i < ebook_spine_count(big); i++) {
					ebook_open_chapter(big, i);
					long now = vmrss_kb();
					if (now > peak) {
						peak = now;
					}
				}
				if (pass == 0) {
					ebook_log_memory(big, "aperto");
				}
				ebook_close(big);
			}
			after[pass] = vmrss_kb();
		}

		ebook_log_memory(NULL, "chiuso");
		printf("         VmRSS: %ld KB prima, %ld al massimo, %ld dopo il primo giro, %ld dopo il secondo\n",
			   before, peak, after[0], after[1]);

		long grew = peak - before;
		ok(grew > 512, "leggendolo il processo cresce di %ld KB", grew);

		// Quello che il libro si porta via chiudendosi: la differenza fra i due
		// giri. Se le arene fossero malloc, il secondo giro lascerebbe indietro
		// quanto il primo e questa sarebbe grande quanto il libro.
		long second = after[1] - after[0];
		ok(second < 64, "il secondo libro non lascia niente di suo (%ld KB)", second);
		ok(after[1] - before < grew / 2, "e in tutto resta meno della meta' di quanto era cresciuto (%ld KB)",
		   after[1] - before);
	}

	printf("\n-- le immagini e il foglio di stile\n");
	{
		snprintf(path, sizeof(path), "%s/stile.epub", dir);
		ebook_t *s = ebook_open(path);
		ok(s != NULL, "stile.epub si apre");
		if (s) {
			// La copertina scritta come <svg><image xlink:href="../images/...">,
			// che e' il caso per cui un lettore che conosce solo <img> mostra
			// una prima pagina bianca.
			ok(ebook_open_chapter(s, 0), "il capitolo della copertina si carica");
			const ebook_block_t *cov = find_block(s, EBOOK_BLOCK_IMAGE, 0);
			ok(cov != NULL, "e dentro c'e' un blocco immagine");
			if (cov) {
				const ebook_span_t *sp = ebook_span(s, cov->first_span);
				const char *pool = ebook_chapter_text(s);
				ok(strncmp(pool + sp->offset, "OEBPS/images/copertina.png", sp->length) == 0,
				   "con il percorso risolto risalendo di una cartella");
				uint32_t size = 0;
				void *bytes = ebook_load_image(s, "OEBPS/images/copertina.png", &size);
				ok(bytes != NULL && size > 8, "e i byte si leggono davvero (%u)", size);
				ok(bytes && memcmp(bytes, "\x89PNG", 4) == 0, "e sono un PNG");
				free(bytes);
			}

			// L'indice di navigazione sta in text/ accanto ai capitoli, come lo
			// scrive Calibre, e i suoi href sono relativi a SE STESSO e non
			// all'OPF. Risolti contro l'OPF puntavano a file che non esistono,
			// ogni voce veniva scartata e la lista dei capitoli tornava
			// silenziosamente a "Capitolo 1, Capitolo 2".
			ok(ebook_toc_count(s) == 2, "l'indice ha %u voci", ebook_toc_count(s));
			if (ebook_toc_count(s) >= 2) {
				ok(ebook_toc(s, 1)->spine == 1, "e la seconda punta al capitolo giusto");
			}

			ok(ebook_open_chapter(s, 1), "il capitolo con le figure si carica");

			// Tre <img>, di cui una che il libro nomina ma non ha: quella deve
			// esserci come blocco (il parser non apre lo ZIP) e deve sparire
			// quando il lettore prova a caricarla.
			uint32_t images = 0;
			for (uint32_t i = 0; i < ebook_block_count(s); i++) {
				if (ebook_block(s, i)->type == EBOOK_BLOCK_IMAGE) {
					images++;
				}
			}
			ok(images == 3, "ci sono %u blocchi immagine", images);
			uint32_t size = 0;
			void *missing = ebook_load_image(s, "OEBPS/images/manca.png", &size);
			ok(missing == NULL, "e quella che non c'e' non si carica");
			free(missing);

			// Il corsivo e il grassetto scritti come classi nel foglio
			// collegato, e il grassetto del foglio interno al documento.
			ok(span_with_text_has_style(s, "il corsivo di classe", EBOOK_STYLE_ITALIC),
			   "il corsivo arriva dal foglio collegato");
			ok(span_with_text_has_style(s, "il grassetto di classe", EBOOK_STYLE_BOLD),
			   "il grassetto pure");
			ok(span_with_text_has_style(s, "il grassetto del foglio interno", EBOOK_STYLE_BOLD),
			   "e quello del <style> dentro il documento");

			// font-style: normal deve battere il corsivo che lo racchiude,
			// altrimenti una classe che spegne il corsivo non serve a niente.
			ok(span_with_text_has_style(s, "fuori", EBOOK_STYLE_ITALIC), "fuori e' corsivo");
			ok(!span_with_text_has_style(s, "dentro", EBOOK_STYLE_ITALIC), "e dentro non lo e'");

			ok(block_with_text_align(s, "Questa e' centrata", EBOOK_ALIGN_CENTRE), "text-align: center arriva");
			ok(block_with_text_align(s, "Questa e' a destra", EBOOK_ALIGN_RIGHT), "e text-align: right anche");
			ok(!chapter_contains(s, "non si deve vedere"), "display: none non si vede");

			// Il selettore con il figlio e quello con l'id non devono essere
			// applicati a meta': meglio ignorarli.
			ok(chapter_contains(s, "Una riga normale"), "il testo normale c'e'");
			ok(!span_with_text_has_style(s, "Una riga normale", EBOOK_STYLE_ITALIC),
			   "e #unico non ha messo in corsivo tutto il documento");

			ebook_close(s);
		}
	}

	printf("\n%d controlli, %d falliti\n", checks, failures);
	return failures ? 1 : 0;
}

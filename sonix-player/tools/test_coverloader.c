// Il passaggio di consegne fra il worker delle copertine e l'interfaccia,
// provato sotto ThreadSanitizer.
//
// Perche' esiste: nei crash del dispositivo il thread che muore e' quello
// dell'interfaccia, e l'ultima riga scritta e' la decodifica di una copertina,
// che la scrive il worker. Quindi i due erano attivi nello stesso istante.
// L'unico punto in cui si toccano e' src/gui/coverloader.c: un mutex, una
// condizione, e un contatore di generazione che decide se un risultato arrivato
// tardi va buttato o consegnato. ASan non vede le corse; TSan si'.
//
// Quello che si riproduce e' il caso dell'utente: coda ordinata per titolo,
// ogni brano in una cartella diversa, quindi una richiesta nuova a ogni cambio
// traccia, spesso prima che la precedente sia finita -- piu' le liste che
// scorrono e chiedono e rilasciano miniature nel frattempo.
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "src/gui/nowplaying/coverloader.h"

static int checks;
static int failures;

static void ok(bool condition, const char *fmt, ...) {
	checks++;
	va_list args;
	va_start(args, fmt);
	if (!condition) {
		printf("  NO   ");
		vprintf(fmt, args);
		printf("\n");
		failures++;
	}
	va_end(args);
}

// ---------------------------------------------------------------------------
// i finti: qui interessa il passaggio di consegne, non da dove vengono i byte
// ---------------------------------------------------------------------------

bool power_screen_is_on(void) { return true; }
void thread_be_low_priority(const char *name) { (void)name; }
void thread_set_background_level(bool foreground) { (void)foreground; }

// ---------------------------------------------------------------------------
// il lato interfaccia
// ---------------------------------------------------------------------------

// Un'immagine consegnata deve avere i pixel suoi e il descrittore che li guarda.
// Se la corsa avesse consegnato una struttura scritta a meta', e' qui che si
// vede anche senza TSan.
static void check_handover(const cover_image_t *img, const char *what) {
	if (!img->pixels) {
		return;
	}
	ok(img->dsc.data == img->pixels, "%s: il descrittore non punta ai suoi pixel", what);
	ok(img->dsc.header.w > 0 && img->dsc.header.h > 0, "%s: %ux%u", what, img->dsc.header.w,
	   img->dsc.header.h);
	ok(img->dsc.data_size == img->dsc.header.w * img->dsc.header.h * 2u, "%s: %u byte dichiarati", what,
	   img->dsc.data_size);

	volatile uint32_t sum = 0;
	for (uint32_t i = 0; i < img->dsc.data_size; i++) {
		sum += img->pixels[i];
	}
	(void)sum;
}

static const char *tracks[16];
static int track_count;

// Quanto aspettare prima di riprovare a ritirare. Il passo cambia a ogni giro e
// non e' un divisore della durata di una decodifica: quello che si vuole
// colpire e' il momento esatto in cui il worker ha appena finito e sta
// scrivendo il risultato, e un'attesa fissa quel momento lo manca sempre.
static void tiny_sleep(unsigned n) {
	struct timespec ts = {.tv_sec = 0, .tv_nsec = (long)(n % 23) * 700000L};
	nanosleep(&ts, NULL);
}

// Aspetta che il lavoro doppio del player finisca e lo ritira, con un tetto:
// senza questo il bench chiede e riparte cosi' in fretta che nessun risultato
// viene mai consegnato, e la meta' interessante del passaggio -- quella in cui
// l'interfaccia prende in mano i pixel del worker -- non si prova affatto.
static bool drain_player(void (*check)(const cover_image_t *, const char *)) {
	for (int tries = 0; tries < 400; tries++) {
		cover_image_t cover, backdrop;
		bool finished = false;
		if (coverloader_take_player(&cover, &backdrop, &finished)) {
			check(&cover, "copertina");
			check(&backdrop, "sfondo");
			cover_free(&cover);
			cover_free(&backdrop);
			return true;
		}
		if (finished) {
			return true; // finito, ma senza copertina: niente da liberare
		}
		tiny_sleep((unsigned)tries + 1);
	}
	return false;
}

int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc < 3) {
		fprintf(stderr, "uso: test_coverloader <giri> <immagine> [immagine...]\n");
		return 2;
	}
	int rounds = atoi(argv[1]);
	for (int i = 2; i < argc && track_count < 16; i++) {
		tracks[track_count++] = argv[i];
	}

	coverloader_start();

	printf("\n-- il cambio traccia che non aspetta il precedente\n");
	// La forma del guasto sul dispositivo: la traccia finisce da sola, si chiede
	// la copertina nuova, e quella di prima e' ancora sotto il decodificatore.
	for (int i = 0; i < rounds; i++) {
		coverloader_request_player(tracks[i % track_count], 360, 360, 480, 240);

		// Un giro su tre si abbandona senza ritirare: e' la strada che nel player
		// non ha un rilascio suo, e lascia il risultato parcheggiato fino alla
		// richiesta dopo. Gli altri due aspettano e prendono in mano i pixel.
		if (i % 3 == 0) {
			tiny_sleep((unsigned)i);
			continue;
		}
		ok(drain_player(check_handover), "il lavoro %d non e' mai arrivato", i);
	}
	printf("  %d cambi traccia\n", rounds);

	printf("\n-- le miniature di una lista che scorre sotto\n");
	// Le righe chiedono e rilasciano mentre il worker lavora sulla copertina
	// grande: e' il caso in cui una riga si e' gia' spostata quando il suo
	// risultato arriva, ed e' quello che la generazione deve buttare.
	for (int i = 0; i < rounds; i++) {
		int slot = i % COVERLOADER_SLOTS;
		coverloader_request(slot, tracks[i % track_count], 44);
		if (i % 4 == 0) {
			coverloader_release(slot); // la riga se n'e' andata durante la decodifica
			continue;
		}
		cover_image_t thumb;
		for (int tries = 0; tries < 400; tries++) {
			bool finished = false;
			if (coverloader_take(slot, &thumb, &finished)) {
				check_handover(&thumb, "miniatura");
				cover_free(&thumb);
				break;
			}
			if (finished) {
				break;
			}
			tiny_sleep((unsigned)(tries + 3));
		}
	}
	printf("  %d righe\n", rounds);

	printf("\n-- il salvaschermo che chiede mentre il player chiede\n");
	// Due lavori doppi in coda insieme, che e' quello che succede quando lo
	// schermo si spegne nel mezzo di un cambio traccia.
	for (int i = 0; i < rounds; i++) {
		coverloader_request_player(tracks[i % track_count], 360, 360, 480, 240);
		coverloader_request_screensaver(tracks[(i + 1) % track_count], 480, 720, 480, 200);

		cover_image_t a, b;
		for (int tries = 0; tries < 400; tries++) {
			bool finished = false;
			if (coverloader_take_screensaver(&a, &b, &finished)) {
				check_handover(&a, "foto");
				check_handover(&b, "fascia");
				cover_free(&a);
				cover_free(&b);
				break;
			}
			if (finished) {
				break;
			}
			tiny_sleep((unsigned)(tries + 5));
		}
		if (i % 2 == 0) {
			coverloader_release_screensaver(); // lo schermo si e' riacceso
		}
		drain_player(check_handover);
	}
	printf("  %d giri a due lavori\n", rounds);

	// Un ultimo momento perche' il worker finisca quello che ha in mano: una
	// corsa sull'uscita non e' quello che si sta cercando.
	tiny_sleep(6);

	printf("\n%d controlli, %d falliti\n", checks, failures);
	return failures ? 1 : 0;
}

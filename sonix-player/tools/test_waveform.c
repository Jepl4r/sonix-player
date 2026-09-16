// La forma d'onda che il layout alternativo disegna al posto della barra.
//
// Si prova la strada intera e vera: un file audio costruito qui, il worker che
// lo decodifica in sottofondo, la risposta che arriva, e il file di cache che
// la seconda volta risponde da solo. Quello che conta e' che i picchi seguano
// davvero l'inviluppo del brano -- una forma d'onda sbagliata non si vede, si
// guarda e basta, ed e' il genere di cosa che resta rotta per mesi.
//
// Uso: run_waveform_bench.sh
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include "src/system/audio/waveform.h"

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

typedef enum {
	SHAPE_HALF,	 // silenzio, poi forte
	SHAPE_RAMP,	 // da zero a fondo scala
	SHAPE_GATED, // impulsi a fondo scala sempre piu' fitti
} shape_t;

// Un WAV a 16 bit, 44100 Hz, mono, con l'inviluppo che gli si chiede.
//
// SHAPE_GATED e' il caso che conta e che ha rotto la prima versione: tocca il
// fondo scala dappertutto, quindi misurato a picco ogni colonna e' 255 e il
// disegno e' un rettangolo -- e un rettangolo, su uno schermo, e' esattamente
// la faccia dell'audio che va in clipping. E' quello che fa un master moderno.
// La forma vera sta in quanto tempo passa a quel livello, non in dove arriva.
static void write_wav(const char *path, int seconds, shape_t shape) {
	int rate = 44100;
	int frames = rate * seconds;
	FILE *f = fopen(path, "wb");
	if (!f) {
		fprintf(stderr, "non riesco a scrivere %s\n", path);
		exit(2);
	}

	uint32_t data_bytes = (uint32_t)frames * 2;
	uint32_t riff = 36 + data_bytes;
	uint16_t one = 1, chans = 1, bits = 16, align = 2;
	uint32_t fmt_size = 16, byte_rate = (uint32_t)rate * 2, srate = (uint32_t)rate;

	fwrite("RIFF", 1, 4, f);
	fwrite(&riff, 4, 1, f);
	fwrite("WAVEfmt ", 1, 8, f);
	fwrite(&fmt_size, 4, 1, f);
	fwrite(&one, 2, 1, f);
	fwrite(&chans, 2, 1, f);
	fwrite(&srate, 4, 1, f);
	fwrite(&byte_rate, 4, 1, f);
	fwrite(&align, 2, 1, f);
	fwrite(&bits, 2, 1, f);
	fwrite("data", 1, 4, f);
	fwrite(&data_bytes, 4, 1, f);

	for (int i = 0; i < frames; i++) {
		double t = (double)i / frames;
		double amp;
		switch (shape) {
		case SHAPE_RAMP:
			amp = t;
			break;
		case SHAPE_GATED:
			// Un impulso ogni millesimo di secondo, a fondo scala, largo da un
			// decimo a tutto: il picco non cambia mai, la potenza media sale.
			amp = ((i % (rate / 1000)) < (rate / 1000) * (0.1 + 0.9 * t)) ? 1.0 : 0.0;
			break;
		case SHAPE_HALF:
		default:
			amp = t < 0.5 ? 0.0 : 0.9;
			break;
		}
		double phase = (double)i * 440.0 * 2.0 * 3.14159265358979 / rate;
		int16_t v = (int16_t)(amp * 30000.0 * (phase - (int)(phase / 6.283185) * 6.283185 < 3.14159 ? 1.0 : -1.0));
		fwrite(&v, 2, 1, f);
	}
	fclose(f);
}

// Aspetta che il worker risponda, fino a `limit_ms`.
static bool wait_for(const char *path, uint8_t *bars, int limit_ms) {
	for (int waited = 0; waited < limit_ms; waited += 50) {
		if (waveform_get(path, bars)) {
			return true;
		}
		usleep(50 * 1000);
	}
	return false;
}

static long file_size(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static int average(const uint8_t *bars, int from, int to) {
	int sum = 0;
	for (int i = from; i < to; i++) {
		sum += bars[i];
	}
	return to > from ? sum / (to - from) : 0;
}

int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc != 2) {
		fprintf(stderr, "uso: test_waveform <cartella-di-lavoro>\n");
		return 2;
	}
	const char *work = argv[1];

	char quiet_then_loud[512], ramp[512], gated[512];
	snprintf(quiet_then_loud, sizeof(quiet_then_loud), "%s/half.wav", work);
	snprintf(ramp, sizeof(ramp), "%s/ramp.wav", work);
	snprintf(gated, sizeof(gated), "%s/gated.wav", work);
	write_wav(quiet_then_loud, 8, SHAPE_HALF);
	write_wav(ramp, 8, SHAPE_RAMP);
	write_wav(gated, 8, SHAPE_GATED);

	waveform_set_cache_dir(work);
	waveform_start();

	uint8_t bars[WAVEFORM_BARS];

	printf("\n-- meta' silenzio, meta' forte\n");
	ok(wait_for(quiet_then_loud, bars, 20000), "la forma arriva dal worker");

	int first = average(bars, 4, WAVEFORM_BARS / 2 - 4);
	int second = average(bars, WAVEFORM_BARS / 2 + 4, WAVEFORM_BARS - 4);
	ok(first < 20, "la prima meta' e' bassa (%d su 255)", first);
	ok(second > 200, "la seconda e' alta (%d su 255)", second);
	ok(second > first * 5, "e la differenza si vede");

	printf("\n-- una rampa sale davvero\n");
	ok(wait_for(ramp, bars, 20000), "la forma arriva");
	int quarter = average(bars, 0, WAVEFORM_BARS / 4);
	int last = average(bars, WAVEFORM_BARS * 3 / 4, WAVEFORM_BARS);
	ok(last > quarter * 2, "la fine e' molto piu' alta dell'inizio (%d contro %d)", last, quarter);

	bool monotonic = true;
	for (int i = 8; i < WAVEFORM_BARS; i += 8) {
		if (bars[i] + 12 < bars[i - 8]) {
			monotonic = false;
		}
	}
	ok(monotonic, "e non torna mai indietro lungo la strada");

	printf("\n-- fondo scala dappertutto non deve diventare un rettangolo\n");
	{
		ok(wait_for(gated, bars, 20000), "la forma arriva");

		int tallest = 0, shortest = 255;
		for (int i = 0; i < WAVEFORM_BARS; i++) {
			if (bars[i] > tallest) {
				tallest = bars[i];
			}
			if (bars[i] < shortest) {
				shortest = bars[i];
			}
		}
		ok(tallest - shortest > 60, "le colonne non sono tutte uguali (da %d a %d)", shortest, tallest);
		ok(average(bars, 0, WAVEFORM_BARS / 4) * 3 / 2 < average(bars, WAVEFORM_BARS * 3 / 4, WAVEFORM_BARS),
		   "e la fine, che sta a quel livello piu' a lungo, e' piu' alta");

		// Il tetto: nessuna colonna tocca il bordo della scatola, altrimenti la
		// forma d'onda finisce per somigliare proprio a quello che non e'.
		ok(tallest <= 240, "nessuna colonna arriva a fondo scala (la piu' alta e' %d)", tallest);
	}

	printf("\n-- la seconda volta risponde la cache\n");
	{
		char cache[600];
		snprintf(cache, sizeof(cache), "%s/.local/waveform.dat", work);
		FILE *f = fopen(cache, "rb");
		ok(f != NULL, "waveform.dat e' stato scritto");
		if (f) {
			fseek(f, 0, SEEK_END);
			long size = ftell(f);
			fclose(f);
			ok(size > (long)WAVEFORM_BARS, "e ha qualcosa dentro (%ld byte)", size);
		}

		// Una richiesta per un brano gia' fatto deve tornare pronta senza che
		// il worker riapra il file: si misura sul tempo.
		uint8_t again[WAVEFORM_BARS];
		struct timespec a, b;
		clock_gettime(CLOCK_MONOTONIC, &a);
		bool fast = wait_for(quiet_then_loud, again, 20000);
		clock_gettime(CLOCK_MONOTONIC, &b);
		long ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
		ok(fast, "il brano gia' fatto risponde");
		ok(ms < 3000, "e risponde in fretta (%ld ms)", ms);
	}

	printf("\n-- la cache segue il file, non solo il suo nome\n");
	{
		// Stessa strada, contenuto diverso. Chi tiene in mano una scheda SD
		// sostituisce i file senza cambiargli nome, e una chiave fatta solo con
		// il percorso restituisce per sempre la forma del brano di prima.
		uint8_t before[WAVEFORM_BARS], after[WAVEFORM_BARS];

		char swapped[512];
		snprintf(swapped, sizeof(swapped), "%s/swapped.wav", work);
		write_wav(swapped, 8, SHAPE_HALF);
		ok(wait_for(swapped, before, 20000), "la prima forma arriva");

		// Nove secondi invece di otto: dimensione diversa, quindi chiave
		// diversa anche se l'orologio non si e' mosso.
		//
		// In mezzo si chiede un altro brano. Il worker tiene l'ultima risposta
		// in memoria, e quella e' per strada e non per chiave: senza questo
		// passaggio risponderebbe subito con la forma di prima e la prova
		// guarderebbe la memoria invece della cache, che e' quello che qui
		// interessa.
		write_wav(swapped, 9, SHAPE_RAMP);
		uint8_t elsewhere[WAVEFORM_BARS];
		wait_for(ramp, elsewhere, 20000);

		ok(wait_for(swapped, after, 20000), "e dopo la sostituzione ne arriva un'altra");
		ok(memcmp(before, after, WAVEFORM_BARS) != 0, "che non e' quella di prima");

		// E ora la meta' che riguarda l'ora: stesso contenuto, stessa
		// dimensione, solo la data spostata indietro di un giorno. Le barre
		// verranno identiche, quindi non si guardano quelle -- si guarda se il
		// file di cache e' cresciuto di una riga, cioe' se il lavoro e' stato
		// rifatto invece di rispondere dalla riga vecchia.
		char cache[600];
		snprintf(cache, sizeof(cache), "%s/.local/waveform.dat", work);
		long grew_from = file_size(cache);

		struct utimbuf when;
		when.actime = time(NULL) - 86400;
		when.modtime = time(NULL) - 86400;
		ok(utime(swapped, &when) == 0, "si puo' spostare indietro la data del file");

		uint8_t again[WAVEFORM_BARS];
		// La risposta per questa strada e' gia' in mano al worker, quindi per
		// fargliela rifare bisogna prima chiedergli altro.
		wait_for(ramp, again, 20000);
		ok(wait_for(swapped, again, 20000), "la forma torna dopo il cambio di data");
		ok(file_size(cache) > grew_from, "ed e' stata ricalcolata (la cache e' cresciuta)");
	}

	printf("\n-- una decodifica interrotta non vale come risposta\n");
	{
		// Cambiare traccia ferma il worker a meta'. Prima questo contava come
		// "questo brano non ha una forma", e il brano restava senza per tutta
		// la sessione: la risposta vuota veniva scritta e non veniva piu'
		// chiesta.
		//
		// Si interrompe cambiando la traccia richiesta e non spegnendo lo
		// schermo: lo schermo spento non ferma piu' niente (vedi waveform.h),
		// quindi quella versione della prova sarebbe passata senza interrompere
		// nulla -- una prova che smette di provare e continua a dire "ok" e'
		// peggio di nessuna prova.
		//
		// Onesta' su cosa prova questa: il file e' lungo quattro minuti e la
		// richiesta cambia pochi millisecondi dopo, quindi il cambio cade quasi
		// sempre dentro la decodifica -- e quando ci cade, il codice vecchio
		// fallisce qui e il nuovo no. Se per ragioni di scheduling non ci
		// cadesse, passerebbe senza aver provato niente; non puo' invece
		// fallire per caso.
		char lungo[512];
		snprintf(lungo, sizeof(lungo), "%s/lungo.wav", work);
		write_wav(lungo, 240, SHAPE_RAMP);

		uint8_t none[WAVEFORM_BARS];
		waveform_get(lungo, none); // lo mette in coda
		usleep(3 * 1000);
		waveform_get(quiet_then_loud, none); // e ora il worker vuole un altro brano
		usleep(100 * 1000);

		ok(wait_for(lungo, none, 30000), "il brano interrotto viene ripreso e risponde");
	}

	printf("\n-- a schermo spento si lavora lo stesso\n");
	{
		// Questo e' il punto della modifica, quindi va scritto qui: il worker
		// NON si ferma quando lo schermo e' buio. La forma non serve adesso,
		// serve al file, e resta nella cache -- quindi lo schermo spento e'
		// anzi il momento migliore per calcolarla, perche' e' l'unico in cui
		// nessun altro vuole il core. Chi un giorno rimettesse la guardia lo
		// scoprira' qui invece che da un utente.
		char buio[512];
		snprintf(buio, sizeof(buio), "%s/buio.wav", work);
		write_wav(buio, 8, SHAPE_RAMP);

		waveform_set_screen_on(false);
		uint8_t shape[WAVEFORM_BARS];
		bool answered = wait_for(buio, shape, 20000);
		waveform_set_screen_on(true);

		ok(answered, "la forma di un brano mai visto arriva anche a schermo spento");
		ok(average(shape, WAVEFORM_BARS * 3 / 4, WAVEFORM_BARS) > average(shape, 0, WAVEFORM_BARS / 4) * 2,
		   "ed e' quella giusta, non una riga qualunque");
	}

	printf("\n-- un file che non e' audio\n");
	{
		char junk[512];
		snprintf(junk, sizeof(junk), "%s/nota.txt", work);
		FILE *f = fopen(junk, "wb");
		if (f) {
			fputs("questo non e' un brano", f);
			fclose(f);
		}
		uint8_t none[WAVEFORM_BARS];
		bool answered = wait_for(junk, none, 3000);
		ok(!answered, "non inventa una forma d'onda per un file di testo");
	}

	printf("\n%d controlli, %d falliti\n", checks, failures);
	return failures ? 1 : 0;
}

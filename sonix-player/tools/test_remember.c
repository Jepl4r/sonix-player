// Banco prova di "Ricorda traccia".
//
// Misura la regola vera: tools/extract_remember.py tira fuori da
// src/system/device_state.c le costanti, remember_state_t e
// remember_should_write(), e questo file ci fa passare sopra delle sessioni
// intere -- riproduzione, pausa, seek, fine coda, ripristino all'avvio --
// contando cosa sarebbe finito nel database.
//
// I tre buchi che il lettore ha segnalato come "non funziona sempre", e che
// qui hanno un controllo ciascuno:
//
//   1. un seek fatto IN PAUSA non veniva mai scritto. Nessuno stato cambiava, e
//      il timer dei dieci secondi gira solo mentre suona: il brano tornava su
//      dove era stata messa la pausa e non dove il dito aveva lasciato la barra;
//
//   2. quando la riproduzione si FERMA (fine della coda) non si scriveva
//      niente: la vecchia guardia era "purche' non sia fermo", quindi restava
//      l'ultima scrittura periodica, fino a dieci secondi prima della fine;
//
//   3. il RIPRISTINO all'avvio poteva cancellarsi da solo. Fra l'apertura del
//      file e l'arrivo della seek il motore riporta 0, e un giro di polling
//      dentro quella finestra scriveva 0 sopra la posizione che stava
//      ripristinando.
//
// Uso: tools/run_remember_bench.sh

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// I tre stati che device_state riporta. Stessi nomi, stesso ordine.
typedef enum {
	AUDIO_STATUS_STOPPED = 0,
	AUDIO_STATUS_PLAYING,
	AUDIO_STATUS_PAUSED,
} audio_status_t;

#include "remember_extracted.h"

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

// Quello che il database si ritroverebbe: l'ultima scrittura, e quante ce ne
// sono state.
static char db_file[512];
static double db_pos = -1;
static int db_writes;

static remember_state_t st;
static uint32_t clock_ms;

static void session_reset(void) {
	memset(&st, 0, sizeof(st));
	st.pos = -1.0;
	st.restore_pos = -1.0;
	db_file[0] = '\0';
	db_pos = -1;
	db_writes = 0;
	clock_ms = 100000; // un'ora qualunque, purche' non zero
}

// Un giro di polling del player.
static void poll(const char *file, audio_status_t status, double pos, uint32_t advance_ms) {
	clock_ms += advance_ms;
	if (remember_should_write(&st, file, status, pos, clock_ms)) {
		snprintf(db_file, sizeof(db_file), "%s", file);
		db_pos = pos;
		db_writes++;
	}
}

// Il ripristino all'avvio, come lo arma device_state_remember_restored().
static void restored(const char *file, double position) {
	snprintf(st.file, sizeof(st.file), "%s", file);
	st.status = AUDIO_STATUS_PAUSED;
	st.pos = position;
	st.saved_ms = clock_ms;
	st.have = true;
	st.restore_pos = position;
	st.restore_ms = clock_ms;
}

#define TRACK "/mnt/sd_0/Music/Album/01 brano.flac"
#define OTHER "/mnt/sd_0/Music/Album/02 brano.flac"

int main(void) {
	printf("la riproduzione normale\n");
	session_reset();
	poll(TRACK, AUDIO_STATUS_PLAYING, 0.0, 500);
	ok(db_writes == 1 && db_pos == 0.0, "un brano che parte si scrive subito");
	int before = db_writes;
	for (int i = 1; i <= 12; i++) {
		poll(TRACK, AUDIO_STATUS_PLAYING, i * 1.0, 1000); // un secondo per giro
	}
	ok(db_writes > before, "e mentre suona si riscrive dopo REMEMBER_PERIOD_MS");
	{
		// Quanto spesso: otto secondi di riproduzione non devono valere otto
		// scritture sulla scheda.
		session_reset();
		poll(TRACK, AUDIO_STATUS_PLAYING, 0.0, 500);
		int start = db_writes;
		for (int i = 1; i <= 60; i++) {
			poll(TRACK, AUDIO_STATUS_PLAYING, i * 0.5, 500); // mezzo secondo per giro
		}
		int per_30s = db_writes - start;
		ok(per_30s <= 4, "trenta secondi di riproduzione costano %d scritture, non sessanta", per_30s);
	}

	printf("\nla pausa\n");
	session_reset();
	poll(TRACK, AUDIO_STATUS_PLAYING, 0.0, 500);
	for (int i = 1; i <= 40; i++) {
		poll(TRACK, AUDIO_STATUS_PLAYING, i * 1.0, 1000);
	}
	poll(TRACK, AUDIO_STATUS_PAUSED, 40.0, 500);
	ok(db_pos == 40.0, "la pausa scrive la posizione in cui e' caduta (%.0f s)", db_pos);

	printf("\n1. il seek fatto in pausa\n");
	{
		int at_pause = db_writes;
		// Il dito porta la barra a 200 s. Lo stato non cambia: e' ancora in pausa.
		poll(TRACK, AUDIO_STATUS_PAUSED, 200.0, 500);
		ok(db_writes > at_pause, "viene scritto anche se nessuno stato e' cambiato");
		ok(db_pos == 200.0, "e la posizione scritta e' quella nuova (%.0f s)", db_pos);
	}
	{
		// E non deve scrivere a ogni giro solo perche' e' in pausa.
		int before_idle = db_writes;
		for (int i = 0; i < 20; i++) {
			poll(TRACK, AUDIO_STATUS_PAUSED, 200.0, 500);
		}
		ok(db_writes == before_idle, "ma dieci secondi fermo in pausa non scrivono niente");
	}

	printf("\n2. la fine della coda\n");
	session_reset();
	poll(TRACK, AUDIO_STATUS_PLAYING, 0.0, 500);
	for (int i = 1; i <= 12; i++) {
		poll(TRACK, AUDIO_STATUS_PLAYING, 280.0 + i * 1.0, 1000);
	}
	double last_periodic = db_pos;
	poll(TRACK, AUDIO_STATUS_STOPPED, 300.0, 500); // il brano e' finito, non c'e' il prossimo
	ok(db_pos == 300.0, "lo stop scrive dove si e' fermato (%.0f s, non %.0f)", db_pos, last_periodic);

	printf("\n3. il ripristino all'avvio\n");
	session_reset();
	restored(TRACK, 189.6);
	// La finestra: il decoder ha aperto il file, la seek non e' ancora arrivata.
	poll(TRACK, AUDIO_STATUS_PAUSED, 0.0, 500);
	poll(TRACK, AUDIO_STATUS_PAUSED, 0.0, 500);
	ok(db_writes == 0, "nessuna scrittura mentre il motore riporta ancora zero");
	// La seek arriva.
	poll(TRACK, AUDIO_STATUS_PAUSED, 189.6, 500);
	ok(db_writes == 0 || db_pos > 180.0, "e quando arriva, quello che si scrive e' la posizione vera");
	poll(TRACK, AUDIO_STATUS_PLAYING, 190.0, 500);
	ok(db_pos > 180.0, "il brano riparte da li' (%.1f s)", db_pos);

	printf("\n   e la trappola non deve restare chiusa per sempre\n");
	session_reset();
	restored(TRACK, 189.6);
	// Una seek che non arriva mai: file danneggiato, formato che non si cerca.
	for (int i = 0; i < 40; i++) {
		poll(TRACK, AUDIO_STATUS_PAUSED, 0.0, 500); // venti secondi
	}
	ok(db_writes > 0, "dopo la grazia il salvataggio riprende comunque");

	printf("\nil cambio di brano\n");
	session_reset();
	poll(TRACK, AUDIO_STATUS_PLAYING, 0.0, 500);
	for (int i = 1; i <= 10; i++) {
		poll(TRACK, AUDIO_STATUS_PLAYING, i * 1.0, 1000);
	}
	poll(OTHER, AUDIO_STATUS_PLAYING, 0.0, 500);
	ok(strcmp(db_file, OTHER) == 0 && db_pos == 0.0, "il brano nuovo si scrive subito, da zero");

	printf("\nlo zero non cancella mai una posizione buona per sbaglio\n");
	{
		// Un giro con lo stesso file a zero senza ripristino di mezzo E' un
		// riavvio del brano voluto: si scrive. Ma solo per un cambio vero.
		session_reset();
		restored(TRACK, 120.0);
		poll(TRACK, AUDIO_STATUS_PAUSED, 0.0, 500);
		ok(db_pos != 0.0, "dopo un ripristino uno zero non finisce nel database");
	}

	printf("\n%d controlli, %d falliti\n", checks, failures);
	return failures ? 1 : 0;
}

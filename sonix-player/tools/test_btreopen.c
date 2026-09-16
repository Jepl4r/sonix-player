// Cosa fa la modalita' ricevitore quando il PCM sparisce sotto.
//
// Il caso che conta e' il cambio di codec dal telefono: il dispositivo resta
// collegato per tutto il tempo, il transport sotto viene buttato giu' e
// ricostruito, e per qualche secondo non c'e' niente da aprire. Prima bastava
// un'apertura fallita per chiudere la modalita' in rosso.
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define REOPEN_WAIT_MS 5000
#define REOPEN_POLL_MS 200
#define BT_MAC_MAX 32
#define BT_NAME_MAX 64

// Il finto bluez: c'e' un mittente oppure no, e -- come quello vero -- svuota i
// buffer che gli si passano prima ancora di guardare se ha una risposta.
static bool source_present;
static const char *source_mac = "24:24:B7:C2:68:D0";
static const char *source_name = "S24 Ultra di Mattia";

static bool bluetooth_receiver_device(char *mac_out, int mac_size, char *name_out, int name_size) {
	if (mac_out && mac_size) {
		mac_out[0] = '\0';
	}
	if (name_out && name_size) {
		name_out[0] = '\0';
	}
	if (!source_present) {
		return false;
	}
	if (mac_out && mac_size) {
		snprintf(mac_out, (size_t)mac_size, "%s", source_mac);
	}
	if (name_out && name_size) {
		snprintf(name_out, (size_t)name_size, "%s", source_name);
	}
	return true;
}

#include "reopen_extracted.h"

static int failures;
static void check(const char *what, bool ok) {
	printf("  %-56s %s\n", what, ok ? "ok" : "FALLITO");
	failures += !ok;
}

static const char *name(reopen_t r) {
	switch (r) {
	case REOPEN_TRY:
		return "riprova";
	case REOPEN_GUESS:
		return "tira a indovinare";
	case REOPEN_GIVE_UP:
		return "si arrende";
	}
	return "?";
}

// Quante volte la modalita' riprova prima di arrendersi, con un'apertura che
// non riesce mai e ogni giro che costa `step` millisecondi.
static int attempts_before_giving_up(unsigned step, int *guesses) {
	bool blind = false;
	bool guessed = false;
	uint32_t since = 0;
	uint32_t clock = 1;
	int tries = 0;
	*guesses = 0;

	for (int guard = 0; guard < 10000; guard++) {
		reopen_t what = reopen_decision(blind, blind ? clock - since : 0, guessed);
		if (what == REOPEN_GIVE_UP) {
			return tries;
		}
		guessed = guessed || what == REOPEN_GUESS;
		tries += 1;
		*guesses += what == REOPEN_GUESS;
		if (!blind) {
			blind = true;
			since = clock;
		}
		clock += step;
	}
	return -1; // non si e' fermata
}

int main(void) {
	printf("finche' il PCM si apre\n");
	check("non si riprova niente", reopen_decision(false, 0, false) == REOPEN_TRY);
	check("nemmeno dopo un'ora", reopen_decision(false, 3600000u, false) == REOPEN_TRY);
	check("e il tentativo a indovinare torna disponibile",
		  reopen_decision(false, 3600000u, true) == REOPEN_TRY);

	printf("\nla finestra dopo un'apertura fallita\n");
	check("subito: riprova", reopen_decision(true, 0, false) == REOPEN_TRY);
	check("a meta': riprova", reopen_decision(true, REOPEN_WAIT_MS / 2, false) == REOPEN_TRY);
	check("un millisecondo prima: riprova", reopen_decision(true, REOPEN_WAIT_MS - 1, false) == REOPEN_TRY);
	check("allo scadere: tira a indovinare", reopen_decision(true, REOPEN_WAIT_MS, false) == REOPEN_GUESS);
	check("molto oltre, se non ha ancora indovinato: indovina",
		  reopen_decision(true, REOPEN_WAIT_MS + 600000u, false) == REOPEN_GUESS);
	check("indovinato e ancora niente: si arrende",
		  reopen_decision(true, REOPEN_WAIT_MS, true) == REOPEN_GIVE_UP);
	check("dentro la finestra l'indovinata non conta",
		  reopen_decision(true, REOPEN_WAIT_MS - 1, true) == REOPEN_TRY);

	printf("\nil giro vero, con un'apertura che non riesce mai\n");
	int guesses = 0;
	int tries = attempts_before_giving_up(REOPEN_POLL_MS, &guesses);
	printf("    %d tentativi da %d ms, di cui %d a indovinare\n", tries, REOPEN_POLL_MS, guesses);
	check("si ferma", tries > 0);
	check("ma non al primo colpo, che era il bug", tries > 1);
	check("prova per tutta la finestra", tries >= REOPEN_WAIT_MS / REOPEN_POLL_MS);
	check("e almeno una volta tira a indovinare", guesses >= 1);

	printf("\ncon i giri lenti -- read_stream_info ci mette un secondo e mezzo\n");
	tries = attempts_before_giving_up(1500, &guesses);
	printf("    %d tentativi da 1500 ms, di cui %d a indovinare\n", tries, guesses);
	check("ne fa comunque piu' di uno", tries > 1);
	check("e indovina prima di arrendersi", guesses >= 1);

	printf("\nun giro lentissimo non fa saltare l'ultima spiaggia\n");
	tries = attempts_before_giving_up(60000, &guesses);
	printf("    %d tentativi, di cui %d a indovinare\n", tries, guesses);
	check("si ferma lo stesso", tries > 0);
	check("dopo aver provato a indovinare", guesses >= 1);

	printf("\nl'indirizzo del mittente attraverso un buco\n");
	{
		char mac[BT_MAC_MAX] = "";
		char name[BT_NAME_MAX] = "";

		source_present = true;
		check("con il mittente presente lo trova",
			  still_there(mac, sizeof(mac), name, sizeof(name)) && strcmp(mac, source_mac) == 0);
		check("e ne prende anche il nome", strcmp(name, source_name) == 0);

		// Il buco vero: fra il vecchio transport che se ne va e il nuovo che
		// arriva, bluez per un istante non ha nessuna sorgente.
		source_present = false;
		check("nel buco dice di no", !still_there(mac, sizeof(mac), name, sizeof(name)));
		check("MA NON CANCELLA L'INDIRIZZO", strcmp(mac, source_mac) == 0);
		check("ne' il nome", strcmp(name, source_name) == 0);

		source_present = true;
		check("e quando torna lo ritrova",
			  still_there(mac, sizeof(mac), name, sizeof(name)) && strcmp(mac, source_mac) == 0);

		// Dieci buchi di fila non devono erodere niente.
		for (int i = 0; i < 10; i++) {
			source_present = false;
			still_there(mac, sizeof(mac), name, sizeof(name));
		}
		check("dieci buchi di fila e l'indirizzo e' ancora quello", strcmp(mac, source_mac) == 0);
	}

	printf("\n%s (allo scadere: %s)\n", failures ? "FALLITO" : "0 problemi",
		   name(reopen_decision(true, REOPEN_WAIT_MS, false)));
	return failures != 0;
}

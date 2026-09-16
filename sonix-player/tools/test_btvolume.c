// Banco per btvolume.c: compila il sorgente vero e lo fa parlare con un server
// bluealsa vero (test/mock/bluealsa-mock del suo albero), rimpiazzando solo il
// volume del player e il popup dell'interfaccia con dei finti che si possono
// leggere.
//
// I due guasti che deve dimostrare risolti:
//
//   * alla riconnessione il player mostrava 100 invece del livello vero delle
//     cuffie. Un trasporto A2DP appena creato parte dal massimo (bluealsa mette
//     volume_init_level = 0 dB = 127/127) e il livello vero arriva subito dopo
//     come AVRCP volume-changed. Leggere una volta sola e subito significa
//     leggere il valore di partenza di bluealsa;
//
//   * al cambio di traccia compariva lo slider del volume con 100. Il PCM viene
//     chiuso e riaperto, qualcosa si muove sul bus, e l'adozione ripartiva --
//     mentre e' una cosa per collegamento, non per ogni movimento.
//
// Uso: test_btvolume <indirizzo-del-dispositivo-finto>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/system/bluetooth/btvolume.h"

// --- i finti ---------------------------------------------------------------

static int player_percent = 55; // dov'e' il volume del player
static int popup_count;			// quante volte e' comparso lo slider
static int popup_last = -1;

int get_volume_percent(void) { return player_percent; }
void set_volume_percent(int percent) { player_percent = percent; }
void gui_notify_volume(int percent) {
	popup_count++;
	popup_last = percent;
	printf("      [interfaccia] slider del volume: %d%%\n", percent);
}
// L'uscita e' gia' passata al Bluetooth: e' la condizione in cui btvolume
// adotta.
bool volume_profile_is_bluetooth(void) { return true; }
void thread_be_background(const char *name) { (void)name; }
void deadline_in_ms(struct timespec *out, unsigned ms) {
	clock_gettime(CLOCK_REALTIME, out);
	out->tv_sec += ms / 1000;
	out->tv_nsec += (long)(ms % 1000) * 1000000L;
	if (out->tv_nsec >= 1000000000L) {
		out->tv_nsec -= 1000000000L;
		out->tv_sec++;
	}
}

// --- il banco --------------------------------------------------------------

static int failures, checks;

static void ok(bool condition, const char *what) {
	checks++;
	printf(condition ? "  ok       %s\n" : "  FALLITO  %s\n", what);
	if (!condition) {
		failures++;
	}
}

static void wait_ms(int ms) {
	struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
	nanosleep(&ts, NULL);
}

static char pcm_path[192];

// Muove una proprieta' del PCM dall'esterno, che e' quello che fanno le cuffie
// (per Volume) e bluealsa stessa quando rifa' il trasporto (per SoftVolume).
static void set_remote(const char *name, const char *variant) {
	char cmd[512];
	// --system con DBUS_SYSTEM_BUS_ADDRESS gia' nell'ambiente: con --address
	// dbus-send apre una connessione punto a punto e non manda Hello, e il bus
	// rifiuta tutto il resto.
	snprintf(cmd, sizeof(cmd),
			 "dbus-send --system --print-reply --dest=org.bluealsa %s "
			 "org.freedesktop.DBus.Properties.Set string:org.bluealsa.PCM1 string:%s variant:%s >/dev/null 2>&1",
			 pcm_path, name, variant);
	if (system(cmd) != 0) {
		printf("      (non sono riuscito a scrivere %s dall'esterno)\n", name);
	}
}

int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IOLBF, 0);
	const char *mac = argc > 1 ? argv[1] : "12:34:56:78:9A:BC";

	char underscored[32];
	snprintf(underscored, sizeof(underscored), "%s", mac);
	for (char *p = underscored; *p; p++) {
		if (*p == ':') {
			*p = '_';
		}
	}
	snprintf(pcm_path, sizeof(pcm_path), "/org/bluealsa/hci0/dev_%s/a2dpsrc/sink", underscored);

	btvolume_init();
	btvolume_set_sync(true);

	printf("\n-- il flusso compare; il player e' a %d%%\n", player_percent);
	btvolume_set_device(mac);
	for (int i = 0; i < 60 && !btvolume_available(); i++) {
		wait_ms(100);
	}
	ok(btvolume_available(), "btvolume ha trovato il flusso sul bus");

	// Il trasporto parte al massimo: e' esattamente il valore che il vecchio
	// codice prendeva per "il livello delle cuffie".
	printf("\n-- le cuffie dicono il loro livello dopo mezzo secondo (64/127 = 50%%)\n");
	wait_ms(500);
	ok(player_percent == 55, "prima della notifica il player non ha ancora toccato niente");
	set_remote("Volume", "uint16:16448"); // 64<<8 | 64
	wait_ms(1500);
	ok(player_percent == 50, "il player ha adottato 50%, non 100%");
	ok(popup_count == 1, "lo slider e' comparso una volta sola");
	ok(popup_last == 50, "e mostrava 50");

	printf("\n-- il cambio di traccia: bluealsa rimette SoftVolume a modo suo\n");
	int popups_before = popup_count;
	int level_before = player_percent;
	set_remote("SoftVolume", "boolean:true");
	wait_ms(1500);
	ok(popup_count == popups_before, "lo slider NON e' ricomparso");
	ok(player_percent == level_before, "e il livello del player non e' cambiato");

	printf("\n-- l'utente muove il volume: deve arrivare alle cuffie\n");
	popups_before = popup_count;
	player_percent = 30;
	btvolume_notify_local(30);
	wait_ms(800);
	ok(popup_count == popups_before, "una mossa dell'utente non fa comparire lo slider da sola");

	printf("\n-- le cuffie muovono il volume dopo l'adozione\n");
	set_remote("Volume", "uint16:25957"); // 101<<8 | 101 -> 79%
	wait_ms(800);
	ok(player_percent > 70 && player_percent < 90, "il player ha seguito le cuffie");

	printf("\n-- le cuffie mandano lo stesso livello che il player mostra gia'\n");
	popups_before = popup_count;
	int again = (player_percent * 127 + 50) / 100;
	char v[32];
	snprintf(v, sizeof(v), "uint16:%d", (again << 8) | again);
	set_remote("Volume", v);
	wait_ms(800);
	ok(popup_count == popups_before, "nessuno slider per un livello che non cambia");

	printf("\n-- un secondo collegamento in cui le cuffie non dicono niente\n");
	// Il caso delle cuffie che non mandano il volume-changed: il flusso e'
	// rimasto a fondo scala e prenderlo per buono vorrebbe dire assordare
	// l'utente. Deve vincere il livello del player.
	btvolume_set_device(NULL);
	wait_ms(500);
	player_percent = 42;
	popups_before = popup_count;
	btvolume_set_device(mac);
	wait_ms(4000); // piu' della finestra di assestamento
	ok(player_percent == 42, "senza notizie dalle cuffie il livello del player resta il suo");
	ok(popup_count == popups_before, "e non compare nessuno slider");

	printf("\n-- il dispositivo se ne va\n");
	btvolume_set_device(NULL);
	wait_ms(600);
	ok(!btvolume_available(), "btvolume ha lasciato il flusso");

	btvolume_stop();
	printf("\n%d controlli, %d falliti\n", checks, failures);
	return failures ? 1 : 0;
}

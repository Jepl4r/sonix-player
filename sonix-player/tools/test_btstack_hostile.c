// I casi che contano: quelli in cui l'altra parte NON si comporta bene.
//
// Un finto che risponde sempre bene non dimostra niente sul codice che esiste
// proprio per i casi in cui non risponde. Ogni prova qui e' una cosa vista o
// temuta sul dispositivo: l'agent che non c'e', il Pair rifiutato, il link che
// sale senza che l'audio arrivi mai, il daemon che muore mentre stiamo
// parlando, e una lista di dispositivi piu' grande del buffer di lettura.
//
// Quale scenario girare arriva in argv[1].

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#include "src/system/bluetooth/btstack.h"

static int failures;
static int checks;

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

static long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

// --- nessun agent risponde --------------------------------------------------
//
// bluez aspetta la conferma, non arriva nessuno, e risponde
// AuthenticationCanceled. Il player deve tornare falso in fretta, non restare
// dentro la chiamata: e' il thread Bluetooth, e se resta li' la pagina si
// blocca e la coda non viene piu' letta.
static void scenario_no_agent(void) {
	ok(btstack_open(), "connessione");
	ok(btstack_wait_adapter(3000), "adapter");
	// L'agent NON viene registrato di proposito.
	long began = now_ms();
	bool paired = btstack_pair("AA:BB:CC:DD:EE:FF", 20000);
	long spent = now_ms() - began;
	ok(!paired, "il Pair senza agent fallisce invece di riuscire");
	ok(spent < 18000, "e torna prima del timeout della chiamata");
	printf("  (il Pair ha impiegato %ld ms)\n", spent);
}

// --- il pairing viene rifiutato ---------------------------------------------
static void scenario_pair_fail(void) {
	ok(btstack_open(), "connessione");
	ok(btstack_wait_adapter(3000), "adapter");
	ok(btstack_register_agent(), "agent");
	ok(!btstack_pair("AA:BB:CC:DD:EE:FF", 10000), "un Pair rifiutato torna falso");
	btstack_device_t devices[8];
	int count = btstack_devices(devices, 8);
	bool paired = false;
	for (int i = 0; i < count; i++) {
		if (strcasecmp(devices[i].address, "AA:BB:CC:DD:EE:FF") == 0) {
			paired = devices[i].paired;
		}
	}
	ok(!paired, "e il dispositivo non risulta accoppiato");
}

// --- il link sale ma l'audio non arriva mai ---------------------------------
//
// E' letteralmente "dice Connesso e non suona niente". Il Connect riesce, il
// PCM di bluealsa non compare, e btstack_audio_sink deve restare falso per
// sempre: e' quello che tiene la musica sul jack invece di scriverla in un
// device che non esiste.
static void scenario_no_pcm(void) {
	ok(btstack_open(), "connessione");
	ok(btstack_wait_adapter(3000), "adapter");
	ok(btstack_connect("AA:BB:CC:DD:EE:FF", 10000), "il Connect riesce");

	char sink[32];
	bool appeared = false;
	for (int waited = 0; waited < 3000 && !appeared; waited += 100) {
		wait_ms(100);
		appeared = btstack_audio_sink(sink, sizeof(sink));
	}
	ok(!appeared, "l'audio non risulta mai pronto");

	btstack_refresh_audio();
	ok(!btstack_audio_sink(sink, sizeof(sink)), "nemmeno rileggendo la lista dei PCM");

	char codecs[8][BT_CODEC_NAME_MAX];
	char selected[BT_CODEC_NAME_MAX] = "x";
	ok(btstack_codecs("AA:BB:CC:DD:EE:FF", codecs, 8, selected, sizeof(selected)) == 0,
	   "e non ci sono codec da elencare");
}

// --- una lista piu' grande del buffer ---------------------------------------
//
// GetManagedObjects con duecento dispositivi passa abbondantemente i 16 KB che
// dbuslite sapeva leggere prima, ed e' il caso in cui la risposta veniva
// troncata a meta' o la connessione chiusa. Deve arrivare intera.
static void scenario_many(void) {
	ok(btstack_open(), "connessione");
	ok(btstack_wait_adapter(3000), "adapter");

	btstack_device_t devices[256];
	int count = btstack_devices(devices, 256);
	printf("  (letti %d dispositivi)\n", count);
	ok(count > 150, "la risposta grande arriva intera");

	bool have_pods = false, have_last = false;
	for (int i = 0; i < count; i++) {
		have_pods = have_pods || strcasecmp(devices[i].address, "18:3F:70:73:EE:48") == 0;
		// L'ultimo generato: se il buffer fosse stato troncato mancherebbe.
		have_last = have_last || strstr(devices[i].name, "numero 199") != NULL;
	}
	ok(have_pods, "il primo record e' leggibile");
	ok(have_last, "e anche l'ultimo");
	ok(btstack_alive(), "la connessione e' rimasta viva");

	// E la stessa cosa due volte di fila: il buffer viene riusato, non
	// riallocato ogni volta, ed e' li' che si rompono le cose.
	count = btstack_devices(devices, 256);
	ok(count > 150, "una seconda lettura da lo stesso risultato");
}

// --- il daemon muore mentre stiamo parlando ---------------------------------
//
// Il bus viene chiuso sotto di noi. Nessuna chiamata deve restare appesa: tutte
// devono tornare falso, e btstack_alive deve dirlo, perche' e' quello che fa
// riaprire la connessione al giro dopo.
static void scenario_bus_dies(void) {
	ok(btstack_open(), "connessione");
	ok(btstack_wait_adapter(3000), "adapter");
	ok(btstack_devices(NULL, 0) == 0, "una chiamata con argomenti vuoti non esplode");

	// Il pid del daemon arriva dal banco: ucciderlo per nome dipenderebbe da
	// quali strumenti ci sono, e questo scenario deve essere quello che dice.
	const char *bus_pid = getenv("FAKE_BUS_PID");
	ok(bus_pid != NULL, "il banco ha passato il pid del bus");
	printf("  (adesso il bus viene ucciso)\n");
	fflush(stdout);
	if (bus_pid) {
		kill((pid_t)strtol(bus_pid, NULL, 10), SIGKILL);
	}
	wait_ms(300);

	long began = now_ms();
	btstack_device_t devices[8];
	int count = btstack_devices(devices, 8);
	bool powered = false;
	bool got = btstack_get_powered(&powered);
	long spent = now_ms() - began;

	ok(count == 0, "l'elenco torna vuoto invece di restare appeso");
	ok(!got, "una lettura di proprieta' fallisce");
	ok(spent < 5000, "e nessuna delle due aspetta il timeout intero");
	printf("  (le due chiamate hanno impiegato %ld ms)\n", spent);
	ok(!btstack_alive(), "btstack_alive dice che la connessione e' caduta");
}

int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IOLBF, 0);
	const char *which = argc > 1 ? argv[1] : "";

	printf("\n== scenario: %s\n", which);
	if (strcmp(which, "no-agent") == 0) {
		scenario_no_agent();
	} else if (strcmp(which, "pair-fail") == 0) {
		scenario_pair_fail();
	} else if (strcmp(which, "no-pcm") == 0) {
		scenario_no_pcm();
	} else if (strcmp(which, "many") == 0) {
		scenario_many();
	} else if (strcmp(which, "bus-dies") == 0) {
		scenario_bus_dies();
	} else {
		printf("scenario sconosciuto: %s\n", which);
		return 2;
	}

	btstack_close();
	printf("%d controlli, %d falliti\n", checks, failures);
	return failures ? 1 : 0;
}

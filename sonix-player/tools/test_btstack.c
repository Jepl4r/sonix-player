// Banco di prova per btstack.c contro tools/fake_bluez.py su un vero
// dbus-daemon. Compila i sorgenti veri, non copie:
//
//   gcc -I. -o /tmp/test_btstack tools/test_btstack.c src/system/btstack.c \
//       src/system/dbuslite.c -lpthread
//
// e poi tools/run_btstack_bench.sh, che monta il bus e il finto bluez.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "src/system/bluetooth/btstack.h"

static int failures;
static int checks;
static volatile int changes;

static void ok(bool condition, const char *what) {
	checks++;
	if (!condition) {
		failures++;
		printf("  FALLITO  %s\n", what);
	} else {
		printf("  ok       %s\n", what);
	}
}

static void on_change(void) { changes++; }

static void wait_ms(int ms) {
	struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
	nanosleep(&ts, NULL);
}

static const btstack_device_t *find(const btstack_device_t *devices, int count, const char *address) {
	for (int i = 0; i < count; i++) {
		if (strcasecmp(devices[i].address, address) == 0) {
			return &devices[i];
		}
	}
	return NULL;
}

int main(void) {
	setvbuf(stdout, NULL, _IOLBF, 0);

	printf("\n-- connessione\n");
	ok(btstack_open(), "btstack_open");
	btstack_set_change_cb(on_change);
	ok(btstack_alive(), "la connessione e' viva");
	ok(btstack_service_ready("org.bluez"), "org.bluez sul bus");
	ok(btstack_service_ready("org.bluealsa"), "org.bluealsa sul bus");
	ok(!btstack_service_ready("org.nonesiste"), "un nome inesistente non risulta");
	ok(btstack_wait_service("org.bluez", 2000), "btstack_wait_service");

	printf("\n-- adapter\n");
	ok(btstack_adapter_ready(), "Adapter1 su /org/bluez/hci0");
	ok(btstack_wait_adapter(1000), "btstack_wait_adapter");

	bool powered = true;
	ok(btstack_get_powered(&powered) && !powered, "all'inizio Powered e' false");
	ok(btstack_set_powered(true), "Powered = true");
	ok(btstack_get_powered(&powered) && powered, "e rileggendolo e' true");
	ok(btstack_set_pairable(true), "Pairable = true");
	ok(btstack_set_discoverable(false), "Discoverable = false");
	ok(btstack_set_alias("Sonix Player"), "Alias");

	printf("\n-- agent\n");
	ok(btstack_register_agent(), "RegisterAgent + RequestDefaultAgent");

	printf("\n-- elenco dispositivi\n");
	btstack_device_t devices[16];
	int count = btstack_devices(devices, 16);
	ok(count == 3, "GetManagedObjects restituisce i tre dispositivi");

	const btstack_device_t *pods = find(devices, count, "18:3F:70:73:EE:48");
	ok(pods != NULL, "le AirPods sono nell'elenco");
	if (pods) {
		ok(strcmp(pods->name, "AirPods Pro") == 0, "il nome arriva da Alias");
		ok(pods->paired && pods->trusted && !pods->connected, "paired, trusted, non connesse");
		ok(pods->audio_sink, "hanno l'UUID di sink A2DP");
		ok(pods->rssi == -52, "l'RSSI e' un intero con segno (-52)");
	}
	const btstack_device_t *anon = find(devices, count, "11:22:33:44:55:66");
	ok(anon != NULL && !anon->audio_sink, "il dispositivo senza UUID audio non e' un sink");

	printf("\n-- discovery\n");
	ok(btstack_discovery(true), "StartDiscovery");
	ok(btstack_discovery(true), "StartDiscovery due volte non e' un errore (InProgress)");
	ok(btstack_discovery(false), "StopDiscovery");
	ok(btstack_discovery(false), "StopDiscovery due volte non e' un errore");

	printf("\n-- indirizzi non validi\n");
	ok(!btstack_connect("non-un-indirizzo", 1000), "un indirizzo malformato non parte");
	ok(!btstack_connect("AA:BB:CC:DD:EE", 1000), "un indirizzo corto non parte");
	ok(!btstack_connect("AA:BB:CC:DD:EE:GG", 1000), "una cifra non esadecimale non passa");
	ok(!btstack_connect("../../etc/passwd", 1000), "niente che assomigli a un percorso");

	printf("\n-- pairing (l'agent deve rispondere da solo)\n");
	int before = changes;
	ok(btstack_pair("AA:BB:CC:DD:EE:FF", 15000), "Pair con conferma dell'agent");
	ok(btstack_trust("AA:BB:CC:DD:EE:FF", true), "Trusted = true");
	wait_ms(300);
	ok(changes > before, "il cambio di proprieta' ha svegliato il callback");

	count = btstack_devices(devices, 16);
	const btstack_device_t *sony = find(devices, count, "AA:BB:CC:DD:EE:FF");
	ok(sony && sony->paired, "dopo il Pair risulta accoppiata");
	ok(sony && sony->trusted, "e fidata");

	printf("\n-- connessione e comparsa del PCM\n");
	char sink[32];
	ok(!btstack_audio_sink(sink, sizeof(sink)), "prima del Connect non c'e' nessun sink");
	ok(btstack_connect("AA:BB:CC:DD:EE:FF", 5000), "Connect");
	ok(!btstack_audio_sink(sink, sizeof(sink)), "il link su non e' ancora l'audio pronto");

	bool arrived = false;
	for (int waited = 0; waited < 3000 && !arrived; waited += 100) {
		wait_ms(100);
		arrived = btstack_audio_sink(sink, sizeof(sink));
	}
	ok(arrived, "il PCM di bluealsa e' comparso");
	ok(arrived && strcasecmp(sink, "AA:BB:CC:DD:EE:FF") == 0, "ed e' quello del dispositivo giusto");

	// Il finto manda i passaggi del flusso poco dopo il Connect. Qui si sta
	// fermi abbastanza perche' arrivino tutti, continuando a girare: i segnali
	// si leggono mentre si chiede qualcosa, non mentre si dorme.
	for (int i = 0; i < 20; i++) {
		btstack_audio_sink(sink, sizeof(sink));
		wait_ms(100);
	}

	printf("\n-- stato del flusso A2DP\n");
	char streams[192];
	ok(btstack_a2dp_streams("AA:BB:CC:DD:EE:FF", streams, sizeof(streams)), "btstack_a2dp_streams risponde");
	// Il fake pubblica quello che gli dice FAKE_BLUEZ_TRANSPORTS; il banco lo
	// fa girare due volte, una con un flusso solo e una con il transport
	// orfano accanto a quello vivo.
	printf("   (i flussi visti: %s)\n", streams);
	{
		const char *expected = getenv("EXPECT_STREAMS");
		ok(expected == NULL || strcmp(streams, expected) == 0, "i flussi sono quelli che bluez pubblica");
	}
	ok(!btstack_a2dp_streams("non-un-indirizzo", streams, sizeof(streams)), "un indirizzo malformato non chiede nulla");
	ok(btstack_a2dp_streams("11:22:33:44:55:66", streams, sizeof(streams)) && strcmp(streams, "nessun flusso") == 0,
	   "un dispositivo non connesso non ha flussi");

	printf("\n-- codec\n");
	char codecs[8][BT_CODEC_NAME_MAX];
	char selected[BT_CODEC_NAME_MAX];
	int codec_count = btstack_codecs("AA:BB:CC:DD:EE:FF", codecs, 8, selected, sizeof(selected));
	ok(codec_count == 4, "GetCodecs elenca i quattro codec");
	bool has_ldac = false;
	for (int i = 0; i < codec_count; i++) {
		has_ldac = has_ldac || strcmp(codecs[i], "LDAC") == 0;
	}
	ok(has_ldac, "LDAC e' fra questi");
	ok(strcmp(selected, "SBC") == 0, "il codec in uso e' SBC");

	ok(btstack_select_codec("AA:BB:CC:DD:EE:FF", "LDAC"), "SelectCodec LDAC");
	btstack_codecs("AA:BB:CC:DD:EE:FF", codecs, 8, selected, sizeof(selected));
	ok(strcmp(selected, "LDAC") == 0, "adesso in uso c'e' LDAC");
	ok(!btstack_select_codec("AA:BB:CC:DD:EE:FF", "MP3"), "un codec che il sink non offre viene rifiutato");

	printf("\n-- l'identita' del percorso audio\n");
	char info[96];
	ok(btstack_audio_info("AA:BB:CC:DD:EE:FF", info, sizeof(info)), "btstack_audio_info risponde");
	printf("      %s\n", info);
	ok(strstr(info, "LDAC") != NULL, "dice il codec in uso");
	ok(strstr(info, "44100 Hz") != NULL, "dice il rate");
	ok(strstr(info, "2 ch") != NULL, "dice i canali");
	ok(strstr(info, "16 bit") != NULL, "e la larghezza dei campioni");

	printf("\n-- la versione del demone\n");
	char version[32];
	ok(btstack_bluealsa_version(version, sizeof(version)), "btstack_bluealsa_version risponde");
	printf("      bluealsa v%s\n", version);
	ok(strcmp(version, "4.3.1") == 0, "ed e' quella che il demone dichiara");

	printf("\n-- rilettura del PCM dopo una riapertura\n");
	btstack_refresh_audio();
	ok(btstack_audio_sink(sink, sizeof(sink)), "GetManagedObjects di bluealsa ritrova il PCM");

	printf("\n-- disconnessione\n");
	ok(btstack_disconnect("AA:BB:CC:DD:EE:FF"), "Disconnect");
	bool gone = false;
	for (int waited = 0; waited < 2000 && !gone; waited += 100) {
		wait_ms(100);
		gone = !btstack_audio_sink(sink, sizeof(sink));
	}
	ok(gone, "il PCM e' sparito con InterfacesRemoved");

	printf("\n-- dimenticare un dispositivo\n");
	ok(btstack_remove("18:3F:70:73:EE:48"), "RemoveDevice");
	ok(btstack_remove("99:99:99:99:99:99"), "rimuovere un device che non c'e' e' comunque un successo");
	count = btstack_devices(devices, 16);
	pods = find(devices, count, "18:3F:70:73:EE:48");
	ok(pods && !pods->paired, "le AirPods non risultano piu' accoppiate");

	printf("\n-- spegnimento ordinato\n");
	ok(btstack_set_powered(false), "Powered = false");
	btstack_close();
	ok(!btstack_alive(), "dopo la chiusura la connessione non e' viva");
	ok(!btstack_devices(devices, 16), "e nessuna chiamata riesce piu'");
	ok(!btstack_audio_sink(sink, sizeof(sink)), "la cache dei PCM e' stata svuotata");

	printf("\n%d controlli, %d falliti\n", checks, failures);
	return failures ? 1 : 0;
}

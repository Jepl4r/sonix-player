// Quando le due radio si spengono da sole con lo schermo scuro.
//
// Il minuto d'attesa esiste per non pagare una riassociazione ogni volta che
// qualcuno mette in pausa per trenta secondi. Partiva pero' da quando si
// spegneva lo schermo, e quelle due cose non succedono insieme: chi ascolta una
// radio via Wi-Fi con lo schermo gia' spento da un'ora, e mette in pausa, aveva
// il minuto gia' scaduto -- il Wi-Fi spariva nel passaggio dopo, un quarto di
// secondo piu' tardi.
//
// Le prove sotto girano su un orologio finto e sono scritte per fallire se
// l'attesa torna a essere misurata dallo schermo.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// Il mondo attorno alla regola, che il banco muove a mano.
static bool g_screen_on;
static bool g_wifi_parked;
static bool g_bt_parked;
static uint32_t g_radio_park_after_ms = 60000;

static bool fake_wifi_in_use;
static bool fake_bt_in_use;
static bool fake_bt_link;
static bool fake_wifi_enabled = true;
static bool fake_bt_enabled = true;

static int wifi_parks;
static int bt_parks;

static bool wifi_in_use(void) { return fake_wifi_in_use; }
static bool bluetooth_in_use(void) { return fake_bt_in_use; }
static bool bluetooth_busy(void) { return false; }
static bool bluetooth_audio_active(void) { return false; }
static bool bluetooth_connected_device(void *out) {
	(void)out;
	return fake_bt_link;
}
static bool wifi_get_enabled(void) { return fake_wifi_enabled; }
static bool bluetooth_get_enabled(void) { return fake_bt_enabled; }

static void park_wifi_now(void) {
	g_wifi_parked = true;
	wifi_parks++;
}
static void park_bluetooth_now(void) {
	g_bt_parked = true;
	bt_parks++;
}

#include "radiopark_extracted.h"

// Un quarto di secondo per passaggio, come il tick vero.
#define TICK_MS 250

static uint32_t clock_ms;

static int failures;

static void check(const char *what, bool ok) {
	printf("  %s   %s\n", ok ? "ok  " : "NO  ", what);
	if (!ok) {
		failures++;
	}
}

// Fa passare `ms` di tempo, un passaggio alla volta.
static void run_for(uint32_t ms) {
	for (uint32_t t = 0; t < ms; t += TICK_MS) {
		clock_ms += TICK_MS;
		park_radios_if_idle(clock_ms);
	}
}

static void reset(bool screen_on) {
	clock_ms = 1000;
	g_screen_on = screen_on;
	g_wifi_parked = false;
	g_bt_parked = false;
	fake_wifi_in_use = false;
	fake_bt_in_use = false;
	fake_bt_link = false;
	fake_wifi_enabled = true;
	fake_bt_enabled = true;
	wifi_parks = 0;
	bt_parks = 0;
	g_wifi_idle_since = clock_ms;
	g_bt_idle_since = clock_ms;
	park_radios_if_idle(clock_ms);
}

int main(void) {
	printf("con lo schermo acceso non si spegne niente\n");
	reset(true);
	run_for(10 * 60 * 1000);
	check("dieci minuti di schermo acceso", !g_wifi_parked && !g_bt_parked);

	printf("\nschermo scuro e niente da fare\n");
	reset(false);
	run_for(30 * 1000);
	check("dopo mezzo minuto sono ancora accese", !g_wifi_parked && !g_bt_parked);
	run_for(40 * 1000);
	check("dopo il minuto il Bluetooth e' spento", g_bt_parked);
	run_for(1000);
	check("e subito dopo anche il Wi-Fi", g_wifi_parked);
	check("una alla volta, non nello stesso passaggio", wifi_parks == 1 && bt_parks == 1);

	printf("\nquello che serve tiene su la radio\n");
	reset(false);
	fake_wifi_in_use = true;
	run_for(10 * 60 * 1000);
	check("dieci minuti di riproduzione via Wi-Fi", !g_wifi_parked);

	printf("\nla pausa dopo un'ora di schermo scuro: e' QUI che sbagliava\n");
	reset(false);
	fake_wifi_in_use = true;
	run_for(60 * 60 * 1000); // un'ora di radio con lo schermo gia' spento
	check("un'ora dopo e' ancora accesa", !g_wifi_parked);
	fake_wifi_in_use = false; // pausa
	run_for(1000);
	check("un secondo dopo la pausa e' ancora accesa", !g_wifi_parked);
	run_for(30 * 1000);
	check("mezzo minuto dopo pure", !g_wifi_parked);
	run_for(35 * 1000);
	check("e si spegne un minuto dopo la pausa, non dopo lo schermo", g_wifi_parked);

	printf("\nriprendere rimette l'attesa da capo\n");
	reset(false);
	fake_wifi_in_use = true;
	run_for(5 * 60 * 1000);
	fake_wifi_in_use = false;
	run_for(50 * 1000); // quasi scaduto
	check("a cinquanta secondi dalla pausa e' ancora accesa", !g_wifi_parked);
	fake_wifi_in_use = true; // riprende
	run_for(2000);
	fake_wifi_in_use = false; // e ripausa
	run_for(50 * 1000);
	check("dopo la ripresa i cinquanta secondi non contano piu'", !g_wifi_parked);
	run_for(15 * 1000);
	check("il minuto riparte dalla seconda pausa", g_wifi_parked);

	printf("\nle cuffie collegate tengono su anche il Wi-Fi\n");
	// Le due radio stanno sullo stesso chip: vedi la nota su wifi_wanted().
	reset(false);
	fake_bt_link = true;
	fake_bt_in_use = true;
	run_for(10 * 60 * 1000);
	check("dieci minuti di cuffie collegate", !g_wifi_parked && !g_bt_parked);
	fake_bt_link = false;
	fake_bt_in_use = false;
	run_for(30 * 1000);
	check("mezzo minuto dopo lo scollegamento sono ancora accese", !g_wifi_parked && !g_bt_parked);
	run_for(40 * 1000);
	check("e il minuto parte dallo scollegamento", g_bt_parked);

	printf("\nuna radio gia' spenta dall'utente non si tocca\n");
	reset(false);
	fake_wifi_enabled = false;
	fake_bt_enabled = false;
	run_for(5 * 60 * 1000);
	check("nessuno la spegne una seconda volta", wifi_parks == 0 && bt_parks == 0);

	printf("\nl'attesa segue l'impostazione\n");
	g_radio_park_after_ms = 5 * 60 * 1000;
	reset(false);
	run_for(4 * 60 * 1000);
	check("a quattro minuti su cinque e' ancora accesa", !g_bt_parked);
	run_for(70 * 1000);
	check("a cinque si spegne", g_bt_parked);
	g_radio_park_after_ms = 60000;

	printf("\nl'orologio che gira su se stesso non azzera l'attesa\n");
	// lv_tick_get() e' un uint32 e torna a zero dopo 49 giorni.
	reset(false);
	clock_ms = 0xFFFFF000u;
	g_wifi_idle_since = clock_ms;
	g_bt_idle_since = clock_ms;
	run_for(30 * 1000);
	check("prima del minuto, a cavallo del giro", !g_bt_parked);
	run_for(40 * 1000);
	check("e dopo il minuto si spegne lo stesso", g_bt_parked);

	printf("\n%s\n", failures ? "FALLITO" : "0 problemi");
	return failures != 0;
}

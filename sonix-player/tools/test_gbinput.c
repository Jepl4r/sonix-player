// Banco di prova del lettore del vetro (src/system/gbinput.c).
//
// Si compila e si esegue sull'HOST, non sul dispositivo:
//
//     gcc -O1 -Wall -I. -Isrc/system -DGB_CORE=1 -o /tmp/test_gbinput tools/test_gbinput.c src/system/gbinput.c -lpthread
//     /tmp/test_gbinput
//
// Gli si danno pacchetti evdev veri attraverso una FIFO e si guarda che
// maschera di tasti ne esce. Esiste perche' il corpo vero di gbinput.c e'
// dietro !HOST_BUILD -- il simulatore non lo compila nemmeno -- e l'unico
// altro modo di provarlo e' avere il dispositivo in mano.
//
// Il caso che conta e' "dito alzato: SYN_REPORT NUDO". Il gt9xx, quando se ne
// va l'ultimo dito, manda un SYN_REPORT senza nessun SYN_MT_REPORT davanti; il
// codice lo leggeva come "pacchetto che non parla di contatti" e lasciava i
// tasti premuti. Per sempre. Le altre righe sono li' perche' rompendo quel
// ramo devono rompersi anche loro.
#include "src/system/gearboy/gbinput.h"
#include "src/gb/gbcore.h"
#include <linux/input.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static const char *FIFO = "/tmp/fakeglass";
const char *panel_touch_device(void) { return FIFO; }
void panel_touch_enable(bool e) { (void)e; }
bool display_get_rotated(void) { return false; }

static volatile uint16_t last;
void gearboy_set_keys(uint16_t m) { last = m; }

static int fd;
static void ev(int type, int code, int value) {
	struct input_event e; memset(&e, 0, sizeof(e));
	e.type = type; e.code = code; e.value = value;
	if (write(fd, &e, sizeof(e)) != sizeof(e)) perror("write");
}
static void contact(int x, int y) { ev(EV_ABS, ABS_MT_POSITION_X, x); ev(EV_ABS, ABS_MT_POSITION_Y, y); ev(EV_SYN, SYN_MT_REPORT, 0); }
static void sync_(void) { ev(EV_SYN, SYN_REPORT, 0); usleep(120000); }

static int fails;
static void check(const char *what, uint16_t want) {
	printf("  %-46s atteso %#06x, ottenuto %#06x  %s\n", what, want, last, last == want ? "OK" : "<<< SBAGLIATO");
	if (last != want) fails++;
}

int main(void) {
	// le stesse zone della pagina
	gbinput_zone_t z[] = {
		{ 32, 502, 52, 52, GB_KEY_UP|GB_KEY_LEFT}, { 84, 502, 52, 52, GB_KEY_UP},
		{ 32, 554, 52, 52, GB_KEY_LEFT},           {136, 554, 52, 52, GB_KEY_RIGHT},
		{ 84, 606, 52, 52, GB_KEY_DOWN},
		{296, 566, 88, 88, GB_KEY_B},              {386, 512, 88, 88, GB_KEY_A},
		{  0,   0, 84, 84, GBINPUT_KEY_EXIT},
		{  0, 372,110, 60, GB_KEY_SELECT},         {370, 372,110, 60, GB_KEY_START},
	};
	unlink(FIFO); mkfifo(FIFO, 0600);
	if (!gbinput_start(z, sizeof(z)/sizeof(z[0]), NULL)) { puts("gbinput_start fallita"); return 1; }
	fd = open(FIFO, O_WRONLY);
	if (fd < 0) { perror("open"); return 1; }

	puts("Protocollo A (quello del gt9xx corretto):");
	contact(110, 630); sync_();                 check("un dito sul GIU'", GB_KEY_DOWN);
	ev(EV_SYN, SYN_REPORT, 0); usleep(120000);  check("dito alzato: SYN_REPORT NUDO", 0);

	contact(110, 630); contact(430, 556); sync_(); check("giu' + A insieme", GB_KEY_DOWN|GB_KEY_A);
	contact(430, 556); sync_();                 check("resta solo A", GB_KEY_A);
	ev(EV_SYN, SYN_MT_REPORT, 0); ev(EV_SYN, SYN_REPORT, 0); usleep(120000);
	                                            check("alzato: SYN_MT_REPORT vuoto", 0);

	contact(50, 520); sync_();                  check("angolo: su+sinistra", GB_KEY_UP|GB_KEY_LEFT);
	contact(50, 520); contact(340, 610); contact(430, 556); sync_();
	                                            check("tre dita: su+sx, B, A", GB_KEY_UP|GB_KEY_LEFT|GB_KEY_B|GB_KEY_A);
	ev(EV_SYN, SYN_REPORT, 0); usleep(120000);  check("tutte alzate", 0);

	puts("BTN_TOUCH ha l'ultima parola:");
	contact(110, 630); sync_();                 check("giu' premuto", GB_KEY_DOWN);
	ev(EV_KEY, BTN_TOUCH, 0); contact(110, 630); sync_();
	                                            check("BTN_TOUCH=0 con un contatto ancora riportato", 0);
	ev(EV_KEY, BTN_TOUCH, 1); contact(110, 630); sync_();
	                                            check("BTN_TOUCH=1: torna a rispondere", GB_KEY_DOWN);

	gbinput_stop();
	printf("\n%s (%d falliti)\n", fails ? "CI SONO ERRORI" : "tutto a posto", fails);
	return fails != 0;
}

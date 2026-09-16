// Verifica il fuso orario di src/system/clock.c.
//
//     gcc -O1 -Wall -o /tmp/test_clock_tz tools/test_clock_tz.c && /tmp/test_clock_tz
//
// Due cose, e tutte e due sono facili da sbagliare in modo invisibile:
//
// 1. IL SEGNO. La stringa TZ di POSIX ha il segno ROVESCIATO rispetto a come
//    si scrive un fuso: "UTC-2" vuol dire due ore A EST, cioe' quello che
//    tutti chiamano UTC+2. Sbagliarlo sposta l'orologio di quattro ore invece
//    che di zero, e sembra funzionare finche' non si guarda.
//
// 2. L'ARROTONDAMENTO al quarto d'ora, che e' come il fuso viene imparato
//    dallo scarto fra l'ora messa a mano e quella della rete. Deve reggere
//    anche i fusi a mezz'ora (India, +5:30) e quelli negativi.
//
// Le due funzioni sono copiate da clock.c perche' li' sono statiche. Se una
// delle due cambia, va cambiata anche qui -- e questo test e' il motivo per
// cui ce ne si accorge.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void tz_apply(int minutes) {
	int posix = -minutes;
	char sign = posix < 0 ? '-' : '+';
	int abs_min = posix < 0 ? -posix : posix;
	char tz[32];
	snprintf(tz, sizeof(tz), "UTC%c%d:%02d", sign, abs_min / 60, abs_min % 60);
	setenv("TZ", tz, 1);
	tzset();
	printf("  offset %+5d min -> TZ=%-12s", minutes, tz);
}

static long round_to_quarter_hour(long s) {
	long q = 900, half = q / 2;
	return s >= 0 ? ((s + half) / q) * q : -(((-s + half) / q) * q);
}

int fails;
static void check_tz(int minutes, int want_hour) {
	tz_apply(minutes);
	time_t t = 1756130400; // 2026-08-25 14:00:00 UTC
	struct tm lt; localtime_r(&t, &lt);
	printf("  14:00 UTC -> %02d:%02d locali  %s\n", lt.tm_hour, lt.tm_min,
	       lt.tm_hour == want_hour ? "OK" : "<<< SBAGLIATO");
	if (lt.tm_hour != want_hour) fails++;
	// e il giro inverso: mktime deve riportare all'UTC di partenza
	struct tm back = lt; back.tm_isdst = -1;
	time_t rt = mktime(&back);
	if (rt != t) { printf("      mktime non torna indietro: %ld invece di %ld <<< SBAGLIATO\n", (long)rt, (long)t); fails++; }
}
static void check_round(long in, long want) {
	long got = round_to_quarter_hour(in);
	printf("  arrotonda %+6ld s -> %+6ld  %s\n", in, got, got == want ? "OK" : "<<< SBAGLIATO");
	if (got != want) fails++;
}

int main(void) {
	puts("Segno del fuso (e giro di ritorno con mktime):");
	check_tz(120, 16);      // Italia estate, UTC+2
	check_tz(60, 15);       // Italia inverno
	check_tz(0, 14);        // UTC
	check_tz(-300, 9);      // New York estate, UTC-5
	check_tz(330, 19);      // India, UTC+5:30 -> 19:30
	puts("\nArrotondamento al quarto d'ora:");
	check_round(7200, 7200);      // due ore esatte
	check_round(7215, 7200);      // due ore e 15 s -> due ore
	check_round(7080, 7200);      // due minuti sotto -> due ore
	check_round(19800, 19800);    // 5h30 (India)
	check_round(-18000, -18000);  // -5h (New York)
	check_round(450, 900);        // mezzo quarto d'ora arrotonda su
	printf("\n%s (%d falliti)\n", fails ? "CI SONO ERRORI" : "tutto a posto", fails);
	return fails != 0;
}

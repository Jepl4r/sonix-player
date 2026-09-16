// La compensazione di guadagno DSD: la tabella presa dal player stock, la
// somma sull'attenuazione grezza del CS43198 e il taglio a zero.
//
// Serve perche' il segno inganna. I numeri stock sono negativi ma NON
// attenuano: il registro del DAC e' un'attenuazione, quindi togliergli qualcosa
// alza il volume. Un giorno qualcuno leggera' "-12" e pensera' "-12 dB": queste
// prove dicono cosa succede davvero.
//
// Le cifre di riferimento vengono dal documento sul player stock, sezione 12:
// a volume 100 in low gain il grezzo e' 12, e i sette passi lo portano a
// 12, 10, 8, 6, 4, 2, 0. In high gain a volume 100 il grezzo e' gia' 0 e non
// c'e' piu' spazio: qualunque compensazione resta 0.
#include <stdbool.h>
#include <stdio.h>

// Le tre variabili che with_dsd_gain() legge. Stanno prima dell'include perche'
// sono quello che il banco muove per costruire i casi, e la funzione estratta
// deve trovarle gia' dichiarate.
static int high_gain;
static int current_dop;
static int dsd_gain_index;

// Le due curve, la tabella DSD e il calcolo arrivano dal sorgente del lettore,
// non da una copia riscritta qui.
#include "dsdgain_extracted.h"

// La parte di percent_to_raw() che conta qui.
static long curve_raw(int percent) { return high_gain ? HIBY_HW_HDB[percent] : HIBY_HW_MDB[percent]; }

static int failures;

static void check(const char *what, bool ok) {
	printf("  %s   %s\n", ok ? "ok  " : "NO  ", what);
	if (!ok) {
		failures++;
	}
}

static void check_raw(const char *what, long got, long want) {
	bool ok = got == want;
	printf("  %s   %-46s %ld (atteso %ld)\n", ok ? "ok  " : "NO  ", what, got, want);
	if (!ok) {
		failures++;
	}
}

int main(void) {
	printf("la tabella e' quella del player stock\n");
	check("sette passi", (int)(sizeof(HIBY_DSD_GAIN_RAW) / sizeof(HIBY_DSD_GAIN_RAW[0])) == DSD_GAIN_STEPS);
	check("il primo non sposta niente", HIBY_DSD_GAIN_RAW[0] == 0);
	bool two_apart = true;
	for (int i = 1; i < DSD_GAIN_STEPS; i++) {
		if (HIBY_DSD_GAIN_RAW[i] != HIBY_DSD_GAIN_RAW[i - 1] - 2) {
			two_apart = false;
		}
	}
	check("e ogni passo toglie due livelli (mezzo dB l'uno)", two_apart);
	check("l'ultimo e' -12, cioe' +6 dB", HIBY_DSD_GAIN_RAW[DSD_GAIN_STEPS - 1] == -12);

	printf("\nfuori dal DSD non cambia niente\n");
	high_gain = 0;
	current_dop = 0;
	for (int i = 0; i < DSD_GAIN_STEPS; i++) {
		dsd_gain_index = i;
		if (with_dsd_gain(curve_raw(50)) != curve_raw(50)) {
			failures++;
		}
	}
	check("il PCM resta sulla sua curva a ogni impostazione", failures == 0);

	printf("\nlow gain, volume 100: i sette valori del documento stock\n");
	high_gain = 0;
	current_dop = 1;
	static const long WANT_LOW[DSD_GAIN_STEPS] = {12, 10, 8, 6, 4, 2, 0};
	for (int i = 0; i < DSD_GAIN_STEPS; i++) {
		char label[64];
		snprintf(label, sizeof(label), i == 0 ? "%d dB" : "+%d dB", i);
		dsd_gain_index = i;
		check_raw(label, with_dsd_gain(curve_raw(100)), WANT_LOW[i]);
	}

	printf("\nhigh gain, volume 100: il DAC e' gia' al massimo\n");
	high_gain = 1;
	check_raw("la curva da sola", curve_raw(100), 0);
	for (int i = 0; i < DSD_GAIN_STEPS; i++) {
		dsd_gain_index = i;
		if (with_dsd_gain(curve_raw(100)) != 0) {
			failures++;
		}
	}
	check("nessuna compensazione lo porta sotto zero", with_dsd_gain(curve_raw(100)) == 0);

	printf("\na meta' scala lo spazio c'e', e il conto e' una sottrazione\n");
	high_gain = 0;
	long base = curve_raw(50);
	bool linear = true;
	for (int i = 0; i < DSD_GAIN_STEPS; i++) {
		dsd_gain_index = i;
		if (with_dsd_gain(base) != base - 2 * i) {
			linear = false;
		}
	}
	check_raw("il grezzo di partenza a volume 50", base, 62);
	check("e ogni dB in piu' ne toglie due", linear);

	printf("\nil volume e' piu' ALTO, non piu' basso\n");
	dsd_gain_index = DSD_GAIN_STEPS - 1;
	check("con la compensazione al massimo l'attenuazione scende", with_dsd_gain(base) < base);

	printf("\nmuto resta muto\n");
	dsd_gain_index = 0;
	check_raw("volume 0 senza compensazione", with_dsd_gain(curve_raw(0)), 255);
	dsd_gain_index = DSD_GAIN_STEPS - 1;
	check_raw("volume 0 con +6 dB: non diventa 243", with_dsd_gain(curve_raw(0)), 255);
	dsd_gain_index = 1;
	check_raw("ne' 253 col passo piu' piccolo", with_dsd_gain(curve_raw(0)), 255);
	dsd_gain_index = DSD_GAIN_STEPS - 1;
	check_raw("mentre volume 1 si compensa normalmente", with_dsd_gain(curve_raw(1)), 188 - 12);

	printf("\nun indice fuori tabella non esce dall'array\n");
	dsd_gain_index = 99;
	check_raw("clampato sull'ultimo", with_dsd_gain(curve_raw(100)), 0);
	high_gain = 0;
	check_raw("anche in low gain", with_dsd_gain(curve_raw(100)), 0);
	dsd_gain_index = -3;
	check_raw("e un indice negativo non compensa", with_dsd_gain(curve_raw(100)), 12);

	printf("\n%s\n", failures ? "FALLITO" : "0 problemi");
	return failures != 0;
}

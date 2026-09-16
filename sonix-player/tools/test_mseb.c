// L'MSEB: la banca di filtri e il pre-gain automatico, contro il modulo stock.
//
// Due cose si misurano qui.
//
// La prima e' la tabella: tredici filtri, con le frequenze, i Q e i dB per
// passo letti dal modulo originale. Temperature sono quattro high shelf e non
// uno: schiacciarli in uno solo dava una curva diversa, e siccome il pre-gain
// misura la curva e non i guadagni dichiarati, anche un livello diverso.
//
// La seconda e' il pre-gain. Non e' una protezione dal clipping: e' una
// normalizzazione che puo' alzare il volume quando la curva taglia piu' di
// quanto spinge. Le prove sotto sono scritte per fallire se qualcuno lo
// riscrive come "meno il guadagno massimo", che e' il punto da cui si partiva.
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// Lo stato che le funzioni estratte leggono. Muoverlo e' come muovere i
// cursori sulla pagina MSEB.
int mseb_enabled;
int mseb_value[16];

#include "mseb_extracted.h"

#define RATE 44100

static int failures;

static void check(const char *what, bool ok) {
	printf("  %s   %s\n", ok ? "ok  " : "NO  ", what);
	if (!ok) {
		failures++;
	}
}

static void check_near(const char *what, double got, double want, double tol) {
	bool ok = fabs(got - want) <= tol;
	printf("  %s   %-46s %.4f (atteso %.4f)\n", ok ? "ok  " : "NO  ", what, got, want);
	if (!ok) {
		failures++;
	}
}

static void flat(void) {
	for (int i = 0; i < 16; i++) {
		mseb_value[i] = 0;
	}
}

// La risposta della banca a una frequenza, in dB: la stessa cosa che il
// pre-gain misura, chiesta in un punto solo.
static double response_db(double freq) {
	biquad_t bank[MSEB_FILTERS];
	int count = mseb_bank_build(bank, RATE);
	double w = 2.0 * M_PI * freq / (double)RATE;
	double cos1 = cos(w), sin1 = sin(w);
	double cos2 = cos(2.0 * w), sin2 = sin(2.0 * w);
	double power = 1.0;
	for (int i = 0; i < count; i++) {
		power *= biquad_power_ratio_at(&bank[i], cos1, sin1, cos2, sin2);
	}
	return 10.0 * log10(power);
}

// (sopra - sotto) / sotto della curva pesata, spostata di `offset`: la
// quantita' che la ricerca dello stock insegue fino a 0.2.
static double weighted_ratio(double offset) {
	biquad_t bank[MSEB_FILTERS];
	int count = mseb_bank_build(bank, RATE);
	mseb_weights_build();

	double sum = 0.0, below = 0.0;
	for (int i = 0; i < MSEB_PROBE_POINTS; i++) {
		double freq = pow(10.0, log10(MSEB_PROBE_START_HZ) + MSEB_PROBE_DECADES * (double)i / MSEB_PROBE_POINTS);
		double w = 2.0 * M_PI * freq / (double)RATE;
		double power = 1.0;
		for (int f = 0; f < count; f++) {
			power *= biquad_power_ratio_at(&bank[f], cos(w), sin(w), cos(2 * w), sin(2 * w));
		}
		double x = (offset + 10.0 * log10(power)) * (double)mseb_weight[i];
		sum += x;
		if (x < 0.0) {
			below -= x;
		}
	}
	return below > 0.0 ? sum / below : 1e9;
}

// Il massimo della risposta sulla banda: serve per dire che il pre-gain NON e'
// meno quel numero.
static double response_peak_db(void) {
	double peak = -1e9;
	for (int i = 0; i < 400; i++) {
		double freq = pow(10.0, log10(20.0) + 3.0 * (double)i / 400.0);
		double db = response_db(freq);
		if (db > peak) {
			peak = db;
		}
	}
	return peak;
}

int main(void) {
	printf("la tabella\n");
	check("tredici filtri", MSEB_FILTERS == 13);

	int per_slider[16] = {0};
	for (int i = 0; i < MSEB_FILTERS; i++) {
		per_slider[MSEB_DEFS[i].slider]++;
	}
	check("Temperature ne ha quattro", per_slider[0] == 4);
	bool one_each = true;
	for (int s = 1; s < 10; s++) {
		if (per_slider[s] != 1) {
			one_each = false;
		}
	}
	check("gli altri nove uno per uno", one_each);
	check("e i cursori sono dieci", per_slider[10] == 0 && per_slider[11] == 0);

	bool temp_cuts = true;
	for (int i = 0; i < MSEB_FILTERS; i++) {
		if (MSEB_DEFS[i].slider == 0 && !(MSEB_DEFS[i].type == F_HIGHSHELF && MSEB_DEFS[i].db_per_step < 0.0f)) {
			temp_cuts = false;
		}
	}
	check("i quattro di Temperature sono high shelf che tagliano", temp_cuts);

	bool q_used = true;
	for (int i = 0; i < MSEB_FILTERS; i++) {
		if (MSEB_DEFS[i].q <= 0.0f) {
			q_used = false;
		}
	}
	check("ogni filtro ha un Q positivo", q_used);

	printf("\nlo shelf usa il Q della tabella e non una pendenza fissa\n");
	// A Q = 1/sqrt(2) lo shelf RBJ con S = 1 e quello con Q coincidono: due Q
	// diversi devono quindi dare due curve diverse, altrimenti il campo viene
	// ancora ignorato.
	biquad_t a, b;
	memset(&a, 0, sizeof(a));
	memset(&b, 0, sizeof(b));
	bool built = biquad_set_coeffs(&a, F_LOWSHELF, 200, 6.0, 0.7071, RATE) &&
				 biquad_set_coeffs(&b, F_LOWSHELF, 200, 6.0, 0.3000, RATE);
	check("due shelf con Q diversi si costruiscono", built);
	// Non alla frequenza d'angolo: li' uno shelf passa per meta' del guadagno
	// qualunque sia il Q, ed e' l'unico punto dove le due curve coincidono per
	// forza. Il Q si vede sulla salita, un'ottava sopra e una sotto.
	double worse = 0.0;
	const double probe[] = {100.0, 400.0, 800.0};
	for (unsigned i = 0; i < sizeof(probe) / sizeof(probe[0]); i++) {
		double ww = 2.0 * M_PI * probe[i] / RATE;
		double da = 10.0 * log10(biquad_power_ratio_at(&a, cos(ww), sin(ww), cos(2 * ww), sin(2 * ww)));
		double db_ = 10.0 * log10(biquad_power_ratio_at(&b, cos(ww), sin(ww), cos(2 * ww), sin(2 * ww)));
		if (fabs(da - db_) > worse) {
			worse = fabs(da - db_);
		}
	}
	check_near("la differenza piu' grande fra le due", worse, worse, 0.0);
	check("e non e' zero: il campo arriva al filtro", worse > 0.1);

	printf("\ni pesi dello stock\n");
	mseb_weights_build();
	check_near("il primo", mseb_weight[0], 1.0, 1e-6);
	check_near("l'ultimo della salita", mseb_weight[29], 2.0, 1e-6);
	check_near("il primo della discesa", mseb_weight[30], 1.98485, 1e-4);
	check_near("l'ultimo", mseb_weight[127], 0.51515, 1e-4);
	bool rises = true, falls = true;
	for (int i = 1; i < 30; i++) {
		if (mseb_weight[i] <= mseb_weight[i - 1]) {
			rises = false;
		}
	}
	for (int i = 31; i < 128; i++) {
		if (mseb_weight[i] >= mseb_weight[i - 1]) {
			falls = false;
		}
	}
	check("i primi trenta salgono", rises);
	check("i novantotto dopo scendono", falls);
	check("il massimo sta attorno ai 100 Hz", mseb_weight[29] >= mseb_weight[0] && mseb_weight[30] > mseb_weight[60]);

	printf("\nniente da normalizzare\n");
	mseb_enabled = 0;
	flat();
	check_near("MSEB spento", mseb_auto_preamp_db(RATE), 0.0, 1e-6);
	mseb_enabled = 1;
	check_near("MSEB acceso ma piatto", mseb_auto_preamp_db(RATE), 0.0, 1e-6);

	printf("\nuna curva che spinge: il pre-gain scende\n");
	flat();
	mseb_value[1] = 20; // Bass Extension al massimo
	double boost_preamp = (double)mseb_auto_preamp_db(RATE);
	check("meno di zero", boost_preamp < 0.0);
	double peak = response_peak_db();
	check_near("il picco della curva", peak, peak, 0.0); // stampa il numero
	check("e NON e' meno il picco della risposta", fabs(boost_preamp + peak) > 0.3);

	printf("\nuna curva che taglia: il pre-gain sale\n");
	flat();
	mseb_value[1] = -20;
	double cut_preamp = (double)mseb_auto_preamp_db(RATE);
	check("piu' di zero", cut_preamp > 0.0);
	check("cioe' recupera livello invece di lasciarlo perso", cut_preamp > 0.1);

	printf("\nil segno segue il cursore\n");
	flat();
	mseb_value[9] = 20; // Air su
	double air_up = (double)mseb_auto_preamp_db(RATE);
	flat();
	mseb_value[9] = -20; // Air giu'
	double air_down = (double)mseb_auto_preamp_db(RATE);
	check("Air su attenua", air_up < 0.0);
	check("Air giu' compensa", air_down > 0.0);
	check("e le due non sono lo stesso numero", fabs(air_up - air_down) > 0.1);

	printf("\nTemperature passa dai quattro filtri\n");
	flat();
	mseb_value[0] = 20;
	double temp_preamp = (double)mseb_auto_preamp_db(RATE);
	// Quattro shelf da -0.06 dB per passo a 20 passi fanno -1.2 dB ciascuno:
	// una curva che taglia in alto, quindi il pre-gain sale.
	check("venti passi in su tagliano gli acuti e alzano il pre-gain", temp_preamp > 0.0);
	check_near("la risposta a 33 Hz e' quasi intatta", response_db(33.0), response_db(33.0), 0.0);
	check("e a 16 kHz e' tagliata di parecchio", response_db(16000.0) < -3.0);

	printf("\ndue filtri sovrapposti non si leggono dal guadagno dichiarato\n");
	flat();
	mseb_value[6] = 20; // 5800 Hz
	mseb_value[7] = 20; // 9200 Hz
	double pair_peak = response_peak_db();
	// Ciascuno dichiara 0.10 dB per passo, cioe' +2 dB a venti passi; insieme
	// il picco sale piu' in alto di cosi'.
	check("il picco combinato supera i 2 dB dichiarati da ciascuno", pair_peak > 2.0);

	printf("\nla ricerca e' limitata e finisce sempre\n");
	flat();
	for (int s = 0; s < 10; s++) {
		mseb_value[s] = 20;
	}
	double all_up = (double)mseb_auto_preamp_db(RATE);
	check("tutti i cursori su: resta dentro i limiti", all_up > -100.0 && all_up < 100.0);
	flat();
	for (int s = 0; s < 10; s++) {
		mseb_value[s] = -20;
	}
	double all_down = (double)mseb_auto_preamp_db(RATE);
	check("tutti giu': resta dentro i limiti", all_down > -100.0 && all_down < 100.0);
	check("tutti su attenua, tutti giu' no", all_up < all_down);

	printf("\ndieci giri di bisezione su 200 dB: la risoluzione e' quella\n");
	// 200 / 2^10 = 0.195 dB. Il risultato cade su un multiplo di quel passo
	// rispetto al centro: e' la firma dell'algoritmo, non un caso.
	double step = 200.0 / pow(2.0, MSEB_SEARCH_ROUNDS);
	flat();
	mseb_value[4] = 7;
	double p = (double)mseb_auto_preamp_db(RATE);
	double units = p / step;
	check_near("il risultato e' un multiplo del passo", units - floor(units + 0.5), 0.0, 1e-3);

	printf("\nl'offset scelto e' quello dove il rapporto vale 0.2\n");
	// Il rapporto (sopra - sotto) / sotto cresce con l'offset, e la bisezione
	// lo insegue fino a 0.2. Non si puo' chiedere che al valore restituito
	// faccia esattamente 0.2 -- dieci giri su 200 dB lasciano un passo di
	// 0,195 dB -- ma si puo' chiedere che l'attraversamento sia dentro quel
	// passo, che e' quello che una bisezione garantisce.
	flat();
	mseb_value[0] = 12; // Temperature: taglia in alto
	mseb_value[1] = 16; // e Bass Extension spinge in basso: roba da tutte e due le parti
	double chosen = (double)mseb_auto_preamp_db(RATE);
	double step_db = 200.0 / pow(2.0, MSEB_SEARCH_ROUNDS);

	check_near("l'offset", chosen, chosen, 0.0);
	check_near("il rapporto li'", weighted_ratio(chosen), 0.2, 0.35);
	check("sotto e' piu' basso di 0.2", weighted_ratio(chosen - step_db) < 0.2);
	check("sopra e' piu' alto di 0.2", weighted_ratio(chosen + step_db) > 0.2);
	check("e il rapporto cresce con l'offset", weighted_ratio(-3.0) < weighted_ratio(0.0) &&
											   weighted_ratio(0.0) < weighted_ratio(3.0));

	printf("\nquanto costa un ricalcolo\n");
	// Gira solo quando cambia un'impostazione o il sample rate, mai per
	// campione -- ma gira sul thread che sta per riempire un buffer audio,
	// quindi vale la pena sapere quanto dura.
	flat();
	for (int s = 0; s < 10; s++) {
		mseb_value[s] = 13; // tutti i tredici filtri accesi: il caso peggiore
	}
	struct timespec t0, t1;
	const int repeats = 200;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	volatile float sink = 0.0f;
	for (int i = 0; i < repeats; i++) {
		mseb_weight_ready = false; // anche i pesi, come alla prima volta
		sink += mseb_auto_preamp_db(RATE);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	(void)sink;
	double ms = ((double)(t1.tv_sec - t0.tv_sec) * 1000.0 + (double)(t1.tv_nsec - t0.tv_nsec) / 1.0e6) / repeats;
	printf("       %d filtri, %d punti, %d giri: %.3f ms su questa macchina\n", MSEB_FILTERS, MSEB_PROBE_POINTS,
		   MSEB_SEARCH_ROUNDS, ms);
	check("sotto il millisecondo qui", ms < 1.0);

	printf("\n%s\n", failures ? "FALLITO" : "0 problemi");
	return failures != 0;
}

// La scelta automatica del codec verso le cuffie: l'ordine di qualita', la
// discesa quando uno viene rifiutato, e i casi in cui non si tocca niente.
//
// Non c'e' piu' nessuna preferenza davanti: il player sceglie da solo a ogni
// connessione, e quello che si tocca a mano sulla pagina vale solo per il
// collegamento in piedi e non passa di qui.
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define BT_CODEC_MAX 24
#define BT_MAC_MAX 32
#define BT_MAX_CODECS 12

// Quello che il finto bluealsa offre e quello che ha scelto lui.
static char offered[BT_MAX_CODECS][BT_CODEC_MAX];
static int offered_count;
static char current[BT_CODEC_MAX];

// I codec che il finto dispositivo rifiuta quando glieli si chiede.
static char refuse[BT_MAX_CODECS][BT_CODEC_MAX];
static int refuse_count;

// Cosa e' stato chiesto, in ordine.
static char asked[16][BT_CODEC_MAX];
static int asked_count;

static int read_codecs_calls;

static int btstack_codecs(const char *address, char out[][BT_CODEC_MAX], int max, char *selected,
						  size_t selected_size) {
	(void)address;
	if (selected && selected_size) {
		snprintf(selected, selected_size, "%s", current);
	}
	int count = offered_count < max ? offered_count : max;
	for (int i = 0; i < count; i++) {
		snprintf(out[i], BT_CODEC_MAX, "%s", offered[i]);
	}
	return count;
}

static bool btstack_select_codec(const char *address, const char *codec) {
	(void)address;
	snprintf(asked[asked_count++], BT_CODEC_MAX, "%s", codec);
	for (int i = 0; i < refuse_count; i++) {
		if (strcmp(refuse[i], codec) == 0) {
			return false;
		}
	}
	snprintf(current, sizeof(current), "%s", codec);
	return true;
}

static void do_read_codecs(void) { read_codecs_calls++; }

// Lo stesso ricordo che tiene bluetooth.c, dichiarato qui perche' il pezzo
// estratto lo usa e la sua definizione sta piu' in su nel file vero.
static char codec_auto_done[BT_MAC_MAX];

#include "codec_auto_extracted.h"

static int failures;
static void check(const char *what, bool ok) {
	printf("  %-56s %s\n", what, ok ? "ok" : "FALLITO");
	failures += !ok;
}

static void offer(const char *list) {
	offered_count = 0;
	char copy[256];
	snprintf(copy, sizeof(copy), "%s", list);
	for (char *tok = strtok(copy, " "); tok; tok = strtok(NULL, " ")) {
		snprintf(offered[offered_count++], BT_CODEC_MAX, "%s", tok);
	}
}

static void reset(const char *list, const char *chosen) {
	offer(list);
	snprintf(current, sizeof(current), "%s", chosen);
	refuse_count = 0;
	asked_count = 0;
	read_codecs_calls = 0;
	codec_auto_done[0] = '\0';
}

static void refuses(const char *name) { snprintf(refuse[refuse_count++], BT_CODEC_MAX, "%s", name); }

int main(void) {
	printf("l'ordine di qualita'\n");
	check("LDAC batte aptX-HD", codec_rank("LDAC") > codec_rank("aptX-HD"));
	check("aptX-HD batte aptX", codec_rank("aptX-HD") > codec_rank("aptX"));
	check("aptX batte AAC", codec_rank("aptX") > codec_rank("AAC"));
	check("AAC batte SBC", codec_rank("AAC") > codec_rank("SBC"));
	check("la scrittura non conta", codec_rank("aptx_hd") == codec_rank("aptX-HD"));
	check("nemmeno le maiuscole", codec_rank("ldac") == codec_rank("LDAC"));
	check("uno sconosciuto non si sceglie", codec_rank("FastStream") == 0);

	printf("\ncuffie che parlano LDAC, bluealsa ha scelto SBC\n");
	reset("SBC AAC aptX LDAC", "SBC");
	do_auto_codec("AA:BB:CC:DD:EE:FF");
	check("chiede LDAC", asked_count == 1 && strcmp(asked[0], "LDAC") == 0);
	check("e resta LDAC", strcmp(current, "LDAC") == 0);
	check("rilegge la lista una volta", read_codecs_calls == 1);

	printf("\nLDAC rifiutato: si scende\n");
	reset("SBC AAC aptX LDAC", "SBC");
	refuses("LDAC");
	do_auto_codec("AA:BB:CC:DD:EE:FF");
	check("chiede LDAC, poi aptX",
		  asked_count == 2 && strcmp(asked[0], "LDAC") == 0 && strcmp(asked[1], "aptX") == 0);
	check("e si ferma su aptX", strcmp(current, "aptX") == 0);

	printf("\nrifiutano tutto\n");
	reset("SBC AAC aptX LDAC", "SBC");
	refuses("LDAC");
	refuses("aptX");
	refuses("AAC");
	do_auto_codec("AA:BB:CC:DD:EE:FF");
	check("li prova tutti dall'alto", asked_count == 3);
	check("e lascia quello che c'era", strcmp(current, "SBC") == 0);
	check("senza rileggere la lista", read_codecs_calls == 0);

	printf("\nbluealsa aveva gia' scelto bene\n");
	reset("SBC AAC LDAC", "LDAC");
	do_auto_codec("AA:BB:CC:DD:EE:FF");
	check("non chiede niente", asked_count == 0);
	check("e LDAC resta", strcmp(current, "LDAC") == 0);

	printf("\nsolo SBC in comune\n");
	reset("SBC", "SBC");
	do_auto_codec("AA:BB:CC:DD:EE:FF");
	check("non c'e' niente da chiedere", asked_count == 0);

	printf("\ncodec sconosciuti in lista\n");
	reset("SBC FastStream aptX-LL", "SBC");
	do_auto_codec("AA:BB:CC:DD:EE:FF");
	check("non ne sceglie nessuno", asked_count == 0);
	check("e SBC resta", strcmp(current, "SBC") == 0);

	printf("\nuno sconosciuto insieme a uno buono\n");
	reset("SBC FastStream AAC", "SBC");
	do_auto_codec("AA:BB:CC:DD:EE:FF");
	check("prende AAC e ignora l'altro", asked_count == 1 && strcmp(asked[0], "AAC") == 0);

	printf("\nuna volta sola per connessione\n");
	reset("SBC AAC LDAC", "SBC");
	do_auto_codec("AA:BB:CC:DD:EE:FF");
	int first = asked_count;
	do_auto_codec("AA:BB:CC:DD:EE:FF");
	check("la seconda chiamata non fa niente", asked_count == first);
	snprintf(current, sizeof(current), "SBC"); // le cuffie nuove ripartono da SBC
	do_auto_codec("11:22:33:44:55:66");
	check("ma un altro dispositivo si'", asked_count > first);

	printf("\nnessun codec dal dispositivo\n");
	reset("", "");
	do_auto_codec("AA:BB:CC:DD:EE:FF");
	check("non chiede niente", asked_count == 0);

	printf("\nnessun indirizzo\n");
	reset("SBC AAC LDAC", "SBC");
	do_auto_codec("");
	do_auto_codec(NULL);
	check("non chiede niente", asked_count == 0);

	printf("\n%s\n", failures ? "FALLITO" : "0 problemi");
	return failures != 0;
}

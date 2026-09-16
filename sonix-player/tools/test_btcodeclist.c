// La lettura dei codec dall'help di bluealsa e la scelta di quali chiedere con
// -c. E' il pezzo che decide cosa il dispositivo mette in aria: se sbaglia,
// l'altro capo vede meno codec di quelli che ci sono e nessuno se ne accorge,
// perche' il collegamento funziona lo stesso -- in SBC.
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define BT_MAX_CODECS 12
#define BT_CODEC_MAX 24

static const char *fake_help;
static const char *bluealsa_help(void) { return fake_help; }

static char local_source_codecs[BT_MAX_CODECS][BT_CODEC_MAX];
static int local_source_count;
static char local_sink_codecs[BT_MAX_CODECS][BT_CODEC_MAX];
static int local_sink_count;

#include "codec_list_extracted.h"

static int failures;
static void check(const char *what, bool ok) {
	printf("  %-56s %s\n", what, ok ? "ok" : "FALLITO");
	failures += !ok;
}

static void reset(const char *help) {
	fake_help = help;
	local_source_count = 0;
	local_sink_count = 0;
}

static char asked[BT_MAX_CODECS][BT_CODEC_MAX];

static int ask(void) { return codecs_to_ask_for(asked, BT_MAX_CODECS); }

static bool asked_for(const char *name, int count) {
	for (int i = 0; i < count; i++) {
		if (strcasecmp(asked[i], name) == 0) {
			return true;
		}
	}
	return false;
}

static const char *HELP_FULL =
	"Usage:\n  bluealsa -p PROFILE [OPTION]...\n"
	"\n"
	"Available BT profiles:\n"
	"  - a2dp-source\tAdvanced Audio Source (v1.4)\n"
	"  - a2dp-sink\tAdvanced Audio Sink (v1.4)\n"
	"  - hfp-ag\tHands-Free Audio Gateway (v1.9)\n"
	"\n"
	"Available BT audio codecs:\n"
	"  a2dp-source:\tSBC, AAC, aptX, aptX-HD, LDAC\n"
	"  a2dp-sink:\tSBC, AAC, aptX, aptX-HD, LDAC\n"
	"  hfp-*:\tCVSD\n";

static const char *HELP_NO_LDAC_SINK =
	"Available BT audio codecs:\n"
	"  a2dp-source:\tSBC, AAC, aptX, aptX-HD, LDAC\n"
	"  a2dp-sink:\tSBC, AAC, aptX, aptX-HD\n"
	"  hfp-*:\tCVSD\n";

static const char *HELP_BARE =
	"Available BT audio codecs:\n"
	"  a2dp-source:\tSBC\n"
	"  a2dp-sink:\tSBC\n"
	"  hfp-*:\tCVSD\n";

static const char *HELP_PROFILES_ONLY =
	"Available BT profiles:\n"
	"  - a2dp-source\tAdvanced Audio Source (v1.4)\n"
	"  - a2dp-sink\tAdvanced Audio Sink (v1.4)\n";

int main(void) {
	printf("la lettura delle due liste\n");
	reset(HELP_FULL);
	read_local_codecs();
	check("cinque codec in trasmissione", local_source_count == 5);
	check("cinque anche in ricezione", local_sink_count == 5);
	check("il primo e' SBC", strcmp(local_source_codecs[0], "SBC") == 0);
	check("l'ultimo e' LDAC", strcmp(local_source_codecs[4], "LDAC") == 0);
	check("aptX-HD non si spezza sul trattino", strcmp(local_source_codecs[3], "aptX-HD") == 0);

	printf("\nla riga dei profili non e' quella dei codec\n");
	reset(HELP_PROFILES_ONLY);
	read_local_codecs();
	check("non legge niente", local_source_count == 0 && local_sink_count == 0);
	check("e non chiede niente", ask() == 0);

	printf("\nquali codec finiscono in -c\n");
	reset(HELP_FULL);
	read_local_codecs();
	int count = ask();
	check("tre, non cinque", count == 3);
	check("SBC non si chiede", !asked_for("SBC", count));
	check("nemmeno AAC", !asked_for("AAC", count));
	check("aptX si'", asked_for("aptX", count));
	check("aptX-HD si'", asked_for("aptX-HD", count));
	check("LDAC si'", asked_for("LDAC", count));

	printf("\nun codec che sta in tutte e due le liste si chiede una volta\n");
	reset(HELP_FULL);
	read_local_codecs();
	count = ask();
	int ldacs = 0;
	for (int i = 0; i < count; i++) {
		ldacs += strcasecmp(asked[i], "LDAC") == 0;
	}
	check("LDAC compare una volta sola", ldacs == 1);

	printf("\nun codec che solo una delle due liste ha\n");
	reset(HELP_NO_LDAC_SINK);
	read_local_codecs();
	count = ask();
	check("LDAC si chiede lo stesso", asked_for("LDAC", count));
	check("sempre tre nomi", count == 3);

	printf("\nun binario con il solo SBC\n");
	reset(HELP_BARE);
	read_local_codecs();
	check("non c'e' niente da chiedere", ask() == 0);

	printf("\nbluealsa non risponde\n");
	reset("");
	read_local_codecs();
	check("nessuna lista", local_source_count == 0 && local_sink_count == 0);
	check("e nessun -c", ask() == 0);

	printf("\npiu' codec di quanti ne stanno\n");
	reset("Available BT audio codecs:\n"
		  "  a2dp-source:\tSBC, AAC, aptX, aptX-HD, LDAC, LC3plus, Opus, FastStream, aptX-LL, "
		  "aptX-TWS, MPEG, CVSDX, EXTRA1, EXTRA2\n"
		  "  a2dp-sink:\tSBC\n");
	read_local_codecs();
	check("la lista letta si ferma al massimo", local_source_count <= BT_MAX_CODECS);
	count = ask();
	check("e anche quella da chiedere", count <= BT_MAX_CODECS);
	check("senza SBC dentro", !asked_for("SBC", count));

	printf("\n%s\n", failures ? "FALLITO" : "0 problemi");
	return failures != 0;
}

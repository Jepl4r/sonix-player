// Come la radio capisce se un flusso e' MP3 o AAC.
//
// Il banco esiste per un errore vero: la prima versione cercava il primo byte
// 0xFF e guardava i tre bit dopo. Dentro i dati di un flusso AAC un byte 0xFF
// capita di continuo, e quasi qualunque byte dopo passa per un header MP3 --
// cosi' una stazione AAC che si riconnetteva finiva sul decoder sbagliato. La
// risposta e' la catena: la lunghezza dichiarata da un header deve cadere su un
// altro header.
//
// I flussi non sono inventati: il banco li fa generare a ffmpeg (AAC LC,
// AAC 5.1, MP3) e li legge dal disco. Senza ffmpeg non gira, e lo dice.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "radiocodec_extracted.h"

static int failures;

static void check(const char *what, bool ok) {
	printf("  %s   %s\n", ok ? "ok  " : "NO  ", what);
	if (!ok) {
		failures++;
	}
}

// Un file intero in memoria, o NULL.
static unsigned char *slurp(const char *path, int *len_out) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size <= 0) {
		fclose(f);
		return NULL;
	}
	unsigned char *buf = malloc((size_t)size);
	if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*len_out = (int)size;
	return buf;
}

// Il pezzo di flusso che arriva quando ci si attacca a meta' trasmissione.
static void from_middle(const unsigned char *data, int len, int off, const unsigned char **out, int *out_len) {
	if (off >= len) {
		off = 0;
	}
	*out = data + off;
	*out_len = len - off;
}

int main(int argc, char **argv) {
	if (argc != 2) {
		fprintf(stderr, "uso: test_radiocodec <cartella con lc.aac, surround.aac, test.mp3>\n");
		return 2;
	}
	const char *dir = argv[1];
	char path[512];

	printf("il Content-Type, che e' solo un indizio\n");
	check("audio/aac", codec_from_content_type("audio/aac") == RADIO_CODEC_AAC);
	check("audio/aacp", codec_from_content_type("audio/aacp") == RADIO_CODEC_AAC);
	check("application/aacp", codec_from_content_type("application/aacp") == RADIO_CODEC_AAC);
	check("audio/mp4a-latm", codec_from_content_type("audio/mp4a-latm") == RADIO_CODEC_AAC);
	check("audio/mpeg", codec_from_content_type("audio/mpeg") == RADIO_CODEC_MP3);
	check("audio/mp3", codec_from_content_type("audio/mp3") == RADIO_CODEC_MP3);
	check("AUDIO/MPEG maiuscolo", codec_from_content_type("AUDIO/MPEG") == RADIO_CODEC_MP3);
	check("audio/mpeg con charset", codec_from_content_type("audio/mpeg; charset=utf-8") == RADIO_CODEC_MP3);
	check("niente", codec_from_content_type("") == RADIO_CODEC_UNKNOWN);
	check("NULL", codec_from_content_type(NULL) == RADIO_CODEC_UNKNOWN);
	check("application/ogg non e' ne' l'uno ne' l'altro",
		  codec_from_content_type("application/ogg") == RADIO_CODEC_UNKNOWN);

	printf("\ni byte, che hanno l'ultima parola\n");

	struct {
		const char *name;
		bool is_aac;
	} files[] = {
		{"lc.aac", true},
		{"surround.aac", true},
		{"test.mp3", false},
	};

	for (unsigned f = 0; f < sizeof(files) / sizeof(files[0]); f++) {
		snprintf(path, sizeof(path), "%s/%s", dir, files[f].name);
		int len = 0;
		unsigned char *data = slurp(path, &len);
		if (!data) {
			printf("  NO     %s non si legge\n", path);
			failures++;
			continue;
		}

		char label[128];
		snprintf(label, sizeof(label), "%s dall'inizio", files[f].name);
		check(label, bytes_look_like_adts(data, len) == files[f].is_aac);

		// Venti punti sparsi nel file: attaccarsi a meta' trasmissione e' il
		// caso normale, non quello strano, e ogni offset e' un pezzo di dati
		// diverso su cui il riconoscimento puo' inciampare.
		bool all_right = true;
		int worst = -1;
		for (int i = 1; i <= 20; i++) {
			const unsigned char *part = NULL;
			int part_len = 0;
			from_middle(data, len, (int)((long)len * i / 21), &part, &part_len);
			if (part_len > 4096) {
				part_len = 4096; // quanto ne guarda il lettore
			}
			if (bytes_look_like_adts(part, part_len) != files[f].is_aac) {
				all_right = false;
				if (worst < 0) {
					worst = i;
				}
			}
		}
		snprintf(label, sizeof(label), "%s da venti punti a meta'%s", files[f].name,
				 all_right ? "" : " -- primo sbagliato qui sotto");
		check(label, all_right);
		if (!all_right) {
			printf("         punto %d/21\n", worst);
		}

		free(data);
	}

	printf("\nquello che non e' audio non diventa AAC\n");
	unsigned char noise[8192];
	unsigned seed = 12345;
	for (unsigned i = 0; i < sizeof(noise); i++) {
		seed = seed * 1103515245u + 12345u;
		noise[i] = (unsigned char)(seed >> 16);
	}
	check("rumore pseudocasuale", !bytes_look_like_adts(noise, (int)sizeof(noise)));

	unsigned char ones[4096];
	memset(ones, 0xFF, sizeof(ones));
	check("un blocco di soli 0xFF", !bytes_look_like_adts(ones, (int)sizeof(ones)));

	unsigned char zeros[4096];
	memset(zeros, 0, sizeof(zeros));
	check("un blocco di zeri", !bytes_look_like_adts(zeros, (int)sizeof(zeros)));

	printf("\nun header da solo non basta\n");
	// Un header ADTS valido che dichiara 200 byte, e poi niente.
	unsigned char lone[512];
	memset(lone, 0, sizeof(lone));
	lone[0] = 0xFF;
	lone[1] = 0xF1;
	lone[2] = 0x4C; // frequenza 3 (48000), canali 2
	lone[3] = 0x80 | ((200 >> 11) & 0x03);
	lone[4] = (unsigned char)((200 >> 3) & 0xFF);
	lone[5] = (unsigned char)(((200 & 0x07) << 5) | 0x1F);
	check("un header solo", adts_frame_len(lone, (int)sizeof(lone), 0) == 200);
	check("...ma la catena si ferma a uno", adts_chain(lone, (int)sizeof(lone), 0, 3) == 1);
	check("e quindi non e' AAC", !bytes_look_like_adts(lone, (int)sizeof(lone)));

	printf("\nbuffer troppo corti non fanno danni\n");
	check("zero byte", !bytes_look_like_adts(lone, 0));
	check("un byte", !bytes_look_like_adts(lone, 1));
	check("sei byte, uno meno di un header", !bytes_look_like_adts(lone, 6));
	check("lunghezza chiesta oltre il buffer", adts_frame_len(lone, 4, 0) == 0);

	printf("\n%s\n", failures ? "FALLITO" : "0 problemi");
	return failures != 0;
}

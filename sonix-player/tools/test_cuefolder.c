// Una cartella con dentro un cue e un file lungo, aperta dal file manager.
//
// Il difetto raccontato da un utente: nel firmware stock entrare in quella
// cartella mostrava le tracce, mentre qui partiva un unico file della durata
// dell'intero album. La libreria le tracce le ha sempre divise
// (library.c, scan_cue_sheet); la coda della cartella no, ed e' la meta' che
// conta -- una lista che mostra dodici righe e una coda che contiene un file
// solo e' peggio di tutte e due le cose sbagliate insieme.
//
// Qui si prova playlist_load_folder() vera, con cue.c vero, su una cartella
// costruita per l'occasione.
//
// Uso: run_cuefolder_bench.sh
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/playback/playlist.h"

// playlist.c e cue.c sono quelli veri, compilati da make. Quello che sta oltre
// -- l'indice della libreria e la configurazione -- e' finto qui: aprire un
// database e leggere i tag non ha niente a che vedere con la domanda, e
// tirarsi dietro la libreria vera vuol dire tirarsi dietro anche tutti i
// decodificatori.
#include "src/system/core/config.h"
#include "src/system/library/library.h"

void config_set_int(const char *s, const char *k, long v) {
	(void)s;
	(void)k;
	(void)v;
}
bool config_save(void) { return true; }

// Il nome del disco che la coda e' viaggia con la coda nel database (round
// 276), e playlist.c lo scrive appena qualcuno lo imposta. Qui non c'e' nessun
// database: interessa cosa fa la cartella con i .cue dentro, non dove finisce
// l'etichetta.
void library_queue_save_album(const char *album) { (void)album; }

library_index_t *library_index_open(library_list_t kind, library_filter_t filter, const char *value,
								   library_order_t order, bool tracks) {
	(void)kind;
	(void)filter;
	(void)value;
	(void)order;
	(void)tracks;
	return NULL;
}
void library_index_close(library_index_t *ix) { (void)ix; }
int library_index_count(const library_index_t *ix) {
	(void)ix;
	return 0;
}
bool library_index_stale(const library_index_t *ix) {
	(void)ix;
	return false;
}
int library_index_window(const library_index_t *ix, int offset, int count, library_row_cb cb, void *user) {
	(void)ix;
	(void)offset;
	(void)count;
	(void)cb;
	(void)user;
	return 0;
}
library_index_t *library_index_clone(const library_index_t *ix) {
	(void)ix;
	return NULL;
}
void library_index_describe(const library_index_t *ix, library_index_spec_t *out) {
	(void)ix;
	if (out) {
		memset(out, 0, sizeof(*out));
	}
}
int library_index_find_path(const library_index_t *ix, const char *path) {
	(void)ix;
	(void)path;
	return -1;
}

static int checks, failures;

static void ok(bool condition, const char *what) {
	checks++;
	if (condition) {
		printf("  ok       %s\n", what);
	} else {
		failures++;
		printf("  FALLITO  %s\n", what);
	}
}

static char work[256];

static void write_file(const char *name, const char *text) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", work, name);
	FILE *f = fopen(path, "wb");
	if (!f) {
		fprintf(stderr, "non riesco a scrivere %s\n", path);
		exit(2);
	}
	if (text) {
		fputs(text, f);
	} else {
		// Un po' di byte: a cue_parse basta che il file esista.
		for (int i = 0; i < 64; i++) {
			fputc(0, f);
		}
	}
	fclose(f);
}

static const char *SHEET = "PERFORMER \"John Coltrane\"\n"
						   "TITLE \"Blue Train\"\n"
						   "FILE \"album.flac\" WAVE\n"
						   "  TRACK 01 AUDIO\n"
						   "    TITLE \"Blue Train\"\n"
						   "    INDEX 01 00:00:00\n"
						   "  TRACK 02 AUDIO\n"
						   "    TITLE \"Moment's Notice\"\n"
						   "    INDEX 01 10:43:00\n"
						   "  TRACK 03 AUDIO\n"
						   "    TITLE \"Locomotion\"\n"
						   "    INDEX 01 20:12:00\n"
						   "  TRACK 04 AUDIO\n"
						   "    TITLE \"I'm Old Fashioned\"\n"
						   "    INDEX 01 27:30:00\n"
						   "  TRACK 05 AUDIO\n"
						   "    TITLE \"Lazy Bird\"\n"
						   "    INDEX 01 35:20:00\n"
						   "  TRACK 06 AUDIO\n"
						   "    TITLE \"Sesta\"\n"
						   "    INDEX 01 40:00:00\n"
						   "  TRACK 07 AUDIO\n"
						   "    TITLE \"Settima\"\n"
						   "    INDEX 01 45:00:00\n"
						   "  TRACK 08 AUDIO\n"
						   "    TITLE \"Ottava\"\n"
						   "    INDEX 01 50:00:00\n"
						   "  TRACK 09 AUDIO\n"
						   "    TITLE \"Nona\"\n"
						   "    INDEX 01 55:00:00\n"
						   "  TRACK 10 AUDIO\n"
						   "    TITLE \"Decima\"\n"
						   "    INDEX 01 60:00:00\n"
						   "  TRACK 11 AUDIO\n"
						   "    TITLE \"Undicesima\"\n"
						   "    INDEX 01 65:00:00\n";

static int track_of(const char *path) {
	const char *mark = strrchr(path ? path : "", '?');
	if (!mark || strncmp(mark, "?track=", 7) != 0) {
		return 0;
	}
	return atoi(mark + 7);
}

static bool queue_holds(const char *needle) {
	char p[1024];
	for (int i = 0; i < playlist_count(); i++) {
		if (playlist_path_at(i, p, sizeof(p)) && strstr(p, needle)) {
			return true;
		}
	}
	return false;
}

static int track_at(int index) {
	char p[1024];
	return playlist_path_at(index, p, sizeof(p)) ? track_of(p) : -1;
}

static int track_now(void) {
	char p[1024];
	return playlist_current_path(p, sizeof(p)) ? track_of(p) : -1;
}

static int track_after(bool forward) {
	char p[1024];
	bool moved = forward ? playlist_next(p, sizeof(p)) : playlist_prev(p, sizeof(p));
	return moved ? track_of(p) : -1;
}

int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc != 2) {
		fprintf(stderr, "uso: test_cuefolder <cartella-di-lavoro>\n");
		return 2;
	}
	snprintf(work, sizeof(work), "%s", argv[1]);

	printf("\n-- una cartella con album.cue e album.flac\n");
	write_file("album.cue", SHEET);
	write_file("album.flac", NULL);

	playlist_set_mode(PLAYBACK_MODE_NORMAL);
	playlist_load_folder(work, NULL);

	ok(playlist_count() == 11, "la coda ha le undici tracce del cue");
	ok(!queue_holds("album.flac"), "e non ha il file lungo che le contiene");

	// L'ordine e' quello del disco: e' la ragione per cui cue_order_tracks
	// esiste, perche' in ordine alfabetico "?track=10" sta fra 1 e 2.
	bool in_order = true;
	for (int i = 0; i < playlist_count(); i++) {
		if (track_at(i) != i + 1) {
			in_order = false;
		}
	}
	ok(in_order, "e sono in ordine di traccia, con la decima dopo la nona");

	{
		char first[1024];
		ok(playlist_path_at(0, first, sizeof(first)) && strstr(first, "album.cue?track=1") != NULL,
		   "la prima e' album.cue?track=1");
	}

	printf("\n-- partire da una traccia in particolare\n");
	playlist_load_folder(work, "album.cue?track=4");
	ok(track_now() == 4, "la coda si apre sulla traccia chiesta");
	ok(playlist_count() == 11, "e le altre dieci ci sono comunque");

	printf("\n-- la traccia dopo, e quella dopo ancora\n");
	ok(track_after(true) == 5, "avanti va alla quinta");
	ok(track_after(true) == 6, "e poi alla sesta");
	ok(track_after(false) == 5, "indietro torna alla quinta");

	printf("\n-- un file normale nella stessa cartella resta un file normale\n");
	write_file("bonus.flac", NULL);
	playlist_load_folder(work, NULL);
	ok(playlist_count() == 12, "undici tracce del cue piu' il file sciolto");
	ok(queue_holds("bonus.flac"), "il file sciolto c'e'");
	ok(!queue_holds("album.flac"), "quello reclamato dal cue no");

	printf("\n-- una cartella senza nessun cue si comporta come sempre\n");
	{
		char plain[512];
		snprintf(plain, sizeof(plain), "%s/plain", work);
		mkdir(plain, 0777);
		char path[600];
		for (int i = 1; i <= 3; i++) {
			snprintf(path, sizeof(path), "%s/0%d - brano.flac", plain, i);
			FILE *f = fopen(path, "wb");
			if (f) {
				fputc(0, f);
				fclose(f);
			}
		}
		playlist_load_folder(plain, NULL);
		ok(playlist_count() == 3, "tre file, tre voci");
		ok(!queue_holds("?track="), "e nessun percorso virtuale");
	}

	printf("\n%d controlli, %d falliti\n", checks, failures);
	return failures ? 1 : 0;
}

// Il riordino di una playlist, contro il library.c vero e uno SQLite vero.
//
// Perche' merita un banco. La colonna che ordina una playlist, `idx`, e' la
// chiave primaria della sua tabella, e non e' un rango: togliere un brano lascia
// un buco, quindi il brano in posizione 7 puo' benissimo avere idx 11. La pagina
// invece conta le righe dall'alto. library_playlist_move() lavora in posizioni e
// deve tradurle, senza mai scrivere su una chiave gia' occupata -- che sarebbe un
// errore di vincolo e un riordino a meta'.
//
// Il banco costruisce playlist vere, buchi compresi, e dopo ogni mossa rilegge
// l'ordine dal database.
//
//   tools/run_playlist_move_bench.sh

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/system/library/library.h"

// I finti che servono a linkare library.c da solo. Niente di tutto questo sta
// sul percorso del riordino: sono i pezzi dello SCAN della libreria (metadati,
// cue, mp4) e della configurazione, e il banco non fa nessuna scansione. Averli
// qui vuoti e' cio' che permette di provare la funzione VERA senza tirarsi
// dietro mezzo player.
#include "src/system/library/cue.h"
#include "src/system/library/metadata.h"
#include "src/system/decode/mp4.h"

bool config_get_bool(const char *s, const char *k, bool f) { (void)s; (void)k; return f; }
void config_set_bool(const char *s, const char *k, bool v) { (void)s; (void)k; (void)v; }
bool config_save(void) { return true; }
void crumb_set(const char *w) { (void)w; }
bool cue_is_sheet(const char *p) { (void)p; return false; }
bool cue_parse(const char *p, cue_sheet_t *o) { (void)p; (void)o; return false; }
void cue_virtual_path(const char *p, int n, char *o, size_t z) { (void)p; (void)n; if (o && z) o[0] = 0; }
bool cue_wav_has_markers(const char *p) { (void)p; return false; }
bool has_extension(const char *n, const char *e) { (void)n; (void)e; return false; }
void metadata_read(const char *p, song_metadata_t *o) { (void)p; if (o) memset(o, 0, sizeof(*o)); }
mp4_codec_t mp4_audio_codec(const mp4_file_t *m) { (void)m; return (mp4_codec_t)0; }
const unsigned char *mp4_audio_config(const mp4_file_t *m, int *l) { (void)m; if (l) *l = 0; return NULL; }
int mp4_audio_sample_rate(const mp4_file_t *m) { (void)m; return 0; }
void mp4_close(mp4_file_t *m) { (void)m; }
bool mp4_file_has_video(const char *p) { (void)p; return false; }
mp4_file_t *mp4_open(const char *p) { (void)p; return NULL; }
bool playlist_is_junk_name(const char *n) { (void)n; return false; }
void thread_be_background(const char *n) { (void)n; }

#define MAX_ROWS 32

static int failures;
static int checks;

static void ok(bool condition, const char *what) {
	checks++;
	if (!condition) {
		failures++;
		printf("  FALLITO  %s\n", what);
	} else {
		printf("  ok       %s\n", what);
	}
}

// L'ordine attuale della playlist, come una stringa di iniziali: "ABCDE".
static void order_of(const char *name, char *out, size_t size) {
	library_playlist_row_t page[MAX_ROWS];
	int got = library_playlist_page(name, 0, MAX_ROWS, page);
	size_t used = 0;
	for (int i = 0; i < got && used + 1 < size; i++) {
		out[used++] = page[i].title[0];
	}
	out[used] = '\0';
}

// Lo stesso, ma come lo vede la PAGINA: attraverso una maniglia
// library_index_t, che e' la query con WHERE present<>0. E' l'unica lettura che
// conta per il riordino, perche' le posizioni che l'utente trascina vengono da
// qui.
static bool collect_cb(const char *name, const char *path, const char *artist, void *user) {
	(void)path;
	(void)artist;
	char *out = user;
	size_t used = strlen(out);
	if (name && name[0] && used + 1 < MAX_ROWS) {
		out[used] = name[0];
		out[used + 1] = '\0';
	}
	return true;
}

static void visible_order_of(const char *name, char *out, size_t size) {
	(void)size;
	out[0] = '\0';
	library_index_t *ix = library_index_open(LIBRARY_LIST_PLAYLIST, LIBRARY_FILTER_NONE, name, LIBRARY_ORDER_DEFAULT, false);
	if (!ix) {
		return;
	}
	library_index_window(ix, 0, library_index_count(ix), collect_cb, out);
	library_index_close(ix);
}

// Marca assenti alcune posizioni della tabella (posizioni NON filtrate, come
// vuole library_playlist_set_presence).
static void mark_absent(const char *name, const int *positions, int count) {
	library_playlist_presence_t marks[MAX_ROWS];
	library_playlist_row_t page[MAX_ROWS];
	int total = library_playlist_page(name, 0, MAX_ROWS, page);
	int n = 0;
	for (int i = 0; i < total && n < MAX_ROWS; i++) {
		bool absent = false;
		for (int k = 0; k < count; k++) {
			absent = absent || positions[k] == i;
		}
		marks[n].index = i;
		marks[n].present = !absent;
		n++;
	}
	library_playlist_set_presence(name, marks, n);
}

static void add(const char *name, char letter) {
	library_playlist_row_t row = {{0}, {0}, {0}, -1, true, true};
	snprintf(row.path, sizeof(row.path), "/mnt/sd_0/Music/%c.flac", letter);
	snprintf(row.title, sizeof(row.title), "%c", letter);
	snprintf(row.artist, sizeof(row.artist), "Prova");
	library_playlist_append(name, &row);
}

// Una playlist con le lettere date, ricreata da zero.
static void build(const char *name, const char *letters) {
	library_playlist_drop(name);
	library_playlist_create(name);
	for (const char *c = letters; *c; c++) {
		add(name, *c);
	}
}

static void check_move(const char *letters, int from, int to, const char *expected, const char *what) {
	const char *name = "banco";
	build(name, letters);
	bool moved = library_playlist_move(name, from, to);
	char now[MAX_ROWS + 1];
	order_of(name, now, sizeof(now));
	if (!moved || strcmp(now, expected) != 0) {
		printf("      %s: %d -> %d ha dato \"%s\", atteso \"%s\"%s\n", letters, from, to, now, expected,
			   moved ? "" : " (la mossa ha detto di no)");
	}
	ok(moved && strcmp(now, expected) == 0, what);
}

int main(void) {
	setvbuf(stdout, NULL, _IOLBF, 0);

	char dbpath[] = "/tmp/sonix_playlist_move_XXXXXX";
	int fd = mkstemp(dbpath);
	if (fd >= 0) {
		close(fd);
		unlink(dbpath);
	}
	if (!library_open(dbpath)) {
		printf("non riesco ad aprire l'indice in %s\n", dbpath);
		return 1;
	}

	printf("\n-- il caso normale, senza buchi\n");
	check_move("ABCDE", 0, 4, "BCDEA", "il primo va in fondo");
	check_move("ABCDE", 4, 0, "EABCD", "l'ultimo va in cima");
	check_move("ABCDE", 1, 3, "ACDBE", "uno in mezzo scende");
	check_move("ABCDE", 3, 1, "ADBCE", "uno in mezzo sale");
	check_move("ABCDE", 0, 1, "BACDE", "uno solo in giu'");
	check_move("ABCDE", 1, 0, "BACDE", "uno solo in su'");
	check_move("AB", 0, 1, "BA", "una lista di due");

	printf("\n-- con i buchi in idx, che e' il caso vero\n");
	// Togliere brani lascia idx non contigui: e' li' che una funzione che
	// confonde posizione e chiave primaria sbaglia riga.
	{
		const char *name = "banco";
		build(name, "ABCDEFGH");
		library_playlist_remove_path(name, "/mnt/sd_0/Music/B.flac");
		library_playlist_remove_path(name, "/mnt/sd_0/Music/D.flac");
		library_playlist_remove_path(name, "/mnt/sd_0/Music/E.flac");
		char before[MAX_ROWS + 1];
		order_of(name, before, sizeof(before));
		ok(strcmp(before, "ACFGH") == 0, "la lista bucata parte da ACFGH");

		ok(library_playlist_move(name, 0, 4), "sposto la prima in fondo");
		char now[MAX_ROWS + 1];
		order_of(name, now, sizeof(now));
		if (strcmp(now, "CFGHA") != 0) {
			printf("      ha dato \"%s\", atteso \"CFGHA\"\n", now);
		}
		ok(strcmp(now, "CFGHA") == 0, "e l'ordine e' CFGHA");

		ok(library_playlist_move(name, 3, 1), "poi la quarta in seconda");
		order_of(name, now, sizeof(now));
		if (strcmp(now, "CHFGA") != 0) {
			printf("      ha dato \"%s\", atteso \"CHFGA\"\n", now);
		}
		ok(strcmp(now, "CHFGA") == 0, "e l'ordine e' CHFGA");

		// Nessuna riga persa e nessuna doppia: il conto e' la prova piu' secca.
		ok(strlen(now) == 5, "cinque righe, come prima");
	}

	printf("\n-- una catena lunga di mosse non perde niente\n");
	{
		const char *name = "banco";
		build(name, "ABCDEFGH");
		unsigned seed = 12345;
		for (int i = 0; i < 200; i++) {
			int from = (int)(rand_r(&seed) % 8);
			int to = (int)(rand_r(&seed) % 8);
			if (from != to) {
				library_playlist_move(name, from, to);
			}
		}
		char now[MAX_ROWS + 1];
		order_of(name, now, sizeof(now));
		char sorted[MAX_ROWS + 1];
		snprintf(sorted, sizeof(sorted), "%s", now);
		for (size_t a = 0; a + 1 < strlen(sorted); a++) {
			for (size_t b = a + 1; b < strlen(sorted); b++) {
				if (sorted[b] < sorted[a]) {
					char t = sorted[a];
					sorted[a] = sorted[b];
					sorted[b] = t;
				}
			}
		}
		printf("      dopo 200 mosse: %s\n", now);
		ok(strcmp(sorted, "ABCDEFGH") == 0, "ci sono ancora tutte e otto, una volta ciascuna");
	}

	printf("\n-- con delle righe non presenti sulla scheda\n");
	// La pagina mostra solo le righe present<>0 e l'utente trascina QUELLE
	// posizioni. Se la mossa conta anche le assenti, sposta la riga sbagliata.
	{
		const char *name = "banco";
		build(name, "ABCDEFGH");
		const int absent[] = {2, 4}; // C ed E
		mark_absent(name, absent, 2);

		char seen[MAX_ROWS + 1];
		visible_order_of(name, seen, sizeof(seen));
		if (strcmp(seen, "ABDFGH") != 0) {
			printf("      la pagina vede \"%s\", atteso \"ABDFGH\"\n", seen);
		}
		ok(strcmp(seen, "ABDFGH") == 0, "la pagina vede ABDFGH, senza C ed E");

		// Nelle posizioni della pagina: A sta in 0, D sta in 2. Portare A dopo D.
		ok(library_playlist_move(name, 0, 2), "sposto la prima riga visibile in terza");
		visible_order_of(name, seen, sizeof(seen));
		if (strcmp(seen, "BDAFGH") != 0) {
			printf("      la pagina vede \"%s\", atteso \"BDAFGH\"\n", seen);
		}
		ok(strcmp(seen, "BDAFGH") == 0, "e adesso vede BDAFGH");

		// Le assenti tengono il loro idx: non vengono ne' spostate ne' perse.
		// Non tengono pero' i loro vicini, e non c'e' modo che lo facciano --
		// le visibili si permutano fra gli idx che occupavano, e una riga
		// nascosta fra due di quelle si ritrova dall'altra parte. Qui C aveva
		// idx 3 e ce l'ha ancora; D e' passata da idx 4 a idx 2, quindi adesso
		// C le sta dopo. Non e' un ordine sbagliato: e' un ordine fra righe che
		// nessuno sta guardando, e l'unica cosa che conta e' che tornino tutte.
		char all[MAX_ROWS + 1];
		order_of(name, all, sizeof(all));
		printf("      la tabella intera e' \"%s\"\n", all);
		char sorted[MAX_ROWS + 1];
		snprintf(sorted, sizeof(sorted), "%s", all);
		for (size_t a = 0; a + 1 < strlen(sorted); a++) {
			for (size_t b = a + 1; b < strlen(sorted); b++) {
				if (sorted[b] < sorted[a]) {
					char t = sorted[a];
					sorted[a] = sorted[b];
					sorted[b] = t;
				}
			}
		}
		ok(strcmp(sorted, "ABCDEFGH") == 0, "nessuna riga persa, nemmeno le assenti");
	}

	printf("\n-- quello che non deve fare\n");
	{
		const char *name = "banco";
		build(name, "ABCDE");
		ok(!library_playlist_move(name, 2, 2), "spostare una riga su se stessa non e' una mossa");
		ok(!library_playlist_move(name, -1, 2), "una posizione negativa viene rifiutata");
		ok(!library_playlist_move(name, 2, -1), "anche come destinazione");
		ok(!library_playlist_move("non-esiste", 0, 1), "una playlist che non c'e' viene rifiutata");
		char now[MAX_ROWS + 1];
		order_of(name, now, sizeof(now));
		ok(strcmp(now, "ABCDE") == 0, "e la lista non si e' mossa");
	}

	library_playlist_drop("banco");
	library_close();
	unlink(dbpath);

	printf("\n%d controlli, %d falliti\n", checks, failures);
	return failures ? 1 : 0;
}

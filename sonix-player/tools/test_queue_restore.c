// La coda salvata e rimessa, contro il playlist.c e il library.c veri e uno
// SQLite vero.
//
// Perche' esiste. Una coda che e' una lista della libreria non viene scritta
// riga per riga -- sarebbero duecentomila righe a ogni cambio traccia -- ma come
// la domanda che l'ha costruita, piu' le poche righe che "Aggiungi alla coda" ci
// ha messo dentro dopo. Di quelle righe pero' si salvava solo il percorso, non
// il posto: al riavvio non c'era altro da fare che rimetterle dopo il brano
// corrente, e cosi' ogni accensione riportava davanti alla riproduzione brani
// ascoltati ore prima, che ripartivano da capo. A ogni riavvio, per sempre.
//
// Il banco fa il giro intero: costruisce una coda, ci aggiunge dei brani a mano,
// porta la riproduzione oltre di loro, la scrive, la rilegge come farebbe un
// avvio, e confronta l'ordine con quello di partenza.
//
//   tools/run_queue_restore_bench.sh
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/system/library/library.h"
#include "src/system/playback/playlist.h"

// I finti che servono a linkare library.c e playlist.c da soli. Nessuno di
// questi sta sul percorso della coda: sono i pezzi della SCANSIONE della
// libreria (metadati, cue, mp4) e della configurazione. Averli qui vuoti e' cio'
// che permette di provare le funzioni VERE senza tirarsi dietro mezzo player.
#include "src/system/library/cue.h"
#include "src/system/library/metadata.h"
#include "src/system/decode/mp4.h"

bool config_get_bool(const char *s, const char *k, bool f) { (void)s; (void)k; return f; }
void config_set_bool(const char *s, const char *k, bool v) { (void)s; (void)k; (void)v; }
long config_get_int(const char *s, const char *k, long f) { (void)s; (void)k; return f; }
void config_set_int(const char *s, const char *k, long v) { (void)s; (void)k; (void)v; }
bool config_save(void) { return true; }
void crumb_set(const char *w) { (void)w; }
bool cue_is_sheet(const char *p) { (void)p; return false; }
bool cue_parse(const char *p, cue_sheet_t *o) { (void)p; (void)o; return false; }
void cue_virtual_path(const char *p, int n, char *o, size_t z) { (void)p; (void)n; if (o && z) o[0] = 0; }
bool cue_wav_has_markers(const char *p) { (void)p; return false; }
bool cue_order_tracks(const char *a, const char *b, int *o) { (void)a; (void)b; (void)o; return false; }
int cue_split_path(const char *p, char *o, size_t z) { (void)p; (void)o; (void)z; return 0; }
bool has_extension(const char *n, const char *e) { (void)n; (void)e; return false; }
void metadata_read(const char *p, song_metadata_t *o) { (void)p; if (o) memset(o, 0, sizeof(*o)); }
mp4_codec_t mp4_audio_codec(const mp4_file_t *m) { (void)m; return (mp4_codec_t)0; }
const unsigned char *mp4_audio_config(const mp4_file_t *m, int *l) { (void)m; if (l) *l = 0; return NULL; }
int mp4_audio_sample_rate(const mp4_file_t *m) { (void)m; return 0; }
void mp4_close(mp4_file_t *m) { (void)m; }
bool mp4_file_has_video(const char *p) { (void)p; return false; }
mp4_file_t *mp4_open(const char *p) { (void)p; return NULL; }
void thread_be_background(const char *n) { (void)n; }

static int checks;
static int failures;

static void ok(bool condition, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#include <stdarg.h>

static void ok(bool condition, const char *fmt, ...) {
	checks++;
	va_list args;
	va_start(args, fmt);
	printf(condition ? "  ok       " : "  FALLITO  ");
	vprintf(fmt, args);
	printf("\n");
	va_end(args);
	if (!condition) {
		failures++;
	}
}

#define MAX_SLOTS 64

// La coda come una stringa di iniziali, letta slot per slot: "ABCxDE". E' la
// forma piu' corta in cui un ordine sbagliato si vede a occhio.
static void queue_string(char *out, size_t size) {
	size_t used = 0;
	int count = playlist_count();
	for (int i = 0; i < count && used + 1 < size; i++) {
		char path[512];
		if (!playlist_path_at(i, path, sizeof(path))) {
			continue;
		}
		const char *slash = strrchr(path, '/');
		out[used++] = slash ? slash[1] : path[0];
	}
	out[used] = '\0';
}

// Una playlist con le lettere date, ricreata da zero, che e' la lista della
// libreria da cui parte la coda.
static void build_list(const char *name, const char *letters) {
	library_playlist_drop(name);
	library_playlist_create(name);
	for (const char *c = letters; *c; c++) {
		library_playlist_row_t row = {{0}, {0}, {0}, -1, true, true};
		snprintf(row.path, sizeof(row.path), "/mnt/sd_0/Music/%c.flac", *c);
		snprintf(row.title, sizeof(row.title), "%c", *c);
		snprintf(row.artist, sizeof(row.artist), "Prova");
		library_playlist_append(name, &row);
	}
}

static library_index_t *open_list(const char *name) {
	return library_index_open(LIBRARY_LIST_PLAYLIST, LIBRARY_FILTER_NONE, name, LIBRARY_ORDER_DEFAULT, false);
}

// Quello che device_state.c fa a ogni cambio traccia.
static void save_queue(void) {
	library_index_spec_t spec;
	if (!playlist_library_spec(&spec)) {
		return;
	}

	int count = playlist_appended_count();
	char **extra = calloc((size_t)(count > 0 ? count : 1), sizeof(*extra));
	int *slots = calloc((size_t)(count > 0 ? count : 1), sizeof(*slots));
	playlist_appended_slots(slots, count);
	for (int i = 0; i < count; i++) {
		char path[512];
		playlist_appended_at(i, path, sizeof(path));
		extra[i] = strdup(path);
	}

	library_queue_save_query(&spec, (int)playlist_current_entry(), (const char *const *)extra, slots, count);

	for (int i = 0; i < count; i++) {
		free(extra[i]);
	}
	free(extra);
	free(slots);
}

// E quello che main.c e device_state.c fanno all'avvio dopo.
static bool restore_queue(const char *remembered) {
	library_index_spec_t spec;
	int spec_pos = 0;
	if (!library_queue_load_query(&spec, &spec_pos)) {
		return false;
	}
	library_index_t *ix = library_index_open(spec.kind, spec.filter, spec.value, spec.order, spec.desc);
	if (library_index_count(ix) <= 0) {
		library_index_close(ix);
		return false;
	}

	char **extra = NULL;
	int *slots = NULL;
	int count = library_queue_load_extra(&extra, &slots);

	playlist_clear();
	if (!playlist_load_index(ix, spec_pos)) {
		library_queue_free(extra, count);
		free(slots);
		return false;
	}
	for (int i = 0; i < count; i++) {
		if (slots && slots[i] >= 0) {
			playlist_insert_at(extra[i], slots[i]);
		} else {
			playlist_insert_next(extra[i]);
		}
	}
	library_queue_free(extra, count);
	free(slots);

	if (remembered && remembered[0]) {
		playlist_locate(remembered);
	}
	return true;
}

int main(void) {
	setvbuf(stdout, NULL, _IOLBF, 0);

	char dbpath[] = "/tmp/sonix_queue_restore_XXXXXX";
	int fd = mkstemp(dbpath);
	if (fd >= 0) {
		close(fd);
		unlink(dbpath);
	}
	if (!library_open(dbpath)) {
		printf("non riesco ad aprire l'indice in %s\n", dbpath);
		return 1;
	}

	char before[MAX_SLOTS + 1];
	char after[MAX_SLOTS + 1];
	char path[512];

	printf("\n-- il guasto: brani gia' ascoltati che tornano davanti\n");
	// Dieci brani, la riproduzione al quarto, due aggiunti a mano subito dopo.
	build_list("coda", "ABCDEFGHIJ");
	playlist_load_index(open_list("coda"), 0);
	playlist_set_current(3); // siamo su D
	playlist_insert_next("/mnt/sd_0/Music/x.flac");
	playlist_insert_next("/mnt/sd_0/Music/y.flac");
	queue_string(before, sizeof(before));
	ok(strcmp(before, "ABCDxyEFGHIJ") == 0, "i due aggiunti stanno dopo D: %s", before);

	// Si ascolta fino in fondo: adesso x e y sono passati da un pezzo.
	playlist_set_current(9); // H, ben oltre i due
	playlist_current_path(path, sizeof(path));
	ok(strcmp(path, "/mnt/sd_0/Music/H.flac") == 0, "la riproduzione e' arrivata a H");

	save_queue();
	int slots[MAX_SLOTS];
	int n = playlist_appended_slots(slots, MAX_SLOTS);
	ok(n == 2 && slots[0] == 4 && slots[1] == 5, "i posti scritti sono %d e %d", slots[0], slots[1]);

	// Si spegne e si riaccende.
	playlist_clear();
	ok(restore_queue("/mnt/sd_0/Music/H.flac"), "la coda torna");
	queue_string(after, sizeof(after));
	ok(strcmp(after, before) == 0, "ed e' la stessa di prima: %s", after);

	playlist_current_path(path, sizeof(path));
	ok(strcmp(path, "/mnt/sd_0/Music/H.flac") == 0, "sullo stesso brano di prima");

	// Il punto di tutta la faccenda: x e y sono DIETRO, non davanti.
	ok(playlist_current_index() > 5, "e i due aggiunti sono alle spalle (slot %d)", playlist_current_index());

	printf("\n-- i bordi\n");
	// In cima a tutto: la riproduzione deve restare sul brano che stava
	// suonando, non scivolare indietro di uno.
	build_list("coda", "ABCDE");
	playlist_load_index(open_list("coda"), 0);
	playlist_set_current(2); // C
	playlist_insert_at("/mnt/sd_0/Music/z.flac", 0);
	queue_string(after, sizeof(after));
	ok(strcmp(after, "zABCDE") == 0, "inserito in cima: %s", after);
	playlist_current_path(path, sizeof(path));
	ok(strcmp(path, "/mnt/sd_0/Music/C.flac") == 0, "e la riproduzione e' rimasta su C");

	// Esattamente sul brano corrente: l'inserito prende il suo posto e lui
	// scala di uno, che e' quello che vuol dire "metti qui".
	build_list("coda", "ABCDE");
	playlist_load_index(open_list("coda"), 0);
	playlist_set_current(2);
	playlist_insert_at("/mnt/sd_0/Music/z.flac", 2);
	queue_string(after, sizeof(after));
	ok(strcmp(after, "ABzCDE") == 0, "inserito sul corrente: %s", after);
	playlist_current_path(path, sizeof(path));
	ok(strcmp(path, "/mnt/sd_0/Music/C.flac") == 0, "e la riproduzione e' ancora su C");

	// Oltre la fine: si accoda, non si esce dall'array.
	build_list("coda", "ABCDE");
	playlist_load_index(open_list("coda"), 0);
	playlist_insert_at("/mnt/sd_0/Music/z.flac", 999);
	queue_string(after, sizeof(after));
	ok(strcmp(after, "ABCDEz") == 0, "inserito oltre la fine: %s", after);

	// Un posto negativo finisce in cima invece che chissa' dove.
	build_list("coda", "ABC");
	playlist_load_index(open_list("coda"), 0);
	playlist_insert_at("/mnt/sd_0/Music/z.flac", -7);
	queue_string(after, sizeof(after));
	ok(strcmp(after, "zABC") == 0, "un posto negativo va in cima: %s", after);

	printf("\n-- piu' aggiunte sparse, salvate e rimesse\n");
	// Tre aggiunte in tre momenti diversi, quindi in tre punti diversi della
	// coda: e' il caso in cui rimetterle una dopo l'altra sbaglia se non si
	// tiene conto che ogni inserimento sposta in avanti quelle dopo.
	build_list("coda", "ABCDEFGH");
	playlist_load_index(open_list("coda"), 0);
	playlist_set_current(0);
	playlist_insert_next("/mnt/sd_0/Music/x.flac"); // dopo A
	playlist_set_current(4);					   // ora su D
	playlist_insert_next("/mnt/sd_0/Music/y.flac"); // dopo D
	playlist_set_current(7);					   // ora su F
	playlist_insert_next("/mnt/sd_0/Music/z.flac"); // dopo F
	queue_string(before, sizeof(before));
	ok(strcmp(before, "AxBCDyEFzGH") == 0, "tre aggiunte sparse: %s", before);

	playlist_set_current(10); // fino in fondo, tutte e tre passate
	save_queue();
	playlist_clear();
	ok(restore_queue("/mnt/sd_0/Music/H.flac"), "la coda con tre aggiunte torna");
	queue_string(after, sizeof(after));
	ok(strcmp(after, before) == 0, "e l'ordine e' identico: %s", after);

	printf("\n-- una coda scritta prima che i posti esistessero\n");
	// Le righe vecchie non hanno lo slot. Devono tornare come tornavano, dopo
	// il brano corrente: non e' giusto, ma e' quello che quella coda dice, e
	// inventare un posto sarebbe peggio.
	build_list("coda", "ABCDE");
	playlist_load_index(open_list("coda"), 0);
	playlist_set_current(1);
	{
		library_index_spec_t spec;
		playlist_library_spec(&spec);
		const char *old_extra[] = {"/mnt/sd_0/Music/v.flac"};
		library_queue_save_query(&spec, 1, old_extra, NULL, 1); // NULL: nessun posto
	}
	playlist_clear();
	ok(restore_queue("/mnt/sd_0/Music/B.flac"), "la coda vecchia torna");
	queue_string(after, sizeof(after));
	ok(strcmp(after, "ABvCDE") == 0, "e l'aggiunta finisce dopo il corrente: %s", after);

	printf("\n-- lo shuffle parte dal brano scelto\n");
	// Quello che si lamentava un utente: facendo partire qualcosa in casuale, il
	// brano che partiva si trovava centinaia di tracce sotto la cima della coda,
	// e sopra c'erano brani che nessuno aveva ascoltato. Un brano fatto partire
	// e' il primo della coda; cio' che sta sopra e' cio' che si e' gia' sentito,
	// e all'inizio non c'e' niente.
	for (int mode = 0; mode < 2; mode++) {
		playback_mode_t which = mode ? PLAYBACK_MODE_SHUFFLE_REPEAT : PLAYBACK_MODE_SHUFFLE;
		const char *name = mode ? "casuale continuo" : "casuale";
		playlist_set_mode(which);

		build_list("coda", "ABCDEFGHIJKLMNOPQRST");
		playlist_load_index(open_list("coda"), 7); // la H, in mezzo alla lista
		playlist_current_path(path, sizeof(path));
		ok(strcmp(path, "/mnt/sd_0/Music/H.flac") == 0, "%s: parte dalla H", name);
		ok(playlist_current_index() == 0, "%s: ed e' la prima della coda (slot %d)", name,
		   playlist_current_index());

		// E il contatore sotto la copertina non c'entra col mescolio: mostra
		// playlist_current_entry(), cioe' il posto del brano nella lista da cui
		// viene -- la playlist, i preferiti, l'album. La H e' l'ottava della
		// lista, quindi dice 8/20 anche se nella coda e' la prima. Un mescolio
		// permuta l'ordine, non le voci, e questo controllo e' qui perche' i due
		// numeri non tornino mai a confondersi.
		ok(playlist_current_entry() == 7, "%s: ma il contatore dice %d/20, il posto nella lista", name,
		   (int)playlist_current_entry() + 1);

		// Il mescolio deve restare un mescolio: tutte e venti le tracce una
		// volta sola, non una fila di doppioni.
		queue_string(after, sizeof(after));
		ok(strlen(after) == 20, "%s: la coda ha ancora venti tracce", name);
		int seen[128] = {0};
		bool once = true;
		for (const char *c = after; *c; c++) {
			once = once && seen[(unsigned char)*c] == 0;
			seen[(unsigned char)*c]++;
		}
		ok(once, "%s: e nessuna compare due volte: %s", name, after);

		// E andando avanti, quelle ascoltate restano sopra.
		char next_path[512];
		playlist_next(next_path, sizeof(next_path));
		ok(playlist_current_index() == 1, "%s: al brano dopo si scende di uno", name);
		char first[512];
		playlist_path_at(0, first, sizeof(first));
		ok(strcmp(first, "/mnt/sd_0/Music/H.flac") == 0, "%s: e la H e' ancora sopra", name);

		// Anche sul brano dopo il contatore resta il posto nella lista, che con
		// il mescolio e' un numero qualsiasi fra 1 e 20 e non il 2 della coda.
		playlist_current_path(path, sizeof(path));
		ok(playlist_current_entry() == (size_t)(path[strlen(path) - 6] - 'A'),
		   "%s: e il contatore segue il brano, non la coda (%d/20 per la %c)", name,
		   (int)playlist_current_entry() + 1, path[strlen(path) - 6]);
	}
	playlist_set_mode(PLAYBACK_MODE_NORMAL);

	printf("\n-- passare a casuale mentre si ascolta non cancella il passato\n");
	// L'altra meta' della stessa domanda, che ha una risposta diversa: qui un
	// passato c'e' davvero, e va lasciato dov'e'.
	build_list("coda", "ABCDEFGHIJ");
	playlist_load_index(open_list("coda"), 0);
	playlist_set_current(4); // si e' ascoltato fino alla E
	playlist_set_mode(PLAYBACK_MODE_SHUFFLE);
	ok(playlist_current_index() == 4, "la riproduzione non si sposta (slot %d)", playlist_current_index());
	queue_string(after, sizeof(after));
	ok(strncmp(after, "ABCDE", 5) == 0, "e le prime cinque sono ancora ABCDE: %s", after);
	playlist_set_mode(PLAYBACK_MODE_NORMAL);

	printf("\n-- una coda senza aggiunte\n");
	build_list("coda", "ABCDEF");
	playlist_load_index(open_list("coda"), 0);
	playlist_set_current(3);
	queue_string(before, sizeof(before));
	save_queue();
	playlist_clear();
	ok(restore_queue("/mnt/sd_0/Music/D.flac"), "torna anche senza niente aggiunto");
	queue_string(after, sizeof(after));
	ok(strcmp(after, before) == 0, "identica: %s", after);
	ok(playlist_appended_count() == 0, "e senza righe inventate");

	playlist_clear();
	library_close();
	unlink(dbpath);

	printf("\n%d controlli, %d falliti\n", checks, failures);
	return failures ? 1 : 0;
}

// I finti condivisi da tools/test_covers.c e tools/test_coverloader.c.
//
// Quello che si prova in tutti e due e' src/gui/cover.c vero. Qui c'e' soltanto
// da dove arrivano i byte -- il file stesso, invece della ricerca fra il tag
// del brano e la cartella attorno -- e le funzioni che il linker chiede ma che
// nessuno dei due bench chiama.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/system/library/albumart.h"

// Il candidato 0 e' il file che il bench sta provando, il resto non c'e'.
//
// Il buffer e' allocato esatto, senza un byte di margine: cosi' una lettura
// oltre la fine dei byte compressi cade fuori dalla regione e AddressSanitizer
// la vede. Con del margine se la mangerebbe in silenzio, che e' precisamente il
// guasto che non si vuole lasciar passare.
bool albumart_load_candidate(const char *filepath, int index, albumart_t *out) {
	memset(out, 0, sizeof(*out));
	if (index != 0 || !filepath || !filepath[0]) {
		return false;
	}

	FILE *f = fopen(filepath, "rb");
	if (!f) {
		return false;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size <= 0 || (size_t)size > ALBUMART_MAX_BYTES) {
		fclose(f);
		return false;
	}

	uint8_t *data = malloc((size_t)size);
	if (!data) {
		fclose(f);
		return false;
	}
	if (fread(data, 1, (size_t)size, f) != (size_t)size) {
		free(data);
		fclose(f);
		return false;
	}
	fclose(f);

	out->data = data;
	out->size = (size_t)size;
	return true;
}

bool albumart_load_dir_candidate(const char *dirpath, int index, albumart_t *out) {
	return albumart_load_candidate(dirpath, index, out);
}

void albumart_free(albumart_t *art) {
	if (!art) {
		return;
	}
	free(art->data);
	memset(art, 0, sizeof(*art));
}

bool podcastcache_ensure_cover(const char *path) {
	(void)path;
	return false;
}

// La briciola per il gestore del crash: qui non c'e' nessun gestore, e il
// bench non deve tirarsi dietro mezzo src/system per una riga di log.
void crumb_set_artwork(const char *path) { (void)path; }

// ---------------------------------------------------------------------------
// La cache su disco resta chiusa: senza cover_set_cache_dir() il puntatore al
// database e' nullo e cover.c esce da solo prima di chiamare qualunque di
// queste. Ci sono perche' il linker le vuole, e abortiscono perche' se una
// venisse chiamata davvero vorrebbe dire che il bench sta provando una strada
// diversa da quella che crede.
// ---------------------------------------------------------------------------
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;

#define STUB_ABORT(name)                                                                                     \
	do {                                                                                                     \
		fprintf(stderr, "bench: %s chiamata, la cache non doveva entrarci\n", name);                          \
		abort();                                                                                             \
	} while (0)

int sqlite3_bind_blob(sqlite3_stmt *a, int b, const void *c, int n, void (*d)(void *));
int sqlite3_bind_int(sqlite3_stmt *a, int b, int c);
int sqlite3_bind_null(sqlite3_stmt *a, int b);
int sqlite3_bind_text(sqlite3_stmt *a, int b, const char *c, int d, void (*e)(void *));
int sqlite3_close(sqlite3 *a);
const void *sqlite3_column_blob(sqlite3_stmt *a, int b);
int sqlite3_column_bytes(sqlite3_stmt *a, int b);
int sqlite3_column_int(sqlite3_stmt *a, int b);
const char *sqlite3_errmsg(sqlite3 *a);
int sqlite3_exec(sqlite3 *a, const char *b, int (*c)(void *, int, char **, char **), void *d, char **e);
int sqlite3_finalize(sqlite3_stmt *a);
void sqlite3_free(void *a);
int sqlite3_open(const char *a, sqlite3 **b);
int sqlite3_prepare_v2(sqlite3 *a, const char *b, int c, sqlite3_stmt **d, const char **e);
int sqlite3_step(sqlite3_stmt *a);

int sqlite3_bind_blob(sqlite3_stmt *a, int b, const void *c, int n, void (*d)(void *)) {
	(void)a, (void)b, (void)c, (void)n, (void)d;
	STUB_ABORT("sqlite3_bind_blob");
}
int sqlite3_bind_int(sqlite3_stmt *a, int b, int c) {
	(void)a, (void)b, (void)c;
	STUB_ABORT("sqlite3_bind_int");
}
int sqlite3_bind_null(sqlite3_stmt *a, int b) {
	(void)a, (void)b;
	STUB_ABORT("sqlite3_bind_null");
}
int sqlite3_bind_text(sqlite3_stmt *a, int b, const char *c, int d, void (*e)(void *)) {
	(void)a, (void)b, (void)c, (void)d, (void)e;
	STUB_ABORT("sqlite3_bind_text");
}
int sqlite3_close(sqlite3 *a) {
	(void)a;
	STUB_ABORT("sqlite3_close");
}
const void *sqlite3_column_blob(sqlite3_stmt *a, int b) {
	(void)a, (void)b;
	STUB_ABORT("sqlite3_column_blob");
}
int sqlite3_column_bytes(sqlite3_stmt *a, int b) {
	(void)a, (void)b;
	STUB_ABORT("sqlite3_column_bytes");
}
int sqlite3_column_int(sqlite3_stmt *a, int b) {
	(void)a, (void)b;
	STUB_ABORT("sqlite3_column_int");
}
const char *sqlite3_errmsg(sqlite3 *a) {
	(void)a;
	STUB_ABORT("sqlite3_errmsg");
}
int sqlite3_exec(sqlite3 *a, const char *b, int (*c)(void *, int, char **, char **), void *d, char **e) {
	(void)a, (void)b, (void)c, (void)d, (void)e;
	STUB_ABORT("sqlite3_exec");
}
int sqlite3_finalize(sqlite3_stmt *a) {
	(void)a;
	STUB_ABORT("sqlite3_finalize");
}
void sqlite3_free(void *a) {
	(void)a;
	STUB_ABORT("sqlite3_free");
}
int sqlite3_open(const char *a, sqlite3 **b) {
	(void)a, (void)b;
	STUB_ABORT("sqlite3_open");
}
int sqlite3_prepare_v2(sqlite3 *a, const char *b, int c, sqlite3_stmt **d, const char **e) {
	(void)a, (void)b, (void)c, (void)d, (void)e;
	STUB_ABORT("sqlite3_prepare_v2");
}
int sqlite3_step(sqlite3_stmt *a) {
	(void)a;
	STUB_ABORT("sqlite3_step");
}

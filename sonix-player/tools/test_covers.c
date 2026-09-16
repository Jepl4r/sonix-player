// La catena delle copertine, provata sotto AddressSanitizer.
//
// Perche' esiste: in due crash sul dispositivo l'ultima riga del log era la
// decodifica di una copertina, e uno dei due era un lv_obj_t sullo heap con il
// campo parent pieno di spazzatura. Le copertine sono l'unico posto del
// programma che fa aritmetica su buffer di quelle dimensioni -- ritagli,
// rapporti d'aspetto, passi di riga calcolati a mano -- e stanno sullo stesso
// heap di glibc di tutto il resto. Se una di quelle moltiplicazioni scrive un
// byte oltre il bordo, lo dice ASan qui invece di dirlo il player fra due
// settimane a casa di qualcuno.
//
// Gira contro src/gui/cover.c vero. Quello che e' finto (tools/cover_stubs.c)
// e' soltanto da dove arrivano i byte: albumart_load_candidate() consegna il
// file che il bench sta provando, cosi' ogni immagine passa per la strada vera
// -- decode_raw -> make_scaled / make_backdrop -> resample_rgb -> pack_rgb565 --
// senza dover costruire una cartella di musica finta attorno a ognuna.
#include <dirent.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "src/gui/nowplaying/cover.h"

static int checks;
static int failures;

static void ok(bool condition, const char *fmt, ...) {
	checks++;
	va_list args;
	va_start(args, fmt);
	if (!condition) {
		printf("  NO   ");
		vprintf(fmt, args);
		printf("\n");
		failures++;
	}
	va_end(args);
}

// ---------------------------------------------------------------------------
// da dove arrivano i byte
//
// I finti (albumart, podcastcache, la cache su disco) stanno in
// tools/cover_stubs.c, condivisi con il bench del worker. Qui serve in piu' una
// copia dei byte in memoria, per la strada che il lettore di epub usa quando la
// copertina sta dentro lo zip e non ha un percorso suo.
// ---------------------------------------------------------------------------

static uint8_t *source_bytes;
static size_t source_size;

static bool source_set(const char *path) {
	free(source_bytes);
	source_bytes = NULL;
	source_size = 0;

	FILE *f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size < 0) {
		fclose(f);
		return false;
	}
	// Allocato esatto, senza un byte in piu': una lettura oltre la fine cade
	// fuori dalla regione e ASan la vede.
	source_bytes = malloc((size_t)size ? (size_t)size : 1);
	source_size = (size_t)size;
	if (source_size && fread(source_bytes, 1, source_size, f) != source_size) {
		fclose(f);
		return false;
	}
	fclose(f);
	return true;
}

// ---------------------------------------------------------------------------
// cosa deve valere di un'immagine costruita
// ---------------------------------------------------------------------------

// Quello che LVGL si aspetta di trovare nel descrittore. Se il passo di riga o
// la dimensione dei dati non corrispondono ai pixel allocati, il disegno legge
// oltre il buffer -- che e' esattamente il tipo di guasto che si sta cercando,
// solo che succede dentro LVGL e non qui.
static void check_image(const cover_image_t *img, const char *what, const char *file) {
	if (!img->pixels) {
		return; // niente immagine e' una risposta valida
	}
	uint32_t w = img->dsc.header.w;
	uint32_t h = img->dsc.header.h;
	ok(w > 0 && h > 0, "%s di %s: %ux%u", what, file, w, h);
	ok(img->dsc.header.stride == w * 2, "%s di %s: passo %u invece di %u", what, file,
	   img->dsc.header.stride, w * 2);
	ok(img->dsc.data_size == w * h * 2, "%s di %s: %u byte dichiarati invece di %u", what, file,
	   img->dsc.data_size, w * h * 2);
	ok(img->dsc.data == img->pixels, "%s di %s: il descrittore non punta ai suoi pixel", what, file);

	// E li si legge tutti, dal primo all'ultimo byte: una lunghezza dichiarata
	// piu' grande del buffer non si vede finche' qualcuno non ci legge dentro,
	// e chi ci legge sul dispositivo e' il disegno.
	volatile uint32_t sum = 0;
	for (uint32_t i = 0; i < img->dsc.data_size; i++) {
		sum += img->pixels[i];
	}
	(void)sum;
}

// I riquadri che il player chiede davvero, piu' quelli che nessuno chiede mai.
static const struct {
	int cover_w, cover_h;
	int back_w, back_h;
	const char *name;
} BOXES[] = {
	{360, 360, 480, 240, "il player"},		  // le misure vere del R3 Pro II
	{400, 400, 480, 260, "player piu' largo"},
	{44, 44, 0, 0, "miniatura di lista"},
	{120, 120, 0, 0, "miniatura grande"},
	{480, 720, 480, 200, "schermo intero"},
	{97, 53, 53, 97, "numeri primi"},
	{1, 1, 1, 1, "un pixel"},
	{2, 1, 1, 2, "una riga e una colonna"},
	{3, 1000, 1000, 3, "una fessura"},
	{640, 3, 3, 640, "una fessura sdraiata"},
};
#define BOX_COUNT ((int)(sizeof(BOXES) / sizeof(BOXES[0])))

static void run_one(const char *dir, const char *name) {
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	if (!source_set(path)) {
		printf("  NO   non riesco a leggere %s\n", name);
		failures++;
		return;
	}

	for (int i = 0; i < BOX_COUNT; i++) {
		cover_image_t cover, backdrop;

		// La coppia che chiede la pagina now playing: una decodifica, due
		// immagini, di cui una girata e sfocata.
		bool has = cover_load_player_images(path, BOXES[i].cover_w, BOXES[i].cover_h, &cover,
											BOXES[i].back_w ? BOXES[i].back_w : 1,
											BOXES[i].back_h ? BOXES[i].back_h : 1, &backdrop);
		(void)has;
		check_image(&cover, "copertina", name);
		check_image(&backdrop, "sfondo", name);

		// Il colore del bagliore legge la copertina a salti di quattro pixel:
		// se il passo di riga e l'altezza non vanno d'accordo, esce dal buffer.
		if (cover.pixels) {
			(void)cover_dominant_tone(&cover);

			// E la copia sfocata, che riparte da pixel gia' impacchettati a 565
			// invece che dalla sorgente decodificata.
			cover_image_t blurred;
			if (cover_blur_copy(&cover, &blurred)) {
				check_image(&blurred, "sfocata", name);
				cover_free(&blurred);
			}
		}

		cover_free(&cover);
		cover_free(&backdrop);

		// La sola copertina, con tutti e due gli adattamenti: COVER ritaglia sul
		// rapporto del riquadro, CONTAIN tiene tutta l'immagine e non la
		// ingrandisce mai -- due aritmetiche diverse sullo stesso file.
		cover_image_t single;
		if (cover_load_for_file(path, BOXES[i].cover_w, BOXES[i].cover_h, COVER_FIT_COVER, &single)) {
			check_image(&single, "ritagliata", name);
		}
		cover_free(&single);
		if (cover_load_for_file(path, BOXES[i].cover_w, BOXES[i].cover_h, COVER_FIT_CONTAIN, &single)) {
			check_image(&single, "contenuta", name);
		}
		cover_free(&single);

		// E la strada della cartella, che e' la stessa con l'altro candidato.
		if (cover_load_for_dir(path, BOXES[i].cover_w, BOXES[i].cover_h, COVER_FIT_COVER, &single)) {
			check_image(&single, "di cartella", name);
		}
		cover_free(&single);

		// I byte in memoria: quello che fa il lettore di epub, dove la
		// copertina sta dentro lo zip e non ha un percorso suo.
		if (cover_load_image_memory(source_bytes, source_size, BOXES[i].cover_w, BOXES[i].cover_h,
									COVER_FIT_COVER, &single)) {
			check_image(&single, "da memoria", name);
		}
		cover_free(&single);

		// E la coppia del salvaschermo, che NON e' quella del player con altri
		// numeri: qui la fascia sfocata sta sopra una foto a tutto schermo, e
		// quindi e' la banda in fondo alla foto stessa, ritagliata dal basso e
		// lasciata diritta. E' l'unico punto che sposta crop.y, e senza questa
		// chiamata quel calcolo non lo prova nessuno.
		cover_image_t photo, strip;
		if (cover_load_screensaver_images(path, BOXES[i].cover_w, BOXES[i].cover_h, &photo,
										  BOXES[i].back_w ? BOXES[i].back_w : 1,
										  BOXES[i].back_h ? BOXES[i].back_h : 1, &strip)) {
			check_image(&photo, "foto", name);
			check_image(&strip, "fascia", name);
		}
		cover_free(&photo);
		cover_free(&strip);
	}
}

int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc != 2) {
		fprintf(stderr, "uso: test_covers <cartella-con-le-immagini>\n");
		return 2;
	}

	// Lo sfondo si schiarisce o si scurisce a seconda del tema, e sono due giri
	// diversi sui pixel: tutte e due vanno provati.
	const char *dir = argv[1];

	for (int light = 0; light < 2; light++) {
		cover_set_backdrop_light(light != 0);
		printf("\n-- sfondo %s\n", light ? "chiaro" : "scuro");

		DIR *d = opendir(dir);
		if (!d) {
			fprintf(stderr, "non riesco ad aprire %s\n", dir);
			return 2;
		}
		int files = 0;
		struct dirent *de;
		while ((de = readdir(d)) != NULL) {
			if (de->d_name[0] == '.') {
				continue;
			}
			run_one(dir, de->d_name);
			files++;
		}
		closedir(d);
		printf("  %d file per %d riquadri\n", files, BOX_COUNT);
	}

	free(source_bytes);
	printf("\n%d controlli, %d falliti\n", checks, failures);
	return failures ? 1 : 0;
}

// fbbench -- quanto costa disegnare dentro il framebuffer, misurato.
//
// Perche' esiste
// --------------
// Il player disegna direttamente dentro /dev/fb0: main.c mmappa due pagine e
// le passa a LVGL con LV_DISPLAY_RENDER_MODE_DIRECT, scambiandole con
// FBIOPAN_DISPLAY. Nessuna copia di frame intera -- ma vuol dire anche che
// ogni blending alpha di LVGL (angoli arrotondati, ombre, il velo del
// popover, l'antialiasing di ogni carattere) fa un read-modify-write su
// memoria la cui cacheabilita' la decide il driver fb di Ingenic, non noi.
//
// Se quella mappa e' cacheata non c'e' niente da fare. Se e' uncached o
// write-combine, ogni lettura di quel read-modify-write e' un giro completo
// in DDR senza riuso della linea di cache -- e su un core con 16 KB di L1
// quello costa piu' di qualunque ottimizzazione a valle.
//
// Questo programma misura la differenza invece di indovinarla.
//
// Come si legge il risultato
// --------------------------
// La colonna che conta e' "rapporto": RAM diviso framebuffer, a parita' di
// codice e di blocco. Vicino a 1 = la mappa e' cacheata come la RAM normale.
// Grande = non lo e', e la lettura del blending sta pagando la DDR piena.
//
// Il blocco da 4 KB sta in L1 (16 KB), quello da 64 KB sta in L2 (128 KB),
// quello da 256 KB no: se i tre rapporti sono simili la memoria e' uncached,
// se crescono con la dimensione e' semplicemente la gerarchia di cache che
// funziona.
//
// Uso
// ---
//   ./fbbench                 # /dev/fb0
//   ./fbbench /dev/fb1
//   ./fbbench /tmp/finto.bin  # modalita' di prova su file normale (per
//                             # verificare il programma su PC, senza fb)
//
// Da lanciare con il player FERMO: vuole il framebuffer per se'. Scrive nella
// seconda pagina quando c'e' (fuori schermo, invisibile); se il framebuffer e'
// a pagina singola scrive in quella visibile e si vedono disturbi sul pannello
// per la durata del test. In entrambi i casi il contenuto viene salvato prima
// e rimesso a posto alla fine.
//
// Compilazione, dalla radice del repo:
//   rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-gcc -O2 -o fbbench tools/fbbench.c
//
// Non tocca nulla del player: e' un eseguibile a se'.

#define _GNU_SOURCE

#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// Quanto a lungo far girare ogni misura. Abbastanza da coprire il rumore di
// scheduling senza far aspettare: ogni riga della tabella sono sei di questi.
#define MEASURE_SECONDS 0.30

// I tre blocchi: dentro L1, dentro L2, fuori da entrambe. Vedere la nota in
// testa al file per come si leggono.
static const size_t BLOCKS[] = {4 * 1024, 64 * 1024, 256 * 1024};
#define BLOCK_COUNT (sizeof(BLOCKS) / sizeof(BLOCKS[0]))

enum { K_WRITE, K_READ, K_BLEND };

static const char *const KERNEL_NAME[] = {
	"scrittura",
	"lettura",
	"blending",
};

// Il compilatore non deve poter buttare via le letture: la somma finisce qui,
// e una scrittura su volatile non si elimina.
static volatile uint32_t g_sink;

// ---------------------------------------------------------------------------
// i tre nuclei
// ---------------------------------------------------------------------------

// Riempimento. Il valore varia con l'indice apposta: un pattern costante
// gcc lo riconosce e lo trasforma in memset(), che non e' il codice che LVGL
// esegue davvero.
static void kernel_write(void *mem, size_t bytes, uint32_t pattern) {
	uint32_t *p = (uint32_t *)mem;
	size_t n = bytes / sizeof(*p);
	for (size_t i = 0; i < n; i++) {
		p[i] = pattern + (uint32_t)i;
	}
}

// Lettura pura. La somma serve solo a rendere i load non eliminabili.
static void kernel_read(const void *mem, size_t bytes) {
	const uint32_t *p = (const uint32_t *)mem;
	size_t n = bytes / sizeof(*p);
	uint32_t sum = 0;
	for (size_t i = 0; i < n; i++) {
		sum += p[i];
	}
	g_sink = sum;
}

// Il caso vero: alpha blending RGB565 sul posto. Legge la destinazione, la
// mescola con un colore sorgente, la riscrive -- la stessa forma di accesso
// che fa LVGL per un'ombra, un angolo arrotondato o un bordo di carattere.
// L'aritmetica e' approssimata (>> 8 invece di / 255) come nel percorso
// veloce di LVGL: qui conta il traffico di memoria, non il colore esatto.
static void kernel_blend(void *mem, size_t bytes, uint16_t src, uint32_t alpha) {
	uint16_t *p = (uint16_t *)mem;
	size_t n = bytes / sizeof(*p);
	uint32_t sr = (src >> 11) & 0x1F, sg = (src >> 5) & 0x3F, sb = src & 0x1F;
	uint32_t ia = 255u - alpha;
	for (size_t i = 0; i < n; i++) {
		uint16_t d = p[i];
		uint32_t r = (((d >> 11) & 0x1F) * ia + sr * alpha) >> 8;
		uint32_t g = (((d >> 5) & 0x3F) * ia + sg * alpha) >> 8;
		uint32_t b = ((d & 0x1F) * ia + sb * alpha) >> 8;
		p[i] = (uint16_t)((r << 11) | (g << 5) | b);
	}
}

static void run_kernel(int kind, void *mem, size_t bytes, uint32_t seed) {
	switch (kind) {
	case K_WRITE:
		kernel_write(mem, bytes, seed);
		break;
	case K_READ:
		kernel_read(mem, bytes);
		break;
	default:
		kernel_blend(mem, bytes, 0x1234, 128);
		break;
	}
}

// ---------------------------------------------------------------------------
// cronometro
// ---------------------------------------------------------------------------

static double now_seconds(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

// MB/s. Un giro a vuoto prima di far partire il cronometro: la prima passata
// paga i page fault della mmap e non e' rappresentativa.
static double measure(int kind, void *mem, size_t bytes) {
	run_kernel(kind, mem, bytes, 1);

	double start = now_seconds();
	double elapsed = 0.0;
	long passes = 0;
	do {
		run_kernel(kind, mem, bytes, (uint32_t)passes);
		passes++;
		elapsed = now_seconds() - start;
	} while (elapsed < MEASURE_SECONDS);

	return ((double)passes * (double)bytes) / elapsed / (1024.0 * 1024.0);
}

// ---------------------------------------------------------------------------
// apertura del framebuffer
// ---------------------------------------------------------------------------

typedef struct {
	int fd;
	uint8_t *map;	   // inizio della mappa
	size_t map_len;	   // quanto e' stato mappato
	uint8_t *region;   // dove si misura
	size_t region_len; // quanto e' utilizzabile li'
	const char *where; // per il messaggio a schermo
	int fake;		   // 1 = file normale, non un framebuffer
} target_t;

static int open_target(const char *path, target_t *t) {
	memset(t, 0, sizeof(*t));

	t->fd = open(path, O_RDWR);
	if (t->fd < 0) {
		perror(path);
		return -1;
	}

	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	size_t page = 0;
	int pages = 1;

	if (ioctl(t->fd, FBIOGET_VSCREENINFO, &var) == 0 && ioctl(t->fd, FBIOGET_FSCREENINFO, &fix) == 0) {
		t->map_len = fix.smem_len;
		page = (size_t)fix.line_length * var.yres;
		pages = (page && var.yres_virtual >= var.yres * 2 && t->map_len >= page * 2) ? 2 : 1;

		printf("framebuffer : %s\n", path);
		printf("  modo      : %ux%u, %u bpp, riga %u byte\n", var.xres, var.yres, var.bits_per_pixel,
			   fix.line_length);
		printf("  virtuale  : %ux%u  (%s)\n", var.xres_virtual, var.yres_virtual,
			   pages == 2 ? "doppio buffer" : "pagina singola");
		printf("  memoria   : %u byte a %#lx\n", fix.smem_len, (unsigned long)fix.smem_start);
	} else {
		// Non e' un framebuffer: modalita' di prova, serve a verificare il
		// programma su PC dove /dev/fb0 non c'e'.
		struct stat st;
		if (fstat(t->fd, &st) < 0 || st.st_size <= 0) {
			fprintf(stderr, "%s: non e' un framebuffer e non ha una dimensione utilizzabile\n", path);
			close(t->fd);
			return -1;
		}
		t->map_len = (size_t)st.st_size;
		page = t->map_len;
		t->fake = 1;
		printf("ATTENZIONE  : %s non e' un framebuffer -- modalita' di prova su file normale.\n", path);
		printf("              I numeri qui sotto non dicono nulla sul pannello.\n");
	}

	if (t->map_len < BLOCKS[0]) {
		fprintf(stderr, "memoria troppo piccola: %zu byte\n", t->map_len);
		close(t->fd);
		return -1;
	}

	t->map = mmap(NULL, t->map_len, PROT_READ | PROT_WRITE, MAP_SHARED, t->fd, 0);
	if (t->map == MAP_FAILED) {
		perror("mmap");
		close(t->fd);
		return -1;
	}

	// Con due pagine si misura sulla seconda: e' fuori schermo, quindi il
	// pannello non mostra i disturbi e lo scan-out non contende la stessa
	// zona di memoria.
	if (pages == 2) {
		t->region = t->map + page;
		t->region_len = page;
		t->where = "seconda pagina, fuori schermo";
	} else {
		t->region = t->map;
		t->region_len = page < t->map_len ? page : t->map_len;
		t->where = t->fake ? "file di prova" : "pagina visibile (si vedranno disturbi)";
	}

	return 0;
}

// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
	const char *path = argc > 1 ? argv[1] : "/dev/fb0";

	target_t t;
	if (open_target(path, &t) < 0) {
		return 1;
	}
	printf("  misura su : %s, %zu byte disponibili\n\n", t.where, t.region_len);

	// Il contenuto va rimesso a posto: la seconda pagina e' quella che il
	// player presentera' al prossimo flip, e la prima e' quella a schermo.
	uint8_t *saved = malloc(t.region_len);
	if (!saved) {
		fprintf(stderr, "niente memoria per il salvataggio (%zu byte)\n", t.region_len);
		return 1;
	}
	memcpy(saved, t.region, t.region_len);

	// Il termine di paragone: memoria normale, sicuramente cacheata, allineata
	// come la mappa cosi' il confronto non misura disallineamenti.
	uint8_t *ram = NULL;
	if (posix_memalign((void **)&ram, 4096, t.region_len) != 0 || !ram) {
		fprintf(stderr, "niente memoria per il riferimento (%zu byte)\n", t.region_len);
		free(saved);
		return 1;
	}
	memset(ram, 0x5A, t.region_len);

	printf("%-10s %8s %12s %12s %10s\n", "operazione", "blocco", "fb MB/s", "RAM MB/s", "rapporto");
	printf("---------------------------------------------------------------\n");

	double blend_ratio_l2 = -1.0, read_ratio_l2 = -1.0;

	for (size_t b = 0; b < BLOCK_COUNT; b++) {
		size_t size = BLOCKS[b];
		if (size > t.region_len) {
			printf("(blocco da %zu KB saltato: non ci sta)\n", size / 1024);
			continue;
		}
		for (int k = K_WRITE; k <= K_BLEND; k++) {
			double fb_mbs = measure(k, t.region, size);
			double ram_mbs = measure(k, ram, size);
			double ratio = fb_mbs > 0.0 ? ram_mbs / fb_mbs : 0.0;

			printf("%-10s %6zu K %12.1f %12.1f %9.1fx\n", KERNEL_NAME[k], size / 1024, fb_mbs, ram_mbs, ratio);

			if (size == 64 * 1024) {
				if (k == K_READ) {
					read_ratio_l2 = ratio;
				}
				if (k == K_BLEND) {
					blend_ratio_l2 = ratio;
				}
			}
		}
		printf("---------------------------------------------------------------\n");
	}

	memcpy(t.region, saved, t.region_len);
	free(saved);
	free(ram);
	munmap(t.map, t.map_len);
	close(t.fd);

	if (t.fake) {
		printf("\n(modalita' di prova: nessun verdetto)\n");
		return 0;
	}

	// Il verdetto guarda il blocco da 64 KB, che sta in L2 ma non in L1: e' li'
	// che la differenza fra memoria cacheata e non cacheata si vede meglio.
	double worst = blend_ratio_l2 > read_ratio_l2 ? blend_ratio_l2 : read_ratio_l2;
	printf("\nverdetto (blocco da 64 KB, lettura %.1fx, blending %.1fx):\n", read_ratio_l2, blend_ratio_l2);
	if (worst < 2.0) {
		printf("  La mappa si comporta come memoria cacheata. Disegnare dentro il\n");
		printf("  framebuffer non costa nulla di speciale: RENDER_MODE_DIRECT va\n");
		printf("  bene com'e' e non c'e' niente da cambiare.\n");
	} else if (worst < 6.0) {
		printf("  Zona grigia -- probabilmente write-combine: le scritture passano,\n");
		printf("  le letture no. Vale la pena provare il rendering in RAM solo se\n");
		printf("  la riga 'blending' e' molto peggio della riga 'scrittura'.\n");
	} else {
		printf("  La mappa NON e' cacheata. Ogni blending alpha di LVGL sta pagando\n");
		printf("  la DDR piena in lettura. Conviene far disegnare LVGL in un buffer\n");
		printf("  in RAM (LV_DISPLAY_RENDER_MODE_PARTIAL) e copiare nella pagina nel\n");
		printf("  flush: si paga una copia in piu' e si guadagna su ogni mescolata.\n");
	}

	return 0;
}

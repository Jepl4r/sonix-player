// Prova che una cartuccia "SGB enhanced" non scrive oltre il buffer dei pixel.
//
// Si compila e si esegue sull'HOST:
//
//     g++ -O2 -DGEARBOY_DISABLE_DISASSEMBLER -fno-exceptions -fno-rtti -w -Isrc/gb -Isrc/gb/core -Isrc/gb/miniz -I. -o /tmp/test_gbsgb tools/test_gbsgb.c src/gb/gbcore.cpp src/gb/core/*.cpp src/gb/core/audio/*.cpp src/gb/miniz/miniz.c
//     /tmp/test_gbsgb
//
// PERCHE' ESISTE
//
// Gearboy sa anche fare il Super Game Boy, e una cartuccia lo accende da sola:
// basta che il byte 0x146 della ROM valga 3. Da quel momento RenderSGBFrame()
// disegna 256x224 -- la cornice del Super Game Boy attorno all'immagine --
// dentro il buffer del CHIAMANTE, che il player dimensiona per 160x144.
//
// Sono 34.304 pixel oltre la fine, 67 KB, a ogni fotogramma, sessanta volte al
// secondo. Sul dispositivo si e' visto cosi': aperto un gioco marcato SGB,
// bloccato sull'avvio, e l'audio che sparava frequenze a caso -- l'heap era
// gia' a pezzi dal primo fotogramma. Ci e' voluto un reset a mano.
//
// gb_core_create() adesso spegne l'SGB (SetSGBEnabled + SetSGBBorder) e il
// buffer si alloca comunque alla misura peggiore. Questo test tiene ferme tutte
// e due le cose: la ROM che si costruisce da sola qui sotto e' marcata SGB, e
// la sentinella dopo il buffer dice quanti pixel il core ha toccato davvero.
//
// Se un giorno qualcuno toglie una di quelle due righe, questo test fallisce
// prima che il dispositivo si blocchi.

#include "gbcore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GUARD 0xA5A5
#define ROM_PATH "/tmp/test_gbsgb.gb"

// Una cartuccia minima che accende l'LCD e disegna qualcosa, marcata SGB.
// Niente di Nintendo dentro: solo codice nostro.
static bool write_rom(void) {
	unsigned char rom[0x8000];
	memset(rom, 0, sizeof(rom));

	static const unsigned char entry[] = {0x00, 0xC3, 0x50, 0x01}; // nop ; jp $0150
	memcpy(rom + 0x100, entry, sizeof(entry));
	memcpy(rom + 0x134, "SGBTEST", 7);

	rom[0x146] = 0x03; // <-- "SGB enhanced": e' questo byte che accendeva la cornice
	rom[0x147] = 0x00; // solo ROM
	rom[0x148] = 0x00; // 32 KB
	rom[0x149] = 0x00; // niente RAM

	unsigned char chk = 0;
	for (int a = 0x134; a < 0x14D; a++) {
		chk = (unsigned char)(chk - rom[a] - 1);
	}
	rom[0x14D] = chk;

	static const unsigned char code[] = {
		0x3E, 0x00, 0xE0, 0x40,				  // LCDC = 0: VRAM libera
		0x21, 0x00, 0x80, 0x01, 0x00, 0x10,   // HL=$8000  BC=$1000
		0x7D, 0x22, 0x0B, 0x78, 0xB1, 0x20, 0xF9, // riempie i tile
		0x21, 0x00, 0x98, 0x01, 0x00, 0x04,   // HL=$9800  BC=$0400
		0x7D, 0x22, 0x0B, 0x78, 0xB1, 0x20, 0xF9, // riempie la mappa
		0x3E, 0xE4, 0xE0, 0x47,				  // BGP
		0x3E, 0x91, 0xE0, 0x40,				  // LCDC = acceso
		0xF0, 0x43, 0x3C, 0xE0, 0x43,		  // SCX++
		0xC3, 0x28, 0x01,					  // jp -> lo SCX++ qui sopra
	};
	memcpy(rom + 0x150, code, sizeof(code));

	FILE *f = fopen(ROM_PATH, "wb");
	if (!f) {
		perror("fopen");
		return false;
	}
	bool ok = fwrite(rom, 1, sizeof(rom), f) == sizeof(rom);
	fclose(f);
	return ok;
}

int main(void) {
	if (!write_rom()) {
		return 1;
	}

	// Il buffer alla misura che usa il player, seguito da spazio riempito di
	// sentinelle: se il core disegna piu' di 160x144, si vede fin dove.
	static uint16_t buf[GB_FRAME_MAX_PIXELS + 4096];
	for (size_t i = 0; i < sizeof(buf) / sizeof(buf[0]); i++) {
		buf[i] = GUARD;
	}

	gb_core_t *core = gb_core_create();
	if (!core) {
		puts("il core non parte");
		return 1;
	}
	gb_core_set_sample_rate(core, 44100);
	if (!gb_core_load_file(core, ROM_PATH, false)) {
		puts("la ROM non si carica");
		return 1;
	}

	int samples = 0;
	for (int f = 0; f < 120; f++) {
		gb_core_run_frame(core, buf, NULL, &samples);
	}

	size_t touched = 0;
	for (size_t i = 0; i < sizeof(buf) / sizeof(buf[0]); i++) {
		if (buf[i] != GUARD) {
			touched = i + 1;
		}
	}
	gb_core_destroy(core);

	const size_t allowed = (size_t)GB_WIDTH * GB_HEIGHT;
	printf("cartuccia marcata SGB (0x146 = 3)\n");
	printf("pixel toccati: %zu   (Game Boy = %zu, Super Game Boy = %d)\n", touched, allowed,
		   GB_FRAME_MAX_PIXELS);

	if (touched > allowed) {
		printf("ERRORE: scritti %zu pixel oltre la fine dello schermo Game Boy.\n", touched - allowed);
		printf("Qualcuno ha tolto SetSGBEnabled(false) o SetSGBBorder(false) da gb_core_create().\n");
		return 1;
	}

	puts("a posto: niente scritto oltre lo schermo.");
	return 0;
}

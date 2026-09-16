#ifndef GBDB_H
#define GBDB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The Game Boy ROM catalogue: which ones are on the card, and what they are
// really called.
//
// A filename is not a game's name. ROMs circulate as `pkmn_red_uv.gb` and a
// list of those is unreadable. The databases in /usr/resource/sonix/components
// (GB-Database.dat and GBC-Database.dat, or the same names without the hyphen;
// libretro-dats' clrmamepro format) hold the official name of every cartridge
// ever published, indexed by the ROM's CRC32. That is the name for the list.
//
// Computing the CRC32 means reading the whole ROM, and a cartridge reaches
// 8 MB. It happens once and the result is kept in
// <sd>/.local/gearboy-cache/index.tsv along with the file's size and mtime. If
// either changes, that ROM is redone and the others are not.
//
// "What is on the card" and "what it is called" stay separate on purpose: a ROM
// the database does not know (a fan translation, a homebrew) still gets listed,
// under a cleaned-up filename. Missing from the catalogue is not an error.

#define GBROM_PATH_MAX 512
#define GBROM_NAME_MAX 128

typedef struct {
	char path[GBROM_PATH_MAX]; // absolute path of the ROM
	char name[GBROM_NAME_MAX]; // from the database, or the cleaned-up filename
	char file[GBROM_NAME_MAX]; // the filename, for the second line
	uint32_t crc;
	bool from_db; // true when the name came from the database
	bool color;   // true when it sits in Games/GBC or ends in .gbc
} gbrom_t;

// The card root, under which Games/GB, Games/GBC and the cache live. Like
// everything else that writes to the card it comes from outside: the simulator
// has no card mounted.
void gbdb_set_root(const char *sd_root);

#include "src/system/core/respath.h"

// Where the databases live. /usr/resource/sonix/components on the device; the simulator moves it
// with SONIX_GBDB_DIR.
#define GBDB_DIR SONIX_RESOURCE_DIR "/components"

// Scans Games/GB and Games/GBC, computes the missing CRCs, resolves the names
// and returns the list sorted by name. `*out` must be freed with free().
//
// Not from the UI thread: the first time round this reads every ROM end to
// end. `progress` is called on the caller's thread while it works and may be
// NULL; returning false from it stops the scan, and what has been done so far
// is still written to the cache.
//
// Returns how many were found, or -1 when there is no root.
int gbdb_scan(gbrom_t **out, bool (*progress)(int done, int total, void *user), void *user);

// <card>/Games/Saves, where the games' battery-backed RAM and save states go.
// Empty string when there is no card. The pointer is to a static buffer: use it
// at once, do not keep it.
//
// It lives here rather than in gearboy.c because it is part of the card's
// layout, like Games/GB and Games/GBC, and this file owns that layout.
const char *gbdb_saves_dir(void);

// <card>/Games/Bios, where the real boot ROMs (dmg_boot.bin and cgb_boot.bin)
// live for the show-boot-rom option. Empty string without a card, as above.
const char *gbdb_bios_dir(void);

// A file's CRC32, or 0 when it cannot be read.
uint32_t gbdb_crc32_file(const char *path);

#endif /* GBDB_H */

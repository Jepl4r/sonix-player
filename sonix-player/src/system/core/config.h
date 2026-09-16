#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>

// Settings that survive a power cycle.
//
// On the device this lives at /usr/data/device_config.ini -- the UBIFS
// partition, which is the only writable place that is always there (the rootfs
// is read-only and the card may not be inserted). The format is a plain INI so
// it can be read and edited over ADB without any tooling:
//
//     [ui]
//     theme = dark
//
//     [clock]
//     configured = 1
//     last_seen = 1786000000
//
// Everything is held in memory; config_save() rewrites the file atomically, so
// a power cut in the middle of a write cannot leave a truncated config behind.

// Loads the file. A missing or unreadable file is not an error: the defaults
// passed to the getters are used and the file appears on the first save.
bool config_init(const char *path);

const char *config_get(const char *section, const char *key, const char *fallback);
long config_get_int(const char *section, const char *key, long fallback);
bool config_get_bool(const char *section, const char *key, bool fallback);

// The setters only touch memory. Call config_save() to put them on disk.
void config_set(const char *section, const char *key, const char *value);
void config_set_int(const char *section, const char *key, long value);
void config_set_bool(const char *section, const char *key, bool value);

// Writes the file if anything changed since the last save. Returns false when
// the file could not be written (read-only filesystem, no space).
bool config_save(void);

// Where the settings are being kept, or NULL if config_init() was never called.
const char *config_path(void);

// ---------------------------------------------------------------------------
// A second file, same shape
//
// The EPUB reader keeps one entry per book ever opened -- where the reading was
// left -- so its file grows with the shelf while device_config.ini grows with
// the player. Mixed together, a reader with two hundred books would slowly fill
// the settings table until a switch somewhere else stopped sticking, which is a
// failure nobody would ever connect back to a bookshelf. So the reader gets
// /usr/data/ebook_config.ini and the same code drives both.
//
// Everything above operates on the main store. These operate on whichever is
// passed, and config_main_store() is what the ones above use.
// ---------------------------------------------------------------------------

typedef struct config_store config_store_t;

config_store_t *config_main_store(void);
config_store_t *config_ebook_store(void);

bool config_store_init(config_store_t *store, const char *path);
const char *config_store_path(const config_store_t *store);

const char *config_store_get(config_store_t *store, const char *section, const char *key, const char *fallback);
long config_store_get_int(config_store_t *store, const char *section, const char *key, long fallback);
void config_store_set(config_store_t *store, const char *section, const char *key, const char *value);
void config_store_set_int(config_store_t *store, const char *section, const char *key, long value);
bool config_store_save(config_store_t *store);

// Moves every key of `section` from one store to the other and returns how many
// moved. What carries a player updated in place over to the split above,
// instead of silently resetting every book it had open.
int config_store_move_section(config_store_t *from, config_store_t *to, const char *section);

#endif /* CONFIG_H */

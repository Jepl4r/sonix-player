#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// A player's settings are a handful of short strings, so a fixed table keeps
// the whole thing allocation-free, which matters more on a device with ten
// megabytes free than an unbounded number of entries would.
//
// A full table is not an error anybody sees: config_set() drops every further
// key with only a line in the log, and the symptom is a setting that will not
// stick -- the switch moves, the value is never stored, and the getter keeps
// returning its default. So the size is counted rather than guessed: 126 keys
// written as literals in the source, plus band0..9 for the equaliser and v0..9
// for MSEB, is about 146 fixed entries. The only thing that would grow on top
// of that is the EPUB reader, which stores one entry per book opened and has a
// file of its own (see below), so 384 leaves room to spare.
#define CONFIG_MAX_ENTRIES 384

// The reader's own file. Its entries are one per book ever opened, so it grows
// with the shelf rather than with the player, which is the whole reason it is
// not in the table above. Five hundred books is a shelf nobody has on a
// forty-gigabyte card, and the table costs 192 bytes an entry.
#define EBOOK_MAX_ENTRIES 512

#define CONFIG_SECTION_LEN 32
#define CONFIG_KEY_LEN 32
#define CONFIG_VALUE_LEN 128

typedef struct {
	char section[CONFIG_SECTION_LEN];
	char key[CONFIG_KEY_LEN];
	char value[CONFIG_VALUE_LEN];
} config_entry_t;

// Two files, one piece of code. The settings and the reader's per-book state
// have nothing to do with each other and no reason to share a table, but they
// have exactly the same shape on disk -- so the store is a struct and the
// functions everything else calls are the same functions pointed at the main
// one.
struct config_store {
	config_entry_t *entries;
	int capacity;
	int count;
	char path[512];
	bool dirty;
	const char *name; // what the log calls it
};

static config_entry_t main_entries[CONFIG_MAX_ENTRIES];
static config_entry_t ebook_entries[EBOOK_MAX_ENTRIES];

static config_store_t g_main = {main_entries, CONFIG_MAX_ENTRIES, 0, "", false, "config"};
static config_store_t g_ebook = {ebook_entries, EBOOK_MAX_ENTRIES, 0, "", false, "ebook config"};

config_store_t *config_main_store(void) { return &g_main; }
config_store_t *config_ebook_store(void) { return &g_ebook; }

const char *config_path(void) { return g_main.path[0] ? g_main.path : NULL; }
const char *config_store_path(const config_store_t *store) {
	return store && store->path[0] ? store->path : NULL;
}

// Trims leading and trailing whitespace in place, returning the new start.
static char *trim(char *s) {
	while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
		s++;
	}

	char *end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
		end--;
	}
	*end = '\0';

	return s;
}

static config_entry_t *find(config_store_t *store, const char *section, const char *key) {
	for (int i = 0; i < store->count; i++) {
		if (strcmp(store->entries[i].section, section) == 0 && strcmp(store->entries[i].key, key) == 0) {
			return &store->entries[i];
		}
	}
	return NULL;
}

bool config_store_init(config_store_t *store, const char *path) {
	store->count = 0;
	store->dirty = false;
	store->path[0] = '\0';

	if (!path || !path[0]) {
		return false;
	}
	snprintf(store->path, sizeof(store->path), "%s", path);

	FILE *f = fopen(store->path, "r");
	if (!f) {
		printf("%s: %s not there yet, starting from defaults\n", store->name, store->path);
		return false;
	}

	char line[256];
	char section[CONFIG_SECTION_LEN] = "";

	while (fgets(line, sizeof(line), f)) {
		char *text = trim(line);

		if (text[0] == '\0' || text[0] == '#' || text[0] == ';') {
			continue; // blank or comment
		}

		if (text[0] == '[') {
			char *close = strchr(text, ']');
			if (close) {
				*close = '\0';
				snprintf(section, sizeof(section), "%s", trim(text + 1));
			}
			continue;
		}

		char *equals = strchr(text, '=');
		if (!equals) {
			continue; // not a key/value line; ignore rather than fail the load
		}

		*equals = '\0';
		config_store_set(store, section, trim(text), trim(equals + 1));
	}

	fclose(f);
	store->dirty = false; // what was just read is by definition already on disk

	printf("%s: loaded %d settings from %s\n", store->name, store->count, store->path);
	return true;
}

bool config_init(const char *path) { return config_store_init(&g_main, path); }

const char *config_store_get(config_store_t *store, const char *section, const char *key, const char *fallback) {
	config_entry_t *entry = find(store, section, key);
	return entry ? entry->value : fallback;
}

long config_store_get_int(config_store_t *store, const char *section, const char *key, long fallback) {
	const char *value = config_store_get(store, section, key, NULL);
	if (!value || !value[0]) {
		return fallback;
	}

	char *end = NULL;
	long parsed = strtol(value, &end, 10);
	return (end && end != value) ? parsed : fallback;
}

void config_store_set(config_store_t *store, const char *section, const char *key, const char *value) {
	if (!store || !section || !key || !value) {
		return;
	}

	config_entry_t *entry = find(store, section, key);
	if (entry) {
		if (strcmp(entry->value, value) == 0) {
			return; // unchanged: not worth a write
		}
		snprintf(entry->value, sizeof(entry->value), "%s", value);
		store->dirty = true;
		return;
	}

	if (store->count >= store->capacity) {
		fprintf(stderr, "%s: no room for %s/%s\n", store->name, section, key);
		return;
	}

	entry = &store->entries[store->count++];
	snprintf(entry->section, sizeof(entry->section), "%s", section);
	snprintf(entry->key, sizeof(entry->key), "%s", key);
	snprintf(entry->value, sizeof(entry->value), "%s", value);
	store->dirty = true;
}

void config_store_set_int(config_store_t *store, const char *section, const char *key, long value) {
	char text[32];
	snprintf(text, sizeof(text), "%ld", value);
	config_store_set(store, section, key, text);
}

const char *config_get(const char *section, const char *key, const char *fallback) {
	return config_store_get(&g_main, section, key, fallback);
}

long config_get_int(const char *section, const char *key, long fallback) {
	return config_store_get_int(&g_main, section, key, fallback);
}

bool config_get_bool(const char *section, const char *key, bool fallback) {
	const char *value = config_get(section, key, NULL);
	if (!value || !value[0]) {
		return fallback;
	}

	return value[0] == '1' || value[0] == 't' || value[0] == 'T' || value[0] == 'y' || value[0] == 'Y';
}

void config_set(const char *section, const char *key, const char *value) {
	config_store_set(&g_main, section, key, value);
}

void config_set_int(const char *section, const char *key, long value) {
	config_store_set_int(&g_main, section, key, value);
}

void config_set_bool(const char *section, const char *key, bool value) {
	config_set(section, key, value ? "1" : "0");
}

// Moves a whole section from one store to the other, leaving nothing behind.
// One use: the reader's settings that a config written by an older build still
// holds in device_config.ini, which would otherwise be lost -- every reading
// position and every choice -- now that the reader has a file of its own.
int config_store_move_section(config_store_t *from, config_store_t *to, const char *section) {
	if (!from || !to || !section) {
		return 0;
	}
	int moved = 0;
	int kept = 0;
	for (int i = 0; i < from->count; i++) {
		if (strcmp(from->entries[i].section, section) == 0) {
			config_store_set(to, section, from->entries[i].key, from->entries[i].value);
			moved++;
			continue;
		}
		if (kept != i) {
			from->entries[kept] = from->entries[i];
		}
		kept++;
	}
	if (moved) {
		from->count = kept;
		from->dirty = true;
	}
	return moved;
}

bool config_store_save(config_store_t *store) {
	if (!store || !store->path[0] || !store->dirty) {
		return true;
	}

	// Write beside the real file and rename over it. Rename is atomic, so a power
	// cut leaves either the old settings or the new ones, never half a file,
	// which on the next boot would read as no settings at all.
	char temp[560];
	snprintf(temp, sizeof(temp), "%s.tmp", store->path);

	FILE *f = fopen(temp, "w");
	if (!f) {
		fprintf(stderr, "%s: cannot write %s: keeping settings in memory only\n", store->name, temp);
		return false;
	}

	fprintf(f, "# sonix_player settings. Rewritten by the player; hand edits are read back.\n");

	// Grouped by section, in the order the sections first appear.
	for (int i = 0; i < store->count; i++) {
		bool section_started = false;
		for (int j = 0; j < i; j++) {
			if (strcmp(store->entries[j].section, store->entries[i].section) == 0) {
				section_started = true;
				break;
			}
		}
		if (section_started) {
			continue;
		}

		fprintf(f, "\n[%s]\n", store->entries[i].section);
		for (int j = i; j < store->count; j++) {
			if (strcmp(store->entries[j].section, store->entries[i].section) == 0) {
				fprintf(f, "%s = %s\n", store->entries[j].key, store->entries[j].value);
			}
		}
	}

	bool ok = (fflush(f) == 0);
	fclose(f);

	if (!ok || rename(temp, store->path) != 0) {
		unlink(temp);
		fprintf(stderr, "%s: could not replace %s\n", store->name, store->path);
		return false;
	}

	store->dirty = false;
	return true;
}

bool config_save(void) { return config_store_save(&g_main); }

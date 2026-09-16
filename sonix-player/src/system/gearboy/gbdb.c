#include "gbdb.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Where ROMs are looked for, under the card root. Two directories rather than
// one because that is how collections are usually kept, and because which of
// the two a file came from is half the answer to whether it is a colour title;
// the extension is the other half.
#define DIR_GB "Games/GB"
#define DIR_GBC "Games/GBC"

// Where what a game produces ends up: battery-backed RAM (.sav) and save
// states (.state1). One directory for all of them, beside the ROMs rather than
// inside their folders, so saves can be copied off in one go and the same
// cartridge present in both GB and GBC does not collide on a file name.
#define DIR_SAVES "Games/Saves"

#define CACHE_DIR ".local/gearboy-cache"
#define CACHE_FILE "index.tsv"

// A Game Boy cartridge is between 32 KB and 8 MB. Anything outside that is not
// a ROM, and it is worth finding out before reading the whole file for its CRC.
#define ROM_MIN_SIZE (32 * 1024)
#define ROM_MAX_SIZE (8 * 1024 * 1024)

// Read size while computing the CRC. 32 KB is large enough to avoid constant
// syscalls and small enough not to matter on a 64 MB device (it is heap
// allocated in any case).
#define CRC_CHUNK (32 * 1024)

static char root_path[GBROM_PATH_MAX];
static pthread_mutex_t root_lock = PTHREAD_MUTEX_INITIALIZER;

void gbdb_set_root(const char *sd_root) {
	pthread_mutex_lock(&root_lock);
	snprintf(root_path, sizeof(root_path), "%s", sd_root ? sd_root : "");
	pthread_mutex_unlock(&root_lock);
}

// See gbdb.h. The directory is not created here: whoever writes into it creates
// it the first time it is actually needed.
const char *gbdb_bios_dir(void) {
	static char path[GBROM_PATH_MAX + 32];
	pthread_mutex_lock(&root_lock);
	if (root_path[0]) {
		snprintf(path, sizeof(path), "%s/Games/Bios", root_path);
	} else {
		path[0] = '\0';
	}
	pthread_mutex_unlock(&root_lock);
	return path;
}

const char *gbdb_saves_dir(void) {
	// Deliberately wider than a root path: the root may itself be as long as a
	// full path, and "/Games/Saves" still has to fit after it.
	static char path[GBROM_PATH_MAX + 32];
	pthread_mutex_lock(&root_lock);
	if (root_path[0]) {
		snprintf(path, sizeof(path), "%s/%s", root_path, DIR_SAVES);
	} else {
		path[0] = '\0';
	}
	pthread_mutex_unlock(&root_lock);
	return path;
}

static bool card_root(char *out, size_t size) {
	pthread_mutex_lock(&root_lock);
	bool have = root_path[0] != '\0';
	if (have) {
		snprintf(out, size, "%s", root_path);
	}
	pthread_mutex_unlock(&root_lock);
	return have;
}

// ---------------------------------------------------------------------------
// CRC32
// ---------------------------------------------------------------------------
//
// The PKZIP/PNG variant, reflected polynomial 0xEDB88320, which is what the
// clrmamepro databases carry. The table is built once on first use: one
// kilobyte, against sixteen shift steps per byte without it.

static uint32_t crc_table[256];
static bool crc_table_ready;
static pthread_once_t crc_once = PTHREAD_ONCE_INIT;

static void crc_build_table(void) {
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;
		for (int k = 0; k < 8; k++) {
			c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
		}
		crc_table[i] = c;
	}
	crc_table_ready = true;
}

static uint32_t crc_update(uint32_t crc, const uint8_t *data, size_t len) {
	crc = ~crc;
	for (size_t i = 0; i < len; i++) {
		crc = crc_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
	}
	return ~crc;
}

uint32_t gbdb_crc32_file(const char *path) {
	pthread_once(&crc_once, crc_build_table);
	if (!crc_table_ready) {
		return 0;
	}

	FILE *f = fopen(path, "rb");
	if (!f) {
		return 0;
	}

	uint8_t *buf = malloc(CRC_CHUNK);
	if (!buf) {
		fclose(f);
		return 0;
	}

	uint32_t crc = 0;
	size_t got;
	while ((got = fread(buf, 1, CRC_CHUNK, f)) > 0) {
		crc = crc_update(crc, buf, got);
	}

	free(buf);
	fclose(f);
	return crc;
}

// ---------------------------------------------------------------------------
// The cache
// ---------------------------------------------------------------------------
//
// One file, one line per ROM, tab-separated fields:
//
//     crc <TAB> size <TAB> mtime <TAB> path <TAB> name
//
// Path and name come last because they are the only two that can contain
// spaces; tabs separate them either way, but this keeps the file readable by
// eye when something is wrong. Size and mtime are the proof that the line still
// describes the file on disk: if either differs, that ROM is read again.

typedef struct {
	uint32_t crc;
	long long size;
	long long mtime;
	char path[GBROM_PATH_MAX];
	char name[GBROM_NAME_MAX];
} cache_entry_t;

typedef struct {
	cache_entry_t *items;
	int count;
	int cap;
} cache_t;

static void cache_free(cache_t *c) {
	free(c->items);
	c->items = NULL;
	c->count = c->cap = 0;
}

static bool cache_push(cache_t *c, const cache_entry_t *e) {
	if (c->count == c->cap) {
		int cap = c->cap ? c->cap * 2 : 32;
		cache_entry_t *grown = realloc(c->items, (size_t)cap * sizeof(*grown));
		if (!grown) {
			return false;
		}
		c->items = grown;
		c->cap = cap;
	}
	c->items[c->count++] = *e;
	return true;
}

// Working paths are deliberately wider than a ROM path: a root already as long
// as GBROM_PATH_MAX gets a subdirectory and a file name appended, which a
// same-sized buffer could not hold. gcc warns about it, correctly.
#define WORK_PATH_MAX (GBROM_PATH_MAX + 64)

static void cache_path(char *out, size_t size, const char *root) {
	snprintf(out, size, "%s/%s/%s", root, CACHE_DIR, CACHE_FILE);
}

static void cache_load(cache_t *c, const char *root) {
	char path[WORK_PATH_MAX];
	cache_path(path, sizeof(path), root);

	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}

	// Twice GBROM_PATH_MAX plus a name covers any line cache_save() can have
	// written; a longer line did not come from here.
	char line[GBROM_PATH_MAX * 2 + GBROM_NAME_MAX];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (!line[0]) {
			continue;
		}

		// Five fields, split by hand rather than with strtok_r: strtok_r
		// collapses an empty field (a name that was never resolved) instead of
		// keeping it, which would shift every following field.
		char *field[5];
		int n = 0;
		char *p = line;
		while (n < 5) {
			field[n++] = p;
			char *tab = strchr(p, '\t');
			if (!tab) {
				break;
			}
			*tab = '\0';
			p = tab + 1;
		}
		if (n < 5) {
			continue;
		}

		cache_entry_t e;
		memset(&e, 0, sizeof(e));
		e.crc = (uint32_t)strtoul(field[0], NULL, 16);
		e.size = strtoll(field[1], NULL, 10);
		e.mtime = strtoll(field[2], NULL, 10);
		snprintf(e.path, sizeof(e.path), "%s", field[3]);
		snprintf(e.name, sizeof(e.name), "%s", field[4]);

		if (e.path[0]) {
			cache_push(c, &e);
		}
	}

	fclose(f);
}

// mkdir -p for the two levels needed: .local almost always exists already (the
// log lives there), gearboy-cache almost never does on the first run.
static void ensure_cache_dir(const char *root) {
	char path[WORK_PATH_MAX];
	snprintf(path, sizeof(path), "%s/.local", root);
	mkdir(path, 0777);
	snprintf(path, sizeof(path), "%s/%s", root, CACHE_DIR);
	mkdir(path, 0777);
}

// Written to a temporary file and renamed, never written in place: if power is
// lost halfway through -- which happens on a battery device that shuts itself
// down -- the old file survives intact instead of becoming half a new one.
static void cache_save(const cache_t *c, const char *root) {
	ensure_cache_dir(root);

	char path[WORK_PATH_MAX];
	char tmp[WORK_PATH_MAX + 8];
	cache_path(path, sizeof(path), root);
	snprintf(tmp, sizeof(tmp), "%s.new", path);

	FILE *f = fopen(tmp, "w");
	if (!f) {
		fprintf(stderr, "gbdb: the cache cannot be written (%s): %s\n", tmp, strerror(errno));
		return;
	}

	for (int i = 0; i < c->count; i++) {
		const cache_entry_t *e = &c->items[i];
		fprintf(f, "%08X\t%lld\t%lld\t%s\t%s\n", e->crc, e->size, e->mtime, e->path, e->name);
	}

	fflush(f);
	fclose(f);

	if (rename(tmp, path) != 0) {
		fprintf(stderr, "gbdb: the cache cannot be renamed: %s\n", strerror(errno));
		remove(tmp);
	}
}

static const cache_entry_t *cache_find(const cache_t *c, const char *path, long long size, long long mtime) {
	for (int i = 0; i < c->count; i++) {
		const cache_entry_t *e = &c->items[i];
		if (e->size == size && e->mtime == mtime && strcmp(e->path, path) == 0) {
			return e;
		}
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// The databases
// ---------------------------------------------------------------------------
//
// clrmamepro format. Only two lines per game matter:
//
//     game (
//         name "007 - The World Is Not Enough (USA, Europe)"
//         ...
//         rom ( name "....gbc" size 2097152 crc E038E666 md5 ... )
//     )
//
// No index is built. The two files together are about a megabyte and nearly
// five thousand games, and holding that in memory to resolve three names is the
// most expensive option on a 64 MB device. Instead the list of missing CRCs is
// collected first, then the files are read once looking for exactly those: one
// pass per scan rather than one per ROM, and later scans make no pass at all
// because the cache answers on its own.

typedef struct {
	uint32_t crc;
	char name[GBROM_NAME_MAX];
	bool found;
} wanted_t;

// Extracts the quoted value of `name "..."`. Returns false if the line is not
// a name line.
static bool parse_quoted_name(const char *line, char *out, size_t out_size) {
	const char *p = line;
	while (*p == ' ' || *p == '\t') {
		p++;
	}
	if (strncmp(p, "name ", 5) != 0) {
		return false;
	}
	p = strchr(p, '"');
	if (!p) {
		return false;
	}
	p++;

	const char *end = strrchr(p, '"');
	if (!end || end <= p) {
		return false;
	}

	size_t len = (size_t)(end - p);
	if (len >= out_size) {
		len = out_size - 1;
	}
	memcpy(out, p, len);
	out[len] = '\0';
	return true;
}

// One pass over a .dat, filling in the names of the CRCs being looked for.
static void scan_dat(const char *dat_path, wanted_t *wanted, int wanted_count) {
	FILE *f = fopen(dat_path, "r");
	if (!f) {
		return;
	}

	char line[1024];
	char game_name[GBROM_NAME_MAX] = "";

	while (fgets(line, sizeof(line), f)) {
		// The game's name line always precedes its rom() line, so remembering
		// the last one seen is enough. The `rom ( name "..." ` line also holds
		// a `name`, but it starts with "rom", so parse_quoted_name rejects it.
		if (parse_quoted_name(line, game_name, sizeof(game_name))) {
			continue;
		}

		const char *crc_at = strstr(line, " crc ");
		if (!crc_at || !game_name[0]) {
			continue;
		}

		uint32_t crc = (uint32_t)strtoul(crc_at + 5, NULL, 16);
		for (int i = 0; i < wanted_count; i++) {
			if (!wanted[i].found && wanted[i].crc == crc) {
				snprintf(wanted[i].name, sizeof(wanted[i].name), "%s", game_name);
				wanted[i].found = true;
			}
		}
	}

	fclose(f);
}

static void resolve_names(wanted_t *wanted, int count) {
	if (count <= 0) {
		return;
	}

	const char *dir = getenv("SONIX_GBDB_DIR");
	if (!dir || !dir[0]) {
		dir = GBDB_DIR;
	}

	// Both spellings, hyphenated and not: collections ship the files under
	// either. A name that is not there costs one failed open.
	static const char *const NAMES[] = {"GB-Database.dat", "GBDatabase.dat", "GBC-Database.dat", "GBCDatabase.dat"};

	char path[WORK_PATH_MAX];
	for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", dir, NAMES[i]);
		scan_dat(path, wanted, count);
	}
}

// ---------------------------------------------------------------------------
// Fallback name
// ---------------------------------------------------------------------------

// Built from the file name when the database knows nothing: extension dropped,
// underscores and dots turned back into spaces. `pkmn_red-uv.gb` becomes
// `pkmn red-uv`. Not the real title, only something readable.
static void name_from_file(const char *file, char *out, size_t out_size) {
	snprintf(out, out_size, "%s", file);

	char *dot = strrchr(out, '.');
	if (dot && dot != out) {
		*dot = '\0';
	}

	for (char *p = out; *p; p++) {
		if (*p == '_' || *p == '.') {
			*p = ' ';
		}
	}
}

// ---------------------------------------------------------------------------
// The scan
// ---------------------------------------------------------------------------

static bool has_ext(const char *name, const char *ext) {
	size_t n = strlen(name), e = strlen(ext);
	if (n <= e) {
		return false;
	}
	return strcasecmp(name + n - e, ext) == 0;
}

typedef struct {
	gbrom_t *items;
	int count;
	int cap;
} romlist_t;

static bool romlist_push(romlist_t *l, const gbrom_t *r) {
	if (l->count == l->cap) {
		int cap = l->cap ? l->cap * 2 : 32;
		gbrom_t *grown = realloc(l->items, (size_t)cap * sizeof(*grown));
		if (!grown) {
			return false;
		}
		l->items = grown;
		l->cap = cap;
	}
	l->items[l->count++] = *r;
	return true;
}

// Lists the candidate files in one directory, without CRC or name. The
// expensive work comes later, and only for the ones the cache does not cover.
static void collect_dir(romlist_t *list, const char *root, const char *subdir, bool color_dir) {
	char dir[WORK_PATH_MAX];
	snprintf(dir, sizeof(dir), "%s/%s", root, subdir);

	DIR *d = opendir(dir);
	if (!d) {
		return;
	}

	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.') {
			continue;
		}
		bool gb = has_ext(de->d_name, ".gb");
		bool gbc = has_ext(de->d_name, ".gbc");
		if (!gb && !gbc) {
			continue;
		}

		gbrom_t r;
		memset(&r, 0, sizeof(r));
		// The explicit precisions keep a long directory plus a long file name
		// from overrunning r.path. Truncating here is intended -- a path longer
		// than this is not a ROM worth keeping.
		snprintf(r.path, sizeof(r.path), "%.*s/%.*s", (int)sizeof(r.path) / 2 - 2, dir,
				 (int)sizeof(r.path) / 2 - 2, de->d_name);
		snprintf(r.file, sizeof(r.file), "%.*s", (int)sizeof(r.file) - 1, de->d_name);
		r.color = gbc || color_dir;

		// Reject an impossible size now, before reading eight megabytes only
		// to discover the file was never a cartridge.
		struct stat st;
		if (stat(r.path, &st) != 0 || !S_ISREG(st.st_mode)) {
			continue;
		}
		if (st.st_size < ROM_MIN_SIZE || st.st_size > ROM_MAX_SIZE) {
			continue;
		}

		romlist_push(list, &r);
	}

	closedir(d);
}

static int compare_by_name(const void *a, const void *b) {
	const gbrom_t *x = a, *y = b;
	int c = strcasecmp(x->name, y->name);
	return c ? c : strcmp(x->path, y->path);
}

int gbdb_scan(gbrom_t **out, bool (*progress)(int done, int total, void *user), void *user) {
	if (out) {
		*out = NULL;
	}

	char root[GBROM_PATH_MAX];
	if (!card_root(root, sizeof(root))) {
		return -1;
	}

	romlist_t list = {0};
	collect_dir(&list, root, DIR_GB, false);
	collect_dir(&list, root, DIR_GBC, true);

	// Logged on every scan, not only on failure: "nothing found" and "looking
	// in the wrong place" are the same empty screen, and without this line they
	// can only be told apart by guessing.
	fprintf(stderr, "gbdb: %s/%s + %s/%s -> %d ROM\n", root, DIR_GB, root, DIR_GBC, list.count);

	if (list.count == 0) {
		free(list.items);
		return 0;
	}

	cache_t cache = {0};
	cache_load(&cache, root);

	// First pass: entries answered by the cache are done; the rest are read
	// through to compute their CRC. This is the only expensive part, and the
	// one `progress` reports on.
	wanted_t *wanted = calloc((size_t)list.count, sizeof(*wanted));
	int wanted_count = 0;
	bool stopped = false;

	for (int i = 0; i < list.count; i++) {
		gbrom_t *r = &list.items[i];

		struct stat st;
		if (stat(r->path, &st) != 0) {
			continue;
		}

		const cache_entry_t *hit = cache_find(&cache, r->path, (long long)st.st_size, (long long)st.st_mtime);
		if (hit) {
			r->crc = hit->crc;
			if (hit->name[0]) {
				snprintf(r->name, sizeof(r->name), "%s", hit->name);
				r->from_db = true;
			}
			continue;
		}

		if (progress && !progress(i, list.count, user)) {
			stopped = true;
			break;
		}

		r->crc = gbdb_crc32_file(r->path);
		if (r->crc && wanted) {
			wanted[wanted_count].crc = r->crc;
			wanted_count++;
		}
	}

	// Second pass: one sweep of the two databases for all new CRCs together.
	// With none of them -- the normal case from the second launch on -- the
	// database files are not even opened.
	if (wanted && wanted_count > 0) {
		resolve_names(wanted, wanted_count);

		for (int i = 0; i < list.count; i++) {
			gbrom_t *r = &list.items[i];
			if (r->from_db || !r->crc) {
				continue;
			}
			for (int w = 0; w < wanted_count; w++) {
				if (wanted[w].found && wanted[w].crc == r->crc) {
					snprintf(r->name, sizeof(r->name), "%s", wanted[w].name);
					r->from_db = true;
					break;
				}
			}
		}
	}
	free(wanted);

	// Anything still unnamed is not in the databases and falls back to its file
	// name. This also covers ROMs skipped by an interrupted scan: better listed
	// under a file name than missing.
	for (int i = 0; i < list.count; i++) {
		if (!list.items[i].name[0]) {
			name_from_file(list.items[i].file, list.items[i].name, sizeof(list.items[i].name));
		}
	}

	// The cache is rewritten from the fresh list rather than patched into the
	// old one, so lines for deleted ROMs disappear instead of lingering
	// forever. ROMs skipped by an interruption have no CRC and are not written,
	// so the next scan picks them up.
	{
		cache_t fresh = {0};
		for (int i = 0; i < list.count; i++) {
			const gbrom_t *r = &list.items[i];
			if (!r->crc) {
				continue;
			}
			struct stat st;
			if (stat(r->path, &st) != 0) {
				continue;
			}
			cache_entry_t e;
			memset(&e, 0, sizeof(e));
			e.crc = r->crc;
			e.size = (long long)st.st_size;
			e.mtime = (long long)st.st_mtime;
			snprintf(e.path, sizeof(e.path), "%s", r->path);
			snprintf(e.name, sizeof(e.name), "%s", r->from_db ? r->name : "");
			cache_push(&fresh, &e);
		}
		cache_save(&fresh, root);
		cache_free(&fresh);
	}

	cache_free(&cache);

	qsort(list.items, (size_t)list.count, sizeof(gbrom_t), compare_by_name);

	if (progress && !stopped) {
		progress(list.count, list.count, user);
	}

	if (out) {
		*out = list.items;
	} else {
		free(list.items);
	}
	return list.count;
}

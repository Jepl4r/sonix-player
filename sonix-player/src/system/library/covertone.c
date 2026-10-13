#include "covertone.h"

#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/gui/shell/gui.h"
#include "src/system/audio/audio.h"
#include "src/system/core/utils.h"
#include "src/system/device/power.h"
#include "src/system/image/albumtone.h"
#include "src/system/image/jpeg_planes.h"
#include "src/system/image/jpeg_scaled.h"
#include "src/system/image/png_scaled.h"
#include "src/system/library/albumart.h"
#include "src/system/library/library.h"
#include "src/system/library/librarycovers.h"

// ---------------------------------------------------------------------------
// Versions
//
// COVERTONE_VERSION is the analysis: raising it makes every row stale, and the
// worker (not a scan) works them all out again. COVERTONE_THUMB_FORMAT is the
// layout of the thumbnail files: a file written under another one is started
// again, and the rows forget the thumbnails it held.
// ---------------------------------------------------------------------------

#define COVERTONE_VERSION 1
#define COVERTONE_THUMB_FORMAT 1

// How a byte of the database stands for a float of OKLCH.
#define CHROMA_SCALE 510.0f
#define LIGHT_SCALE 255.0f

// The JPEG reductions asked for. A box of 28 keeps the short side of the
// decode at 56 or more -- 1/8 of a 600 px cover, 1/8 of anything larger --
// which is the small thumbnail at about one source pixel and a half per pixel,
// and four thousand points for the hue. A box of 120 keeps it at 240 or more
// for the large one.
#define BOX_SMALL 28
#define BOX_LARGE 120

static uint32_t mono_ms(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)(now.tv_sec * 1000u) + (uint32_t)(now.tv_nsec / 1000000);
}

// ---------------------------------------------------------------------------
// Album ids
// ---------------------------------------------------------------------------

static uint64_t fnv_fold(uint64_t h, const char *text) {
	for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; p++) {
		unsigned c = *p;
		if (c >= 'A' && c <= 'Z') {
			c += 'a' - 'A';
		}
		h ^= c;
		h *= 1099511628211ULL;
	}
	return h;
}

int64_t covertone_album_id_parts(const char *album, const char *album_key) {
	if (!album || !album[0]) {
		return 0;
	}
	// The name folded as SQLite's NOCASE folds it -- ASCII only, which is how
	// the library tells albums apart -- the separator, and the key.
	uint64_t h = fnv_fold(1469598103934665603ULL, album);
	h ^= 0x1f;
	h *= 1099511628211ULL;
	h = fnv_fold(h, album_key);
	// 63 bits: SQLite's rowid is signed, and a positive id reads the same in
	// the database, in the log and in the thumbnail files.
	h &= 0x7fffffffffffffffULL;
	return (int64_t)(h ? h : 1);
}

int64_t covertone_album_id(const char *album_value) {
	if (!album_value || !album_value[0]) {
		return 0;
	}
	const char *sep = strchr(album_value, LIBRARY_ALBUM_KEY_SEP);
	if (!sep) {
		return covertone_album_id_parts(album_value, "");
	}
	char name[512];
	size_t len = (size_t)(sep - album_value);
	if (len >= sizeof(name)) {
		len = sizeof(name) - 1;
	}
	memcpy(name, album_value, len);
	name[len] = '\0';
	return covertone_album_id_parts(name, sep + 1);
}

uint16_t covertone_hash_hue(const char *album_title) { return albumtone_hash_hue(album_title); }

// ---------------------------------------------------------------------------
// State
//
// `state_lock` guards everything in memory below and is only ever held for a
// few lines: the UI thread takes it for every lookup. `job_lock` is held for
// the whole of one album -- decode, files, row -- by whichever of the scan
// thread and the worker is making it, so the two never write the same slot,
// and by detach to wait for the album in hand. `io_lock` is held around every
// open thumbnail file, so detach knows none is left open when it returns.
//
// Order: job_lock, then io_lock, then state_lock. The library's own lock is
// taken inside library calls only, and never while state_lock is held.
// ---------------------------------------------------------------------------

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t io_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wakeup = PTHREAD_COND_INITIALIZER;

static bool attached;	// state_lock
static bool io_allowed; // io_lock: the files may be opened
static char covers_dir[512]; // io_lock

// The tones, by album id: open addressing, linear probing, at most half full.
// Sixteen bytes an album.
typedef struct {
	int64_t id; // 0 is an empty place
	uint16_t hue;
	uint8_t chroma, light, kind, thumbs;
	int32_t slot;
} entry_t;

static entry_t *table;
static size_t table_cap; // a power of two, or 0
static size_t table_count;

// Which slots of the thumbnail files are taken: one byte a slot.
static uint8_t *slot_used;
static int32_t slot_cap;

static volatile uint32_t revision;
static void (*listener)(void);
static bool notify_pending; // something landed since the listener last heard
static bool notify_posted;	// a call to it is on its way to the UI thread
static uint32_t notify_last_ms;
#define NOTIFY_EVERY_MS 250

// The track -> album lookups of covertone_for_path(), last few.
typedef struct {
	uint64_t path_hash;
	int64_t id;
	unsigned rev;
	bool used;
} memo_t;
#define MEMO_SIZE 8
static memo_t memo[MEMO_SIZE];
static int memo_next;

// Work for the worker.
#define REQUEST_MAX 32
static int64_t requests[REQUEST_MAX]; // newest first
static int request_count;
static bool fill_wanted;	// look for stale albums and missing thumbnails
static bool prune_wanted;	// the albums may have changed
static bool worker_started;

// The worker walks the albums in rowid order with a cursor per phase, so a
// pause or a request in between resumes where it left off.
static int64_t fill_after;
static int fill_phase; // 0: stale tones, 1: missing thumbnails
static int fill_sweep_made;

// Pausing.
static int pause_count;
static uint32_t pause_since_ms;
static volatile uint32_t ui_busy_until_ms;
static pthread_t ui_thread;
static volatile bool ui_thread_known;
#define PAUSE_LAPSE_MS 30000
#define UI_BUSY_HOLD_MS 400
// A track started less than this ago may still be reading its first blocks off
// the card; the waveform waits the same way (audio_quiet_ms).
#define AUDIO_QUIET_MS 2500

// The memory one album's decode held at most this session, for the log.
static size_t peak_bytes;

// The most a progressive JPEG's decode may take on the worker: a quarter of
// what is free, between one and three megabytes. Only the plane decoder needs
// a budget; baseline JPEGs and PNGs are decoded scaled and never come near.
#define PLANES_BUDGET_MIN (1024 * 1024)
#define PLANES_BUDGET_MAX (3 * 1024 * 1024)

// The asynchronous thumbnails.
typedef enum {
	TICKET_FREE = 0,
	TICKET_WAITING,
	TICKET_RUNNING,
	TICKET_READY,
} ticket_state_t;

typedef struct {
	uint32_t ticket;
	int64_t id;
	int size;
	covertone_thumb_cb cb;
	void *user;
	ticket_state_t state;
	bool cancelled;
	bool posted;
	uint16_t *pixels; // big_alloc'd, COVERTONE_THUMB_BYTES(size)
} ticket_t;

#define TICKET_MAX 16
static ticket_t tickets[TICKET_MAX];
static uint32_t ticket_next = 1;

static void bump_revision_locked(void) {
	revision++;
	notify_pending = true;
}

// ---------------------------------------------------------------------------
// The table
// ---------------------------------------------------------------------------

static size_t home_of(int64_t id, size_t cap) {
	uint64_t h = (uint64_t)id * 0x9E3779B97F4A7C15ULL;
	return (size_t)(h >> 24) & (cap - 1);
}

static entry_t *table_find_locked(int64_t id) {
	if (!table_cap || !id) {
		return NULL;
	}
	for (size_t i = home_of(id, table_cap), n = 0; n < table_cap; i = (i + 1) & (table_cap - 1), n++) {
		if (table[i].id == id) {
			return &table[i];
		}
		if (!table[i].id) {
			return NULL;
		}
	}
	return NULL;
}

static bool table_grow_locked(void) {
	size_t cap = table_cap ? table_cap * 2 : 256;
	entry_t *grown = calloc(cap, sizeof(*grown));
	if (!grown) {
		return false;
	}
	for (size_t i = 0; i < table_cap; i++) {
		if (table[i].id) {
			size_t j = home_of(table[i].id, cap);
			while (grown[j].id) {
				j = (j + 1) & (cap - 1);
			}
			grown[j] = table[i];
		}
	}
	free(table);
	table = grown;
	table_cap = cap;
	return true;
}

static void slot_mark_locked(int32_t slot, bool used) {
	if (slot < 0) {
		return;
	}
	if (slot >= slot_cap) {
		if (!used) {
			return;
		}
		int32_t cap = slot_cap ? slot_cap : 1024;
		while (cap <= slot) {
			cap *= 2;
		}
		uint8_t *grown = realloc(slot_used, (size_t)cap);
		if (!grown) {
			return;
		}
		memset(grown + slot_cap, 0, (size_t)(cap - slot_cap));
		slot_used = grown;
		slot_cap = cap;
	}
	slot_used[slot] = used ? 1 : 0;
}

// The lowest free slot, taken. -1 when memory runs out.
static int32_t slot_take_locked(void) {
	for (int32_t i = 0; i < slot_cap; i++) {
		if (!slot_used[i]) {
			slot_used[i] = 1;
			return i;
		}
	}
	int32_t slot = slot_cap;
	slot_mark_locked(slot, true);
	return slot < slot_cap ? slot : -1;
}

static int32_t slot_highest_locked(void) {
	for (int32_t i = slot_cap - 1; i >= 0; i--) {
		if (slot_used[i]) {
			return i;
		}
	}
	return -1;
}

static void table_put_locked(const library_cover_brief_t *row) {
	entry_t *e = table_find_locked(row->id);
	if (!e) {
		if ((table_count + 1) * 2 > table_cap && !table_grow_locked()) {
			return;
		}
		size_t i = home_of(row->id, table_cap);
		while (table[i].id) {
			i = (i + 1) & (table_cap - 1);
		}
		e = &table[i];
		table_count++;
	} else if (e->slot != row->slot) {
		slot_mark_locked(e->slot, false);
	}
	e->id = row->id;
	e->hue = row->hue;
	e->chroma = row->chroma;
	e->light = row->light;
	e->kind = row->kind;
	e->thumbs = row->thumbs;
	e->slot = row->slot;
	slot_mark_locked(row->slot, true);
}

// Deletion from a linear-probing table: the entries after the hole that would
// not be found past it are moved back into it.
static void table_remove_locked(int64_t id) {
	entry_t *e = table_find_locked(id);
	if (!e) {
		return;
	}
	slot_mark_locked(e->slot, false);
	size_t hole = (size_t)(e - table);
	table[hole].id = 0;
	table_count--;
	for (size_t i = (hole + 1) & (table_cap - 1); table[i].id; i = (i + 1) & (table_cap - 1)) {
		size_t home = home_of(table[i].id, table_cap);
		// Whether `home` lies cyclically in (hole, i]: then the entry is
		// reachable without the hole and stays.
		bool stays = hole <= i ? (home > hole && home <= i) : (home > hole || home <= i);
		if (!stays) {
			table[hole] = table[i];
			table[i].id = 0;
			hole = i;
		}
	}
}

static void table_clear_locked(void) {
	free(table);
	table = NULL;
	table_cap = table_count = 0;
	free(slot_used);
	slot_used = NULL;
	slot_cap = 0;
	memset(memo, 0, sizeof(memo));
}

typedef struct {
	library_cover_brief_t *rows;
	int count, cap;
} rows_t;

static void collect_row_cb(const library_cover_brief_t *row, void *user) {
	rows_t *rows = user;
	if (rows->count == rows->cap) {
		int cap = rows->cap ? rows->cap * 2 : 256;
		library_cover_brief_t *grown = realloc(rows->rows, (size_t)cap * sizeof(*grown));
		if (!grown) {
			return;
		}
		rows->rows = grown;
		rows->cap = cap;
	}
	rows->rows[rows->count++] = *row;
}

// Every row into memory: a few thousand rows, a few milliseconds. Collected
// first and put in afterwards, so the library's lock and ours are never held
// together.
static void table_reload(void) {
	uint32_t started = mono_ms();
	rows_t rows = {0};
	library_covers_load(collect_row_cb, &rows);
	pthread_mutex_lock(&state_lock);
	table_clear_locked();
	for (int i = 0; i < rows.count; i++) {
		table_put_locked(&rows.rows[i]);
	}
	bump_revision_locked();
	pthread_mutex_unlock(&state_lock);
	free(rows.rows);
	printf("covertone: %d album colour(s) loaded in %u ms\n", rows.count, mono_ms() - started);
}

// ---------------------------------------------------------------------------
// The thumbnail files
//
// One file per size, .local/covers/thumbs56.sxt and thumbs240.sxt, beside the
// index. A 64-byte header -- "SXCOVER1", the format, the side, the bytes per
// slot -- then fixed-size slots, one per album, the slot number kept in the
// album's row:
//
//     u64 album id | u32 checksum of the pixels | u16 side | u16 'TX'
//     side x side RGB565, row by row, little-endian as LVGL draws it
//
// The slot header names its album, so a slot reused by another album, a row
// that outlived its file, or a write cut short by a pulled card (the header is
// wiped first and written last) all read as a miss and the thumbnail is made
// again. Nothing is ever fsync'd: this is a cache.
//
// Albums that go away free their slots (the rows are pruned after a scan),
// new ones take the lowest free slot, and a file is cut back when its last
// slots are free. 56 px is 6.3 KB a slot, 240 px 115 KB.
// ---------------------------------------------------------------------------

#define PACK_HEADER 64
#define SLOT_HEADER 16
#define SLOT_MAGIC 0x5854u
static const char PACK_MAGIC[8] = {'S', 'X', 'C', 'O', 'V', 'E', 'R', '1'};

typedef struct {
	int64_t id;
	uint32_t sum;
	uint16_t side;
	uint16_t magic;
} slot_header_t;

static size_t slot_bytes(int side) { return SLOT_HEADER + COVERTONE_THUMB_BYTES(side); }

static off_t slot_offset(int side, int32_t slot) { return (off_t)PACK_HEADER + (off_t)slot * (off_t)slot_bytes(side); }

static uint32_t pixels_sum(const uint16_t *px, size_t count) {
	uint32_t h = 0x811C9DC5u;
	for (size_t i = 0; i < count; i++) {
		h = ((h << 5) | (h >> 27)) ^ px[i];
	}
	return h;
}

static void pack_header_fill(uint8_t *header, int side) {
	memset(header, 0, PACK_HEADER);
	memcpy(header, PACK_MAGIC, sizeof(PACK_MAGIC));
	uint32_t words[3] = {COVERTONE_THUMB_FORMAT, (uint32_t)side, (uint32_t)slot_bytes(side)};
	memcpy(header + 8, words, sizeof(words));
}

// With io_lock held.
static bool pack_path(int side, char *out, size_t size) {
	if (!io_allowed || !covers_dir[0]) {
		return false;
	}
	return snprintf(out, size, "%s/thumbs%d.sxt", covers_dir, side) < (int)size;
}

// Opens the file of `side` for writing, making it (and its folder) when it is
// not there and starting it again when its header is not this format's. With
// io_lock held. *restarted says the slots it held are gone.
static int pack_open_write(int side, bool *restarted) {
	*restarted = false;
	char path[600];
	if (!pack_path(side, path, sizeof(path))) {
		return -1;
	}
	mkdir(covers_dir, 0755);
	int fd = open(path, O_RDWR | O_CREAT, 0644);
	if (fd < 0) {
		return -1;
	}
	uint8_t want[PACK_HEADER], have[PACK_HEADER];
	pack_header_fill(want, side);
	ssize_t got = pread(fd, have, sizeof(have), 0);
	if (got == (ssize_t)sizeof(have) && memcmp(want, have, sizeof(want)) == 0) {
		return fd;
	}
	if (got != 0) {
		printf("covertone: %s is not in this format; starting it again\n", path);
		*restarted = true;
	}
	if (ftruncate(fd, 0) != 0 || pwrite(fd, want, sizeof(want), 0) != (ssize_t)sizeof(want)) {
		close(fd);
		return -1;
	}
	return fd;
}

// The slots of a file started again are empty: the rows forget them.
static void pack_forget(int side) {
	int bit = side == COVERTONE_THUMB_SMALL ? LIBRARY_COVER_THUMB_SMALL : LIBRARY_COVER_THUMB_LARGE;
	library_covers_forget_thumbs(bit);
	pthread_mutex_lock(&state_lock);
	for (size_t i = 0; i < table_cap; i++) {
		table[i].thumbs &= (uint8_t)~bit;
	}
	bump_revision_locked();
	pthread_mutex_unlock(&state_lock);
}

static bool pack_write(int side, int32_t slot, int64_t id, const uint16_t *pixels) {
	bool restarted = false;
	bool ok = false;
	pthread_mutex_lock(&io_lock);
	int fd = pack_open_write(side, &restarted);
	if (fd >= 0) {
		off_t at = slot_offset(side, slot);
		slot_header_t header = {.id = id,
								.sum = pixels_sum(pixels, (size_t)side * side),
								.side = (uint16_t)side,
								.magic = SLOT_MAGIC};
		slot_header_t blank = {0};
		size_t bytes = COVERTONE_THUMB_BYTES(side);
		// Wiped, filled, then named: a write cut short is a miss, never a
		// picture of half one album and half another.
		ok = pwrite(fd, &blank, sizeof(blank), at) == (ssize_t)sizeof(blank) &&
			 pwrite(fd, pixels, bytes, at + SLOT_HEADER) == (ssize_t)bytes &&
			 pwrite(fd, &header, sizeof(header), at) == (ssize_t)sizeof(header);
		close(fd);
	}
	pthread_mutex_unlock(&io_lock);
	if (restarted) {
		pack_forget(side);
	}
	return ok;
}

static bool pack_read(int side, int32_t slot, int64_t id, uint16_t *pixels) {
	if (slot < 0) {
		return false;
	}
	bool ok = false;
	char path[600];
	pthread_mutex_lock(&io_lock);
	int fd = pack_path(side, path, sizeof(path)) ? open(path, O_RDONLY) : -1;
	if (fd >= 0) {
		uint8_t want[PACK_HEADER], have[PACK_HEADER];
		pack_header_fill(want, side);
		off_t at = slot_offset(side, slot);
		slot_header_t header;
		size_t bytes = COVERTONE_THUMB_BYTES(side);
		ok = pread(fd, have, sizeof(have), 0) == (ssize_t)sizeof(have) && memcmp(want, have, sizeof(want)) == 0 &&
			 pread(fd, &header, sizeof(header), at) == (ssize_t)sizeof(header) && header.id == id &&
			 header.side == side && header.magic == SLOT_MAGIC &&
			 pread(fd, pixels, bytes, at + SLOT_HEADER) == (ssize_t)bytes &&
			 header.sum == pixels_sum(pixels, (size_t)side * side);
		close(fd);
	}
	pthread_mutex_unlock(&io_lock);
	return ok;
}

// A slot that changes hands with its album's id (the index filed again):
// only the header is rewritten.
static void pack_rekey(int side, int32_t slot, int64_t old_id, int64_t new_id) {
	char path[600];
	pthread_mutex_lock(&io_lock);
	int fd = pack_path(side, path, sizeof(path)) ? open(path, O_RDWR) : -1;
	if (fd >= 0) {
		off_t at = slot_offset(side, slot);
		slot_header_t header;
		if (pread(fd, &header, sizeof(header), at) == (ssize_t)sizeof(header) && header.id == old_id) {
			header.id = new_id;
			if (pwrite(fd, &header, sizeof(header), at) != (ssize_t)sizeof(header)) {
				fprintf(stderr, "covertone: could not move a thumbnail to its album's new id\n");
			}
		}
		close(fd);
	}
	pthread_mutex_unlock(&io_lock);
}

// Cuts a file back to its last slot in use.
static void pack_trim(int side, int32_t highest) {
	char path[600];
	pthread_mutex_lock(&io_lock);
	int fd = pack_path(side, path, sizeof(path)) ? open(path, O_RDWR) : -1;
	if (fd >= 0) {
		struct stat st;
		off_t want = slot_offset(side, highest + 1);
		if (fstat(fd, &st) == 0 && st.st_size > want && st.st_size >= PACK_HEADER) {
			if (ftruncate(fd, want) == 0) {
				printf("covertone: %s cut back to %d slot(s)\n", path, highest + 1);
			}
		}
		close(fd);
	}
	pthread_mutex_unlock(&io_lock);
}

// ---------------------------------------------------------------------------
// Making an album
// ---------------------------------------------------------------------------

#define WANT_TONE 1u
#define WANT_SMALL 2u
#define WANT_LARGE 4u

typedef struct {
	int looked;		// albums looked at
	int unchanged;	// kept as they were
	int made;		// with a picture decoded
	int without;	// no picture, a grey one, or one that would not decode
	int deferred;	// left for the worker
	int adopted;	// a row of the same picture under the album's old id
	uint32_t ms;	// in make_album
} stats_t;

typedef struct {
	uint8_t *rgb;
	int w, h;
	size_t peak;
	bool deferred; // a progressive JPEG on a pass that does not read them
} picture_t;

static size_t planes_budget(void) {
	size_t free_now = 0;
	FILE *f = fopen("/proc/meminfo", "r");
	if (f) {
		char line[128];
		size_t kb = 0;
		while (fgets(line, sizeof(line), f)) {
			if (sscanf(line, "MemFree: %zu kB", &kb) == 1) {
				free_now = kb * 1024;
				break;
			}
		}
		fclose(f);
	}
	size_t budget = free_now / 4;
	if (budget < PLANES_BUDGET_MIN) {
		budget = PLANES_BUDGET_MIN;
	}
	if (budget > PLANES_BUDGET_MAX) {
		budget = PLANES_BUDGET_MAX;
	}
	return budget;
}

// Decodes the picture small: TJpgDec's 1/2 .. 1/8 for a baseline JPEG (the
// cost is the Huffman pass over the file, the output is a few hundred
// kilobytes at most), the streaming PNG decoder for a PNG. A progressive JPEG
// needs the plane decoder and up to a few megabytes, so only `heavy` passes --
// the worker's -- read one; the scan's leaves it for the worker.
static bool picture_decode(const uint8_t *data, size_t size, int box, bool heavy, picture_t *out) {
	memset(out, 0, sizeof(*out));
	if (!data || size < 4) {
		return false;
	}
	if (data[0] == 0xFF && data[1] == 0xD8) {
		out->rgb = jpeg_scaled_decode(data, size, box, &out->w, &out->h, NULL, NULL);
		if (out->rgb) {
			out->peak = (size_t)out->w * out->h * 3 + 8192;
			return true;
		}
		if (!heavy) {
			out->deferred = true;
			return false;
		}
		size_t peak = 0;
		out->rgb = jpeg_planes_decode(data, size, box * 4, planes_budget(), &out->w, &out->h, &peak);
		out->peak = peak;
		return out->rgb != NULL;
	}
	if (data[0] == 0x89 && data[1] == 'P') {
		out->rgb = png_scaled_decode(data, size, box * 2, &out->w, &out->h);
		if (out->rgb) {
			// Two rows of the source and the inflate window besides the output;
			// the rows are not known here, a generous allowance is.
			out->peak = (size_t)out->w * out->h * 3 + 64 * 1024;
		}
		return out->rgb != NULL;
	}
	return false;
}

// The file of a track, without the "?track=" a sheet's tracks carry.
static void track_file(const char *track, char *out, size_t size) {
	const char *mark = strstr(track, "?track=");
	size_t len = mark ? (size_t)(mark - track) : strlen(track);
	if (len >= size) {
		len = size - 1;
	}
	memcpy(out, track, len);
	out[len] = '\0';
}

static int64_t dir_mtime_of(const char *track) {
	char dir[512];
	track_file(track, dir, sizeof(dir));
	char *slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
	}
	struct stat st;
	return stat(dir, &st) == 0 ? (int64_t)st.st_mtime : 0;
}

// Whether what is kept for `a` still describes its picture: the same track,
// unchanged, and the file the picture came from unchanged (a cover.jpg
// overwritten in place changes nothing else), and for a picture that is not
// the track's own, a folder that has not gained or lost a file -- a new
// cover.jpg beside a track that had none. A stat or two; nothing is read.
static bool kept_still_valid(const library_cover_album_t *a) {
	if (!a->kept || a->brief.version != COVERTONE_VERSION || a->brief.kind == LIBRARY_COVER_DEFERRED) {
		return false;
	}
	if (strcmp(a->kept_track, a->track) != 0 || a->kept_track_mtime != a->track_mtime) {
		return false;
	}
	char file[512];
	track_file(a->track, file, sizeof(file));
	bool own = strcmp(a->kept_src, file) == 0;
	if (a->kept_src[0]) {
		struct stat st;
		if (stat(a->kept_src, &st) != 0 || (int64_t)st.st_mtime != a->kept_src_mtime ||
			(int64_t)st.st_size != a->kept_src_size) {
			return false;
		}
	}
	return own || dir_mtime_of(a->track) == a->kept_dir_mtime;
}

static bool has_picture(uint8_t kind) { return kind == LIBRARY_COVER_PICTURE || kind == LIBRARY_COVER_GREY; }

// A row for the album under its old id, made from the same track: the index
// was filed again and the album's key changed. The row and its thumbnails
// take the new id; nothing is decoded.
static bool adopt_old_row(const library_cover_album_t *a, stats_t *stats) {
	int64_t old = library_cover_find_by_track(a->track, a->track_mtime, COVERTONE_VERSION);
	if (!old || old == a->id) {
		return false;
	}
	pthread_mutex_lock(&state_lock);
	entry_t *e = table_find_locked(old);
	entry_t was = e ? *e : (entry_t){0};
	pthread_mutex_unlock(&state_lock);
	if (!e || !library_cover_rekey(old, a->id, a->album, a->album_key)) {
		return false;
	}
	if (was.slot >= 0) {
		pack_rekey(COVERTONE_THUMB_SMALL, was.slot, old, a->id);
		pack_rekey(COVERTONE_THUMB_LARGE, was.slot, old, a->id);
	}
	library_cover_brief_t row = {.id = a->id,
								 .hue = was.hue,
								 .chroma = was.chroma,
								 .light = was.light,
								 .kind = was.kind,
								 .thumbs = was.thumbs,
								 .slot = was.slot,
								 .version = COVERTONE_VERSION};
	pthread_mutex_lock(&state_lock);
	table_remove_locked(old);
	table_put_locked(&row);
	bump_revision_locked();
	pthread_mutex_unlock(&state_lock);
	stats->adopted++;
	return true;
}

static uint8_t to_byte(float v, float scale) {
	float x = v * scale + 0.5f;
	return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
}

// Makes what `want` asks of one album and writes it down: its tone (from the
// picture, or the title's hash), and its thumbnails. With job_lock held.
// False when the row could not be written -- the database is not taking
// writes, and going on would make the same album over and over.
static bool make_album(const library_cover_album_t *a, unsigned want, bool heavy, stats_t *stats) {
	uint32_t started = mono_ms();
	stats->looked++;

	// The worker may have been told to let go while it waited for the lock.
	pthread_mutex_lock(&state_lock);
	bool live = attached;
	entry_t kept = {0};
	entry_t *e = table_find_locked(a->id);
	if (e) {
		kept = *e;
	}
	pthread_mutex_unlock(&state_lock);
	if (!live) {
		return true;
	}
	// Thumbnails alone keep the tone they have; with no tone in memory to
	// keep, it is made again with them.
	if (!kept.id) {
		want |= WANT_TONE;
	}

	if ((want & WANT_TONE) && !a->kept && adopt_old_row(a, stats)) {
		pthread_mutex_lock(&state_lock);
		e = table_find_locked(a->id);
		kept = e ? *e : kept;
		pthread_mutex_unlock(&state_lock);
		want = 0;
		if (has_picture(kept.kind)) {
			want |= (kept.thumbs & LIBRARY_COVER_THUMB_SMALL) ? 0 : WANT_SMALL;
			want |= (kept.thumbs & LIBRARY_COVER_THUMB_LARGE) ? 0 : WANT_LARGE;
		}
		if (!want) {
			stats->ms += mono_ms() - started;
			return true;
		}
	}

	// The picture: embedded first, then the cover file beside the track, as
	// the lists and the player look for it, until one decodes.
	char src[512] = "";
	int64_t src_mtime = 0, src_size = 0;
	bool had_bytes = false, deferred = false;
	picture_t pic = {0};
	int box = (want & WANT_LARGE) ? BOX_LARGE : BOX_SMALL;
	for (int i = 0; i < ALBUMART_CANDIDATES && !pic.rgb; i++) {
		char path[512];
		albumart_t art;
		bool loaded;
		if (i == 0) {
			track_file(a->track, path, sizeof(path));
			loaded = albumart_load_candidate(a->track, 0, &art);
		} else {
			loaded = albumart_cover_file(a->track, path, sizeof(path)) && albumart_load_candidate(path, 0, &art);
		}
		if (!loaded) {
			continue;
		}
		had_bytes = true;
		crumb_set_artwork(path);
		bool ok = picture_decode(art.data, art.size, box, heavy, &pic) && !albumart_faulted(&art);
		crumb_set_artwork(NULL);
		albumart_free(&art);
		if (!ok) {
			deferred |= pic.deferred;
			free(pic.rgb);
			memset(&pic, 0, sizeof(pic));
			continue;
		}
		snprintf(src, sizeof(src), "%s", path);
		struct stat st;
		if (stat(path, &st) == 0) {
			src_mtime = (int64_t)st.st_mtime;
			src_size = (int64_t)st.st_size;
		}
	}

	char title[256];
	library_album_title(a->album, title, sizeof(title));
	library_cover_brief_t row = {.id = a->id, .slot = kept.id ? kept.slot : -1, .version = COVERTONE_VERSION};
	row.thumbs = kept.id ? kept.thumbs : 0;

	if (pic.rgb) {
		size_t peak = pic.peak;
		if (want & WANT_TONE) {
			albumtone_t tone;
			albumtone_extract(pic.rgb, pic.w, pic.h, &tone);
			if (tone.coloured) {
				row.kind = LIBRARY_COVER_PICTURE;
				row.hue = (uint16_t)((int)(tone.hue + 0.5f) % 360);
			} else {
				row.kind = LIBRARY_COVER_GREY;
				row.hue = albumtone_hash_hue(title);
			}
			row.chroma = to_byte(tone.chroma, CHROMA_SCALE);
			row.light = to_byte(tone.light, LIGHT_SCALE);
		} else {
			row.kind = kept.kind;
			row.hue = kept.hue;
			row.chroma = kept.chroma;
			row.light = kept.light;
		}

		// A slot of its own, for both sizes.
		if (row.slot < 0) {
			pthread_mutex_lock(&state_lock);
			row.slot = slot_take_locked();
			pthread_mutex_unlock(&state_lock);
		}
		unsigned sizes = want & (WANT_SMALL | WANT_LARGE);
		if (want & WANT_TONE) {
			// A new picture: whatever the slot held is of the old one.
			row.thumbs = 0;
			sizes |= WANT_SMALL;
		}
		if (row.slot >= 0) {
			size_t bytes = COVERTONE_THUMB_BYTES(COVERTONE_THUMB_LARGE);
			uint16_t *px = big_alloc(bytes);
			if (px) {
				if ((sizes & WANT_SMALL) &&
					albumtone_square565(pic.rgb, pic.w, pic.h, COVERTONE_THUMB_SMALL, px) &&
					pack_write(COVERTONE_THUMB_SMALL, row.slot, a->id, px)) {
					row.thumbs |= LIBRARY_COVER_THUMB_SMALL;
				}
				if ((sizes & WANT_LARGE) &&
					albumtone_square565(pic.rgb, pic.w, pic.h, COVERTONE_THUMB_LARGE, px) &&
					pack_write(COVERTONE_THUMB_LARGE, row.slot, a->id, px)) {
					row.thumbs |= LIBRARY_COVER_THUMB_LARGE;
				}
				big_free(px, bytes);
				peak += bytes;
			}
		}
		free(pic.rgb);
		if (peak > peak_bytes) {
			peak_bytes = peak;
		}
		stats->made++;
	} else {
		// No picture to paint with: the title's hash, and nothing in a slot.
		row.kind = deferred ? LIBRARY_COVER_DEFERRED : had_bytes ? LIBRARY_COVER_FAILED : LIBRARY_COVER_NONE;
		row.hue = albumtone_hash_hue(title);
		row.chroma = 0;
		row.light = 0;
		row.thumbs = 0;
		row.slot = -1;
		if (deferred) {
			stats->deferred++;
		} else {
			stats->without++;
		}
	}

	bool stored = library_cover_store(a, &row, src, src_mtime, src_size, dir_mtime_of(a->track));
	if (stored) {
		pthread_mutex_lock(&state_lock);
		table_put_locked(&row);
		bump_revision_locked();
		pthread_mutex_unlock(&state_lock);
	} else {
		fprintf(stderr, "covertone: the index would not take the colour of '%s'\n", title);
		// The slot is not the row's after all.
		if (row.slot >= 0 && (!kept.id || kept.slot != row.slot)) {
			pthread_mutex_lock(&state_lock);
			slot_mark_locked(row.slot, false);
			pthread_mutex_unlock(&state_lock);
		}
	}
	uint32_t took = mono_ms() - started;
	stats->ms += took;
	if (took > 1500) {
		printf("covertone: '%s' took %u ms\n", title, took);
	}
	return stored;
}

// What `a` still lacks: everything when its row is stale, otherwise the
// thumbnails a picture has and the slot does not.
static unsigned wanted_for(const library_cover_album_t *a, bool check_files) {
	bool valid = check_files ? kept_still_valid(a)
							 : (a->kept && a->brief.version == COVERTONE_VERSION &&
								a->brief.kind != LIBRARY_COVER_DEFERRED && strcmp(a->kept_track, a->track) == 0 &&
								a->kept_track_mtime == a->track_mtime);
	if (!valid) {
		return WANT_TONE | WANT_SMALL | WANT_LARGE;
	}
	if (!has_picture(a->brief.kind)) {
		return 0;
	}
	unsigned want = 0;
	want |= (a->brief.thumbs & LIBRARY_COVER_THUMB_SMALL) ? 0 : WANT_SMALL;
	want |= (a->brief.thumbs & LIBRARY_COVER_THUMB_LARGE) ? 0 : WANT_LARGE;
	return want;
}

static void wake_worker(void) {
	pthread_mutex_lock(&state_lock);
	fill_wanted = true;
	fill_after = 0;
	fill_phase = 0;
	fill_sweep_made = 0;
	pthread_cond_signal(&wakeup);
	pthread_mutex_unlock(&state_lock);
}

// ---------------------------------------------------------------------------
// The scan's pass
// ---------------------------------------------------------------------------

#define PASS_BATCH 16

void covertone_scan_pass(int id_base, uint32_t budget_ms, volatile bool *cancel, void (*progress)(const char *folder)) {
	pthread_mutex_lock(&state_lock);
	bool live = attached;
	pthread_mutex_unlock(&state_lock);
	if (!live) {
		return;
	}
	library_cover_album_t *batch = malloc(PASS_BATCH * sizeof(*batch));
	if (!batch) {
		return;
	}
	uint32_t started = mono_ms();
	stats_t stats = {0};
	bool out_of_time = false;
	int64_t after = 0;
	// A full scan checks every album against what is kept, with a stat or two
	// each; Detect changes asks the index only for the albums it read in.
	library_covers_select_t which = id_base > 0 ? LIBRARY_COVERS_STALE : LIBRARY_COVERS_ALL;
	while (!out_of_time && !(cancel && *cancel)) {
		int n = library_covers_albums(which, COVERTONE_VERSION, after, id_base, batch, PASS_BATCH);
		if (n <= 0) {
			break;
		}
		for (int i = 0; i < n; i++) {
			const library_cover_album_t *a = &batch[i];
			after = a->group_rowid;
			if (cancel && *cancel) {
				break;
			}
			if (mono_ms() - started >= budget_ms) {
				out_of_time = true;
				break;
			}
			// The scan makes the colour and the small picture; the large one
			// is the worker's, which decodes at four times the area.
			unsigned want = wanted_for(a, true) & (WANT_TONE | WANT_SMALL);
			if (!want) {
				stats.unchanged++;
				continue;
			}
			if (progress) {
				char folder[512];
				track_file(a->track, folder, sizeof(folder));
				char *slash = strrchr(folder, '/');
				if (slash) {
					*slash = '\0';
				}
				progress(folder);
			}
			pthread_mutex_lock(&job_lock);
			bool ok = make_album(a, want, false, &stats);
			pthread_mutex_unlock(&job_lock);
			if (!ok) {
				out_of_time = true; // the index is not taking writes: leave it
				break;
			}
		}
	}
	free(batch);
	malloc_trim(0);
	printf("covertone: scan pass in %u ms (budget %u): %d album(s) made from their picture, %d without one, "
		   "%d unchanged, %d moved to a new key, %d left for the worker%s; %u ms making them, peak %zu KB\n",
		   mono_ms() - started, budget_ms, stats.made, stats.without, stats.unchanged, stats.adopted, stats.deferred,
		   out_of_time ? ", out of time: the rest too" : "", stats.ms, peak_bytes / 1024);
	wake_worker();
}

// ---------------------------------------------------------------------------
// Pausing
// ---------------------------------------------------------------------------

static bool paused_now(void) {
	uint32_t now = mono_ms();
	pthread_mutex_lock(&state_lock);
	bool paused = pause_count > 0 && now - pause_since_ms < PAUSE_LAPSE_MS;
	pthread_mutex_unlock(&state_lock);
	if (paused) {
		return true;
	}
	if ((int32_t)(ui_busy_until_ms - now) > 0) {
		return true;
	}
	long quiet = audio_quiet_ms();
	if (quiet >= 0 && quiet < AUDIO_QUIET_MS) {
		return true;
	}
	if (quiet < 0) {
		return true; // a track is starting right now
	}
	return library_scan_running();
}

void covertone_pause(bool paused) {
	pthread_mutex_lock(&state_lock);
	if (paused) {
		if (pause_count++ == 0) {
			pause_since_ms = mono_ms();
		}
	} else if (pause_count > 0) {
		pause_count--;
	}
	if (!pause_count) {
		pthread_cond_signal(&wakeup);
	}
	pthread_mutex_unlock(&state_lock);
}

// The UI thread's watcher: a finger on the screen, a list still moving under
// its own momentum, or a screen sliding in all mean the card and the
// processor are better left alone for a moment.
static void ui_watch_cb(lv_timer_t *timer) {
	(void)timer;
	bool busy = false;
	for (lv_indev_t *indev = lv_indev_get_next(NULL); indev && !busy; indev = lv_indev_get_next(indev)) {
		if (lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) {
			continue;
		}
		busy = lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED || lv_indev_get_scroll_obj(indev) != NULL;
	}
	lv_display_t *display = lv_display_get_default();
	if (!busy && display && lv_display_get_screen_prev(display)) {
		busy = true;
	}
	if (busy) {
		ui_busy_until_ms = mono_ms() + UI_BUSY_HOLD_MS;
	}
}

void covertone_ui_init(void) {
	ui_thread = pthread_self();
	ui_thread_known = true;
	lv_timer_create(ui_watch_cb, 100, NULL);
}

static bool on_ui_thread(void) { return ui_thread_known && pthread_equal(pthread_self(), ui_thread); }

// ---------------------------------------------------------------------------
// Telling the interface
// ---------------------------------------------------------------------------

static void notify_cb(void *user) {
	(void)user;
	pthread_mutex_lock(&state_lock);
	notify_posted = false;
	notify_pending = false;
	notify_last_ms = mono_ms();
	void (*cb)(void) = listener;
	pthread_mutex_unlock(&state_lock);
	if (cb) {
		cb();
	}
}

// From the worker: at most every NOTIFY_EVERY_MS. With state_lock held.
static void notify_flush_locked(void) {
	if (!notify_pending || notify_posted) {
		return;
	}
	if (!listener) {
		notify_pending = false;
		return;
	}
	if (mono_ms() - notify_last_ms < NOTIFY_EVERY_MS) {
		return;
	}
	notify_posted = gui_post(notify_cb, NULL);
}

void covertone_set_listener(void (*changed)(void)) {
	pthread_mutex_lock(&state_lock);
	listener = changed;
	pthread_mutex_unlock(&state_lock);
}

uint32_t covertone_revision(void) { return revision; }

// ---------------------------------------------------------------------------
// Answers
// ---------------------------------------------------------------------------

bool covertone_for_album(int64_t album_id, float *hue_deg, float *chroma, float *lightness) {
	pthread_mutex_lock(&state_lock);
	entry_t *e = table_find_locked(album_id);
	entry_t copy = e ? *e : (entry_t){0};
	pthread_mutex_unlock(&state_lock);
	if (!copy.id) {
		return false;
	}
	if (hue_deg) {
		*hue_deg = (float)copy.hue;
	}
	if (chroma) {
		*chroma = (float)copy.chroma / CHROMA_SCALE;
	}
	if (lightness) {
		*lightness = (float)copy.light / LIGHT_SCALE;
	}
	return true;
}

static uint64_t path_hash(const char *path) {
	uint64_t h = 1469598103934665603ULL;
	for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
		h ^= *p;
		h *= 1099511628211ULL;
	}
	return h;
}

bool covertone_for_path(const char *track_path, float *hue_deg, float *chroma, float *lightness) {
	if (!track_path || !track_path[0]) {
		return false;
	}
	// Which album: remembered for the last few tracks while the index has not
	// changed, asked of the index otherwise.
	uint64_t h = path_hash(track_path);
	unsigned rev = library_revision(LIBRARY_LIST_TRACKS);
	int64_t id = -1;
	pthread_mutex_lock(&state_lock);
	for (int i = 0; i < MEMO_SIZE; i++) {
		if (memo[i].used && memo[i].path_hash == h && memo[i].rev == rev) {
			id = memo[i].id;
			break;
		}
	}
	pthread_mutex_unlock(&state_lock);
	if (id < 0) {
		char value[512];
		id = library_track_album_value(track_path, value, sizeof(value)) ? covertone_album_id(value) : 0;
		pthread_mutex_lock(&state_lock);
		memo[memo_next] = (memo_t){.path_hash = h, .id = id, .rev = rev, .used = true};
		memo_next = (memo_next + 1) % MEMO_SIZE;
		pthread_mutex_unlock(&state_lock);
	}
	return id > 0 && covertone_for_album(id, hue_deg, chroma, lightness);
}

bool covertone_tone_provider(const char *track_path, float *hue_deg, float *chroma) {
	return covertone_for_path(track_path, hue_deg, chroma, NULL);
}

static void dsc_fill(lv_image_dsc_t *dsc, int size, const void *pixels) {
	memset(dsc, 0, sizeof(*dsc));
	dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
	dsc->header.cf = LV_COLOR_FORMAT_RGB565;
	dsc->header.w = (uint32_t)size;
	dsc->header.h = (uint32_t)size;
	dsc->header.stride = (uint32_t)size * 2;
	dsc->data_size = (uint32_t)COVERTONE_THUMB_BYTES(size);
	dsc->data = pixels;
}

static int thumb_bit(int size) { return size == COVERTONE_THUMB_SMALL ? LIBRARY_COVER_THUMB_SMALL : LIBRARY_COVER_THUMB_LARGE; }

// The slot of an album whose thumbnail of `size` exists, or -1.
static int32_t thumb_slot(int64_t id, int size) {
	pthread_mutex_lock(&state_lock);
	entry_t *e = table_find_locked(id);
	int32_t slot = e && (e->thumbs & thumb_bit(size)) ? e->slot : -1;
	pthread_mutex_unlock(&state_lock);
	return slot;
}

// A thumbnail the row promised and the file did not have: forgotten, and
// made again.
static void thumb_lost(int64_t id, int size) {
	pthread_mutex_lock(&state_lock);
	entry_t *e = table_find_locked(id);
	if (e) {
		e->thumbs &= (uint8_t)~thumb_bit(size);
	}
	pthread_mutex_unlock(&state_lock);
	covertone_request(id);
}

bool covertone_thumb(int64_t album_id, int size, lv_image_dsc_t *dsc, void *buf, size_t buf_size) {
	if (!dsc || !buf || (size != COVERTONE_THUMB_SMALL && size != COVERTONE_THUMB_LARGE) ||
		buf_size < COVERTONE_THUMB_BYTES(size)) {
		return false;
	}
	if (size == COVERTONE_THUMB_LARGE && on_ui_thread()) {
		static bool said;
		if (!said) {
			said = true;
			fprintf(stderr, "covertone: the 240 px thumbnail was asked for on the UI thread; use covertone_thumb_async\n");
		}
		return false;
	}
	int32_t slot = thumb_slot(album_id, size);
	if (slot < 0) {
		return false;
	}
	if (!pack_read(size, slot, album_id, buf)) {
		thumb_lost(album_id, size);
		return false;
	}
	dsc_fill(dsc, size, buf);
	return true;
}

void covertone_thumb_free(lv_image_dsc_t *dsc) {
	if (!dsc || !dsc->data) {
		return;
	}
	big_free((void *)dsc->data, COVERTONE_THUMB_BYTES(dsc->header.w));
	dsc->data = NULL;
}

// ---------------------------------------------------------------------------
// The asynchronous thumbnails
// ---------------------------------------------------------------------------

static ticket_t *ticket_find_locked(uint32_t ticket) {
	for (int i = 0; i < TICKET_MAX; i++) {
		if (tickets[i].state != TICKET_FREE && tickets[i].ticket == ticket) {
			return &tickets[i];
		}
	}
	return NULL;
}

static void deliver_cb(void *user) {
	uint32_t number = (uint32_t)(uintptr_t)user;
	pthread_mutex_lock(&state_lock);
	ticket_t *t = ticket_find_locked(number);
	if (!t || t->state != TICKET_READY) {
		pthread_mutex_unlock(&state_lock);
		return;
	}
	ticket_t copy = *t;
	memset(t, 0, sizeof(*t));
	pthread_mutex_unlock(&state_lock);

	if (copy.cancelled || !copy.cb) {
		if (copy.pixels) {
			big_free(copy.pixels, COVERTONE_THUMB_BYTES(copy.size));
		}
		return;
	}
	if (copy.pixels) {
		lv_image_dsc_t dsc;
		dsc_fill(&dsc, copy.size, copy.pixels);
		copy.cb(copy.id, copy.size, &dsc, copy.user);
	} else {
		copy.cb(copy.id, copy.size, NULL, copy.user);
	}
}

// Hands every finished ticket that is not on its way yet to the UI thread.
// With state_lock held.
static void tickets_post_locked(void) {
	for (int i = 0; i < TICKET_MAX; i++) {
		ticket_t *t = &tickets[i];
		if (t->state == TICKET_READY && !t->posted) {
			t->posted = gui_post(deliver_cb, (void *)(uintptr_t)t->ticket);
		}
	}
}

uint32_t covertone_thumb_async(int64_t album_id, int size, covertone_thumb_cb cb, void *user) {
	if (!cb || (size != COVERTONE_THUMB_SMALL && size != COVERTONE_THUMB_LARGE)) {
		return 0;
	}
	pthread_mutex_lock(&state_lock);
	ticket_t *t = NULL;
	for (int i = 0; i < TICKET_MAX && !t; i++) {
		if (tickets[i].state == TICKET_FREE) {
			t = &tickets[i];
		}
	}
	if (!t) {
		// Full: the oldest one still waiting makes room, and is told so.
		ticket_t *oldest = NULL;
		for (int i = 0; i < TICKET_MAX; i++) {
			if (tickets[i].state == TICKET_WAITING && (!oldest || tickets[i].ticket < oldest->ticket)) {
				oldest = &tickets[i];
			}
		}
		if (!oldest) {
			pthread_mutex_unlock(&state_lock);
			return 0;
		}
		oldest->state = TICKET_READY;
		oldest->pixels = NULL;
		oldest->posted = false;
		tickets_post_locked();
		for (int i = 0; i < TICKET_MAX && !t; i++) {
			if (tickets[i].state == TICKET_FREE) {
				t = &tickets[i];
			}
		}
		if (!t) {
			pthread_mutex_unlock(&state_lock);
			return 0;
		}
	}
	uint32_t number = ticket_next++;
	if (!ticket_next) {
		ticket_next = 1;
	}
	*t = (ticket_t){.ticket = number, .id = album_id, .size = size, .cb = cb, .user = user, .state = TICKET_WAITING};
	pthread_cond_signal(&wakeup);
	pthread_mutex_unlock(&state_lock);
	return number;
}

void covertone_thumb_cancel(uint32_t ticket) {
	if (!ticket) {
		return;
	}
	pthread_mutex_lock(&state_lock);
	ticket_t *t = ticket_find_locked(ticket);
	if (t) {
		if (t->state == TICKET_WAITING) {
			memset(t, 0, sizeof(*t));
		} else {
			t->cancelled = true;
		}
	}
	pthread_mutex_unlock(&state_lock);
}

void covertone_request(int64_t album_id) {
	if (album_id <= 0) {
		return;
	}
	pthread_mutex_lock(&state_lock);
	// To the front, once.
	int at = request_count < REQUEST_MAX ? request_count : REQUEST_MAX - 1;
	for (int i = 0; i < request_count; i++) {
		if (requests[i] == album_id) {
			at = i;
			break;
		}
	}
	memmove(&requests[1], &requests[0], (size_t)at * sizeof(requests[0]));
	requests[0] = album_id;
	if (at == request_count && request_count < REQUEST_MAX) {
		request_count++;
	}
	pthread_cond_signal(&wakeup);
	pthread_mutex_unlock(&state_lock);
}

// ---------------------------------------------------------------------------
// The worker
// ---------------------------------------------------------------------------

typedef enum {
	JOB_NONE,
	JOB_TICKET,
	JOB_PRUNE,
	JOB_REQUEST,
	JOB_FILL,
} job_t;

// One album by id, whatever it lacks. For a request and a ticket.
static void make_by_id(int64_t id, unsigned extra, stats_t *stats) {
	library_cover_album_t *a = malloc(sizeof(*a));
	if (!a) {
		return;
	}
	if (library_covers_album(id, COVERTONE_VERSION, a)) {
		unsigned want = wanted_for(a, false) | extra;
		if (want) {
			pthread_mutex_lock(&job_lock);
			make_album(a, want, true, stats);
			pthread_mutex_unlock(&job_lock);
		}
	}
	free(a);
}

static void run_ticket(ticket_t *t_in) {
	int64_t id = t_in->id;
	int size = t_in->size;
	uint32_t number = t_in->ticket;
	size_t bytes = COVERTONE_THUMB_BYTES(size);
	uint16_t *px = big_alloc(bytes);
	bool ok = false;
	if (px) {
		int32_t slot = thumb_slot(id, size);
		ok = slot >= 0 && pack_read(size, slot, id, px);
		if (!ok) {
			// Not made yet, or lost: made now, then read.
			stats_t stats = {0};
			make_by_id(id, size == COVERTONE_THUMB_LARGE ? WANT_LARGE : WANT_SMALL, &stats);
			slot = thumb_slot(id, size);
			ok = slot >= 0 && pack_read(size, slot, id, px);
		}
		if (!ok) {
			big_free(px, bytes);
			px = NULL;
		}
	}
	pthread_mutex_lock(&state_lock);
	ticket_t *t = ticket_find_locked(number);
	if (t) {
		t->state = TICKET_READY;
		t->pixels = px;
		t->posted = false;
		tickets_post_locked();
	} else if (px) {
		big_free(px, bytes);
	}
	pthread_mutex_unlock(&state_lock);
}

static void run_prune(void) {
	int gone = library_covers_prune();
	if (gone > 0) {
		printf("covertone: %d album(s) gone from the index, their colours with them\n", gone);
		table_reload();
	}
	pthread_mutex_lock(&state_lock);
	int32_t highest = slot_highest_locked();
	pthread_mutex_unlock(&state_lock);
	pthread_mutex_lock(&job_lock);
	pack_trim(COVERTONE_THUMB_SMALL, highest);
	pack_trim(COVERTONE_THUMB_LARGE, highest);
	pthread_mutex_unlock(&job_lock);
}

#define FILL_BATCH 4

// A few albums of the background walk: first every album whose colour is
// missing or stale, then every picture whose thumbnails are. Returns when the
// batch is done or something more urgent came in.
static void run_fill(stats_t *stats) {
	library_cover_album_t *batch = malloc(FILL_BATCH * sizeof(*batch));
	if (!batch) {
		return;
	}
	pthread_mutex_lock(&state_lock);
	int64_t after = fill_after;
	int phase = fill_phase;
	pthread_mutex_unlock(&state_lock);

	library_covers_select_t which = phase == 0 ? LIBRARY_COVERS_STALE : LIBRARY_COVERS_THUMBS;
	int n = library_covers_albums(which, COVERTONE_VERSION, after, 0, batch, FILL_BATCH);
	bool broken = false;
	int made = 0;
	for (int i = 0; i < n; i++) {
		const library_cover_album_t *a = &batch[i];
		unsigned want = wanted_for(a, false);
		if (want) {
			pthread_mutex_lock(&job_lock);
			broken = !make_album(a, want, true, stats);
			pthread_mutex_unlock(&job_lock);
			made++;
		}
		after = a->group_rowid;
		if (broken) {
			break;
		}
		pthread_mutex_lock(&state_lock);
		bool urgent = request_count > 0 || prune_wanted || !attached;
		for (int k = 0; k < TICKET_MAX && !urgent; k++) {
			urgent = tickets[k].state == TICKET_WAITING;
		}
		pthread_mutex_unlock(&state_lock);
		if (urgent || paused_now()) {
			break;
		}
	}
	free(batch);

	bool complete = false;
	pthread_mutex_lock(&state_lock);
	fill_after = after;
	fill_sweep_made += made;
	if (broken) {
		fill_wanted = false;
	} else if (n == 0) {
		// The end of this phase's walk. A walk of the stale albums that made
		// nothing at all means every album has its colour.
		if (phase == 0) {
			complete = fill_sweep_made == 0;
			if (!complete) {
				fill_after = 0; // once more from the top: the rows just made are not stale
			} else {
				fill_phase = 1;
				fill_after = 0;
			}
		} else {
			fill_wanted = false;
			fill_phase = 0;
			fill_after = 0;
		}
		fill_sweep_made = 0;
	}
	pthread_mutex_unlock(&state_lock);
	if (complete) {
		library_covers_complete();
	}
}

static void *worker_main(void *arg) {
	(void)arg;
	thread_be_background("covertone");
	stats_t stats = {0};
	bool was_filling = false;
	uint32_t fill_started = 0;

	for (;;) {
		pthread_mutex_lock(&state_lock);
		tickets_post_locked();
		notify_flush_locked();

		job_t job = JOB_NONE;
		ticket_t ticket = {0};
		int64_t request = 0;
		if (attached) {
			for (int i = 0; i < TICKET_MAX && job == JOB_NONE; i++) {
				if (tickets[i].state == TICKET_WAITING) {
					tickets[i].state = TICKET_RUNNING;
					ticket = tickets[i];
					job = JOB_TICKET;
				}
			}
		}
		bool work = attached && (prune_wanted || request_count > 0 || fill_wanted);
		bool hold = false;
		if (job == JOB_NONE && work) {
			pthread_mutex_unlock(&state_lock);
			hold = paused_now();
			pthread_mutex_lock(&state_lock);
			if (!hold && attached) {
				// Pruning waits for the walk of the stale albums: an album whose
				// key changed finds its old row there and takes it over
				// (adopt_old_row), and the prune would delete that row first.
				if (prune_wanted && (!fill_wanted || fill_phase != 0)) {
					prune_wanted = false;
					job = JOB_PRUNE;
				} else if (request_count > 0) {
					request = requests[0];
					memmove(&requests[0], &requests[1], (size_t)(request_count - 1) * sizeof(requests[0]));
					request_count--;
					job = JOB_REQUEST;
				} else if (fill_wanted) {
					job = JOB_FILL;
				}
			}
		}

		if (job == JOB_NONE) {
			if (was_filling && !fill_wanted) {
				was_filling = false;
				if (stats.looked) {
					printf("covertone: background done in %u s: %d album(s) made from their picture, %d without one, "
					   "%d moved to a new key; %u ms making them, peak %zu KB\n",
					   (mono_ms() - fill_started) / 1000, stats.made, stats.without, stats.adopted, stats.ms,
					   peak_bytes / 1024);
				}
				memset(&stats, 0, sizeof(stats));
				pthread_mutex_unlock(&state_lock);
				malloc_trim(0);
				continue;
			}
			// Paused with work waiting, or a notice still to send: look again
			// shortly. Otherwise sleep until asked.
			if (hold || notify_pending) {
				struct timespec deadline;
				deadline_in_ms(&deadline, hold ? 250 : NOTIFY_EVERY_MS);
				pthread_cond_timedwait(&wakeup, &state_lock, &deadline);
			} else {
				pthread_cond_wait(&wakeup, &state_lock);
			}
			pthread_mutex_unlock(&state_lock);
			continue;
		}
		pthread_mutex_unlock(&state_lock);

		switch (job) {
		case JOB_TICKET:
			// Somebody is looking at the screen waiting for it.
			thread_set_background_level(power_screen_is_on());
			run_ticket(&ticket);
			break;
		case JOB_PRUNE:
			run_prune();
			break;
		case JOB_REQUEST:
			thread_set_background_level(power_screen_is_on());
			make_by_id(request, 0, &stats);
			break;
		case JOB_FILL:
			if (!was_filling) {
				was_filling = true;
				fill_started = mono_ms();
			}
			// Nobody is waiting for these: only what the interface leaves.
			thread_set_background_level(false);
			run_fill(&stats);
			break;
		case JOB_NONE:
			break;
		}
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// Attaching to an index
// ---------------------------------------------------------------------------

void covertone_attach(const char *db_path) {
	if (!db_path || !db_path[0]) {
		return;
	}
	char dir[512];
	snprintf(dir, sizeof(dir), "%s", db_path);
	char *slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
	} else {
		snprintf(dir, sizeof(dir), ".");
	}

	pthread_mutex_lock(&io_lock);
	// A path too long for the buffer is no thumbnails rather than thumbnails
	// written somewhere else.
	io_allowed = snprintf(covers_dir, sizeof(covers_dir), "%s/covers", dir) < (int)sizeof(covers_dir);
	pthread_mutex_unlock(&io_lock);

	table_reload();

	pthread_mutex_lock(&state_lock);
	attached = true;
	prune_wanted = true;
	fill_wanted = true;
	fill_after = 0;
	fill_phase = 0;
	fill_sweep_made = 0;
	bool start = !worker_started;
	worker_started = true;
	pthread_cond_signal(&wakeup);
	pthread_mutex_unlock(&state_lock);

	if (start) {
		pthread_t thread;
		if (pthread_create(&thread, NULL, worker_main, NULL) == 0) {
			pthread_detach(thread);
		} else {
			fprintf(stderr, "covertone: could not start the worker\n");
			pthread_mutex_lock(&state_lock);
			worker_started = false;
			pthread_mutex_unlock(&state_lock);
		}
	}
}

void covertone_detach(void) {
	pthread_mutex_lock(&state_lock);
	attached = false;
	fill_wanted = false;
	prune_wanted = false;
	request_count = 0;
	// Whoever waits for a picture hears that there will be none.
	for (int i = 0; i < TICKET_MAX; i++) {
		if (tickets[i].state == TICKET_WAITING) {
			tickets[i].state = TICKET_READY;
			tickets[i].pixels = NULL;
			tickets[i].posted = false;
		}
	}
	tickets_post_locked();
	pthread_cond_signal(&wakeup);
	pthread_mutex_unlock(&state_lock);

	// The album in hand is finished, and no file of ours stays open.
	pthread_mutex_lock(&job_lock);
	pthread_mutex_lock(&io_lock);
	io_allowed = false;
	covers_dir[0] = '\0';
	pthread_mutex_unlock(&io_lock);
	pthread_mutex_unlock(&job_lock);

	pthread_mutex_lock(&state_lock);
	table_clear_locked();
	bump_revision_locked();
	pthread_mutex_unlock(&state_lock);
}

void covertone_library_changed(void) {
	pthread_mutex_lock(&state_lock);
	if (attached) {
		prune_wanted = true;
		fill_wanted = true;
		fill_after = 0;
		fill_phase = 0;
		fill_sweep_made = 0;
		memset(memo, 0, sizeof(memo));
		pthread_cond_signal(&wakeup);
	}
	pthread_mutex_unlock(&state_lock);
}

#include "geniusbuild.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/core/sha256.h"
#include "src/system/db/sqlite3.h"
#include "src/system/library/library.h"

#define STAGING ".genius-build.db"
#define HEADER_BYTES 80
#define IO_BYTES 16384

typedef struct {
	const char *path, *artist, *album, *genre;
	int64_t year;
	uint32_t artist_id, album_id, cluster;
} track_t;

typedef struct {
	const char *text;
	uint32_t id, group;
} order_t;

typedef struct {
	const atomic_bool *cancel;
	geniusbuild_progress_cb progress;
	void *user;
	geniusbuild_result_t result;
	size_t retained, text_bytes;
	track_t *tracks;
	char *text;
	uint32_t count, rows_seen;
} build_t;

static bool cancelled(const build_t *b) { return b->cancel && atomic_load(b->cancel); }
static int interrupt_sql(void *user) { return cancelled(user); }

static void report(build_t *b, geniusbuild_phase_t phase, int completed) {
	if (b->progress) {
		b->progress(phase, completed, (int)b->count, b->user);
	}
}

static void *allocate(build_t *b, size_t count, size_t width) {
	if (!count) count = 1;
	if (width && count > (GENIUSBUILD_MAX_RETAINED_BYTES - b->retained) / width) {
		b->result = GENIUSBUILD_LIMIT;
		return NULL;
	}
	void *p = calloc(count, width);
	if (!p) {
		b->result = GENIUSBUILD_NOMEM;
	} else {
		b->retained += count * width;
	}
	return p;
}

static void put32(unsigned char *p, uint32_t v) {
	for (unsigned i = 0; i < 4; i++) {
		p[i] = (unsigned char)(v >> (8 * i));
	}
}

static void put64(unsigned char *p, uint64_t v) {
	put32(p, (uint32_t)v);
	put32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t get32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

// In-place heapsort: bounded O(n log n) work, no libc sort scratch allocation.
static void swap_rows(unsigned char *a, unsigned char *b, size_t width) {
	while (width--) { unsigned char byte = *a; *a++ = *b; *b++ = byte; }
}

static void sift(unsigned char *rows, size_t width, size_t root, size_t n, int (*compare)(const void *, const void *)) {
	while (root < n / 2) {
		size_t child = root * 2 + 1;
		if (child + 1 < n && compare(rows + child * width, rows + (child + 1) * width) < 0) child++;
		if (compare(rows + root * width, rows + child * width) >= 0) break;
		swap_rows(rows + root * width, rows + child * width, width);
		root = child;
	}
}

static bool sort_rows(build_t *b, void *rows, size_t n, size_t width, int (*compare)(const void *, const void *)) {
	unsigned char *p = rows;
	for (size_t root = n / 2; root > 0;) {
		if (cancelled(b)) return false;
		sift(p, width, --root, n, compare);
	}
	for (size_t end = n; end > 1;) {
		if (cancelled(b)) return false;
		swap_rows(p, p + --end * width, width);
		sift(p, width, 0, end, compare);
	}
	return true;
}

static bool utf8(const unsigned char *p, size_t bytes) {
	for (size_t i = 0; i < bytes;) {
		unsigned char c = p[i++];
		if (c && c < 0x80) {
			continue;
		}
		unsigned n = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
		if (!n || n > bytes - i || (c == 0xe0 && p[i] < 0xa0) || (c == 0xed && p[i] >= 0xa0) ||
			(c == 0xf0 && p[i] < 0x90) || (c == 0xf4 && p[i] >= 0x90)) {
			return false;
		}
		while (n--) {
			if ((p[i++] & 0xc0) != 0x80) {
				return false;
			}
		}
	}
	return true;
}

static const char *copy_text(build_t *b, const char *text, size_t bytes) {
	if (bytes > GENIUSBUILD_MAX_FIELD_BYTES || bytes + 1 > GENIUSBUILD_MAX_TEXT_BYTES - b->text_bytes) {
		b->result = GENIUSBUILD_LIMIT;
		return NULL;
	}
	if (!utf8((const unsigned char *)text, bytes)) {
		b->result = GENIUSBUILD_INVALID;
		return NULL;
	}
	char *out = b->text + b->text_bytes;
	memcpy(out, text, bytes);
	out[bytes] = 0;
	b->text_bytes += bytes + 1;
	return out;
}

static bool collect(const library_tags_t *tags, void *user) {
	build_t *b = user;
	if (cancelled(b)) {
		return false;
	}
	if (++b->rows_seen > GENIUSBUILD_MAX_TRACKS) {
		b->result = GENIUSBUILD_LIMIT;
		return false;
	}
	if (!tags->path_bytes) return true; // same exclusion as the reference SQL
	track_t *t = &b->tracks[b->count];
	t->path = copy_text(b, tags->path, tags->path_bytes);
	t->artist = copy_text(b, tags->artist, tags->artist_bytes);
	t->album = copy_text(b, tags->album, tags->album_bytes);
	t->genre = copy_text(b, tags->genre, tags->genre_bytes);
	t->year = tags->year;
	if (!t->path || !t->artist || !t->album || !t->genre) {
		return false;
	}
	b->count++;
	return true;
}

static int nocase(const char *a, const char *b) {
	for (;;) {
		unsigned char x = (unsigned char)*a++, y = (unsigned char)*b++;
		if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
		if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
		if (x != y || !x) return (int)x - (int)y;
	}
}

static int path_compare(const void *a, const void *b) { return nocase(((const track_t *)a)->path, ((const track_t *)b)->path); }

static int order_compare(const void *a, const void *b) {
	const order_t *x = a, *y = b;
	return x->group != y->group ? (x->group > y->group ? 1 : -1) : strcmp(x->text, y->text);
}

// UTF-8 byte order equals Unicode scalar order for valid strings. Unlike the
// library's browsing collations, tags here are case-sensitive, as in Python.
static uint32_t dense_ids(build_t *b, order_t *order, unsigned field) {
	for (uint32_t i = 0; i < b->count; i++) {
		track_t *t = &b->tracks[i];
		order[i] = (order_t){field == 0 ? t->artist : field == 1 ? t->album : t->genre, i, 0};
	}
	if (!sort_rows(b, order, b->count, sizeof(*order), order_compare)) return 0;
	uint32_t group = 0;
	for (uint32_t i = 0; i < b->count; i++) {
		if (i && strcmp(order[i - 1].text, order[i].text)) group++;
		track_t *t = &b->tracks[order[i].id];
		if (field == 0) t->artist_id = group;
		else if (field == 1) t->album_id = group;
		else t->cluster = group;
	}
	return b->count ? group + 1 : 0;
}

// Canonical input identity: domain (including NUL), then in NOCASE path order
// four u32-length-prefixed UTF-8 strings (path/artist/album/genre), then s64
// year; NULL tags/year are normalized to empty/zero. No row ids or timestamps.
static void input_digest(build_t *b, char out[SHA256_HEX_LEN]) {
	sha256_ctx hash;
	sha256_init(&hash);
	static const char domain[] = "sonix-genius-input-v1";
	sha256_update(&hash, domain, sizeof(domain));
	for (uint32_t i = 0; i < b->count; i++) {
		if (cancelled(b)) return;
		track_t *t = &b->tracks[i];
		const char *texts[] = {t->path, t->artist, t->album, t->genre};
		unsigned char word[8];
		for (unsigned j = 0; j < 4; j++) {
			size_t n = strlen(texts[j]);
			put32(word, (uint32_t)n);
			sha256_update(&hash, word, 4);
			sha256_update(&hash, texts[j], n);
		}
		put64(word, (uint64_t)t->year);
		sha256_update(&hash, word, 8);
	}
	sha256_final_hex(&hash, out);
}

// Read only the identity and bounded header, never the whole old BLOB. Legacy
// exporters lacking input_sha256 are compared by their static graph digest
// after ranking; an exactly equivalent PC graph is kept, including its producer.
static bool read_generation(build_t *b, const char *path, char out[4097]) {
	sqlite3 *db = NULL;
	sqlite3_stmt *stmt = NULL;
	sqlite3_blob *blob = NULL;
	bool ok = false;
	if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) goto done;
	sqlite3_progress_handler(db, 1024, interrupt_sql, b);
	sqlite3_exec(db, "PRAGMA cache_size=-128", NULL, NULL, NULL);
	const char *sql = "SELECT id, CASE WHEN typeof(generation)='text' AND octet_length(generation)"
		" BETWEEN 1 AND 4096 THEN generation END,typeof(data),length(data) FROM GENIUS_SNAPSHOT";
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK || sqlite3_step(stmt) != SQLITE_ROW) goto done;
	int bytes = sqlite3_column_bytes(stmt, 1);
	const char *text = (const char *)sqlite3_column_text(stmt, 1);
	const char *type = (const char *)sqlite3_column_text(stmt, 2);
	int64_t length = sqlite3_column_int64(stmt, 3);
	if (sqlite3_column_type(stmt, 0) != SQLITE_INTEGER || sqlite3_column_int64(stmt, 0) != 0 || !type || strcmp(type, "blob") ||
		!text || bytes <= 0 || bytes > 4096 || memchr(text, 0, (size_t)bytes) || length < HEADER_BYTES || length > (int64_t)GENIUSBUILD_MAX_BLOB_BYTES) goto done;
	memcpy(out, text, (size_t)bytes);
	out[bytes] = 0;
	if (sqlite3_step(stmt) != SQLITE_DONE) goto done;
	sqlite3_finalize(stmt);
	stmt = NULL;
	if (sqlite3_blob_open(db, "main", "GENIUS_SNAPSHOT", "data", 0, 0, &blob) != SQLITE_OK) goto done;
	unsigned char header[HEADER_BYTES], buffer[IO_BYTES];
	if (cancelled(b) || sqlite3_blob_read(blob, header, sizeof(header), 0) != SQLITE_OK ||
		memcmp(header, "SONIXGS\0", 8) || get32(header + 8) != 1 || get32(header + 12) != HEADER_BYTES ||
		get32(header + 16) != length || get32(header + 20) != (uint32_t)bytes || get32(header + 44) != HEADER_BYTES || get32(header + 76)) goto done;
	if (sqlite3_blob_read(blob, buffer, bytes, HEADER_BYTES) != SQLITE_OK || memcmp(buffer, out, (size_t)bytes)) goto done;
	uint64_t paths = get32(header + 24), edges = get32(header + 28), groups = get32(header + 32), neighbours = get32(header + 36), pool = get32(header + 40);
	if (paths > INT32_MAX || edges > INT32_MAX || groups > INT32_MAX || neighbours > INT32_MAX || (!groups && neighbours)) goto done;
	uint64_t sizes[] = {(paths + 1) * 4, pool, paths * 36, paths * 8, edges * 8, groups * 12, neighbours * 4};
	uint64_t at = HEADER_BYTES + (uint32_t)bytes;
	sha256_ctx hash;
	sha256_init(&hash);
	sha256_update(&hash, header + 24, 20);
	for (unsigned i = 0; i < 7; i++) {
		if (get32(header + 48 + i * 4) != at || at + sizes[i] > (uint64_t)length) goto done;
		put32(buffer, (uint32_t)sizes[i]);
		sha256_update(&hash, buffer, 4);
		uint64_t end = at + sizes[i];
		while (at < end) {
			int n = end - at < IO_BYTES ? (int)(end - at) : IO_BYTES;
			if (cancelled(b) || sqlite3_blob_read(blob, buffer, n, (int)at) != SQLITE_OK) goto done;
			sha256_update(&hash, buffer, (size_t)n);
			at += (unsigned)n;
		}
	}
	char digest[SHA256_HEX_LEN], field[80];
	sha256_final_hex(&hash, digest);
	snprintf(field, sizeof(field), "\"sha256\":\"%s\"", digest);
	ok = at == (uint64_t)length && strstr(out, field);
done:
	if (blob) sqlite3_blob_close(blob);
	sqlite3_finalize(stmt);
	if (db) sqlite3_close(db);
	return ok;
}

static bool field_matches(const char *generation, const char *key, const char *value) {
	char field[128];
	snprintf(field, sizeof(field), "\"%s\":\"%s\"", key, value);
	return strstr(generation, field) != NULL;
}

static geniusbuild_result_t publish(build_t *b, int dir, const char *parent, const struct stat *directory, unsigned revision, const char *generation, const unsigned char *data, size_t bytes) {
	geniusbuild_result_t result = GENIUSBUILD_IO;
	sqlite3 *db = NULL;
	sqlite3_stmt *stmt = NULL;
	sqlite3_blob *blob = NULL;
	int fd = -1;
	struct stat current, artifact;
	if (stat(parent, &current) != 0 || current.st_dev != directory->st_dev || current.st_ino != directory->st_ino || cancelled(b)) goto done;
	// These two names belong exclusively to this builder, including orphans.
	unlinkat(dir, STAGING "-journal", 0);
	unlinkat(dir, STAGING, 0);
	fd = openat(dir, STAGING, O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW, 0600);
	if (fd < 0 || fstat(fd, &artifact) != 0) goto done;
	close(fd);
	fd = -1;
	char staging[64];
	snprintf(staging, sizeof(staging), "/proc/self/fd/%d/" STAGING, dir);
	if (sqlite3_open_v2(staging, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) goto done;
	const char *opened = sqlite3_db_filename(db, "main");
	if (!opened || stat(opened, &current) != 0 || current.st_dev != artifact.st_dev || current.st_ino != artifact.st_ino) goto done;
	sqlite3_progress_handler(db, 1024, interrupt_sql, b);
	sqlite3_limit(db, SQLITE_LIMIT_LENGTH, (int)GENIUSBUILD_MAX_BLOB_BYTES + 4096);
	if (sqlite3_exec(db, "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; PRAGMA cache_size=-128;"
		" BEGIN; CREATE TABLE GENIUS_SNAPSHOT(id INTEGER PRIMARY KEY CHECK(id=0),generation TEXT,data BLOB)", NULL, NULL, NULL) != SQLITE_OK) goto done;
	if (sqlite3_prepare_v2(db, "INSERT INTO GENIUS_SNAPSHOT VALUES(0,?1,?2)", -1, &stmt, NULL) != SQLITE_OK ||
		sqlite3_bind_text(stmt, 1, generation, -1, SQLITE_STATIC) != SQLITE_OK ||
		sqlite3_bind_zeroblob(stmt, 2, (int)bytes) != SQLITE_OK || sqlite3_step(stmt) != SQLITE_DONE) goto done;
	sqlite3_finalize(stmt);
	stmt = NULL;
	if (sqlite3_blob_open(db, "main", "GENIUS_SNAPSHOT", "data", 0, 1, &blob) != SQLITE_OK) goto done;
	for (size_t offset = 0; offset < bytes;) {
		size_t n = bytes - offset < IO_BYTES ? bytes - offset : IO_BYTES;
		if (cancelled(b) || sqlite3_blob_write(blob, data + offset, (int)n, (int)offset) != SQLITE_OK) goto done;
		offset += n;
	}
	if (sqlite3_blob_close(blob) != SQLITE_OK) { blob = NULL; goto done; }
	blob = NULL;
	if (cancelled(b) || sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) goto done;
	if (sqlite3_prepare_v2(db, "PRAGMA quick_check", -1, &stmt, NULL) != SQLITE_OK || sqlite3_step(stmt) != SQLITE_ROW ||
		!sqlite3_column_text(stmt, 0) || strcmp((const char *)sqlite3_column_text(stmt, 0), "ok") || sqlite3_step(stmt) != SQLITE_DONE) goto done;
	sqlite3_finalize(stmt);
	stmt = NULL;
	if (sqlite3_close(db) != SQLITE_OK) goto done;
	db = NULL;
	fd = openat(dir, STAGING, O_RDWR | O_NOFOLLOW);
	if (fd < 0 || fsync(fd) != 0 || fstat(fd, &current) != 0 || current.st_dev != artifact.st_dev || current.st_ino != artifact.st_ino) goto done;
	close(fd);
	fd = -1;
	// A directory descriptor pins this mount. Never publish to a new card that
	// happens to have acquired the same mount-point name while I/O was failing.
	if (stat(parent, &current) != 0 || current.st_dev != directory->st_dev || current.st_ino != directory->st_ino) goto done;
	if (library_revision(LIBRARY_LIST_TRACKS) != revision) { result = GENIUSBUILD_CHANGED; goto done; }
	if (cancelled(b)) goto done;
	if (renameat(dir, STAGING, dir, "genius.db") != 0) goto done;
	result = GENIUSBUILD_OK;
	// Some card filesystems do not support directory fsync. The complete file
	// has already been synced and atomically renamed; report that distinction.
	if (fsync(dir) != 0) fprintf(stderr, "geniusbuild: published; directory sync failed: %s\n", strerror(errno));
	report(b, GENIUSBUILD_PUBLISHED, (int)b->count);
done:
	if (fd >= 0) close(fd);
	if (blob) sqlite3_blob_close(blob);
	sqlite3_finalize(stmt);
	if (db) sqlite3_close(db);
	if (result != GENIUSBUILD_OK) {
		unlinkat(dir, STAGING "-journal", 0);
		unlinkat(dir, STAGING, 0);
		if (cancelled(b)) result = GENIUSBUILD_CANCELLED;
	}
	return result;
}

geniusbuild_result_t geniusbuild_run(const char *path, const atomic_bool *cancel, geniusbuild_progress_cb progress, void *user) {
	build_t b = {.cancel = cancel, .progress = progress, .user = user, .result = GENIUSBUILD_OK};
	order_t *order = NULL;
	uint16_t *targets = NULL;
	uint32_t *groups = NULL;
	unsigned char *counts = NULL, *artists = NULL, *albums = NULL, *scores = NULL, *data = NULL;
	unsigned revision = 0;
	bool published = false;
	char parent[512];
	struct stat directory;
	char old[4097] = "", input[SHA256_HEX_LEN], graph[SHA256_HEX_LEN], generation[512];
	report(&b, GENIUSBUILD_CHECKING, 0);
	if (!path || !path[0]) return GENIUSBUILD_INVALID;
	if (cancelled(&b)) return GENIUSBUILD_CANCELLED;
	const char *slash = strrchr(path, '/');
	if (!slash || slash == path || strlen(path) >= sizeof(parent) || strcmp(slash + 1, "genius.db")) return GENIUSBUILD_INVALID;
	snprintf(parent, sizeof(parent), "%.*s", (int)(slash - path), path);
	// Pin the card before collecting or ranking, not only when writing starts.
	int dir = open(parent, O_RDONLY | O_DIRECTORY);
	if (dir < 0) return GENIUSBUILD_IO;
	if (fstat(dir, &directory) != 0) { close(dir); return GENIUSBUILD_IO; }
	unlinkat(dir, STAGING "-journal", 0);
	unlinkat(dir, STAGING, 0);
	b.tracks = allocate(&b, GENIUSBUILD_MAX_TRACKS, sizeof(*b.tracks));
	b.text = allocate(&b, GENIUSBUILD_MAX_TEXT_BYTES, 1);
	if (!b.tracks || !b.text) goto done;
	if (!library_read_tags(collect, &b, cancel, &revision)) {
		if (b.result == GENIUSBUILD_OK) b.result = GENIUSBUILD_INVALID;
		goto done;
	}
	if (!sort_rows(&b, b.tracks, b.count, sizeof(*b.tracks), path_compare)) goto done;
	for (uint32_t i = 1; i < b.count; i++) {
		if (!nocase(b.tracks[i - 1].path, b.tracks[i].path)) { b.result = GENIUSBUILD_INVALID; goto done; }
	}
	input_digest(&b, input);
	if (cancelled(&b)) goto done;
	bool have = read_generation(&b, path, old);
	if (have && field_matches(old, "algorithm", "sonix-genius-1") && field_matches(old, "input_sha256", input)) {
		b.result = library_revision(LIBRARY_LIST_TRACKS) == revision ? GENIUSBUILD_CURRENT : GENIUSBUILD_CHANGED;
		goto done;
	}
	order = allocate(&b, b.count, sizeof(*order));
	targets = allocate(&b, (size_t)b.count * GENIUSBUILD_TOP, sizeof(*targets));
	groups = allocate(&b, (size_t)b.count + 1, sizeof(*groups));
	counts = allocate(&b, b.count, 1);
	artists = allocate(&b, b.count, 1);
	albums = allocate(&b, b.count, 1);
	scores = allocate(&b, b.count, 1);
	if (!order || !targets || !groups || !counts || !artists || !albums || !scores) goto done;
	dense_ids(&b, order, 0);
	dense_ids(&b, order, 1);
	uint32_t clusters = dense_ids(&b, order, 2);
	if (cancelled(&b)) goto done;
	for (uint32_t i = 0; i < b.count; i++) {
		order[i] = (order_t){b.tracks[i].path, i, b.tracks[i].cluster};
		groups[b.tracks[i].cluster + 1]++;
	}
	for (uint32_t i = 1; i <= clusters; i++) groups[i] += groups[i - 1];
	if (!sort_rows(&b, order, b.count, sizeof(*order), order_compare)) goto done;
	uint32_t edges = 0;
	report(&b, GENIUSBUILD_RANKING, 0);
	for (uint32_t i = 0; i < b.count; i++) {
		if (cancelled(&b)) goto done;
		track_t *seed = &b.tracks[i];
		uint32_t first = groups[seed->cluster], end = groups[seed->cluster + 1];
		memset(artists, 0, b.count);
		memset(albums, 0, b.count);
		for (uint32_t j = first; j < end; j++) {
			if ((j & 31) == 0 && cancelled(&b)) goto done;
			track_t *other = &b.tracks[order[j].id];
			scores[j] = (seed->album[0] && seed->album_id == other->album_id ? 3 : 0) +
				(seed->artist[0] && seed->artist_id == other->artist_id ? 2 : 0) +
				(seed->year && seed->year == other->year ? 1 : 0);
		}
		// At most 7 * 4096 visits per track, constant-time caps. The order is
		// (-score, raw UTF-8 path), exactly the reference producer's sort.
		for (int score = 6; score >= 0 && counts[i] < GENIUSBUILD_TOP; score--) {
			for (uint32_t j = first; j < end && counts[i] < GENIUSBUILD_TOP; j++) {
				if ((j & 31) == 0 && cancelled(&b)) goto done;
				uint32_t id = order[j].id;
				track_t *other = &b.tracks[id];
				if (id == i || scores[j] != score || artists[other->artist_id] >= 2 || albums[other->album_id] >= 3) continue;
				artists[other->artist_id]++;
				albums[other->album_id]++;
				targets[(size_t)i * GENIUSBUILD_TOP + counts[i]++] = (uint16_t)id;
			}
		}
		edges += counts[i];
		report(&b, GENIUSBUILD_RANKING, (int)i + 1);
	}
	uint32_t pool = 0;
	for (uint32_t i = 0; i < b.count; i++) pool += (uint32_t)strlen(b.tracks[i].path) + 5;
	uint32_t sizes[] = {(b.count + 1) * 4, pool, b.count * 36, b.count * 8, edges * 8, clusters * 12, clusters * 4};
	size_t section_bytes = 0;
	for (unsigned i = 0; i < 7; i++) section_bytes += sizes[i];
	// Reserve bounded generation space before encoding. The final layout is
	// packed below once the static graph digest is known.
	if (section_bytes + HEADER_BYTES + sizeof(generation) > GENIUSBUILD_MAX_BLOB_BYTES) { b.result = GENIUSBUILD_LIMIT; goto done; }
	data = allocate(&b, section_bytes + HEADER_BYTES + sizeof(generation), 1);
	if (!data) goto done;
	unsigned char *sections[7];
	unsigned char *cursor = data + HEADER_BYTES + sizeof(generation);
	for (unsigned i = 0; i < 7; i++) { sections[i] = cursor; cursor += sizes[i]; }
	uint32_t offset = 0, edge = 0;
	for (uint32_t i = 0; i < b.count; i++) {
		if ((i & 31) == 0 && cancelled(&b)) goto done;
		track_t *t = &b.tracks[i];
		uint32_t len = (uint32_t)strlen(t->path);
		put32(sections[0] + i * 4, offset);
		put32(sections[1] + offset, len);
		memcpy(sections[1] + offset + 4, t->path, len + 1);
		offset += len + 5;
		unsigned char *m = sections[2] + i * 36;
		put32(m, t->cluster);
		put64(m + 4, t->artist_id);
		for (unsigned k = 12; k <= 28; k += 8) put64(m + k, i + 1);
		put32(sections[3] + i * 8, edge);
		put32(sections[3] + i * 8 + 4, counts[i]);
		for (unsigned j = 0; j < counts[i]; j++, edge++) {
			put32(sections[4] + edge * 8, i);
			put32(sections[4] + edge * 8 + 4, targets[(size_t)i * GENIUSBUILD_TOP + j]);
		}
	}
	put32(sections[0] + b.count * 4, offset);
	for (uint32_t i = 0; i < clusters; i++) {
		put32(sections[5] + i * 12, i);
		put32(sections[5] + i * 12 + 4, i);
		put32(sections[5] + i * 12 + 8, 1);
		put32(sections[6] + i * 4, i);
	}
	sha256_ctx hash;
	sha256_init(&hash);
	unsigned char word[20];
	uint32_t values[] = {b.count, edges, clusters, clusters, pool};
	for (unsigned i = 0; i < 5; i++) put32(word + i * 4, values[i]);
	sha256_update(&hash, word, 20);
	for (unsigned i = 0; i < 7; i++) {
		put32(word, sizes[i]);
		sha256_update(&hash, word, 4);
		for (size_t at = 0; at < sizes[i];) {
			size_t n = sizes[i] - at < IO_BYTES ? sizes[i] - at : IO_BYTES;
			if (cancelled(&b)) goto done;
			sha256_update(&hash, sections[i] + at, n);
			at += n;
		}
	}
	sha256_final_hex(&hash, graph);
	if (have && field_matches(old, "producer", "export_genius_db.py") && field_matches(old, "algorithm", "sonix-genius-1") && field_matches(old, "sha256", graph)) {
		b.result = library_revision(LIBRARY_LIST_TRACKS) == revision ? GENIUSBUILD_CURRENT : GENIUSBUILD_CHANGED;
		goto done;
	}
	snprintf(generation, sizeof(generation), "{\"algorithm\":\"sonix-genius-1\",\"input_sha256\":\"%s\",\"producer\":\"sonix-geniusbuild-1\",\"schema\":\"1\",\"sha256\":\"%s\",\"source\":\"library-tags-v1\",\"tracks\":%u}", input, graph, b.count);
	size_t gen_bytes = strlen(generation), total = HEADER_BYTES + gen_bytes + section_bytes;
	memmove(data + HEADER_BYTES + gen_bytes, data + HEADER_BYTES + sizeof(generation), section_bytes);
	memcpy(data + HEADER_BYTES, generation, gen_bytes);
	memcpy(data, "SONIXGS\0", 8);
	uint32_t header[] = {1, HEADER_BYTES, (uint32_t)total, (uint32_t)gen_bytes, b.count, edges, clusters, clusters, pool};
	for (unsigned i = 0; i < 9; i++) put32(data + 8 + i * 4, header[i]);
	put32(data + 44, HEADER_BYTES);
	uint32_t position = HEADER_BYTES + (uint32_t)gen_bytes;
	for (unsigned i = 0; i < 7; i++) { put32(data + 48 + i * 4, position); position += sizes[i]; }
	put32(data + 76, 0);
	report(&b, GENIUSBUILD_PUBLISHING, (int)b.count);
	b.result = publish(&b, dir, parent, &directory, revision, generation, data, total);
	published = b.result == GENIUSBUILD_OK;
	if (b.result == GENIUSBUILD_OK) printf("geniusbuild: published %u tracks, %u edges, %u genres, %zu bytes; input %s\n", b.count, edges, clusters, total, input);
done:
	if (cancelled(&b) && !published) b.result = GENIUSBUILD_CANCELLED;
	free(data);
	free(scores);
	free(albums);
	free(artists);
	free(counts);
	free(groups);
	free(targets);
	free(order);
	free(b.text);
	free(b.tracks);
	close(dir);
	printf("geniusbuild: result %d, %u tracks, retained peak %zu bytes\n", b.result, b.count, b.retained);
	return b.result;
}

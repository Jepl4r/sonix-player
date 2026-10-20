// ---------------------------------------------------------------------------
// One immutable graph per mount, private membership per request.
// The binary reader decodes explicit little-endian fields directly into the
// graph. Only legacy databases need row scans and temporary path hashing.
// database.lock serializes loads, reference ownership and the global budget;
// accessors need no lock because published graph and request data never change.
// ---------------------------------------------------------------------------

#include "src/system/library/geniusdb.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "src/system/db/sqlite3.h"
#include "src/system/library/library.h"

#define GENIUSDB_CANCEL_ROWS 32
#define GENIUSDB_CANCEL_INSTRUCTIONS 1024
#define GENIUSDB_READ_BYTES 16384
#define GENIUSDB_HEADER_BYTES 80
#define GENIUSDB_GENERATION_BYTES 4096

typedef struct {
	int32_t cluster;
	int64_t k1, k2, k3, k20;
} geniusdb_meta_t;

typedef struct {
	int32_t first, count;
} geniusdb_range_t;

typedef struct {
	int32_t cluster;
	geniusdb_range_t range;
} geniusdb_cluster_t;

typedef struct {
	uint32_t hash;
	int32_t id;
} geniusdb_path_entry_t;

typedef struct {
	geniusdb_path_entry_t *entries;
	size_t capacity;
} geniusdb_path_table_t;

typedef struct {
	const char **paths;
	char *path_pool;
	char *generation;
	geniusdb_meta_t *meta;
	geniusdb_range_t *sims;
	genius_edge_t *edges;
	geniusdb_cluster_t *clusters;
	int32_t *neighbours;
	int32_t path_count, edge_count, cluster_count, neighbour_count;
	size_t bytes;
	size_t references;
} geniusdb_graph_t;

struct geniusdb_snapshot {
	geniusdb_graph_t *graph;
	genius_meta_t *meta;
	size_t bytes;
	unsigned revision;
};

// Lifecycle threads own the path under lock. One worker reads at a time;
// cancellation is atomic so close interrupts it before waiting for the lock.
static struct {
	pthread_mutex_t lock;
	char path[512];
	atomic_bool closing, available;
	atomic_uint revision;
	geniusdb_graph_t *cache;
	size_t bytes;
} database = {.lock = PTHREAD_MUTEX_INITIALIZER};

typedef struct {
	const atomic_bool *cancel;
	unsigned revision;
} geniusdb_read_t;

static bool geniusdb_read_cancelled(const geniusdb_read_t *read) { return atomic_load(&database.closing) || read->revision != atomic_load(&database.revision) || (read->cancel && atomic_load(read->cancel)); }

static int geniusdb_interrupt_read(void *user) { return geniusdb_read_cancelled(user); }

// Match SQLite NOCASE without depending on the process locale.
static int geniusdb_compare_paths(const char *a, const char *b) {
	for (;;) {
		unsigned char ca = (unsigned char)*a++;
		unsigned char cb = (unsigned char)*b++;
		if (ca >= 'A' && ca <= 'Z') {
			ca += 'a' - 'A';
		}
		if (cb >= 'A' && cb <= 'Z') {
			cb += 'a' - 'A';
		}
		if (ca != cb || !ca) {
			return (int)ca - (int)cb;
		}
	}
}

// Metadata and edges normally share the same spelling.
static bool geniusdb_match_paths(const char *a, const char *b) { return strcmp(a, b) == 0 || geniusdb_compare_paths(a, b) == 0; }

// Called under database.lock. Charge all retained generations and temporary
// allocations, rather than giving each request an independent 4 MiB allowance.
static void *geniusdb_allocate_rows(size_t *bytes, size_t count, size_t size, geniusdb_result_t *result) {
	if (*result != GENIUSDB_OK || !count) {
		return NULL;
	}
	if ((size && count > SIZE_MAX / size) || *bytes > GENIUSDB_MAX_SNAPSHOT_BYTES || database.bytes > GENIUSDB_MAX_SNAPSHOT_BYTES || count * size > GENIUSDB_MAX_SNAPSHOT_BYTES - *bytes || count * size > GENIUSDB_MAX_SNAPSHOT_BYTES - database.bytes) {
		*result = GENIUSDB_LIMIT;
		return NULL;
	}
	void *rows = calloc(count, size);
	if (!rows) {
		*result = GENIUSDB_NOMEM;
		return NULL;
	}
	*bytes += count * size;
	database.bytes += count * size;
	return rows;
}

static uint32_t geniusdb_hash_path(const char *path) {
	uint32_t hash = UINT32_C(2166136261);
	size_t bytes = strlen(path);
	while (bytes >= sizeof(uint32_t)) {
		uint32_t word;
		memcpy(&word, path, sizeof(word));
		// Per-byte sums straddle bit 7 only for ASCII A through Z.
		// The original high bits exclude non-ASCII bytes from the fold.
		uint32_t ascii = word & UINT32_C(0x7f7f7f7f);
		uint32_t letters = ((ascii + UINT32_C(0x3f3f3f3f)) ^ (ascii + UINT32_C(0x25252525))) & ~word & UINT32_C(0x80808080);
		word |= letters >> 2;
		hash = (hash ^ word) * UINT32_C(16777619);
		path += sizeof(word);
		bytes -= sizeof(word);
	}
	for (unsigned char c; (c = (unsigned char)*path++) != 0;) {
		if (c >= 'A' && c <= 'Z') {
			c += 'a' - 'A';
		}
		hash = (hash ^ c) * UINT32_C(16777619);
	}
	hash ^= hash >> 16;
	hash *= UINT32_C(16777619);
	return hash ^ (hash >> 16);
}

static geniusdb_result_t geniusdb_build_path_table(geniusdb_graph_t *snapshot, geniusdb_path_table_t *table, const geniusdb_read_t *read) {
	if (!snapshot->path_count) {
		return GENIUSDB_OK;
	}
	// A power-of-two capacity keeps at least half the slots empty.
	size_t capacity = 1;
	while (capacity / 2 < (size_t)snapshot->path_count) {
		if (capacity > SIZE_MAX / 2) {
			return GENIUSDB_LIMIT;
		}
		capacity *= 2;
	}
	geniusdb_result_t result = GENIUSDB_OK;
	table->entries = geniusdb_allocate_rows(&snapshot->bytes, capacity, sizeof(*table->entries), &result);
	if (result != GENIUSDB_OK) {
		return result;
	}
	table->capacity = capacity;
	for (int32_t i = 0; i < snapshot->path_count; i++) {
		if (i % GENIUSDB_CANCEL_ROWS == 0 && geniusdb_read_cancelled(read)) {
			return GENIUSDB_CANCELLED;
		}
		uint32_t hash = geniusdb_hash_path(snapshot->paths[i]);
		size_t slot = hash & (capacity - 1);
		while (table->entries[slot].id) {
			slot = (slot + 1) & (capacity - 1);
		}
		// Zero marks an empty slot; stored ids are offset by one.
		table->entries[slot] = (geniusdb_path_entry_t){hash, i + 1};
	}
	return GENIUSDB_OK;
}

static int32_t geniusdb_lookup_path(const geniusdb_graph_t *snapshot, const geniusdb_path_table_t *table, const char *path) {
	if (!table->capacity) {
		return -1;
	}
	uint32_t hash = geniusdb_hash_path(path);
	size_t slot = hash & (table->capacity - 1);
	while (table->entries[slot].id) {
		const geniusdb_path_entry_t *entry = &table->entries[slot];
		int32_t id = entry->id - 1;
		if (entry->hash == hash && geniusdb_match_paths(path, snapshot->paths[id])) {
			return id;
		}
		slot = (slot + 1) & (table->capacity - 1);
	}
	return -1;
}

static geniusdb_result_t geniusdb_count_rows(sqlite3 *db, const char *sql, int32_t *count, size_t *bytes) {
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
		return GENIUSDB_INVALID;
	}
	geniusdb_result_t result = GENIUSDB_INVALID;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		int64_t rows = sqlite3_column_int64(stmt, 0);
		int64_t size = bytes ? sqlite3_column_int64(stmt, 1) : 0;
		if (rows < 0 || rows > INT32_MAX || size < 0 || (uint64_t)size > GENIUSDB_MAX_SNAPSHOT_BYTES) {
			result = GENIUSDB_LIMIT;
		} else {
			*count = (int32_t)rows;
			if (bytes) {
				*bytes = (size_t)size;
			}
			result = GENIUSDB_OK;
		}
	}
	sqlite3_finalize(stmt);
	return result;
}

static bool geniusdb_valid_path(sqlite3_stmt *stmt, int column, const char **out) {
	const char *path = (const char *)sqlite3_column_text(stmt, column);
	int bytes = sqlite3_column_bytes(stmt, column);
	if (!path || bytes <= 0 || strlen(path) != (size_t)bytes) {
		return false;
	}
	*out = path;
	return true;
}

static geniusdb_result_t geniusdb_read_paths(sqlite3 *db, geniusdb_graph_t *snapshot, size_t pool_size, const geniusdb_read_t *read) {
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, "SELECT path FROM GENIUS_META GROUP BY path COLLATE NOCASE ORDER BY path COLLATE NOCASE", -1, &stmt, NULL) != SQLITE_OK) {
		return GENIUSDB_INVALID;
	}
	int rc;
	int32_t count = 0;
	size_t offset = 0;
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		const char *path;
		if (count >= snapshot->path_count || !geniusdb_valid_path(stmt, 0, &path)) {
			break;
		}
		size_t bytes = (size_t)sqlite3_column_bytes(stmt, 0) + 1;
		if (offset > pool_size || bytes > pool_size - offset) {
			break;
		}
		memcpy(snapshot->path_pool + offset, path, bytes);
		snapshot->paths[count++] = snapshot->path_pool + offset;
		offset += bytes;
		if (count % GENIUSDB_CANCEL_ROWS == 0 && geniusdb_read_cancelled(read)) {
			break;
		}
	}
	sqlite3_finalize(stmt);
	return rc == SQLITE_DONE && count == snapshot->path_count && offset == pool_size ? GENIUSDB_OK : GENIUSDB_INVALID;
}

static geniusdb_result_t geniusdb_read_meta(sqlite3 *db, geniusdb_graph_t *snapshot, const geniusdb_path_table_t *table, const geniusdb_read_t *read) {
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, "SELECT path,cluster,k1,k2,k3,k20 FROM GENIUS_META ORDER BY path COLLATE NOCASE,path COLLATE BINARY", -1, &stmt, NULL) != SQLITE_OK) {
		return GENIUSDB_INVALID;
	}
	int rc;
	unsigned rows = 0;
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		const char *path;
		if (!geniusdb_valid_path(stmt, 0, &path)) {
			break;
		}
		int32_t id = geniusdb_lookup_path(snapshot, table, path);
		int64_t cluster = sqlite3_column_int64(stmt, 1);
		if (id < 0 || (unsigned)id != rows || cluster < INT32_MIN || cluster > INT32_MAX) {
			break;
		}
		// Metadata preserves the file's spelling on case-sensitive cards.
		// NOCASE equal strings have the same length and sort position.
		memcpy((char *)snapshot->paths[id], path, strlen(path) + 1);
		snapshot->meta[id] = (geniusdb_meta_t){
			.cluster = (int32_t)cluster,
			.k1 = sqlite3_column_int64(stmt, 2),
			.k2 = sqlite3_column_int64(stmt, 3),
			.k3 = sqlite3_column_int64(stmt, 4),
			.k20 = sqlite3_column_int64(stmt, 5),
		};
		if (++rows % GENIUSDB_CANCEL_ROWS == 0 && geniusdb_read_cancelled(read)) {
			break;
		}
	}
	sqlite3_finalize(stmt);
	return rc == SQLITE_DONE && rows == (unsigned)snapshot->path_count ? GENIUSDB_OK : GENIUSDB_INVALID;
}

static geniusdb_result_t geniusdb_read_edges(sqlite3 *db, geniusdb_graph_t *snapshot, const geniusdb_path_table_t *table, const geniusdb_read_t *read) {
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, "SELECT path,other FROM GENIUS_SIM ORDER BY path COLLATE NOCASE,rank", -1, &stmt, NULL) != SQLITE_OK) {
		return GENIUSDB_INVALID;
	}
	int rc;
	int32_t count = 0;
	// The query orders by path, so consecutive rows name the same track. One
	// hash lookup per track rather than one per edge. The comparison is
	// against the interned copy, not the column pointer: that one dies at the
	// next step.
	int32_t last_id = -1;
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		const char *path, *other;
		if (count >= snapshot->edge_count || !geniusdb_valid_path(stmt, 0, &path) || !geniusdb_valid_path(stmt, 1, &other)) {
			break;
		}
		int32_t id;
		if (last_id >= 0 && geniusdb_match_paths(path, snapshot->paths[last_id])) {
			id = last_id;
		} else {
			id = geniusdb_lookup_path(snapshot, table, path);
			last_id = id;
		}
		int32_t target = geniusdb_lookup_path(snapshot, table, other);
		if (id < 0 || target < 0) {
			break;
		}
		geniusdb_range_t *range = &snapshot->sims[id];
		if (!range->count) {
			range->first = count;
		}
		range->count++;
		snapshot->edges[count++] = (genius_edge_t){id, target};
		if (count % GENIUSDB_CANCEL_ROWS == 0 && geniusdb_read_cancelled(read)) {
			break;
		}
	}
	sqlite3_finalize(stmt);
	return rc == SQLITE_DONE && count == snapshot->edge_count ? GENIUSDB_OK : GENIUSDB_INVALID;
}

static geniusdb_result_t geniusdb_read_clusters(sqlite3 *db, geniusdb_graph_t *snapshot, const geniusdb_read_t *read) {
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, "SELECT cluster,neighbour FROM GENIUS_CLUSTER ORDER BY cluster,neighbour", -1, &stmt, NULL) != SQLITE_OK) {
		return GENIUSDB_INVALID;
	}
	int rc;
	int32_t count = 0, groups = 0;
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		int64_t cluster = sqlite3_column_int64(stmt, 0);
		int64_t neighbour = sqlite3_column_int64(stmt, 1);
		if (count >= snapshot->neighbour_count || cluster < INT32_MIN || cluster > INT32_MAX || neighbour < INT32_MIN || neighbour > INT32_MAX) {
			break;
		}
		if (!groups || snapshot->clusters[groups - 1].cluster != cluster) {
			if (groups >= snapshot->cluster_count) {
				break;
			}
			snapshot->clusters[groups++] = (geniusdb_cluster_t){.cluster = (int32_t)cluster, .range.first = count};
		}
		snapshot->clusters[groups - 1].range.count++;
		snapshot->neighbours[count++] = (int32_t)neighbour;
		if (count % GENIUSDB_CANCEL_ROWS == 0 && geniusdb_read_cancelled(read)) {
			break;
		}
	}
	sqlite3_finalize(stmt);
	return rc == SQLITE_DONE && count == snapshot->neighbour_count && groups == snapshot->cluster_count ? GENIUSDB_OK : GENIUSDB_INVALID;
}

// References and the byte ledger are protected by database.lock. Retired
// graphs remain charged until their last worker releases its borrowed rows.
static void geniusdb_release_graph(geniusdb_graph_t *graph) {
	if (!graph || --graph->references) {
		return;
	}
	database.bytes -= graph->bytes;
	free(graph->paths);
	free(graph->path_pool);
	free(graph->generation);
	free(graph->meta);
	free(graph->sims);
	free(graph->edges);
	free(graph->clusters);
	free(graph->neighbours);
	free(graph);
}

static void geniusdb_release_snapshot(geniusdb_snapshot_t *snapshot) {
	if (snapshot) {
		geniusdb_release_graph(snapshot->graph);
		database.bytes -= snapshot->bytes;
		free(snapshot->meta);
		free(snapshot);
	}
}

static geniusdb_result_t geniusdb_read_rows(sqlite3 *db, geniusdb_graph_t *snapshot, const geniusdb_read_t *read) {
	geniusdb_path_table_t table = {0};
	size_t path_bytes = 0;
	geniusdb_result_t result = geniusdb_count_rows(db, "SELECT count(*),coalesce(sum(length(CAST(path AS BLOB))+1),0) FROM (SELECT path FROM GENIUS_META GROUP BY path COLLATE NOCASE)", &snapshot->path_count, &path_bytes);
	if (result == GENIUSDB_OK) {
		result = geniusdb_count_rows(db, "SELECT count(*) FROM GENIUS_SIM", &snapshot->edge_count, NULL);
	}
	if (result == GENIUSDB_OK) {
		result = geniusdb_count_rows(db, "SELECT count(*) FROM GENIUS_CLUSTER", &snapshot->neighbour_count, NULL);
	}
	if (result == GENIUSDB_OK) {
		result = geniusdb_count_rows(db, "SELECT count(*) FROM (SELECT cluster FROM GENIUS_CLUSTER GROUP BY cluster)", &snapshot->cluster_count, NULL);
	}
	snapshot->paths = geniusdb_allocate_rows(&snapshot->bytes, (size_t)snapshot->path_count, sizeof(*snapshot->paths), &result);
	snapshot->path_pool = geniusdb_allocate_rows(&snapshot->bytes, path_bytes, 1, &result);
	snapshot->meta = geniusdb_allocate_rows(&snapshot->bytes, (size_t)snapshot->path_count, sizeof(*snapshot->meta), &result);
	snapshot->sims = geniusdb_allocate_rows(&snapshot->bytes, (size_t)snapshot->path_count, sizeof(*snapshot->sims), &result);
	snapshot->edges = geniusdb_allocate_rows(&snapshot->bytes, (size_t)snapshot->edge_count, sizeof(*snapshot->edges), &result);
	snapshot->clusters = geniusdb_allocate_rows(&snapshot->bytes, (size_t)snapshot->cluster_count, sizeof(*snapshot->clusters), &result);
	snapshot->neighbours = geniusdb_allocate_rows(&snapshot->bytes, (size_t)snapshot->neighbour_count, sizeof(*snapshot->neighbours), &result);
	if (result == GENIUSDB_OK) {
		result = geniusdb_read_paths(db, snapshot, path_bytes, read);
	}
	if (result == GENIUSDB_OK) {
		result = geniusdb_build_path_table(snapshot, &table, read);
	}
	if (result == GENIUSDB_OK) {
		result = geniusdb_read_meta(db, snapshot, &table, read);
	}
	if (result == GENIUSDB_OK) {
		result = geniusdb_read_edges(db, snapshot, &table, read);
	}
	if (result == GENIUSDB_OK) {
		result = geniusdb_read_clusters(db, snapshot, read);
	}
	if (table.entries) {
		size_t bytes = table.capacity * sizeof(*table.entries);
		free(table.entries);
		snapshot->bytes -= bytes;
		database.bytes -= bytes;
	}
	return result;
}

static uint32_t geniusdb_u32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int32_t geniusdb_i32(const unsigned char *p) {
	uint32_t value = geniusdb_u32(p);
	return value <= INT32_MAX ? (int32_t)value : -1 - (int32_t)(UINT32_MAX - value);
}

static int64_t geniusdb_i64(const unsigned char *p) {
	uint64_t value = geniusdb_u32(p) | (uint64_t)geniusdb_u32(p + 4) << 32;
	return value <= INT64_MAX ? (int64_t)value : -1 - (int64_t)(UINT64_MAX - value);
}

static geniusdb_result_t geniusdb_bad_blob(const char *message) {
	fprintf(stderr, "genius: invalid binary snapshot: %s\n", message);
	return GENIUSDB_INVALID;
}

static geniusdb_result_t geniusdb_read_blob_bytes(sqlite3_blob *blob, void *destination, size_t offset, size_t bytes, const geniusdb_read_t *read) {
	size_t length = (size_t)sqlite3_blob_bytes(blob);
	if (offset > length || bytes > length - offset) {
		return geniusdb_bad_blob("read outside BLOB");
	}
	unsigned char *out = destination;
	while (bytes) {
		if (geniusdb_read_cancelled(read)) {
			return GENIUSDB_CANCELLED;
		}
		size_t chunk = bytes < GENIUSDB_READ_BYTES ? bytes : GENIUSDB_READ_BYTES;
		int rc = sqlite3_blob_read(blob, out, (int)chunk, (int)offset);
		if (rc != SQLITE_OK) {
			return rc == SQLITE_NOMEM ? GENIUSDB_NOMEM : geniusdb_bad_blob("BLOB read failed");
		}
		if (geniusdb_read_cancelled(read)) {
			return GENIUSDB_CANCELLED;
		}
		out += chunk;
		offset += chunk;
		bytes -= chunk;
	}
	return GENIUSDB_OK;
}

// Reject embedded NUL, overlong sequences, surrogates and values > U+10FFFF.
// Long individual paths must not postpone cancellation until the next record.
static bool geniusdb_utf8(const unsigned char *text, size_t bytes, const geniusdb_read_t *read) {
	size_t checked = 0;
	for (size_t i = 0; i < bytes;) {
		if (i - checked >= 4096 || i == 0) {
			if (geniusdb_read_cancelled(read)) {
				return false;
			}
			checked = i;
		}
		unsigned char c = text[i++];
		if (c && c < 0x80) {
			continue;
		}
		unsigned following = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
		if (!following || following > bytes - i) {
			return false;
		}
		unsigned char next = text[i];
		if ((c == 0xe0 && next < 0xa0) || (c == 0xed && next >= 0xa0) || (c == 0xf0 && next < 0x90) || (c == 0xf4 && next >= 0x90)) {
			return false;
		}
		while (following--) {
			if ((text[i++] & 0xc0) != 0x80) {
				return false;
			}
		}
	}
	return true;
}

static geniusdb_result_t geniusdb_decode_blob(sqlite3_blob *blob, const char *generation, size_t generation_bytes, geniusdb_graph_t *graph, const geniusdb_read_t *read) {
	unsigned char header[GENIUSDB_HEADER_BYTES];
	geniusdb_result_t result = geniusdb_read_blob_bytes(blob, header, 0, sizeof(header), read);
	if (result != GENIUSDB_OK) {
		return result;
	}
	if (memcmp(header, "SONIXGS\0", 8) != 0) {
		return geniusdb_bad_blob("magic");
	}
	uint32_t h[20];
	for (unsigned i = 2; i < 20; i++) {
		h[i] = geniusdb_u32(header + 4 * i);
	}
	if (h[2] != 1 || h[3] != sizeof(header) || h[19]) {
		return geniusdb_bad_blob("version, header size or reserved field");
	}
	if (h[4] != (uint32_t)sqlite3_blob_bytes(blob) || !h[5] || h[5] > GENIUSDB_GENERATION_BYTES || h[5] != generation_bytes) {
		return geniusdb_bad_blob("length or generation length");
	}
	for (unsigned i = 6; i <= 9; i++) {
		if (h[i] > INT32_MAX) {
			return geniusdb_bad_blob("count exceeds INT32_MAX");
		}
	}
	uint64_t sizes[] = {h[5], ((uint64_t)h[6] + 1) * 4, h[10], (uint64_t)h[6] * 36, (uint64_t)h[6] * 8, (uint64_t)h[7] * 8, (uint64_t)h[8] * 12, (uint64_t)h[9] * 4};
	uint64_t end = sizeof(header);
	for (unsigned i = 0; i < 8; i++) {
		if (h[11 + i] != end || sizes[i] > h[4] || end + sizes[i] > h[4]) {
			return geniusdb_bad_blob("section offsets or sizes");
		}
		end += sizes[i];
	}
	if (end != h[4] || (uint64_t)h[6] * 6 > h[10]) {
		return geniusdb_bad_blob("trailing bytes or path pool size");
	}
	graph->path_count = (int32_t)h[6];
	graph->edge_count = (int32_t)h[7];
	graph->cluster_count = (int32_t)h[8];
	graph->neighbour_count = (int32_t)h[9];
	graph->generation = geniusdb_allocate_rows(&graph->bytes, h[5] + 1, 1, &result);
	graph->paths = geniusdb_allocate_rows(&graph->bytes, h[6], sizeof(*graph->paths), &result);
	graph->path_pool = geniusdb_allocate_rows(&graph->bytes, h[10], 1, &result);
	graph->meta = geniusdb_allocate_rows(&graph->bytes, h[6], sizeof(*graph->meta), &result);
	graph->sims = geniusdb_allocate_rows(&graph->bytes, h[6], sizeof(*graph->sims), &result);
	graph->edges = geniusdb_allocate_rows(&graph->bytes, h[7], sizeof(*graph->edges), &result);
	graph->clusters = geniusdb_allocate_rows(&graph->bytes, h[8], sizeof(*graph->clusters), &result);
	graph->neighbours = geniusdb_allocate_rows(&graph->bytes, h[9], sizeof(*graph->neighbours), &result);
	size_t temporary = 0;
	unsigned char *buffer = geniusdb_allocate_rows(&temporary, GENIUSDB_READ_BYTES, 1, &result);
	if (result != GENIUSDB_OK) {
		goto done;
	}
	result = geniusdb_read_blob_bytes(blob, graph->generation, h[11], h[5], read);
	if (result != GENIUSDB_OK) {
		goto done;
	}
	if (memcmp(graph->generation, generation, h[5]) || !geniusdb_utf8((const unsigned char *)graph->generation, h[5], read)) {
		result = geniusdb_bad_blob("generation mismatch or invalid UTF-8");
		goto done;
	}
	result = geniusdb_read_blob_bytes(blob, graph->path_pool, h[13], h[10], read);
	if (result != GENIUSDB_OK) {
		goto done;
	}
	uint32_t previous = 0;
	for (uint32_t i = 0; i <= h[6];) {
		uint32_t count = h[6] - i + 1;
		if (count > GENIUSDB_READ_BYTES / 4) {
			count = GENIUSDB_READ_BYTES / 4;
		}
		result = geniusdb_read_blob_bytes(blob, buffer, h[12] + (size_t)i * 4, (size_t)count * 4, read);
		if (result != GENIUSDB_OK) {
			goto done;
		}
		for (uint32_t j = 0; j < count; j++, i++) {
			if (i % GENIUSDB_CANCEL_ROWS == 0 && geniusdb_read_cancelled(read)) {
				result = GENIUSDB_CANCELLED;
				goto done;
			}
			uint32_t next = geniusdb_u32(buffer + j * 4);
			if ((!i && next) || next > h[10] || (i && (next < previous || next - previous < 6))) {
				result = geniusdb_bad_blob("path offsets");
				goto done;
			}
			if (i) {
				const unsigned char *record = (const unsigned char *)graph->path_pool + previous;
				uint32_t bytes = geniusdb_u32(record);
				if ((uint64_t)bytes + 5 != next - previous || record[next - previous - 1] || !geniusdb_utf8(record + 4, bytes, read)) {
					result = geniusdb_bad_blob("path length, terminator or UTF-8");
					goto done;
				}
				graph->paths[i - 1] = (const char *)record + 4;
				if (i > 1 && geniusdb_compare_paths(graph->paths[i - 2], graph->paths[i - 1]) >= 0) {
					result = geniusdb_bad_blob("path order or duplicate NOCASE identity");
					goto done;
				}
			}
			previous = next;
		}
	}
	if (previous != h[10]) {
		result = geniusdb_bad_blob("path offset sentinel");
		goto done;
	}
	// Decode fixed-width records in bounded batches; never cast disk bytes to
	// native structs, even on little-endian targets with compatible alignment.
	const uint32_t counts[] = {h[6], h[6], h[7], h[8], h[9]};
	const unsigned widths[] = {36, 8, 8, 12, 4};
	uint32_t edge_cursor = 0, neighbour_cursor = 0, group = 0;
	for (unsigned section = 0; section < 5; section++) {
		for (uint32_t i = 0; i < counts[section];) {
			uint32_t count = counts[section] - i;
			unsigned width = widths[section];
			if (count > GENIUSDB_READ_BYTES / width) {
				count = GENIUSDB_READ_BYTES / width;
			}
			result = geniusdb_read_blob_bytes(blob, buffer, h[14 + section] + (size_t)i * width, (size_t)count * width, read);
			if (result != GENIUSDB_OK) {
				goto done;
			}
			for (uint32_t j = 0; j < count; j++, i++) {
				if (i % GENIUSDB_CANCEL_ROWS == 0 && geniusdb_read_cancelled(read)) {
					result = GENIUSDB_CANCELLED;
					goto done;
				}
				const unsigned char *p = buffer + j * width;
				if (section == 0) {
					graph->meta[i] = (geniusdb_meta_t){geniusdb_i32(p), geniusdb_i64(p + 4), geniusdb_i64(p + 12), geniusdb_i64(p + 20), geniusdb_i64(p + 28)};
				} else if (section == 1) {
					uint32_t first = geniusdb_u32(p), n = geniusdb_u32(p + 4);
					if (first != edge_cursor || n > h[7] - edge_cursor) {
						result = geniusdb_bad_blob("adjacency ranges");
						goto done;
					}
					graph->sims[i] = (geniusdb_range_t){(int32_t)first, (int32_t)n};
					edge_cursor += n;
				} else if (section == 2) {
					uint32_t source = geniusdb_u32(p), other = geniusdb_u32(p + 4);
					if (source >= h[6] || other >= h[6] || i < (uint32_t)graph->sims[source].first || i - (uint32_t)graph->sims[source].first >= (uint32_t)graph->sims[source].count) {
						result = geniusdb_bad_blob("edge index or range owner");
						goto done;
					}
					graph->edges[i] = (genius_edge_t){(int32_t)source, (int32_t)other};
				} else if (section == 3) {
					int32_t cluster = geniusdb_i32(p);
					uint32_t first = geniusdb_u32(p + 4), n = geniusdb_u32(p + 8);
					if ((i && cluster <= graph->clusters[i - 1].cluster) || first != neighbour_cursor || !n || n > h[9] - neighbour_cursor) {
						result = geniusdb_bad_blob("cluster order or neighbour ranges");
						goto done;
					}
					graph->clusters[i] = (geniusdb_cluster_t){cluster, {(int32_t)first, (int32_t)n}};
					neighbour_cursor += n;
				} else {
					while (group < h[8] && i >= (uint32_t)graph->clusters[group].range.first + (uint32_t)graph->clusters[group].range.count) {
						group++;
					}
					int32_t neighbour = geniusdb_i32(p);
					if (group >= h[8] || (i > (uint32_t)graph->clusters[group].range.first && neighbour <= graph->neighbours[i - 1])) {
						result = geniusdb_bad_blob("neighbour order or group coverage");
						goto done;
					}
					graph->neighbours[i] = neighbour;
				}
			}
		}
	}
	if (edge_cursor != h[7] || neighbour_cursor != h[9]) {
		result = geniusdb_bad_blob("incomplete adjacency or neighbour coverage");
	}
done:
	free(buffer);
	database.bytes -= temporary;
	return geniusdb_read_cancelled(read) ? GENIUSDB_CANCELLED : result;
}

static geniusdb_result_t geniusdb_read_binary(sqlite3 *db, geniusdb_graph_t *graph, bool *found, const geniusdb_read_t *read) {
	sqlite3_stmt *stmt = NULL;
	*found = false;
	if (sqlite3_prepare_v2(db, "SELECT type FROM sqlite_master WHERE name='GENIUS_SNAPSHOT' COLLATE NOCASE", -1, &stmt, NULL) != SQLITE_OK) {
		return geniusdb_bad_blob("cannot inspect schema");
	}
	int rc = sqlite3_step(stmt);
	if (rc == SQLITE_DONE) {
		sqlite3_finalize(stmt);
		return GENIUSDB_OK;
	}
	*found = true;
	const unsigned char *type = rc == SQLITE_ROW ? sqlite3_column_text(stmt, 0) : NULL;
	bool table = type && strcmp((const char *)type, "table") == 0;
	sqlite3_finalize(stmt);
	if (!table) {
		return geniusdb_bad_blob("GENIUS_SNAPSHOT is not a table");
	}
	// Do not materialize an unbounded, malformed generation column just to
	// reject its length. The graph payload itself is only read incrementally.
	if (sqlite3_prepare_v2(db, "SELECT id,typeof(data),length(data),typeof(generation),length(CAST(generation AS BLOB)),CASE WHEN length(CAST(generation AS BLOB)) BETWEEN 1 AND 4096 THEN generation END FROM GENIUS_SNAPSHOT", -1, &stmt, NULL) != SQLITE_OK) {
		return geniusdb_bad_blob("row schema");
	}
	char generation[GENIUSDB_GENERATION_BYTES + 1];
	size_t bytes = 0;
	geniusdb_result_t result = GENIUSDB_INVALID;
	rc = sqlite3_step(stmt);
	const unsigned char *data_type = rc == SQLITE_ROW ? sqlite3_column_text(stmt, 1) : NULL;
	const unsigned char *generation_type = rc == SQLITE_ROW ? sqlite3_column_text(stmt, 3) : NULL;
	if (!data_type || !generation_type || sqlite3_column_type(stmt, 0) != SQLITE_INTEGER || sqlite3_column_int64(stmt, 0) != 0 || strcmp((const char *)data_type, "blob") || strcmp((const char *)generation_type, "text")) {
		result = geniusdb_bad_blob("expected one id=0 row with TEXT generation and BLOB data");
		goto done;
	}
	int64_t length = sqlite3_column_int64(stmt, 2), gen_length = sqlite3_column_int64(stmt, 4);
	if (length < GENIUSDB_HEADER_BYTES || gen_length <= 0 || gen_length > GENIUSDB_GENERATION_BYTES) {
		result = geniusdb_bad_blob("BLOB or generation length");
		goto done;
	}
	if (length > (int64_t)GENIUSDB_MAX_SNAPSHOT_BYTES) {
		result = GENIUSDB_LIMIT;
		fprintf(stderr, "genius: binary snapshot exceeds byte limit\n");
		goto done;
	}
	const void *text = sqlite3_column_text(stmt, 5);
	if (!text || sqlite3_column_bytes(stmt, 5) != gen_length) {
		result = geniusdb_bad_blob("generation text");
		goto done;
	}
	bytes = (size_t)gen_length;
	memcpy(generation, text, bytes);
	generation[bytes] = '\0';
	result = sqlite3_step(stmt) == SQLITE_DONE ? GENIUSDB_OK : geniusdb_bad_blob("multiple rows");
done:
	sqlite3_finalize(stmt);
	if (result == GENIUSDB_OK) {
		sqlite3_blob *blob = NULL;
		if (sqlite3_blob_open(db, "main", "GENIUS_SNAPSHOT", "data", 0, 0, &blob) != SQLITE_OK) {
			return geniusdb_bad_blob("cannot open incremental BLOB");
		}
		result = geniusdb_decode_blob(blob, generation, bytes, graph, read);
		int closed = sqlite3_blob_close(blob);
		if (closed != SQLITE_OK && result == GENIUSDB_OK) {
			result = geniusdb_bad_blob("cannot close BLOB");
		}
	}
	return result;
}

bool geniusdb_open(const char *path) {
	if (!path || !path[0] || strlen(path) >= sizeof(database.path)) {
		fprintf(stderr, "genius: invalid database path\n");
		return false;
	}
	pthread_mutex_lock(&database.lock);
	if (database.path[0]) {
		pthread_mutex_unlock(&database.lock);
		return true;
	}
	struct stat info;
	bool ok = stat(path, &info) == 0;
	if (!ok && errno != ENOENT && errno != ENOTDIR) {
		fprintf(stderr, "genius: cannot stat %s: %s\n", path, strerror(errno));
	}
	if (ok && !S_ISREG(info.st_mode)) {
		fprintf(stderr, "genius: database is not a regular file\n");
		ok = false;
	}
	if (ok) {
		snprintf(database.path, sizeof(database.path), "%s", path);
		atomic_fetch_add(&database.revision, 1);
		atomic_store(&database.closing, false);
		atomic_store(&database.available, true);
	}
	pthread_mutex_unlock(&database.lock);
	return ok;
}

void geniusdb_close(void) {
	atomic_store(&database.closing, true);
	atomic_store(&database.available, false);
	// Invalidate workers immediately, before waiting for any in-flight I/O.
	atomic_fetch_add(&database.revision, 1);
	pthread_mutex_lock(&database.lock);
	database.path[0] = '\0';
	geniusdb_release_graph(database.cache);
	database.cache = NULL;
	pthread_mutex_unlock(&database.lock);
}

bool geniusdb_available(void) { return atomic_load(&database.available) && !atomic_load(&database.closing); }

geniusdb_result_t geniusdb_load_snapshot(geniusdb_snapshot_t **out, const atomic_bool *cancel) {
	if (!out) {
		fprintf(stderr, "genius: snapshot output is missing\n");
		return GENIUSDB_INVALID;
	}
	*out = NULL;
	pthread_mutex_lock(&database.lock);
	geniusdb_read_t read = {.cancel = cancel, .revision = atomic_load(&database.revision)};
	geniusdb_result_t result = GENIUSDB_UNAVAILABLE;
	sqlite3 *db = NULL;
	geniusdb_snapshot_t *snapshot = NULL;
	geniusdb_graph_t *graph = NULL;
	if (!database.path[0] || geniusdb_read_cancelled(&read)) {
		goto done;
	}
	result = GENIUSDB_OK;
	if (database.cache) {
		graph = database.cache;
		graph->references++;
	} else {
		size_t bytes = 0;
		graph = geniusdb_allocate_rows(&bytes, 1, sizeof(*graph), &result);
		if (!graph) {
			goto done;
		}
		graph->bytes = bytes;
		graph->references = 1;
		if (sqlite3_open_v2(database.path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
			result = GENIUSDB_UNAVAILABLE;
			goto done;
		}
		sqlite3_progress_handler(db, GENIUSDB_CANCEL_INSTRUCTIONS, geniusdb_interrupt_read, &read);
		if (sqlite3_exec(db, "PRAGMA cache_size=-128; BEGIN", NULL, NULL, NULL) != SQLITE_OK) {
			result = GENIUSDB_INVALID;
			goto done;
		}
		bool binary = false;
		result = geniusdb_read_binary(db, graph, &binary, &read);
		if (result == GENIUSDB_OK && !binary) {
			result = geniusdb_read_rows(db, graph, &read);
		}
		if (result == GENIUSDB_OK && sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
			result = GENIUSDB_INVALID;
		}
	}
done:
	if (db) {
		if (sqlite3_errcode(db) == SQLITE_NOMEM) {
			result = GENIUSDB_NOMEM;
		}
		if (result != GENIUSDB_OK && !geniusdb_read_cancelled(&read)) {
			fprintf(stderr, "genius: snapshot read failed (%d): %s\n", result, sqlite3_errmsg(db));
		}
		int closed = sqlite3_close(db);
		if (closed != SQLITE_OK) {
			fprintf(stderr, "genius: cannot close snapshot reader (%d)\n", closed);
			sqlite3_close_v2(db);
			result = GENIUSDB_INVALID;
		}
	}
	if (result == GENIUSDB_OK) {
		size_t bytes = 0;
		snapshot = geniusdb_allocate_rows(&bytes, 1, sizeof(*snapshot), &result);
		if (snapshot) {
			snapshot->bytes = bytes;
			snapshot->revision = read.revision;
			snapshot->graph = graph;
			graph = NULL; // Transfer this request's reference to its wrapper.
			snapshot->meta = geniusdb_allocate_rows(&snapshot->bytes, (size_t)snapshot->graph->path_count, sizeof(*snapshot->meta), &result);
		}
	}
	if (result == GENIUSDB_OK) {
		for (int32_t i = 0; i < snapshot->graph->path_count; i++) {
			if (geniusdb_read_cancelled(&read)) {
				result = GENIUSDB_CANCELLED;
				break;
			}
			bool indexed;
			if (!library_get_indexed(snapshot->graph->paths[i], &indexed)) {
				fprintf(stderr, "genius: cannot read index membership\n");
				result = GENIUSDB_INVALID;
				break;
			}
			const geniusdb_meta_t *meta = &snapshot->graph->meta[i];
			snapshot->meta[i] = (genius_meta_t){.cluster = meta->cluster, .k1 = meta->k1, .k2 = meta->k2, .k3 = meta->k3, .k20 = meta->k20, .present = indexed};
		}
	}
	if (geniusdb_read_cancelled(&read)) {
		result = GENIUSDB_CANCELLED;
	}
	if (result == GENIUSDB_OK) {
		if (!database.cache) {
			database.cache = snapshot->graph;
			database.cache->references++;
		}
		*out = snapshot;
	} else {
		fprintf(stderr, "genius: snapshot unavailable (reason %d)\n", result);
		geniusdb_release_snapshot(snapshot);
	}
	geniusdb_release_graph(graph);
	pthread_mutex_unlock(&database.lock);
	return result;
}

void geniusdb_free_snapshot(geniusdb_snapshot_t *snapshot) {
	if (!snapshot) {
		return;
	}
	pthread_mutex_lock(&database.lock);
	geniusdb_release_snapshot(snapshot);
	pthread_mutex_unlock(&database.lock);
}

bool geniusdb_snapshot_current(const geniusdb_snapshot_t *snapshot) { return snapshot && !atomic_load(&database.closing) && snapshot->revision == atomic_load(&database.revision); }

size_t geniusdb_snapshot_bytes(const geniusdb_snapshot_t *snapshot) { return snapshot ? snapshot->bytes + snapshot->graph->bytes : 0; }
int32_t geniusdb_path_count(const geniusdb_snapshot_t *snapshot) { return snapshot ? snapshot->graph->path_count : 0; }

int32_t geniusdb_find_path(const geniusdb_snapshot_t *snapshot, const char *path) {
	if (!snapshot || !path || !path[0]) {
		return -1;
	}
	int32_t lo = 0, hi = snapshot->graph->path_count;
	while (lo < hi) {
		int32_t mid = lo + (hi - lo) / 2;
		int order = geniusdb_compare_paths(path, snapshot->graph->paths[mid]);
		if (!order) {
			return mid;
		}
		if (order < 0) {
			hi = mid;
		} else {
			lo = mid + 1;
		}
	}
	return -1;
}

const char *geniusdb_get_path(const geniusdb_snapshot_t *snapshot, int32_t id) { return snapshot && id >= 0 && id < snapshot->graph->path_count ? snapshot->graph->paths[id] : NULL; }

const genius_meta_t *geniusdb_get_meta(const geniusdb_snapshot_t *snapshot, int32_t id) { return snapshot && id >= 0 && id < snapshot->graph->path_count && snapshot->meta[id].present ? &snapshot->meta[id] : NULL; }

genius_sim_list_t geniusdb_get_sims(const geniusdb_snapshot_t *snapshot, int32_t id) {
	if (!snapshot || id < 0 || id >= snapshot->graph->path_count || !snapshot->graph->sims[id].count) {
		return (genius_sim_list_t){0};
	}
	geniusdb_range_t range = snapshot->graph->sims[id];
	return (genius_sim_list_t){snapshot->graph->edges + range.first, range.count};
}

genius_cluster_set_t geniusdb_get_neighbours(const geniusdb_snapshot_t *snapshot, int32_t cluster) {
	int32_t lo = 0, hi = snapshot ? snapshot->graph->cluster_count : 0;
	while (lo < hi) {
		int32_t mid = lo + (hi - lo) / 2;
		const geniusdb_cluster_t *group = &snapshot->graph->clusters[mid];
		if (cluster == group->cluster) {
			return (genius_cluster_set_t){snapshot->graph->neighbours + group->range.first, group->range.count};
		}
		if (cluster < group->cluster) {
			hi = mid;
		} else {
			lo = mid + 1;
		}
	}
	return (genius_cluster_set_t){0};
}

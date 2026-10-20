/*
 * GENIUS_SNAPSHOT binary contract, version 1 (producer independent).
 *
 * SQLite container:
 *   CREATE TABLE GENIUS_SNAPSHOT(
 *       id INTEGER PRIMARY KEY CHECK(id = 0), generation TEXT, data BLOB);
 * Exactly one row, id 0. generation is nonempty UTF-8 (at most 4096 bytes),
 * identifies source/schema/algorithm/content, and equals the embedded string.
 * Publish a complete replacement database atomically. A mounted graph remains
 * cached until geniusdb_close(); replacement takes effect on the next mount.
 *
 * All integers are little-endian, with the explicit widths below. Signed
 * integers use two's complement. There are no native structs, pointers, or
 * implicit alignment/padding bytes. Offsets are unsigned 32-bit byte offsets
 * from the beginning of data, except path offsets, which are pool-relative.
 *
 * Header (80 bytes):
 *    0  magic[8] = {'S','O','N','I','X','G','S',0}
 *    8  u32 format_version = 1
 *   12  u32 header_bytes = 80
 *   16  u32 total_bytes (must equal the BLOB length)
 *   20  u32 generation_bytes (excludes any terminator)
 *   24  u32 path_count; 28 u32 edge_count
 *   32  u32 cluster_group_count; 36 u32 neighbour_count
 *   40  u32 path_pool_bytes
 *   44  u32 generation_offset; 48 u32 path_offsets_offset
 *   52  u32 path_pool_offset; 56 u32 metadata_offset
 *   60  u32 adjacency_offset; 64 u32 edges_offset
 *   68  u32 cluster_groups_offset; 72 u32 neighbours_offset
 *   76  u32 reserved = 0
 *
 * Sections occur in exactly this order, contiguously, without gaps or tails:
 *   generation: generation_bytes UTF-8 bytes, no NUL or terminator.
 *   path offsets: (path_count + 1) u32 offsets. First is 0, last equals
 *       path_pool_bytes. Each interval contains exactly one path record.
 *   path pool: each record is u32 UTF-8 byte length, that many nonempty UTF-8
 *       bytes without embedded NUL, then one zero byte. The trailing zero
 *       permits borrowed C strings. Paths are strictly increasing under
 *       SQLite's ASCII-only NOCASE comparison; retain metadata's spelling.
 *   metadata: path_count records of 36 bytes: s32 cluster, then s64 k1, k2,
 *       k3, k20. All paths have static metadata; membership is absent.
 *   adjacency: path_count records of 8 bytes: u32 first, u32 count. Ranges
 *       partition the entire edge array in path order, including empty ranges
 *       whose first equals the running edge cursor.
 *   edges: edge_count records of 8 bytes: u32 path, u32 other (path indices).
 *       Each range's source is its owning path; retain ascending SQL rank
 *       order within a range. Rank values themselves are not stored.
 *   cluster groups: cluster_group_count records of 12 bytes: s32 cluster,
 *       u32 first, u32 count. Cluster IDs strictly increase. Nonempty ranges
 *       partition the neighbour array in group order.
 *   neighbours: neighbour_count s32 cluster IDs, strictly increasing within
 *       each group. These are cluster IDs, not path indices; a referenced
 *       cluster need not itself have an outgoing group.
 * Counts, path indices and range values must fit INT32_MAX. Zero paths,
 * edges, or groups are allowed (zero groups requires zero neighbours).
 * Unknown versions, malformed sections or a missing/invalid row are errors.
 * Only absence of the table selects the legacy relational reader.
 */

#ifndef GENIUSDB_H
#define GENIUSDB_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Smart Mix data belongs to the card, independently of the scanned music index.
// Paths survive a rescan's row ids. Producers supply compact integer edges.
// SQLite NOCASE identity folds ASCII letters only.
//
// A mount lazily caches one immutable graph. Each worker owns a snapshot with
// fresh membership. Card removal invalidates its result, but borrowed
// rows remain alive until that worker releases the snapshot.
// No database descriptor survives a close or a completed snapshot read.
// ---------------------------------------------------------------------------

// Global bound: all live graphs (including retired mounts), request wrappers,
// private metadata, full path pools and temporary loader allocations combined.
#define GENIUSDB_MAX_SNAPSHOT_BYTES ((size_t)4 * 1024 * 1024)

typedef enum {
	GENIUSDB_OK,
	GENIUSDB_UNAVAILABLE,
	GENIUSDB_INVALID,
	GENIUSDB_LIMIT,
	GENIUSDB_NOMEM,
	GENIUSDB_CANCELLED,
} geniusdb_result_t;

typedef struct geniusdb_snapshot geniusdb_snapshot_t;

typedef struct {
	int32_t cluster;
	int64_t k1, k2, k3, k20;
	bool present;
} genius_meta_t;

typedef struct {
	int32_t path, other;
} genius_edge_t;

typedef struct {
	const genius_edge_t *edges;
	int32_t count;
} genius_sim_list_t;

typedef struct {
	const int32_t *neighbours;
	int32_t count;
} genius_cluster_set_t;

// Card lifecycle, from any thread. A missing file is ordinary unavailability.
// Close cancels and waits for an active read before the card is unmounted.
bool geniusdb_open(const char *path);
void geniusdb_close(void);
bool geniusdb_available(void);

// Background worker only. A failure never returns a partial snapshot.
// Cancellation is checked every 32 records, 1024 SQLite VM instructions,
// and before/after each bulk read of at most 16 KiB.
geniusdb_result_t geniusdb_load_snapshot(geniusdb_snapshot_t **out, const atomic_bool *cancel);
void geniusdb_free_snapshot(geniusdb_snapshot_t *snapshot);
bool geniusdb_snapshot_current(const geniusdb_snapshot_t *snapshot);
// Bytes reachable through this request, including its shared graph once.
size_t geniusdb_snapshot_bytes(const geniusdb_snapshot_t *snapshot);
int32_t geniusdb_path_count(const geniusdb_snapshot_t *snapshot);

// Memory-only accessors. Returned pointers are borrowed from the snapshot.
int32_t geniusdb_find_path(const geniusdb_snapshot_t *snapshot, const char *path);
const char *geniusdb_get_path(const geniusdb_snapshot_t *snapshot, int32_t id);
const genius_meta_t *geniusdb_get_meta(const geniusdb_snapshot_t *snapshot, int32_t id);
genius_sim_list_t geniusdb_get_sims(const geniusdb_snapshot_t *snapshot, int32_t id);
genius_cluster_set_t geniusdb_get_neighbours(const geniusdb_snapshot_t *snapshot, int32_t cluster);

#endif // GENIUSDB_H

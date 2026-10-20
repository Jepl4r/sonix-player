#ifndef GENIUS_H
#define GENIUS_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "src/system/library/geniusdb.h"

// ---------------------------------------------------------------------------
// A Smart Mix is an ordered answer to a walk through the similarity graph.
// It needs a path list because its spacing rules and random choices cannot be
// expressed as a library sort. Paths also survive the row ids of a rescan.
//
// The generator consumes a memory snapshot held by its worker. It owns no
// database connection and writes no playlist or playback state. The caller
// receives an independent path list to hand to the playback queue.
// ---------------------------------------------------------------------------

typedef struct {
	const char **paths;
	int count;
} genius_mix_t;

typedef enum {
	GENIUS_OK,
	GENIUS_NO_DATA,
	GENIUS_TOO_FEW,
	GENIUS_BUDGET,
	GENIUS_CANCELLED,
	GENIUS_NOMEM,
	GENIUS_INVALID,
} genius_result_t;

#define GENIUS_MAX_NODES 4096
#define GENIUS_MAX_EDGES 131072
// Snapshot, traversal arrays, rule state and independently owned output.
#define GENIUS_MAX_RETAINED_BYTES ((size_t)6 * 1024 * 1024)
#define GENIUS_CANCEL_INTERVAL 32

// Background worker only.
// The seed counts toward the target of ten and minimum of two. Failures leave
// out empty; a successful list stays valid after the snapshot is released.
// Cancellation is checked at each node and every 32 edges or candidates.
genius_result_t genius_mix_build(const geniusdb_snapshot_t *snapshot, const char *seed_path, unsigned seed, const atomic_bool *cancel, genius_mix_t *out);
void genius_mix_free(genius_mix_t *mix);

#endif // GENIUS_H

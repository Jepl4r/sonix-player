#include "src/system/library/genius.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/system/library/geniusdb.h"

#define GENIUS_TARGET 10
#define GENIUS_MINIMUM 2

typedef enum { GENIUS_RULE_CLUSTER, GENIUS_RULE_DISTANCE, GENIUS_RULE_JITTER } genius_rule_kind_t;

typedef struct {
	genius_rule_kind_t kind;
	int field;									   /* GENIUS_RULE_DISTANCE: which key */
	int floor, distance, jitter;				   /* GENIUS_RULE_DISTANCE */
	int thr_first, thr_later;					   /* GENIUS_RULE_JITTER */
} genius_rule_t;

static const genius_rule_t GENIUS_RULES[] = {
	{GENIUS_RULE_CLUSTER, .field = 0}, {GENIUS_RULE_DISTANCE, .field = 1, .floor = 2, .distance = 6, .jitter = 6}, {GENIUS_RULE_DISTANCE, .field = 2, .floor = 2, .distance = 10, .jitter = 6}, {GENIUS_RULE_DISTANCE, .field = 3, .floor = 3, .distance = 10, .jitter = 6}, {GENIUS_RULE_JITTER, .thr_first = 70, .thr_later = 100}, {GENIUS_RULE_DISTANCE, .field = 20, .floor = 50, .distance = 10, .jitter = 10},
};

#define GENIUS_RULE_COUNT (sizeof(GENIUS_RULES) / sizeof(GENIUS_RULES[0]))

typedef struct {
	int32_t *ids;
	int32_t count;
} genius_id_list_t;

typedef struct {
	int64_t key;
	int count;
} genius_key_counter_t;

typedef struct {
	genius_key_counter_t counters[GENIUS_TARGET];
	int count, distance, seen;
} genius_rule_state_t;

// Each background build owns its traversal and borrows one immutable snapshot.
typedef struct {
	const geniusdb_snapshot_t *snapshot;
	const atomic_bool *cancel;
	int32_t seed;
	genius_cluster_set_t allowed;
	genius_id_list_t candidates, queue;
	unsigned char *seen, *used;
	int32_t queue_index, capacity;
	int output_index;
	int32_t output[GENIUS_TARGET];
	int count, round;
	unsigned nodes, edges, discovered;
	size_t bytes;
	uint32_t random;
	genius_rule_state_t states[GENIUS_RULE_COUNT];
	genius_result_t result;
} genius_playlist_t;

static bool genius_check_cancel(genius_playlist_t *playlist) {
	if ((playlist->cancel && atomic_load(playlist->cancel)) || !geniusdb_snapshot_current(playlist->snapshot)) {
		playlist->result = GENIUS_CANCELLED;
	}
	return playlist->result != GENIUS_OK;
}

static bool genius_reserve_bytes(genius_playlist_t *playlist, size_t count, size_t size) {
	if ((size && count > SIZE_MAX / size) || playlist->bytes > GENIUS_MAX_RETAINED_BYTES || count * size > GENIUS_MAX_RETAINED_BYTES - playlist->bytes) {
		playlist->result = GENIUS_BUDGET;
		return false;
	}
	playlist->bytes += count * size;
	return true;
}

static bool genius_test_bit(const unsigned char *bits, int32_t id) { return (bits[(unsigned)id / 8] & (1u << ((unsigned)id % 8))) != 0; }
static void genius_set_bit(unsigned char *bits, int32_t id) { bits[(unsigned)id / 8] |= 1u << ((unsigned)id % 8); }

// sub_82392F8: s = 1103515245u * s + 12345u, with uint32 wrap;
// r = (2u * s) >> 17 (15 bits, 0..32767).
// sub_8146ED0 uses software modulo helper sub_81718FC: idx = ((r + 1) % n) + 1.
// sub_814E7E6 clamps idx >= n to n - 1; n == 0 returns 0 without a draw.
// For n > 1, index 0 is unreachable and index n - 1 has double weight.
// Draw count and order are part of the contract.
static uint32_t genius_lcg(uint32_t *state) {
	*state = UINT32_C(1103515245) * *state + UINT32_C(12345);
	return (UINT32_C(2) * *state) >> 17;
}

static unsigned genius_pick(uint32_t *state, unsigned n) {
	if (n == 0) {
		return 0;
	}
	unsigned idx = ((genius_lcg(state) + 1) % n) + 1;
	return (idx >= n) ? n - 1 : idx;
}

static int64_t genius_meta_key(const genius_meta_t *meta, int field) {
	switch (field) {
	case 1:
		return meta->k1;
	case 2:
		return meta->k2;
	case 3:
		return meta->k3;
	case 20:
		return meta->k20;
	default:
		return 0;
	}
}

static bool genius_rule_in_effect(const genius_rule_t *rule, const genius_rule_state_t *state) { return rule->kind != GENIUS_RULE_JITTER || state->seen <= 1; }

// 0 accepts, 1 leaves a candidate retryable, and 2 marks it permanently seen.
static int genius_check_rule(genius_playlist_t *playlist, const genius_rule_t *rule, const genius_rule_state_t *state, const genius_meta_t *meta) {
	switch (rule->kind) {
	case GENIUS_RULE_CLUSTER: {
		int lo = 0, hi = playlist->allowed.count;
		while (lo < hi) {
			int mid = lo + (hi - lo) / 2;
			int32_t cluster = playlist->allowed.neighbours[mid];
			if (meta->cluster == cluster) {
				return 0;
			}
			if (meta->cluster < cluster) {
				hi = mid;
			} else {
				lo = mid + 1;
			}
		}
		return 2;
	}
	case GENIUS_RULE_DISTANCE: {
		int64_t key = genius_meta_key(meta, rule->field);
		for (int i = 0; i < state->count; i++) {
			if (state->counters[i].key == key) {
				return state->counters[i].count < state->distance ? 1 : 0;
			}
		}
		return 0;
	}
	case GENIUS_RULE_JITTER: {
		int threshold = state->seen ? rule->thr_later : rule->thr_first;
		return (int)genius_pick(&playlist->random, 100) > threshold ? 1 : 0;
	}
	}
	return 0;
}

static int genius_eval_chain(genius_playlist_t *playlist, const genius_meta_t *meta) {
	for (size_t i = 0; i < GENIUS_RULE_COUNT; i++) {
		if (genius_rule_in_effect(&GENIUS_RULES[i], &playlist->states[i])) {
			int result = genius_check_rule(playlist, &GENIUS_RULES[i], &playlist->states[i], meta);
			if (result) {
				return result;
			}
		}
	}
	return 0;
}

static void genius_consume_track(genius_playlist_t *playlist, int32_t id) {
	const genius_meta_t *meta = geniusdb_get_meta(playlist->snapshot, id);
	if (!meta || playlist->count >= GENIUS_TARGET) {
		playlist->result = GENIUS_INVALID;
		return;
	}
	playlist->round++;
	for (size_t i = 0; i < GENIUS_RULE_COUNT; i++) {
		const genius_rule_t *rule = &GENIUS_RULES[i];
		if (rule->kind != GENIUS_RULE_DISTANCE) {
			continue;
		}
		genius_rule_state_t *state = &playlist->states[i];
		int64_t key = genius_meta_key(meta, rule->field);
		bool found = false;
		for (int j = 0; j < state->count; j++) {
			genius_key_counter_t *counter = &state->counters[j];
			if (counter->key == key) {
				counter->count = 2;
				found = true;
			} else {
				counter->count++;
			}
		}
		if (!found) {
			if (state->count >= GENIUS_TARGET) {
				fprintf(stderr, "genius: distance rule counter limit exceeded\n");
				playlist->result = GENIUS_BUDGET;
				return;
			}
			state->counters[state->count++] = (genius_key_counter_t){.key = key, .count = 1};
		}
	}
	playlist->output[playlist->count++] = id;
}

static void genius_on_accept(genius_playlist_t *playlist) {
	for (size_t i = 0; i < GENIUS_RULE_COUNT; i++) {
		const genius_rule_t *rule = &GENIUS_RULES[i];
		if (rule->kind == GENIUS_RULE_DISTANCE) {
			playlist->states[i].distance = rule->distance + (rule->jitter ? (int)genius_pick(&playlist->random, (unsigned)rule->jitter) : 0);
		}
	}
}

static int genius_relax_rule(const genius_rule_t *rule, genius_rule_state_t *state) {
	if (rule->kind == GENIUS_RULE_DISTANCE) {
		if (rule->floor >= state->distance) {
			return 0;
		}
		state->distance = rule->floor + 1 >= state->distance ? rule->floor : state->distance - 1;
		return 1;
	}
	if (rule->kind == GENIUS_RULE_JITTER && !state->seen) {
		state->seen = 1;
		return 1;
	}
	return 0;
}

static bool genius_expand_track(genius_playlist_t *playlist, int32_t id) {
	if (genius_check_cancel(playlist)) {
		return false;
	}
	if (playlist->nodes >= GENIUS_MAX_NODES) {
		playlist->result = GENIUS_BUDGET;
		return false;
	}
	playlist->nodes++;
	if (!geniusdb_get_meta(playlist->snapshot, id)) {
		return false;
	}
	genius_sim_list_t sims = geniusdb_get_sims(playlist->snapshot, id);
	bool got = false;
	for (int32_t i = 0; i < sims.count; i++) {
		if (i % GENIUS_CANCEL_INTERVAL == 0 && genius_check_cancel(playlist)) {
			return false;
		}
		if (playlist->edges >= GENIUS_MAX_EDGES || playlist->bytes > GENIUS_MAX_RETAINED_BYTES) {
			playlist->result = GENIUS_BUDGET;
			return false;
		}
		playlist->edges++;
		int32_t other = sims.edges[i].other;
		if (genius_test_bit(playlist->used, other)) {
			continue;
		}
		if (other != playlist->seed) {
			if (playlist->discovered >= GENIUS_MAX_NODES) {
				playlist->result = GENIUS_BUDGET;
				return false;
			}
			playlist->discovered++;
		}
		if (playlist->queue.count >= playlist->capacity || playlist->candidates.count >= playlist->capacity) {
			playlist->result = GENIUS_BUDGET;
			return false;
		}
		genius_set_bit(playlist->used, other);
		playlist->candidates.ids[playlist->candidates.count++] = other;
		playlist->queue.ids[playlist->queue.count++] = other;
		got = true;
	}
	return got;
}

static bool genius_pump_queue(genius_playlist_t *playlist) {
	bool got = false;
	while (playlist->queue_index < playlist->queue.count) {
		int32_t id = playlist->queue.ids[playlist->queue_index++];
		if (genius_check_cancel(playlist)) {
			return false;
		}
		if (genius_expand_track(playlist, id)) {
			got = true;
		}
		if (playlist->result != GENIUS_OK) {
			return false;
		}
	}
	while (playlist->output_index < playlist->count) {
		int32_t id = playlist->output[playlist->output_index++];
		if (genius_expand_track(playlist, id)) {
			got = true;
		}
		if (playlist->result != GENIUS_OK) {
			return false;
		}
	}
	return got;
}

static bool genius_next_track(genius_playlist_t *playlist) {
	if (genius_check_cancel(playlist)) {
		return false;
	}
	if (!playlist->round) {
		genius_consume_track(playlist, playlist->seed);
		genius_pump_queue(playlist);
		return playlist->result == GENIUS_OK;
	}
	for (int guard = 0; guard < 5000; guard++) {
		for (int32_t i = 0; i < playlist->candidates.count; i++) {
			if (i % GENIUS_CANCEL_INTERVAL == 0 && genius_check_cancel(playlist)) {
				return false;
			}
			int32_t id = playlist->candidates.ids[i];
			if (genius_test_bit(playlist->seen, id)) {
				continue;
			}
			const genius_meta_t *meta = geniusdb_get_meta(playlist->snapshot, id);
			if (!meta) {
				genius_set_bit(playlist->seen, id);
				continue;
			}
			int result = genius_eval_chain(playlist, meta);
			if (!result) {
				genius_consume_track(playlist, id);
				genius_set_bit(playlist->seen, id);
				genius_on_accept(playlist);
				return playlist->result == GENIUS_OK;
			}
			if (result == 2) {
				genius_set_bit(playlist->seen, id);
			}
		}
		bool more = genius_pump_queue(playlist);
		if (playlist->result != GENIUS_OK) {
			return false;
		}
		int relaxed = 0;
		for (size_t i = 0; i < GENIUS_RULE_COUNT; i++) {
			if (!relaxed && genius_rule_in_effect(&GENIUS_RULES[i], &playlist->states[i])) {
				relaxed = genius_relax_rule(&GENIUS_RULES[i], &playlist->states[i]);
			}
		}
		if (!(relaxed | (more ? 1 : 0))) {
			return false;
		}
	}
	playlist->result = GENIUS_BUDGET;
	return false;
}

genius_result_t genius_mix_build(const geniusdb_snapshot_t *snapshot, const char *seed_path, unsigned seed, const atomic_bool *cancel, genius_mix_t *out) {
	if (!out) {
		fprintf(stderr, "genius: mix output is missing\n");
		return GENIUS_INVALID;
	}
	*out = (genius_mix_t){0};
	int32_t id = geniusdb_find_path(snapshot, seed_path);
	const genius_meta_t *meta = geniusdb_get_meta(snapshot, id);
	if (!meta) {
		fprintf(stderr, "genius: seed has no metadata\n");
		return GENIUS_NO_DATA;
	}
	genius_playlist_t playlist = {.snapshot = snapshot, .cancel = cancel, .seed = id, .random = (uint32_t)seed, .discovered = 1};
	playlist.bytes = geniusdb_snapshot_bytes(snapshot);
	int32_t paths = geniusdb_path_count(snapshot);
	playlist.capacity = paths < GENIUS_MAX_NODES ? paths : GENIUS_MAX_NODES;
	playlist.allowed = geniusdb_get_neighbours(snapshot, meta->cluster);
	size_t bits = ((size_t)paths + 7) / 8;
	if (genius_check_cancel(&playlist) || !genius_reserve_bytes(&playlist, 1, sizeof(playlist)) || !genius_reserve_bytes(&playlist, (size_t)playlist.capacity, 2 * sizeof(int32_t)) || !genius_reserve_bytes(&playlist, bits, 2)) {
		goto done;
	}
	playlist.candidates.ids = malloc((size_t)playlist.capacity * sizeof(int32_t));
	playlist.queue.ids = malloc((size_t)playlist.capacity * sizeof(int32_t));
	playlist.seen = calloc(bits, 1);
	playlist.used = calloc(bits, 1);
	if (!playlist.candidates.ids || !playlist.queue.ids || !playlist.seen || !playlist.used) {
		playlist.result = GENIUS_NOMEM;
		goto done;
	}
	for (size_t i = 0; i < GENIUS_RULE_COUNT; i++) {
		playlist.states[i].distance = GENIUS_RULES[i].distance;
	}
	while (playlist.count < GENIUS_TARGET && genius_next_track(&playlist)) {
	}
	if (playlist.result != GENIUS_OK || genius_check_cancel(&playlist)) {
		goto done;
	}
	if (playlist.count < GENIUS_MINIMUM) {
		playlist.result = GENIUS_TOO_FEW;
		goto done;
	}
	if (!genius_reserve_bytes(&playlist, (size_t)playlist.count, sizeof(*out->paths))) {
		goto done;
	}
	out->paths = calloc((size_t)playlist.count, sizeof(*out->paths));
	if (!out->paths) {
		playlist.result = GENIUS_NOMEM;
		goto done;
	}
	for (int i = 0; i < playlist.count; i++) {
		if (genius_check_cancel(&playlist)) {
			goto done;
		}
		const char *path = geniusdb_get_path(snapshot, playlist.output[i]);
		size_t size = strlen(path) + 1;
		if (!genius_reserve_bytes(&playlist, 1, size)) {
			goto done;
		}
		char *copy = malloc(size);
		if (!copy) {
			playlist.result = GENIUS_NOMEM;
			goto done;
		}
		memcpy(copy, path, size);
		out->paths[out->count++] = copy;
	}
done:
	free(playlist.candidates.ids);
	free(playlist.queue.ids);
	free(playlist.seen);
	free(playlist.used);
	if (playlist.result != GENIUS_OK) {
		fprintf(stderr, "genius: mix failed (reason %d, nodes %u, edges %u, bytes %zu)\n", playlist.result, playlist.nodes, playlist.edges, playlist.bytes);
		genius_mix_free(out);
	}
	return playlist.result;
}

void genius_mix_free(genius_mix_t *mix) {
	if (!mix) {
		return;
	}
	for (int i = 0; i < mix->count; i++) {
		free((void *)mix->paths[i]);
	}
	free(mix->paths);
	*mix = (genius_mix_t){0};
}

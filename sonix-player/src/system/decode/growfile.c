#include "growfile.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Longest wait for a chunk that never arrives. Past this the download is
// treated as dead: a track that ends early beats an audio thread blocked
// forever.
#define GROWFILE_WAIT_TIMEOUT_MS 20000

typedef struct {
	char path[512];
	long done;	// bytes written so far
	long total; // final size, 0 when the server did not say
	bool active;
	bool failed;
} slot_t;

static slot_t slots[GROWFILE_MAX];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t grew = PTHREAD_COND_INITIALIZER;

// A reader of a growing file can block on a chunk that never arrives, for up to
// twenty seconds. In that time stop, pause or another track may have been
// pressed, and the wait has to give up at once: an audio thread that ignores
// commands looks like a dead player.
//
// The playback layer registers the "is there a reason to stop waiting?"
// question; the wait asks it on every 100 ms slice.
static bool (*reader_abort_cb)(void);

void growfile_set_reader_abort_cb(bool (*cb)(void)) { reader_abort_cb = cb; }

// Call with the lock held.
static slot_t *find_locked(const char *path) {
	if (!path) {
		return NULL;
	}
	for (int i = 0; i < GROWFILE_MAX; i++) {
		if (slots[i].active && strcmp(slots[i].path, path) == 0) {
			return &slots[i];
		}
	}
	return NULL;
}

void growfile_announce(const char *path, long total) {
	if (!path) {
		return;
	}

	pthread_mutex_lock(&lock);

	slot_t *slot = find_locked(path);
	if (!slot) {
		for (int i = 0; i < GROWFILE_MAX; i++) {
			if (!slots[i].active) {
				slot = &slots[i];
				break;
			}
		}
	}
	if (slot) {
		snprintf(slot->path, sizeof(slot->path), "%s", path);
		slot->done = 0;
		slot->total = total;
		slot->active = true;
		slot->failed = false;
		// Log the exact path being tracked: it is the reference when a caller
		// asks whether a file is growing and gets told no for a file that is
		// very much downloading.
		fprintf(stderr, "growfile: following '%s' (%ld bytes)\n", path, total);
	} else {
		// No free slot: the file still grows, but readers will not know and
		// will stop at whatever is already on disk. Logged because the symptom
		// -- a track ending early -- points nowhere on its own.
		fprintf(stderr, "growfile: no slot for %s; its reader will not wait\n", path);
	}

	pthread_mutex_unlock(&lock);
}

void growfile_progress(const char *path, long done) {
	pthread_mutex_lock(&lock);
	slot_t *slot = find_locked(path);
	if (slot) {
		slot->done = done;
	}
	// Broadcast regardless: a reader waiting on a path that has since been
	// closed has to be able to find out.
	pthread_cond_broadcast(&grew);
	pthread_mutex_unlock(&lock);
}

void growfile_finish(const char *path, bool ok) {
	pthread_mutex_lock(&lock);
	slot_t *slot = find_locked(path);
	if (slot) {
		slot->active = false;
		slot->failed = !ok;
	}
	pthread_cond_broadcast(&grew);
	pthread_mutex_unlock(&lock);
}

bool growfile_prefix_is_growing(const char *prefix) {
	if (!prefix || !*prefix) {
		return false;
	}
	size_t len = strlen(prefix);

	pthread_mutex_lock(&lock);
	bool found = false;
	for (int i = 0; i < GROWFILE_MAX && !found; i++) {
		found = slots[i].active && strncmp(slots[i].path, prefix, len) == 0;
	}
	pthread_mutex_unlock(&lock);
	return found;
}

bool growfile_is_growing(const char *path) {
	pthread_mutex_lock(&lock);
	bool growing = find_locked(path) != NULL;
	pthread_mutex_unlock(&lock);
	return growing;
}

// ---------------------------------------------------------------------------
// the reader
// ---------------------------------------------------------------------------

struct growfile {
	FILE *f;
	char path[512];
	long pos;

	// Set whenever a read had to wait for data that had not been downloaded
	// yet: the signal that the reader has caught up with the download edge.
	// Playback needs it, because from outside a read that waits and returns
	// full looks like an ordinary slow read.
	bool starved;
};

growfile_t *growfile_open(const char *path) {
	if (!path) {
		return NULL;
	}

	FILE *f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}

	growfile_t *g = calloc(1, sizeof(*g));
	if (!g) {
		fclose(f);
		return NULL;
	}
	g->f = f;
	snprintf(g->path, sizeof(g->path), "%s", path);
	return g;
}

void growfile_close(growfile_t *g) {
	if (!g) {
		return;
	}
	fclose(g->f);
	free(g);
}

// Waits until at least `need` bytes are on disk. Returns how many there really
// are when it stops waiting, which can be fewer than `need` if the download
// ended -- successfully or not -- before getting there.
static long wait_for_bytes_timeout(growfile_t *g, long need, int timeout_ms) {
	pthread_mutex_lock(&lock);

	slot_t *slot = find_locked(g->path);
	if (!slot) {
		// Nobody is writing it, so the file is whatever it already is.
		pthread_mutex_unlock(&lock);
		return -1;
	}

	int waited_ms = 0;
	while (slot->active && slot->done < need) {
		// A pending command -- stop, pause, another track -- outranks any
		// missing chunk: give up immediately and let playback handle it.
		if (reader_abort_cb && reader_abort_cb()) {
			break;
		}

		struct timespec deadline;
		clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_nsec += 100 * 1000000L;
		if (deadline.tv_nsec >= 1000000000L) {
			deadline.tv_sec++;
			deadline.tv_nsec -= 1000000000L;
		}
		pthread_cond_timedwait(&grew, &lock, &deadline);

		waited_ms += 100;
		if (waited_ms >= timeout_ms) {
			if (timeout_ms >= 1000) {
				fprintf(stderr, "growfile: %s stalled for %d s at %ld bytes; giving up waiting\n", g->path,
						waited_ms / 1000, slot->done);
			}
			break;
		}

		// The slot may have been reused for another file during the wait; if it
		// no longer names this path there is nothing left to wait for.
		slot = find_locked(g->path);
		if (!slot) {
			break;
		}
	}

	long available = slot ? slot->done : -1;
	pthread_mutex_unlock(&lock);
	return available;
}

static long wait_for_bytes(growfile_t *g, long need) {
	return wait_for_bytes_timeout(g, need, GROWFILE_WAIT_TIMEOUT_MS);
}

size_t growfile_read(growfile_t *g, void *out, size_t bytes) {
	if (!g || !out || bytes == 0) {
		return 0;
	}

	size_t got = fread(out, 1, bytes, g->f);
	g->pos += (long)got;
	if (got == bytes) {
		return got;
	}

	// Short read: either the file ends here or the data has not arrived yet,
	// and only the writer knows which.
	//
	// If the file is still growing, reaching this point means the read position
	// has caught up with the download edge. That is recorded for playback
	// (growfile_take_starved): the read may still fill up by waiting, but the
	// fact that it had to wait is what lets playback stop and rebuild a buffer
	// instead of trickling.
	if (growfile_is_growing(g->path)) {
		g->starved = true;
	}
	while (got < bytes) {
		// A pending command ends the read at once, with whatever is in hand.
		// The check inside wait_for_bytes() only breaks a single wait; this loop
		// keeps going as long as even one byte arrives, so with a trickling
		// download it would otherwise run for minutes a byte at a time.
		if (reader_abort_cb && reader_abort_cb()) {
			break;
		}

		long need = g->pos + (long)(bytes - got);
		long available = wait_for_bytes(g, need);
		if (available < 0 || available <= g->pos) {
			break; // nobody is writing it any more, or it did not grow
		}

		// A short fread leaves the stream at end-of-file; without clearing it
		// every later read returns zero no matter how much data arrived.
		clearerr(g->f);
		if (fseek(g->f, g->pos, SEEK_SET) != 0) {
			break;
		}

		size_t more = fread((char *)out + got, 1, bytes - got, g->f);
		if (more == 0) {
			break;
		}
		got += more;
		g->pos += (long)more;
	}

	return got;
}

bool growfile_seek(growfile_t *g, long offset, int origin) {
	if (!g) {
		return false;
	}

	// A seek invalidates the starvation flag, which belonged to the previous
	// position: carrying it over would make playback wait for a download edge
	// that the new position is nowhere near.
	g->starved = false;

	long target = origin == 1 ? g->pos + offset : offset;
	if (target < 0) {
		return false;
	}

	// Seeking past what has been downloaded means either a fragment still on
	// its way or half the track missing. The first is worth waiting for, the
	// second is not: the waiter is the audio thread, and a decoder's binary
	// search asks for positions scattered across the whole file. Hence the
	// shorter seek timeout.
	if (growfile_is_growing(g->path)) {
		long available = wait_for_bytes_timeout(g, target, GROWFILE_SEEK_WAIT_MS);
		if (available >= 0 && available < target) {
			return false;
		}
	}

	clearerr(g->f);
	if (fseek(g->f, target, SEEK_SET) != 0) {
		return false;
	}
	g->pos = target;
	return true;
}

long growfile_tell(growfile_t *g) { return g ? g->pos : 0; }

bool growfile_take_starved(growfile_t *g) {
	if (!g || !g->starved) {
		return false;
	}
	g->starved = false;
	return true;
}

bool growfile_span(growfile_t *g, long *done, long *total) {
	if (!g) {
		return false;
	}
	pthread_mutex_lock(&lock);
	slot_t *slot = find_locked(g->path);
	bool growing = slot != NULL && slot->total > 0;
	if (growing) {
		if (done) {
			*done = slot->done;
		}
		if (total) {
			*total = slot->total;
		}
	}
	pthread_mutex_unlock(&lock);
	return growing;
}

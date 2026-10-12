#include "streamturn.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool busy;

void streamturn_acquire(void) {
	pthread_mutex_lock(&lock);
	while (busy) {
		pthread_cond_wait(&changed, &lock);
	}
	busy = true;
	pthread_mutex_unlock(&lock);
}

void streamturn_release(void) {
	pthread_mutex_lock(&lock);
	busy = false;
	// Broadcast, not signal: a waiter may be another download or somebody
	// parked in streamturn_wait_idle(), and waking just one leaves the other
	// stuck until someone else comes through.
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&lock);
}

// Five registrants -- Qobuz, Tidal, Subsonic, podcasts, Yandex -- plus margin.
// One that does not fit is refused with a log line rather than silently: its
// downloads would never give way to a tap on another service's track, and that
// is a fault worth seeing in the log.
//
// The table has its own lock: registration comes from the main thread at
// startup, from the card supervisor thread (inserting a card redoes set_root)
// and from the UI thread, while the readers are the pages' workers. Without it
// the count can become visible before the function it counts -- one preemption
// between two instructions -- and the call lands on a null pointer.
#define ABANDON_MAX 8
static pthread_mutex_t abandon_lock = PTHREAD_MUTEX_INITIALIZER;
static streamturn_abandon_fn abandon_fns[ABANDON_MAX];
static int abandon_count;

void streamturn_register_abandon(streamturn_abandon_fn fn) {
	if (!fn) {
		return;
	}
	pthread_mutex_lock(&abandon_lock);
	bool known = false;
	for (int i = 0; i < abandon_count && !known; i++) {
		known = abandon_fns[i] == fn;
	}
	bool full = false;
	if (!known) {
		if (abandon_count < ABANDON_MAX) {
			abandon_fns[abandon_count] = fn;
			abandon_count++;
		} else {
			full = true;
		}
	}
	pthread_mutex_unlock(&abandon_lock);
	if (full) {
		fprintf(stderr, "streamturn: no room for another abandon function (%d), not registered\n", ABANDON_MAX);
	}
}

void streamturn_abandon_all(void) {
	// The table is copied and the calls made outside the lock: an abandon
	// closes a socket under a blocked recv(), and must not do that while
	// holding off anyone trying to register.
	streamturn_abandon_fn copy[ABANDON_MAX];
	int count;

	pthread_mutex_lock(&abandon_lock);
	count = abandon_count;
	for (int i = 0; i < count; i++) {
		copy[i] = abandon_fns[i];
	}
	pthread_mutex_unlock(&abandon_lock);

	for (int i = 0; i < count; i++) {
		copy[i]();
	}
}

void streamturn_wait_idle(void) {
	pthread_mutex_lock(&lock);
	while (busy) {
		pthread_cond_wait(&changed, &lock);
	}
	pthread_mutex_unlock(&lock);
}

// ---------------------------------------------------------------------------
// resuming
// ---------------------------------------------------------------------------

static void set_active(const streamturn_resume_t *r, http_stream_t *value) {
	pthread_mutex_lock(r->active_lock);
	if (value || *r->active_stream == r->stream) {
		*r->active_stream = value;
	}
	pthread_mutex_unlock(r->active_lock);
}

bool streamturn_resume(const streamturn_resume_t *r, int *attempts) {
	// Out of view first, closed second: the other order is a wake on a
	// descriptor number that may already belong to someone else.
	set_active(r, NULL);
	http_stream_close(r->stream);

	if (!r->url || !r->url[0]) {
		return false;
	}

	while (*attempts < STREAM_RESUME_ATTEMPTS) {
		// The pause, a tenth of a second at a time: an abandon must not have
		// to wait it out while this download holds the turn.
		int tenths = 10 << *attempts;
		(*attempts)++;
		for (int i = 0; i < tenths; i++) {
			if (r->generation() != r->started_generation) {
				return false;
			}
			usleep(100 * 1000);
		}

		fprintf(stderr, "%s: connection lost at %ld bytes, resuming (attempt %d)\n", r->who, r->have, *attempts);
		if (!http_stream_open_range(r->stream, r->url, r->have, r->timeout_secs)) {
			continue;
		}

		// Another size is another file -- a re-encode, a podcast host
		// stitching in different adverts -- and splicing it onto what is
		// already there would be noise. Better to give up as before.
		if (r->total > 0 && r->stream->total_length > 0 && r->stream->total_length != r->total) {
			fprintf(stderr, "%s: the server now has %ld bytes instead of %ld, not resuming\n", r->who, r->stream->total_length, r->total);
			http_stream_close(r->stream);
			return false;
		}

		set_active(r, r->stream);
		if (r->generation() != r->started_generation) {
			set_active(r, NULL);
			http_stream_close(r->stream);
			return false;
		}

		if (r->stream->range_start != r->have) {
			fprintf(stderr, "%s: no ranges from this server, reading past %ld bytes\n", r->who, r->have);
			if (!http_stream_skip(r->stream, r->have - r->stream->range_start)) {
				set_active(r, NULL);
				http_stream_close(r->stream);
				continue;
			}
		}
		return true;
	}
	fprintf(stderr, "%s: giving up after %d attempts\n", r->who, *attempts);
	return false;
}

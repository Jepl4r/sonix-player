#include "yandexwave.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/system/core/utils.h"

// See yandexwave.h.

static long monotonic_ms(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

// ---------------------------------------------------------------------------
// state (GUI thread, and the worker for what it reports back: under `lock`)
// ---------------------------------------------------------------------------

// The next batch is asked for when this many tracks or fewer are left after
// the one playing: two tracks are five minutes or more, time enough for a
// request, its retries and the first track's download.
#define REFILL_AT 2

// The pause after a failed request, growing: a network that is down for a
// minute is not asked twelve times, one that blinked is asked again soon.
static const int RETRY_SECS[] = {5, 10, 20, 40, 60};
#define RETRY_STEPS ((int)(sizeof(RETRY_SECS) / sizeof(RETRY_SECS[0])))

// What the "queue" of a request names: the tracks most recently handed out,
// oldest first. Ten are plenty to keep the station from repeating itself.
#define RECEIVED_MAX 10
#define REF_MAX (YANDEX_ID_MAX * 2)

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool running;
static yandexwave_batch_cb batch_cb;
static void *batch_user;
static char received[RECEIVED_MAX][REF_MAX];
static int received_count;
// A request is out (or its batch is on its way into the queue).
static bool in_flight;
static int strikes;
static long next_try_ms;

// The track playing, as last seen by the watch (GUI thread only).
static char now_id[YANDEX_ID_MAX], now_album[YANDEX_ID_MAX];
static int now_duration;
static long heard_ms;
static long last_watch_ms;
static char disliked_id[YANDEX_ID_MAX];

// Call with `lock` held.
static void remember_received(const yandex_track_t *tracks, int count) {
	for (int i = 0; i < count; i++) {
		if (received_count == RECEIVED_MAX) {
			memmove(received[0], received[1], (RECEIVED_MAX - 1) * sizeof(received[0]));
			received_count--;
		}
		if (tracks[i].album_id[0]) {
			snprintf(received[received_count], sizeof(received[0]), "%s:%s", tracks[i].id, tracks[i].album_id);
		} else {
			snprintf(received[received_count], sizeof(received[0]), "%s", tracks[i].id);
		}
		received_count++;
	}
}

// ---------------------------------------------------------------------------
// the worker: a short queue of requests, in order
// ---------------------------------------------------------------------------

typedef enum { JOB_FEEDBACK, JOB_MORE } job_kind_t;

typedef struct {
	job_kind_t kind;
	yandex_wave_event_t event;
	char id[YANDEX_ID_MAX];
	char album[YANDEX_ID_MAX];
	int secs;
} job_t;

#define QUEUE_MAX 16
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static job_t queue[QUEUE_MAX];
static int queue_len;
static bool worker_started;

static void *worker_main(void *arg);

// Call with `lock` held. A full queue (the network gone for a long while)
// drops its oldest report, never a request for tracks: that one is what keeps
// the music going.
static void submit_locked(const job_t *job) {
	if (!worker_started) {
		pthread_t thread;
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setstacksize(&attr, 256 * 1024); // TLS, as the page's worker
		if (pthread_create(&thread, &attr, worker_main, NULL) == 0) {
			pthread_detach(thread);
			worker_started = true;
		}
		pthread_attr_destroy(&attr);
	}
	if (queue_len == QUEUE_MAX) {
		int drop = 0;
		while (drop < queue_len && queue[drop].kind != JOB_FEEDBACK) {
			drop++;
		}
		if (drop == queue_len) {
			return;
		}
		fprintf(stderr, "yandex: wave: report queue full, dropping a %s\n", queue[drop].kind == JOB_FEEDBACK ? "report" : "request");
		memmove(&queue[drop], &queue[drop + 1], (size_t)(queue_len - drop - 1) * sizeof(queue[0]));
		queue_len--;
	}
	queue[queue_len++] = *job;
	pthread_cond_signal(&ready);
}

static void report(yandex_wave_event_t event, const char *id, const char *album, int secs) {
	job_t job = {.kind = JOB_FEEDBACK, .event = event, .secs = secs};
	snprintf(job.id, sizeof(job.id), "%s", id ? id : "");
	snprintf(job.album, sizeof(job.album), "%s", album ? album : "");
	pthread_mutex_lock(&lock);
	submit_locked(&job);
	pthread_mutex_unlock(&lock);
}

static void run_more(void) {
	// The queue of the request, copied: the GUI may add to it meanwhile.
	char refs[RECEIVED_MAX][REF_MAX];
	const char *ptrs[RECEIVED_MAX];
	pthread_mutex_lock(&lock);
	int n = received_count;
	memcpy(refs, received, sizeof(refs));
	pthread_mutex_unlock(&lock);
	for (int i = 0; i < n; i++) {
		ptrs[i] = refs[i];
	}

	yandex_track_t *tracks = calloc(YANDEXWAVE_BATCH_MAX, sizeof(*tracks));
	bool restarted = false;
	int got = tracks ? yandex_wave_more(ptrs, n, tracks, YANDEXWAVE_BATCH_MAX, &restarted) : -1;

	pthread_mutex_lock(&lock);
	bool still = running;
	yandexwave_batch_cb cb = batch_cb;
	void *user = batch_user;
	if (got > 0 && still) {
		remember_received(tracks, got);
		strikes = 0;
	} else {
		// Nothing to add: asked again after a pause. An empty batch counts as
		// a failure too -- asking again at once would only get another.
		int pause = RETRY_SECS[strikes < RETRY_STEPS ? strikes : RETRY_STEPS - 1];
		strikes++;
		next_try_ms = monotonic_ms() + pause * 1000L;
		in_flight = false;
		if (still) {
			fprintf(stderr, "yandex: wave: no new tracks (%s), asking again in %d s\n", got < 0 ? yandex_last_error() : "an empty batch", pause);
		}
	}
	pthread_mutex_unlock(&lock);
	if (got > 0 && still && cb) {
		cb(tracks, got, restarted, user); // in_flight stays up until batch_done
	} else if (got > 0 && still) {
		pthread_mutex_lock(&lock);
		in_flight = false;
		pthread_mutex_unlock(&lock);
	}
	free(tracks);
}

static void *worker_main(void *arg) {
	(void)arg;
	// Below the interface, not starved by it: see yandexsync.c for why not
	// SCHED_IDLE. A batch is a request somebody's music is waiting on.
	thread_be_low_priority("yandex wave");
	for (;;) {
		pthread_mutex_lock(&lock);
		while (queue_len == 0) {
			pthread_cond_wait(&ready, &lock);
		}
		job_t job = queue[0];
		memmove(&queue[0], &queue[1], (size_t)(--queue_len) * sizeof(queue[0]));
		pthread_mutex_unlock(&lock);
		if (job.kind == JOB_MORE) {
			run_more();
		} else {
			yandex_wave_feedback(job.event, job.id[0] ? job.id : NULL, job.album, job.secs);
		}
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// the calls
// ---------------------------------------------------------------------------

// The track before has gone: finished when it was heard to within five
// seconds of its end, skipped otherwise -- unless it was disliked, which has
// already said all there is to say about it.
static void report_ended(void) {
	if (!now_id[0]) {
		return;
	}
	int secs = (int)(heard_ms / 1000);
	if (strcmp(disliked_id, now_id) != 0) {
		bool finished = now_duration > 0 && secs >= now_duration - 5;
		report(finished ? YANDEX_WAVE_TRACK_FINISHED : YANDEX_WAVE_SKIP, now_id, now_album, secs);
	}
	now_id[0] = now_album[0] = disliked_id[0] = '\0';
	heard_ms = 0;
}

void yandexwave_begin(const yandex_track_t *first, int count, yandexwave_batch_cb batch, void *user) {
	// The track of a wave before, if any, is not reported: its session has
	// already been replaced by this one's.
	now_id[0] = now_album[0] = disliked_id[0] = '\0';
	heard_ms = 0;
	pthread_mutex_lock(&lock);
	running = true;
	batch_cb = batch;
	batch_user = user;
	received_count = 0;
	remember_received(first, count);
	in_flight = false;
	strikes = 0;
	next_try_ms = 0;
	// Requests of the wave before have no business with this one.
	int kept = 0;
	for (int i = 0; i < queue_len; i++) {
		if (queue[i].kind == JOB_FEEDBACK) {
			queue[kept++] = queue[i];
		}
	}
	queue_len = kept;
	pthread_mutex_unlock(&lock);
	last_watch_ms = monotonic_ms();
	printf("yandex: wave: queue started with %d tracks\n", count);
}

void yandexwave_end(void) {
	pthread_mutex_lock(&lock);
	bool was = running;
	running = false;
	pthread_mutex_unlock(&lock);
	if (was) {
		report_ended();
		printf("yandex: wave: no longer the queue\n");
	}
}

bool yandexwave_running(void) {
	pthread_mutex_lock(&lock);
	bool r = running;
	pthread_mutex_unlock(&lock);
	return r;
}

void yandexwave_watch(const yandex_track_t *current, bool playing, int remaining) {
	pthread_mutex_lock(&lock);
	bool r = running;
	pthread_mutex_unlock(&lock);
	if (!r) {
		return;
	}
	long now = monotonic_ms();
	long step = now - last_watch_ms;
	last_watch_ms = now;

	if (!current || !current->id[0]) {
		// Something else plays: the wave track, if one was on, has been left.
		report_ended();
		return;
	}
	if (strcmp(current->id, now_id) != 0) {
		report_ended();
		snprintf(now_id, sizeof(now_id), "%s", current->id);
		snprintf(now_album, sizeof(now_album), "%s", current->album_id);
		now_duration = current->duration;
		heard_ms = 0;
		report(YANDEX_WAVE_TRACK_STARTED, now_id, now_album, 0);
	} else if (playing && step > 0) {
		// Heard time, not wall time: a pause does not make a track
		// "finished". A long gap (the loop stalled, the device slept) counts
		// as one tick.
		heard_ms += step < 2000 ? step : 1000;
	}

	pthread_mutex_lock(&lock);
	if (remaining <= REFILL_AT && !in_flight && now >= next_try_ms) {
		in_flight = true;
		printf("yandex: wave: %d track%s left after %s, asking for more\n", remaining, remaining == 1 ? "" : "s", now_id);
		job_t job = {.kind = JOB_MORE};
		submit_locked(&job);
	}
	pthread_mutex_unlock(&lock);
}

void yandexwave_batch_done(int appended) {
	pthread_mutex_lock(&lock);
	in_flight = false;
	pthread_mutex_unlock(&lock);
	printf("yandex: wave: %d tracks added to the queue\n", appended);
}

bool yandexwave_requesting(void) {
	pthread_mutex_lock(&lock);
	bool r = running && in_flight;
	pthread_mutex_unlock(&lock);
	return r;
}

int yandexwave_disliked(const char *track_id) {
	if (!track_id || strcmp(track_id, now_id) != 0) {
		return 0;
	}
	snprintf(disliked_id, sizeof(disliked_id), "%s", track_id);
	return (int)(heard_ms / 1000);
}

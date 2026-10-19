#include "workouttimer.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "src/system/core/config.h"

// A minute and a half: a common rest between sets, and a better first answer
// than nothing at all for somebody who presses the button before visiting the
// settings.
#define DEFAULT_SECONDS 90

// The config stores names, not numbers, like the key map does.
static const char *const SOUND_KEYS[CUE_COUNT] = {
	[CUE_BEEP] = "beep",
	[CUE_TRIPLE] = "triple",
	[CUE_COUNTDOWN] = "countdown",
	[CUE_ACK] = "ack", // never offered, but a name all the same
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake;
static bool thread_started;

static int seconds = DEFAULT_SECONDS;
static cue_sound_t sound = CUE_BEEP;
static long long deadline_ms; // 0 while it is not counting
static bool cued;			  // the cue for this run has been asked for
static void (*listener)(bool fired);

static long long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int clamp_seconds(int s) {
	if (s < WORKOUTTIMER_MIN_SECONDS) {
		return WORKOUTTIMER_MIN_SECONDS;
	}
	return s > WORKOUTTIMER_MAX_SECONDS ? WORKOUTTIMER_MAX_SECONDS : s;
}

static void notify(bool fired) {
	void (*cb)(bool) = listener;
	if (cb) {
		cb(fired);
	}
}

// Sleeps until the cue is due, asks for it, sleeps until the end, says so.
// Woken early by every start and stop, and then simply works it out again from
// the state as it stands.
static void *timer_main(void *unused) {
	(void)unused;
	pthread_mutex_lock(&lock);
	for (;;) {
		if (!deadline_ms) {
			pthread_cond_wait(&wake, &lock);
			continue;
		}

		long long now = now_ms();
		long long cue_at = deadline_ms - cue_lead_ms(sound);
		if (!cued && now >= cue_at) {
			cued = true;
			cue_sound_t s = sound;
			pthread_mutex_unlock(&lock);
			if (!cue_play(s)) {
				fprintf(stderr, "workouttimer: time is up, but there is no output to sound it on\n");
			}
			pthread_mutex_lock(&lock);
			continue;
		}
		if (now >= deadline_ms) {
			deadline_ms = 0;
			printf("workouttimer: time is up\n");
			pthread_mutex_unlock(&lock);
			notify(true);
			pthread_mutex_lock(&lock);
			continue;
		}

		long long until = cued ? deadline_ms : cue_at;
		struct timespec ts = {.tv_sec = (time_t)(until / 1000), .tv_nsec = (long)(until % 1000) * 1000000};
		pthread_cond_timedwait(&wake, &lock, &ts);
	}
	return NULL;
}

void workouttimer_init(void) {
	seconds = clamp_seconds((int)config_get_int("workout", "seconds", DEFAULT_SECONDS));
	const char *name = config_get("workout", "sound", SOUND_KEYS[CUE_BEEP]);
	sound = CUE_BEEP;
	for (int i = 0; i < CUE_COUNT; i++) {
		if (i != CUE_ACK && name && strcmp(name, SOUND_KEYS[i]) == 0) {
			sound = (cue_sound_t)i;
		}
	}

	if (thread_started) {
		return;
	}
	// The deadline is a CLOCK_MONOTONIC reading, so the wait has to be on that
	// clock too: on the default one, setting the time would move the beep.
	pthread_condattr_t attr;
	pthread_condattr_init(&attr);
	pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
	pthread_cond_init(&wake, &attr);
	pthread_condattr_destroy(&attr);

	pthread_t thread;
	pthread_attr_t tattr;
	pthread_attr_init(&tattr);
	pthread_attr_setdetachstate(&tattr, PTHREAD_CREATE_DETACHED);
	thread_started = pthread_create(&thread, &tattr, timer_main, NULL) == 0;
	pthread_attr_destroy(&tattr);
	if (!thread_started) {
		fprintf(stderr, "workouttimer: could not start its thread\n");
	}
}

int workouttimer_seconds(void) { return seconds; }

void workouttimer_set_seconds(int s) {
	s = clamp_seconds(s);
	if (s == seconds) {
		return;
	}
	seconds = s;
	config_set_int("workout", "seconds", s);
	config_save();
}

cue_sound_t workouttimer_sound(void) { return sound; }

void workouttimer_set_sound(cue_sound_t s) {
	if (s < 0 || s >= CUE_COUNT || s == CUE_ACK || s == sound) {
		return;
	}
	pthread_mutex_lock(&lock);
	sound = s;
	pthread_cond_signal(&wake); // a countdown's pips start earlier than a beep
	pthread_mutex_unlock(&lock);
	config_set("workout", "sound", SOUND_KEYS[s]);
	config_save();
}

bool workouttimer_start(void) {
	if (!thread_started) {
		return false;
	}
	pthread_mutex_lock(&lock);
	deadline_ms = now_ms() + (long long)seconds * 1000;
	cued = false;
	pthread_cond_signal(&wake);
	pthread_mutex_unlock(&lock);
	printf("workouttimer: %d s\n", seconds);

	// Heard rather than seen: the press usually comes from a pocket.
	cue_play(CUE_ACK);
	notify(false);
	return true;
}

void workouttimer_stop(void) {
	pthread_mutex_lock(&lock);
	bool was = deadline_ms != 0;
	deadline_ms = 0;
	pthread_cond_signal(&wake);
	pthread_mutex_unlock(&lock);
	if (was) {
		notify(false);
	}
}

bool workouttimer_running(void) {
	pthread_mutex_lock(&lock);
	bool running = deadline_ms != 0;
	pthread_mutex_unlock(&lock);
	return running;
}

int workouttimer_remaining(void) {
	pthread_mutex_lock(&lock);
	long long left = deadline_ms ? deadline_ms - now_ms() : 0;
	pthread_mutex_unlock(&lock);
	return left > 0 ? (int)((left + 999) / 1000) : 0;
}

void workouttimer_set_listener(void (*cb)(bool fired)) { listener = cb; }

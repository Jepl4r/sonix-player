#include "sleeptimer.h"

#include <stdio.h>
#include <time.h>

#include "src/system/core/config.h"

// A whole day, which is the most the two wheels can be set to and well past
// anything anybody means by a sleep timer. The clamp is here so a hand-edited
// config cannot arm something that never expires.
#define SLEEP_MAX_MINUTES (23 * 60 + 59)

// Where each one is written down. The audiobook pair are the keys the audiobook
// timer has always used, so an existing config keeps its switch and its length.
// A length of nought there means no time chosen: "at the end of the chapter" is
// a switch of its own on the audiobook page, not a timer length.
static const struct {
	const char *section;
	const char *key_on;
	const char *key_minutes;
	const char *name;
} PROFILES[SLEEPTIMER_COUNT] = {
	[SLEEPTIMER_MUSIC] = {"player", "sleep_timer", "sleep_minutes", "music"},
	[SLEEPTIMER_AUDIOBOOK] = {"audiobook", "sleep_timer", "sleep_minutes", "audiobook"},
	[SLEEPTIMER_PODCAST] = {"podcast", "sleep_timer", "sleep_minutes", "podcast"},
};

typedef struct {
	bool enabled;
	int minutes;
	bool counting;
	long long remaining_ms;
	long long last_ms;
} timer_state_t;

static timer_state_t timers[SLEEPTIMER_COUNT];

// Which kind is playing, kept apart from the countdowns: "something is playing"
// and "a stretch is running" are not the same thing, and folding them into one
// flag is how switching the option off and on again while the music played
// armed nothing at all -- the first call cleared the flag the second one needed
// to read.
static sleeptimer_kind_t playing_kind = SLEEPTIMER_COUNT;

static bool in_range(sleeptimer_kind_t kind) { return kind >= 0 && kind < SLEEPTIMER_COUNT; }

static long long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int clamp_minutes(int minutes) {
	if (minutes < 0) {
		return 0;
	}
	return minutes > SLEEP_MAX_MINUTES ? SLEEP_MAX_MINUTES : minutes;
}

void sleeptimer_init(void) {
	for (int i = 0; i < SLEEPTIMER_COUNT; i++) {
		timers[i].enabled = config_get_int(PROFILES[i].section, PROFILES[i].key_on, 0) != 0;
		timers[i].minutes = clamp_minutes((int)config_get_int(PROFILES[i].section, PROFILES[i].key_minutes, 0));
		timers[i].counting = false;
		timers[i].remaining_ms = 0;
	}
	playing_kind = SLEEPTIMER_COUNT;
}

bool sleeptimer_enabled(sleeptimer_kind_t kind) { return in_range(kind) && timers[kind].enabled; }

int sleeptimer_minutes(sleeptimer_kind_t kind) { return in_range(kind) ? timers[kind].minutes : 0; }

// Armed means there is a stretch to count: switched on and longer than nothing.
static bool armed(sleeptimer_kind_t kind) { return timers[kind].enabled && timers[kind].minutes > 0; }

// Starts a full stretch, or stops counting, from the state as it stands. Every
// change goes through here, so there is one place that decides and one place
// where `remaining_ms` is ever set.
static void restart(sleeptimer_kind_t kind) {
	timer_state_t *t = &timers[kind];
	t->counting = playing_kind == kind && armed(kind);
	t->remaining_ms = t->counting ? (long long)t->minutes * 60000 : 0;
	t->last_ms = now_ms();
}

void sleeptimer_set_enabled(sleeptimer_kind_t kind, bool on) {
	if (!in_range(kind) || on == timers[kind].enabled) {
		return;
	}
	timers[kind].enabled = on;
	config_set_int(PROFILES[kind].section, PROFILES[kind].key_on, on ? 1 : 0);
	config_save();
	// Switching it off in the middle of a stretch disarms it now, and switching
	// it on while something is already playing starts one: otherwise the setting
	// would only take effect at the next pause.
	restart(kind);
}

void sleeptimer_set_minutes(sleeptimer_kind_t kind, int minutes) {
	if (!in_range(kind)) {
		return;
	}
	minutes = clamp_minutes(minutes);
	if (minutes == timers[kind].minutes) {
		return;
	}
	timers[kind].minutes = minutes;
	config_set_int(PROFILES[kind].section, PROFILES[kind].key_minutes, minutes);
	config_save();
	// Moving the wheel is choosing a new stretch, not editing the one running:
	// whoever just set thirty minutes means thirty minutes from now.
	restart(kind);
}

void sleeptimer_note_playing(sleeptimer_kind_t kind) {
	if (!in_range(kind)) {
		kind = SLEEPTIMER_COUNT; // nothing any of them counts
	}
	if (kind == playing_kind) {
		return;
	}
	playing_kind = kind;
	// All of them, not only the one named: whatever stopped playing has a
	// countdown to put away, and going from an album to a podcast is both at
	// once.
	for (int i = 0; i < SLEEPTIMER_COUNT; i++) {
		restart((sleeptimer_kind_t)i);
		if (timers[i].counting) {
			fprintf(stderr, "sleeptimer: %s, %d min\n", PROFILES[i].name, timers[i].minutes);
		}
	}
}

bool sleeptimer_running(sleeptimer_kind_t kind) { return in_range(kind) && timers[kind].counting; }

bool sleeptimer_any_running(void) {
	for (int i = 0; i < SLEEPTIMER_COUNT; i++) {
		if (timers[i].counting) {
			return true;
		}
	}
	return false;
}

bool sleeptimer_expired(sleeptimer_kind_t kind) {
	if (!sleeptimer_running(kind)) {
		return false;
	}
	timer_state_t *t = &timers[kind];

	long long now = now_ms();
	long long elapsed = now - t->last_ms;
	t->last_ms = now;
	if (elapsed < 0) {
		elapsed = 0; // the clock is monotonic, but do not trust a negative step
	}

	if (elapsed >= t->remaining_ms) {
		t->counting = false;
		t->remaining_ms = 0;
		fprintf(stderr, "sleeptimer: %s, time is up; stopping\n", PROFILES[kind].name);
		return true;
	}
	t->remaining_ms -= elapsed;
	return false;
}

int sleeptimer_remaining(sleeptimer_kind_t kind) {
	if (!sleeptimer_running(kind)) {
		return 0;
	}
	return (int)((timers[kind].remaining_ms + 999) / 1000);
}

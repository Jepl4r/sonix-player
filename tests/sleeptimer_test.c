#include "../sonix-player/src/system/playback/sleeptimer.c"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

// An in-memory config: enough for sleeptimer.c, which reads and writes
// integers.
#define STUB_MAX 16
static char stub_section[STUB_MAX][32];
static char stub_key[STUB_MAX][32];
static long stub_value[STUB_MAX];
static int stub_count;

static int stub_find(const char *section, const char *key) {
  for (int i = 0; i < stub_count; i++) {
    if (strcmp(stub_section[i], section) == 0 &&
        strcmp(stub_key[i], key) == 0) {
      return i;
    }
  }
  return -1;
}

long config_get_int(const char *section, const char *key, long fallback) {
  int i = stub_find(section, key);
  return i < 0 ? fallback : stub_value[i];
}

void config_set_int(const char *section, const char *key, long value) {
  int i = stub_find(section, key);
  if (i < 0) {
    assert(stub_count < STUB_MAX);
    i = stub_count++;
    snprintf(stub_section[i], sizeof(stub_section[0]), "%s", section);
    snprintf(stub_key[i], sizeof(stub_key[0]), "%s", key);
  }
  stub_value[i] = value;
}

bool config_save(void) { return true; }

static void fresh(void) {
  stub_count = 0;
  sleeptimer_init();
}

static void quick_set_while_playing(void) {
  fresh();
  sleeptimer_note_playing(SLEEPTIMER_AUDIOBOOK);
  sleeptimer_quick_set(SLEEPTIMER_AUDIOBOOK, 30);
  assert(sleeptimer_running(SLEEPTIMER_AUDIOBOOK));
  assert(sleeptimer_remaining(SLEEPTIMER_AUDIOBOOK) >= 1799);
  assert(sleeptimer_remaining(SLEEPTIMER_AUDIOBOOK) <= 1800);
  assert(sleeptimer_minutes(SLEEPTIMER_AUDIOBOOK) == 30);
  assert(sleeptimer_enabled(SLEEPTIMER_AUDIOBOOK));
}

static void quick_set_while_idle(void) {
  fresh();
  sleeptimer_note_playing(SLEEPTIMER_COUNT);
  sleeptimer_quick_set(SLEEPTIMER_AUDIOBOOK, 15);
  assert(sleeptimer_enabled(SLEEPTIMER_AUDIOBOOK));
  assert(sleeptimer_minutes(SLEEPTIMER_AUDIOBOOK) == 15);
  assert(!sleeptimer_running(SLEEPTIMER_AUDIOBOOK));
  assert(sleeptimer_remaining(SLEEPTIMER_AUDIOBOOK) == 0);
  sleeptimer_note_playing(SLEEPTIMER_AUDIOBOOK);
  assert(sleeptimer_running(SLEEPTIMER_AUDIOBOOK));
  assert(sleeptimer_remaining(SLEEPTIMER_AUDIOBOOK) >= 899);
  assert(sleeptimer_remaining(SLEEPTIMER_AUDIOBOOK) <= 900);
}

static void quick_set_off(void) {
  quick_set_while_playing();
  sleeptimer_quick_set(SLEEPTIMER_AUDIOBOOK, 0);
  assert(!sleeptimer_enabled(SLEEPTIMER_AUDIOBOOK));
  assert(!sleeptimer_running(SLEEPTIMER_AUDIOBOOK));
}

static void restart_replaces_old_stretch(void) {
  fresh();
  sleeptimer_set_minutes(SLEEPTIMER_AUDIOBOOK, 30);
  sleeptimer_set_enabled(SLEEPTIMER_AUDIOBOOK, true);
  sleeptimer_note_playing(SLEEPTIMER_AUDIOBOOK);
  assert(sleeptimer_remaining(SLEEPTIMER_AUDIOBOOK) >= 1799);
  sleeptimer_quick_set(SLEEPTIMER_AUDIOBOOK, 15);
  assert(sleeptimer_remaining(SLEEPTIMER_AUDIOBOOK) >= 899);
  assert(sleeptimer_remaining(SLEEPTIMER_AUDIOBOOK) <= 900);
}

// The same length picked again while counting still starts a fresh stretch.
static void same_minutes_restarts(void) {
  fresh();
  sleeptimer_note_playing(SLEEPTIMER_AUDIOBOOK);
  sleeptimer_quick_set(SLEEPTIMER_AUDIOBOOK, 15);
  timers[SLEEPTIMER_AUDIOBOOK].remaining_ms = 5000;
  sleeptimer_quick_set(SLEEPTIMER_AUDIOBOOK, 15);
  assert(sleeptimer_remaining(SLEEPTIMER_AUDIOBOOK) >= 899);
}

// Another kind playing: the pick is stored but arms nothing.
static void other_kind_stays_idle(void) {
  fresh();
  sleeptimer_note_playing(SLEEPTIMER_MUSIC);
  sleeptimer_quick_set(SLEEPTIMER_AUDIOBOOK, 45);
  assert(sleeptimer_enabled(SLEEPTIMER_AUDIOBOOK));
  assert(!sleeptimer_running(SLEEPTIMER_AUDIOBOOK));
  assert(!sleeptimer_running(SLEEPTIMER_MUSIC));
}

static void deadline_tracks_countdown(void) {
	fresh();
	assert(sleeptimer_deadline_ms(SLEEPTIMER_AUDIOBOOK) == 0);
	sleeptimer_note_playing(SLEEPTIMER_AUDIOBOOK);
	sleeptimer_quick_set(SLEEPTIMER_AUDIOBOOK, 15);
	long long expected = now_ms() + 900000;
	long long deadline = sleeptimer_deadline_ms(SLEEPTIMER_AUDIOBOOK);
	assert(llabs(deadline - expected) <= 50);
	timers[SLEEPTIMER_AUDIOBOOK].last_ms -= 5000;
	assert(llabs(sleeptimer_deadline_ms(SLEEPTIMER_AUDIOBOOK) - (deadline - 5000)) <= 50);
	sleeptimer_note_playing(SLEEPTIMER_COUNT);
	assert(sleeptimer_deadline_ms(SLEEPTIMER_AUDIOBOOK) == 0);
	assert(sleeptimer_deadline_ms(SLEEPTIMER_COUNT) == 0);
}

int main(void) {
	deadline_tracks_countdown();
  quick_set_while_playing();
  quick_set_while_idle();
  quick_set_off();
  restart_replaces_old_stretch();
  same_minutes_restarts();
  other_kind_stays_idle();
  return 0;
}

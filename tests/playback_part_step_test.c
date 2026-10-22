#include <assert.h>
#include <time.h>
static long long test_now_ms;
static int test_clock_gettime(clockid_t clock, struct timespec *ts) {
	assert(clock == CLOCK_MONOTONIC);
	ts->tv_sec = test_now_ms / 1000;
	ts->tv_nsec = (test_now_ms % 1000) * 1000000;
	return 0;
}
#define clock_gettime test_clock_gettime
#include "../sonix-player/src/system/playback/device_state.c"
#undef clock_gettime
static audio_status_t test_status = AUDIO_STATUS_PLAYING;
static double sought = -1;
static double current_position = 123;
static bool crossing;
static bool floor_armed;
static bool ordered_restore;
static double boot_gap;
static double observed_gap;
static double previous_duration = 600;
static bool book_loaded = true;
static bool rewind_on = true;
static bool suppress_rewind;
static bool interrupt_snapshot;
static void rewind_after_break(void);
static const char *const parts[] = {"a.mp3", "b.mp3"};
static int queued = -1;
static bool saved;
static bool loaded;
static bool fail_load;
static bool playing;
static char selected[512] = "a.mp3";
bool audiobook_is_playing(void) { return book_loaded; }
void audio_get_progress(double *current, double *total) {
  *current = current_position;
  *total = 1000;
}
void audiobook_note_position(double current, double total, bool force) {
  assert(current == current_position && total == 1000 && force);
  assert(!crossing || !floor_armed);
  assert(strcmp(selected, "a.mp3") == 0 || crossing);
  saved = true;
}
void audiobook_track_changed(const char *path) {
  assert(saved);
  snprintf(selected, sizeof(selected), "%s", path);
}
void playlist_load_paths_ordered(const char *const *list, int count,
                                 int start_index) {
  ordered_restore = true;
  assert(count == 2 && strcmp(list[0], parts[0]) == 0 && strcmp(list[1], parts[1]) == 0);
  queued = fail_load ? -1 : start_index;
}
bool playlist_current_path(char *out, size_t out_size) {
	if (queued < 0)
		return false;
	snprintf(out, out_size, "%s", parts[queued]);
	return true;
}
int audio_play_paused(const char *path, double position) {
  assert(saved);
  assert(strcmp(path, crossing ? "a.mp3" : "b.mp3") == 0 && position == 0);
  loaded = true;
  return 0;
}
int audio_play(const char *path) {
	assert(playing && saved && strcmp(path, "b.mp3") == 0);
	loaded = true;
	return 0;
}
int audio_play_at(const char *path, double position) {
  assert(!"a paused step started playback");
  return 0;
}
void playlist_load_paths(const char *const *list, int count, int index) { ordered_restore = false; }
bool audiobookdb_book_for_file(const char *file, char *book, size_t size) { return book_loaded; }
int main(void) {
	load_restored_paths(parts, 2, 1, "b.mp3");
	assert(ordered_restore);
	book_loaded = false;
	load_restored_paths(parts, 2, 1, "b.mp3");
	assert(!ordered_restore);
	book_loaded = true;

	fail_load = true;
	assert(!device_state_load_list_ordered_paused(parts, 2, 1));
	assert(!saved && !loaded && strcmp(selected, "a.mp3") == 0);
	assert(!device_state_play_list_ordered(parts, 2, 1));
	assert(!saved && !loaded && strcmp(selected, "a.mp3") == 0);
	fail_load = false;
	assert(device_state_load_list_ordered_paused(parts, 2, 1));
	assert(queued == 1 && saved && loaded);
	assert(strcmp(selected, "b.mp3") == 0);
	saved = loaded = false;
	snprintf(selected, sizeof(selected), "%s", "a.mp3");
	playing = true;
	assert(device_state_play_list_ordered(parts, 2, 1));
	assert(queued == 1 && saved && loaded);
	assert(strcmp(selected, "b.mp3") == 0);
	suppress_rewind = false;
	rewind_after_break();
	return 0;
}
void audio_set_speed(double speed) {}
double audiobook_speed(void) { return 1.0; }
bool audiobook_apply_eq(bool book_playing) { return true; }
void audiobook_suppress_rewind_once(void) { suppress_rewind = true; }
void metadata_read(const char *path, song_metadata_t *out) {
  memset(out, 0, sizeof(*out));
}
const char *metadata_shown_artist(const song_metadata_t *m) { return ""; }
void playhistory_left(void) {}
void playhistory_started(const char *p, const char *t, const char *a, bool k,
                         bool w) {}
int playlist_count(void) { return 0; }
int playlist_current_index(void) { return -1; }
void radio_clear(void) {}
void replaygain_load(const song_metadata_t *m) {}
bool podcastcache_is_episode(const char *path) { return false; }
bool growfile_is_growing(const char *path) { return false; }
double podcaststate_resume_point(const char *path) { return -1.0; }

// The engine and host UI boundary; the production getter observes these states.
audio_status_t audio_get_status(void) { return test_status; }
void audio_pause(void) { test_status = AUDIO_STATUS_PAUSED; }
void audio_resume(void) { test_status = AUDIO_STATUS_PLAYING; }
void audio_seek(double seconds) { sought = seconds; }
void audio_get_current_file(char *out, size_t size) {
	if (interrupt_snapshot) {
		interrupt_snapshot = false;
		test_status = AUDIO_STATUS_PLAYING;
		device_state_t newer;
		device_state_get(&newer);
	}
	snprintf(out, size, "%s", selected);
}
void audio_get_stream_info(int *rate, int *channels) { *rate = 44100; *channels = 2; }
bool radio_is_active(void) { return false; }
bool radio_is_connecting(void) { return false; }
bool radio_is_playing(void) { return false; }
void radio_get_now(radio_now_t *out) { memset(out, 0, sizeof(*out)); }
bool radio_resume(void) { return false; }
void radio_stop(void) {}
char *read_battery_percent(void) { return "100"; }
bool read_battery_charging(void) { return false; }
int get_volume_percent(void) { return 50; }
const char *tr(const char *key) { return key; }
void playhistory_resumed(void) {}
bool dlna_owns_path(const char *path) { return false; }
bool audiobook_rewind_enabled(void) { return rewind_on; }
bool audiobook_take_rewind_suppression(void) {
	bool value = suppress_rewind;
	suppress_rewind = false;
	return value;
}
int audiobook_rewind_seconds(void) { return 5; }
int audiobook_rewind_long_seconds(void) { return 30; }
// The pure rule is covered with all boundaries in audiobook_units_test.c.
int audiobook_rewind_for_break(double seconds, int short_s, int long_s) {
	assert(short_s == 5 && long_s == 30);
	assert(seconds >= 0);
	observed_gap = seconds;
	return seconds == 90 ? 5 : 30;
}

static void rewind_after_break(void) {
	device_state_t state;
	assert(device_state_break_seconds() == 0);
	device_state_get(&state);
	test_now_ms = 1000;
	assert(device_state_toggle_play_pause() == AUDIO_STATUS_PAUSED);
	device_state_get(&state);
	test_now_ms += 90000;
	device_state_get(&state); // repeated polls must not restart the clock
	assert(device_state_break_seconds() == 90);
	sought = -1;
	assert(device_state_toggle_play_pause() == AUDIO_STATUS_PLAYING);
	assert(sought == 118);
	assert(device_state_break_seconds() == 0);
	device_state_get(&state);
	puts("PASS playback-rewind-short-break");

	// Direct engine pause stands for sleep, unplug, suspend and paused loads.
	audio_pause();
	device_state_get(&state);
	test_now_ms += 4000000;
	assert(device_state_break_seconds() == 4000);
	sought = -1;
	assert(device_state_toggle_play_pause() == AUDIO_STATUS_PLAYING);
	assert(sought == 93);
	device_state_get(&state);
	puts("PASS playback-rewind-long-break");

	// Leaving PAUSED resets the break, including STOPPED.
	audio_pause();
	device_state_get(&state);
	test_now_ms += 1234;
	assert(device_state_break_seconds() == 1.234);
	test_status = AUDIO_STATUS_STOPPED;
	device_state_get(&state);
	assert(device_state_break_seconds() == 0);
	test_status = AUDIO_STATUS_PAUSED;
	device_state_get(&state);
	assert(device_state_break_seconds() == 0);
	puts("PASS playback-break-clock-reset");

	// A delayed snapshot must not reapply its old PAUSED status after a newer read.
	interrupt_snapshot = true;
	device_state_get(&state);
	test_now_ms += 4000000;
	test_status = AUDIO_STATUS_PAUSED;
	device_state_get(&state);
	assert(device_state_break_seconds() == 0);
	puts("PASS playback-break-clock-stale-snapshot");

	// A resume and another pause can both happen between GUI polls.
	test_now_ms += 4000000;
	suppress_rewind = true;
	assert(device_state_toggle_play_pause() == AUDIO_STATUS_PLAYING);
	test_now_ms += 100;
	assert(device_state_toggle_play_pause() == AUDIO_STATUS_PAUSED);
	test_now_ms += 1234;
	assert(device_state_break_seconds() == 1.234);
	puts("PASS playback-break-clock-fast-toggle");

	// Existing suppression and non-book / disabled paths keep their positions.
	for (int i = 0; i < 3; i++) {
		test_status = AUDIO_STATUS_PLAYING;
		device_state_get(&state);
		test_status = AUDIO_STATUS_PAUSED;
		device_state_get(&state);
		test_now_ms += 4000000;
		suppress_rewind = i == 0;
		book_loaded = i != 1;
		rewind_on = i != 2;
		sought = -1;
		assert(device_state_toggle_play_pause() == AUDIO_STATUS_PLAYING);
		assert(sought == -1);
	}
	puts("PASS playback-rewind-gates");
	book_loaded = rewind_on = true;
	test_status = AUDIO_STATUS_PAUSED;
	device_state_get(&state);
	sought = -1;
	// A bookmark/slider seek while paused suppresses exactly the next rewind.
	book_loaded = false;
	device_state_seek(321);
	book_loaded = true;
	assert(sought == 321);
	assert(device_state_toggle_play_pause() == AUDIO_STATUS_PLAYING);
	assert(sought == 321);
	puts("PASS playback-explicit-paused-seek");
	test_status = AUDIO_STATUS_PAUSED;
	rewind_on = false;
	suppress_rewind = true;
	boot_gap = 7200;
	assert(device_state_toggle_play_pause() == AUDIO_STATUS_PLAYING);
	assert(!suppress_rewind && boot_gap == 0);
	rewind_on = true;
	puts("PASS playback-disabled-consumes-suppression");
	floor_armed = false;
	crossing = true;
	current_position = 10;
	snprintf(selected, sizeof(selected), "b.mp3");
	test_status = AUDIO_STATUS_PAUSED;
	device_state_get(&state);
	boot_gap = 7200;
	sought = -1;
	assert(device_state_toggle_play_pause() == AUDIO_STATUS_PLAYING);
	assert(sought == 580 && strcmp(selected, "a.mp3") == 0);
	assert(boot_gap == 0 && !suppress_rewind && observed_gap == 7200);
	puts("PASS playback-rewind-previous-part-boot");
	test_status = AUDIO_STATUS_PAUSED;
	device_state_get(&state);
	floor_armed = false;
	previous_duration = 0;
	boot_gap = 7200;
	snprintf(selected, sizeof(selected), "b.mp3");
	sought = -1;
	assert(device_state_toggle_play_pause() == AUDIO_STATUS_PLAYING);
	assert(sought == 0 && strcmp(selected, "b.mp3") == 0);
	puts("PASS playback-rewind-unknown-previous");
}

void audiobook_clear_rewind_floor(void) { floor_armed = false; }

void audiobook_set_rewind_floor(const char *file, double seconds) {
	assert(strcmp(file, crossing ? "b.mp3" : selected) == 0 && seconds == current_position);
	floor_armed = true;
}
double audiobook_take_boot_break(void) { double gap = boot_gap; boot_gap = 0; return gap; }
int audiobook_part_current(void) { return crossing ? 1 : 0; }
double audiobook_part_duration(int index) { assert(index == 0); return previous_duration; }
int audiobook_part_count(void) { return 2; }
bool audiobook_part(int index, char *title, size_t ts, char *path, size_t ps) {
	snprintf(path, ps, "%s", parts[index]); return true;
}
double audiobook_clamp_seek(double seconds, double total) { return seconds; }

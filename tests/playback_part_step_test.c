#include <assert.h>
#include <time.h>
static long long test_now_ms;
static unsigned break_clock_reads;
static int test_break_clock_gettime(clockid_t clock, struct timespec *ts) {
#ifdef CLOCK_BOOTTIME
	assert(clock == CLOCK_BOOTTIME);
#else
	assert(clock == CLOCK_MONOTONIC);
#endif
	break_clock_reads++;
	ts->tv_sec = test_now_ms / 1000;
	ts->tv_nsec = (test_now_ms % 1000) * 1000000;
	return 0;
}
#define clock_gettime test_break_clock_gettime
#include "../sonix-player/src/system/device/clock.h"
#undef clock_gettime

// Position persistence and sleep-checkpoint deadlines stay on MONOTONIC.
static int test_clock_gettime(clockid_t clock, struct timespec *ts) {
	assert(clock == CLOCK_MONOTONIC);
	ts->tv_sec = test_now_ms / 1000;
	ts->tv_nsec = (test_now_ms % 1000) * 1000000;
	return 0;
}
#define access test_access
#define clock_gettime test_clock_gettime
#include "../sonix-player/src/system/playback/device_state.c"
#undef clock_gettime
#undef access
static bool navigation_readable = true;
int test_access(const char *path, int mode) { return navigation_readable ? 0 : -1; }
static audio_status_t test_status = AUDIO_STATUS_PLAYING;
static double sought = -1;
static int jump_marks;
static bool navigation_test;
static char origin_file[512];
static double origin_seconds;

static double current_position = 123;
void audiobook_mark_jump(const char *file, double seconds) {
	assert(seconds >= 0);
	jump_marks++;
	snprintf(origin_file, sizeof(origin_file), "%s", file);
	origin_seconds = seconds;
}
bool audiobook_jump_origin(char *file, size_t size, double *seconds) {
	if (!origin_file[0])
		return false;
        if (file && size)
          snprintf(file, size, "%s", origin_file);
        if (seconds)
          *seconds = origin_seconds;
        return true;
}
void audiobook_clear_jump(void) { origin_file[0] = 0; }

static bool crossing;
static bool floor_armed;
static bool outgoing_floor;
static bool ordered_restore;
static bool automatic_stop_test;
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
bool audiobook_current_book(char *book, size_t bs, char *file, size_t fs) {
	snprintf(book, bs, "book");
	snprintf(file, fs, "%s", selected);
	return book_loaded;
}
bool audiobook_is_playing(void) { return book_loaded; }
void audio_get_progress(double *current, double *total) {
  *current = current_position;
  *total = 1000;
}
void audiobook_note_position(double current, double total, bool force) {
  if (navigation_test) {
    assert(force);
    outgoing_floor = floor_armed;
    saved = true;
    current_position = current;
    return;
  }
  if (automatic_stop_test) {
    assert(current == 100 && total == 0 && force && floor_armed);
    return;
  }
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
  assert((navigation_test || strcmp(path, crossing ? "a.mp3" : "b.mp3") == 0) && position == 0);
  if (navigation_test) test_status = AUDIO_STATUS_PAUSED;
  loaded = true;
  return 0;
}
int audio_play(const char *path) {
	assert(saved && (navigation_test || (playing && strcmp(path, "b.mp3") == 0)));
	if (navigation_test) test_status = AUDIO_STATUS_PLAYING;
	loaded = true;
	return 0;
}
int audio_play_at(const char *path, double position) {
  assert(navigation_test);
  test_status = AUDIO_STATUS_PLAYING;
  loaded = true;
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
	boot_gap = 7200;
	assert(device_state_play_list_ordered(parts, 2, 1));
	assert(queued == 1 && saved && loaded);
	assert(strcmp(selected, "b.mp3") == 0);
	assert(boot_gap == 0);
	suppress_rewind = false;
	rewind_after_break();
	navigation_test = true;
	crossing = false;
	test_status = AUDIO_STATUS_PAUSED;
	current_position = 123;
	snprintf(selected, sizeof(selected), "a.mp3");
	audiobook_mark_jump("a.mp3", 123);
	assert(device_state_seek_book_part("b.mp3", 50));
	assert(test_status == AUDIO_STATUS_PAUSED && sought == 50 && saved);
	assert(strcmp(origin_file, "a.mp3") == 0 && origin_seconds == 123);
	navigation_readable = false;
	assert(!device_state_back_before_jump());
	assert(strcmp(origin_file, "a.mp3") == 0 && origin_seconds == 123);
	navigation_readable = true;
	fail_load = true;
	assert(!device_state_back_before_jump());
	assert(strcmp(origin_file, "a.mp3") == 0 && origin_seconds == 123);
	fail_load = false;
	floor_armed = true;
	assert(device_state_back_before_jump());
	assert(test_status == AUDIO_STATUS_PAUSED && sought == 123 && saved);
	assert(!origin_file[0] && !floor_armed);
	assert(suppress_rewind);
	puts("PASS playback-jump-cross-part-paused-return-retry");
	test_status = AUDIO_STATUS_PLAYING;
	current_position = 55;
	snprintf(selected, sizeof(selected), "b.mp3");
	audiobook_mark_jump("b.mp3", 55);
	assert(device_state_seek_book_part("a.mp3", 10));
	assert(device_state_back_before_jump());
	assert(test_status == AUDIO_STATUS_PLAYING && sought == 55 && !origin_file[0]);
	device_state_seek(100);
	assert(origin_seconds == 55 && jump_marks > 0);
	assert(device_state_back_before_jump());
	assert(test_status == AUDIO_STATUS_PLAYING && sought == 55 && !origin_file[0]);
	puts("PASS playback-jump-playing-and-same-file-return");
	// In this fixture an armed floor stands in b.mp3, beyond a.mp3's end.
	for (int with_floor = 1; with_floor >= 0; with_floor--) {
		floor_armed = with_floor;
		snprintf(selected, sizeof(selected), "a.mp3");
		test_status = AUDIO_STATUS_PLAYING;
		loaded = false;
		char next[512] = "";
		assert(device_state_advance_auto(next, sizeof(next)));
		assert(loaded && !strcmp(next, "b.mp3") && !strcmp(selected, "b.mp3"));
		assert(test_status == (with_floor ? AUDIO_STATUS_PLAYING : AUDIO_STATUS_PAUSED));
		assert(floor_armed == with_floor);
		puts(with_floor ? "PASS playback-folder-next-floor-continues" : "PASS playback-folder-no-floor-stops-at-part-end");
	}
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
void audio_pause(void) {
	test_status = AUDIO_STATUS_PAUSED;
#ifdef AUDIO_PAUSE_HOOK
	AUDIO_PAUSE_HOOK();
#endif
}
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
	unsigned reads_before = break_clock_reads;
	assert(device_state_break_seconds() == 0);
	assert(break_clock_reads == reads_before + 1);
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
	book_loaded = true;
	snprintf(selected, sizeof(selected), "%s", "a.mp3");
	int before_marks = jump_marks;
	device_state_seek(123);
	assert(jump_marks == before_marks + 1);
	book_loaded = false;
	device_state_seek(321);
	assert(jump_marks == before_marks + 1);
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
	crossing = false;
	automatic_stop_test = true;
	floor_armed = true;
	test_status = AUDIO_STATUS_PLAYING;
	unsigned serial = device_state_seek_serial();
	int marks_before_automatic = jump_marks;
	device_state_pause_book_at(100);
	assert(sought == 100 && floor_armed && test_status == AUDIO_STATUS_PAUSED);
	assert(device_state_seek_serial() == serial);
	assert(jump_marks == marks_before_automatic);
	puts("PASS playback-automatic-chapter-stop-preserves-floor");
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
bool audiobook_has_next_part(void) { return !strcmp(selected, "a.mp3"); }
bool audiobook_stop_at_chapter_end(void) { return true; }
bool audiobook_rewind_floor_beyond(double seconds) {
	// DBL_MAX asks about the next part; GUI chapter checks use local seconds.
	return floor_armed && (seconds == DBL_MAX || seconds < 250);
}
bool playlist_advance_auto(char *path, size_t size) {
	queued = 1;
	snprintf(path, size, "b.mp3");
	return true;
}
// Non-book auto-advance dependencies: these fixtures must never enter them.
// Other harnesses rename main and supply their own boundary stubs.
#ifndef main
long config_get_int(const char *section, const char *key, long fallback) {
	assert(0);
	return fallback;
}
bool playlist_is_custom(void) {
	assert(0);
	return false;
}
bool playlist_at_end(void) {
	assert(0);
	return false;
}
bool playlist_locate(const char *path) {
	assert(0);
	return false;
}
playback_mode_t playlist_get_mode(void) {
	assert(0);
	return PLAYBACK_MODE_NORMAL;
}
const char *playlist_album(void) {
	assert(0);
	return NULL;
}
const char *playlist_card_root(void) {
	assert(0);
	return NULL;
}
void playlist_set_album(const char *album) { assert(0); }
void playlist_load_folder(const char *folder, const char *filename) { assert(0); }
bool playlist_next_folder(const char *root, const char *folder, char *out, size_t size) {
	assert(0);
	return false;
}
bool playlist_prev_folder(const char *root, const char *folder, char *out, size_t size) {
	assert(0);
	return false;
}
bool library_album_same(const char *a, const char *b) {
	assert(0);
	return false;
}
void library_album_title(const char *value, char *out, size_t size) { assert(0); }
int library_for_each(library_list_t kind, library_filter_t filter, const char *value, library_row_cb cb, void *user) {
	assert(0);
	return 0;
}
int library_for_each_ordered(library_list_t kind, library_filter_t filter, const char *value, library_order_t order, library_row_cb cb, void *user) {
	assert(0);
	return 0;
}
library_index_t *library_index_open(library_list_t kind, library_filter_t filter, const char *value, library_order_t order, bool desc) {
	assert(0);
	return NULL;
}
void library_index_close(library_index_t *ix) { assert(0); }
int library_index_count(const library_index_t *ix) {
	assert(0);
	return 0;
}
int library_index_window(const library_index_t *ix, int offset, int count, library_row_cb cb, void *user) {
	assert(0);
	return 0;
}
#endif
double audiobook_clamp_seek(double seconds, double total) { return seconds; }

#include "../sonix-player/src/system/playback/device_state.c"
#include <assert.h>
static bool saved;
static bool seeking;
static bool book_loaded = true;
static int notes;
static int seeks;
static double expected_target;
static double sought;
static char selected[512] = "old.mp3";
bool audiobook_is_playing(void) { return book_loaded; }
static bool eq_applied;
bool audiobook_apply_eq(bool book_playing) {
  assert(book_playing);
  eq_applied = true;
  return true;
}
void audio_get_progress(double *current, double *total) {
  *current = 123;
  *total = 1000;
}
void audiobook_note_position(double current, double total, bool force) {
	if (seeking) {
		assert(current == expected_target && total == 1000 && force);
		assert(seeks == 1 && sought == expected_target);
		notes++;
		return;
	}
  assert(current == 123 && total == 1000 && force);
  assert(strcmp(selected, "old.mp3") == 0);
  saved = true;
}
void audiobook_track_changed(const char *path) {
  assert(saved);
  snprintf(selected, sizeof(selected), "%s", path);
}
int main(void) {
  load_paused("new.mp3");
  assert(saved);
  assert(eq_applied);
	seeking = true;
	expected_target = 999;
	device_state_seek(1200);
	assert(notes == 1 && seeks == 1);
	notes = seeks = 0;
	expected_target = 0;
	device_state_seek(-5);
	assert(notes == 1 && seeks == 1);
	notes = seeks = 0;
	expected_target = 400;
	device_state_seek(400);
	assert(notes == 1 && seeks == 1);
	notes = seeks = 0;
	expected_target = 999;
	device_state_scrub_by(1200);
	assert(notes == 0 && seeks == 0);
	device_state_scrub_commit();
	assert(notes == 1 && seeks == 1);
	notes = seeks = 0;
	device_state_scrub_commit();
	assert(notes == 0 && seeks == 0);
	device_state_scrub_by(10);
	snprintf(current_metadata_file, sizeof(current_metadata_file), "%s", "other.mp3");
	device_state_scrub_commit();
	assert(notes == 0 && seeks == 0);
	book_loaded = false;
	device_state_seek(400);
	assert(notes == 0 && seeks == 1 && sought == 400);
  return 0;
}
int audio_play_paused(const char *path, double position) {
  assert(saved);
  return 0;
}
void audio_set_speed(double speed) {}
double audiobook_speed(void) { return 1.0; }
void audiobook_suppress_rewind_once(void) {}
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

audio_status_t audio_get_status(void) { return AUDIO_STATUS_PAUSED; }
void audio_seek(double seconds) {
	seeks++;
	sought = seconds;
}
double audiobook_clamp_seek(double seconds, double length) {
	double end = length > 1 ? length - 1 : 0;
	return seconds < 0 ? 0 : seconds > end ? end : seconds;
}
int audio_play(const char *path) { return 0; }
int audio_play_at(const char *path, double position) { return 0; }
bool podcastcache_is_episode(const char *path) { return false; }
bool growfile_is_growing(const char *path) { return false; }
double podcaststate_resume_point(const char *path) { return -1.0; }
bool dlna_owns_path(const char *path) { return false; }

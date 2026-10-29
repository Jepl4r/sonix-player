#include "../sonix-player/src/system/playback/device_state.c"
#include <assert.h>
static bool saved;
static char selected[512] = "old.mp3";
bool audiobook_is_playing(void) { return true; }
void audio_get_progress(double *current, double *total) {
  *current = 123;
  *total = 1000;
}
void audiobook_note_position(double current, double total, bool force) {
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

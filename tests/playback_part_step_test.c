#include "../sonix-player/src/system/playback/device_state.c"
#include <assert.h>
static const char *const parts[] = {"a.mp3", "b.mp3"};
static int queued = -1;
static bool saved;
static bool loaded;
static char selected[512] = "a.mp3";
bool audiobook_is_playing(void) { return true; }
void audio_get_progress(double *current, double *total) {
  *current = 123;
  *total = 1000;
}
void audiobook_note_position(double current, double total, bool force) {
  assert(current == 123 && total == 1000 && force);
  assert(strcmp(selected, "a.mp3") == 0);
  saved = true;
}
void audiobook_track_changed(const char *path) {
  assert(saved);
  snprintf(selected, sizeof(selected), "%s", path);
}
void playlist_load_paths_ordered(const char *const *list, int count,
                                 int start_index) {
  assert(list == parts && count == 2);
  queued = start_index;
}
bool playlist_current_path(char *out, size_t out_size) {
  assert(queued >= 0);
  snprintf(out, out_size, "%s", parts[queued]);
  return true;
}
int audio_play_paused(const char *path, double position) {
  assert(saved);
  assert(strcmp(path, "b.mp3") == 0 && position == 0);
  loaded = true;
  return 0;
}
int audio_play(const char *path) {
  assert(!"a paused step started playback");
  return 0;
}
int audio_play_at(const char *path, double position) {
  assert(!"a paused step started playback");
  return 0;
}
int main(void) {
  device_state_load_list_ordered_paused(parts, 2, 1);
  assert(queued == 1 && saved && loaded);
  assert(strcmp(selected, "b.mp3") == 0);
  return 0;
}
void audio_set_speed(double speed) {}
double audiobook_speed(void) { return 1.0; }
bool audiobook_apply_eq(bool book_playing) { return true; }
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

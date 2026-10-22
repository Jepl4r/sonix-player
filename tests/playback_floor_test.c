#define main audiobook_units_main
#include "audiobook_units_test.c"
#undef main
#define now_ms device_now_ms
#include "../sonix-player/src/system/playback/device_state.c"
#undef now_ms
static double outgoing = 70;
void audio_get_progress(double *current, double *total) { *current = outgoing; *total = 600; }
// Exercise the GUI selection before the real outgoing-write boundary. Decoder
// startup and queue construction are irrelevant to this persistence regression.
static void select_file(const char *file, double seconds) {
  supersede_book_restore();
  change_book_track(file);
}
static void select_start(const char *file) { select_file(file, 0); }
static void unused_restore(const char *file, double seconds) { assert(0); }
static void unused_restore_list(const char *const *paths, int count, int index,
                                bool ordered, const char *file,
                                double seconds) {
  assert(0);
}
static void unused_select_list(const char *const *paths, int count, int index,
                               double seconds) {
  assert(0);
}
static audio_status_t unused_toggle(void) {
  assert(0);
  return AUDIO_STATUS_STOPPED;
}
static void destination_position(const char *book, const char *file,
                                 double seconds) {}
#define device_state_play_file_at select_file
#define device_state_play_file select_start
#define device_state_restore_file unused_restore
#define device_state_restore_list unused_restore_list
#define device_state_play_list_ordered_at unused_select_list
#define device_state_toggle_play_pause unused_toggle
#define audiobookdb_save_position destination_position
#include "../sonix-player/src/gui/library/audiobooks.c"
#undef audiobookdb_save_position
#undef device_state_toggle_play_pause
#undef device_state_play_list_ordered_at
#undef device_state_restore_list
#undef device_state_restore_file
#undef device_state_play_file
#undef device_state_play_file_at
bool audiobookdb_is_folder_book(const char *book) { return false; }
void audiobookdb_touch(const char *book) {}
void player_refresh_now_playing(void) {}
audio_status_t audio_get_status(void) { return AUDIO_STATUS_PAUSED; }

int main(void) {
	audiobook_track_changed("/other.m4b");
	audiobook_set_rewind_floor("/other.m4b", 100);
	position_writes = 0;
        audiobooks_play_at("/music.mp3", "/music.mp3", 50);
        assert(saved_seconds == 100 && strcmp(saved_file, "/other.m4b") == 0);
	assert(position_writes == 1 && !explicit_book_load);
	audiobook_track_changed("/book/2.mp3");
	audiobook_set_rewind_floor("/book/2.mp3", 10);
	audiobook_track_changed("/book/1.mp3");
	outgoing = 580;
	position_writes = 0;
        audiobooks_play_at("/other.m4b", "/other.m4b", 50);
        assert(saved_seconds == 10 && strcmp(saved_file, "/book/2.mp3") == 0);
	assert(position_writes == 1 && !explicit_book_load);
	puts("PASS playback-outgoing-floor");
}

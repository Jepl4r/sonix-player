// Reuse the engine boundary stubs and break clock from the playback harness.
#define main playback_part_step_main
#include "playback_part_step_test.c"
#undef main
#include "../sonix-player/src/gui/library/audiobooks.c"
#define books_screen extras_books_screen
#define books_list extras_books_list
#define books_loaded_cb extras_books_loaded_cb
#include "../sonix-player/src/gui/library/audiobookextras.c"
#undef books_list
#undef books_loaded_cb
#undef books_screen

static mark_t picked_mark = {.file = "b.mp3", .seconds = 50};
bool player_sheet_drag_active(void) { return false; }
bool switcher_back_drag_active(void) { return false; }
void player_refresh_now_playing(void) {}
void player_sheet_open(bool animated) {}
void back_btn_cb(lv_event_t *event) {}
void *lv_event_get_user_data(lv_event_t *event) { return NULL; }
void playlist_load_folder(const char *folder, const char *filename) {}
bool audiobookdb_is_folder_book(const char *book) { return false; }
int audiobookdb_parts_for_each(const char *book, audiobook_part_cb cb,
                               void *user) {
  return 0;
}
void audiobookdb_touch(const char *book) {}
void audiobookdb_save_position(const char *book, const char *file,
                               double seconds) {}
bool audiobookdb_get_position(const char *book, char *file, size_t size,
                              double *seconds, time_t *last) {
  return false;
}
void audiobook_note_boot_break(time_t last) {}
const char *audiobook_current_path(void) { return selected; }
bool audiobook_resume_point(const char *book, char *file, size_t size,
                            double *seconds) {
  snprintf(file, size, "%s", selected);
  *seconds = current_position;
  return true;
}

static double current_total_length;
static void update_progress(void) {}
#include "gui-player-navigation.h"
bool audiobook_step_target(double seconds, int direction,
                           audiobook_step_t *out) {
  *out = (audiobook_step_t){.kind = AUDIOBOOK_STEP_PART, .part = 1};
  return true;
}

int main(int argc, char **argv) {
  assert(argc == 2);
  navigation_test = true;
  saved = true;
  test_status = AUDIO_STATUS_PAUSED;
  floor_armed = true;
  snprintf(selected, sizeof(selected), "a.mp3");
  snprintf(open_book, sizeof(open_book), "book");
  marks = &picked_mark;
  mark_count = 1;
  audiobook_mark_jump("a.mp3", current_position);
  int before = jump_marks;
  if (!strcmp(argv[1], "failed-bookmark")) {
    navigation_readable = false;
    mark_pick_cb(NULL);
    assert(floor_armed && jump_marks == before);
    puts("PASS bookmark-failed-part-keeps-floor-and-origin");
    return 0;
  }
  if (!strcmp(argv[1], "failed-chapter")) {
    fail_load = true;
    player_step_chapter(1);
    assert(floor_armed && jump_marks == before);
    puts("PASS chapter-failed-load-keeps-floor-and-origin");
    return 0;
  }
  if (!strcmp(argv[1], "skip-origin")) {
    player_step_chapter(1);
    assert(jump_marks == before + 1 && origin_seconds == 123);
    char before_file[512];
    snprintf(before_file, sizeof(before_file), "%s", origin_file);
    device_state_t state;
    device_state_get(&state);
    floor_armed = true;
    suppress_rewind = false;
    unsigned serial = device_state_seek_serial();
    audiobook_seek_by(10, &state);
    assert(jump_marks == before + 1 && origin_seconds == 123 &&
           !strcmp(origin_file, before_file));
    assert(!floor_armed && suppress_rewind &&
           device_state_seek_serial() == serial + 1 && sought == 10);
    puts("PASS skip-after-chapter-keeps-origin");
    return 0;
  }
  if (!strcmp(argv[1], "outgoing-floor")) {
    snprintf(open_book, sizeof(open_book), "other-book");
    mark_pick_cb(NULL);
    assert(outgoing_floor);
    puts("PASS bookmark-other-book-keeps-outgoing-floor");
    return 0;
  }

  navigation_readable = true;
  suppress_rewind = false;
  book_loaded = rewind_on = true;
  crossing = false;
  current_position = 123;
  device_state_t state;
  device_state_get(&state);
  boot_gap = 7200;
  sought = -1;
  play_book("book");
  assert(sought == 93 && boot_gap == 0 && observed_gap == 7200);
  play_book("book");
  assert(sought == 93);
  puts("PASS library-loaded-paused-book-rewinds-once");
}

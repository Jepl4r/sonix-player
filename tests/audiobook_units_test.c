#include "../sonix-player/src/system/playback/audiobook.c"
#include <assert.h>

// What audiobook.c links against, none of which these cases reach.
int audiobookdb_parts_for_each(const char *book,
                               bool (*cb)(const char *, const char *, void *),
                               void *user) {
  return 0;
}
const char *tr(const char *key) { return key; }

static void set_chapters(const double *starts, int count) {
  free(chapters);
  chapters = calloc((size_t)count, sizeof(*chapters));
  for (int i = 0; i < count; i++) {
    chapters[i].start = starts[i];
    snprintf(chapters[i].title, sizeof(chapters[i].title), "Chapter %d", i + 1);
  }
  chapter_count = count;
}

static void set_parts(int count, int index) {
  free(parts);
  parts = calloc((size_t)count, sizeof(*parts));
  for (int i = 0; i < count; i++) {
    snprintf(parts[i].path, sizeof(parts[i].path), "/book/%d.mp3", i);
    snprintf(parts[i].title, sizeof(parts[i].title), "Part %d", i + 1);
  }
  part_count = count;
  part_capacity = count;
  part_index = index;
}

static void reset(void) {
  clear_locked();
  current_is_book = true;
}

static audiobook_step_t step(double seconds, int direction, bool *ok) {
  audiobook_step_t target = {AUDIOBOOK_STEP_NONE, -1, -1};
  *ok = audiobook_step_target(seconds, direction, &target);
  return target;
}

static void expect_seek(double seconds, int direction, double want) {
  bool ok;
  audiobook_step_t t = step(seconds, direction, &ok);
  assert(ok && t.kind == AUDIOBOOK_STEP_SEEK && t.seconds == want);
}

static void expect_part(double seconds, int direction, int want) {
  bool ok;
  audiobook_step_t t = step(seconds, direction, &ok);
  assert(ok && t.kind == AUDIOBOOK_STEP_PART && t.part == want);
}

static void expect_none(double seconds, int direction) {
  bool ok;
  audiobook_step_t t = step(seconds, direction, &ok);
  assert(ok && t.kind == AUDIOBOOK_STEP_NONE);
}

static void chapters_next_prev(void) {
  reset();
  const double starts[] = {0, 100, 200};
  set_chapters(starts, 3);
  assert(audiobook_unit_count() == 3);
  assert(audiobook_unit_current(250) == 2);
  assert(audiobook_unit_current(0) == 0);
  char title[64];
  assert(audiobook_unit_title(1, title, sizeof(title)) &&
         strcmp(title, "Chapter 2") == 0);
  assert(!audiobook_unit_title(3, title, sizeof(title)));
  assert(!audiobook_unit_title(-1, title, sizeof(title)));
  expect_seek(150, +1, 200);
  expect_none(250, +1);
  expect_seek(102, -1, 0);
  expect_seek(105, -1, 100);
  expect_seek(103, -1, 0); // exactly 3.0 s in is still the start of the unit
  expect_seek(1, -1, 0);
}

static void parts_next_prev(void) {
  reset();
  set_parts(3, 1);
  assert(audiobook_unit_count() == 3);
  assert(audiobook_unit_current(50) == 1);
  char title[64];
  assert(audiobook_unit_title(1, title, sizeof(title)) &&
         strcmp(title, "Part 2") == 0);
  assert(!audiobook_unit_title(3, title, sizeof(title)));
  expect_part(50, +1, 2);
  expect_part(2, -1, 0);
  expect_seek(10, -1, 0);
  part_index = 2;
  expect_none(50, +1);
  part_index = 0;
  expect_seek(1, -1, 0);
  expect_part(50, +1, 1);
}

static void parts_win_over_chapters(void) {
  reset();
  const double starts[] = {0, 100, 200, 300};
  set_chapters(starts, 4);
  set_parts(2, 0);
  assert(audiobook_unit_count() == 2);
  assert(audiobook_unit_current(250) == 0);
  expect_part(250, +1, 1);
  part_index = 1;
  expect_part(1, -1, 0);
  expect_none(250, +1);
}

static void one_part_uses_chapters(void) {
  reset();
  const double starts[] = {0, 100};
  set_chapters(starts, 2);
  set_parts(1, 0);
  assert(audiobook_unit_count() == 2);
  assert(audiobook_unit_current(150) == 1);
}

static void no_units(void) {
  reset();
  assert(audiobook_unit_count() == 0);
  assert(audiobook_unit_current(10) == -1);
  char title[64];
  assert(!audiobook_unit_title(0, title, sizeof(title)));
  bool ok;
  audiobook_step_t t = step(10, +1, &ok);
  assert(!ok && t.kind == AUDIOBOOK_STEP_NONE);
  step(10, -1, &ok);
  assert(!ok);
}

int main(void) {
  chapters_next_prev();
  parts_next_prev();
  parts_win_over_chapters();
  one_part_uses_chapters();
  no_units();
  return 0;
}

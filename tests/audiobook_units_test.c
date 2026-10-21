#include "../sonix-player/src/system/playback/audiobook.c"
#include <assert.h>

// What audiobook.c links against, none of which these cases reach.
int audiobookdb_parts_for_each(const char *book,
                               bool (*cb)(const char *, const char *, void *),
                               void *user) {
  return 0;
}
const char *tr(const char *key) { return key; }

// An in-memory [audiobook] config, for the equalizer setting.
#define STUB_MAX 16
static char stub_key[STUB_MAX][64];
static char stub_value[STUB_MAX][64];
static int stub_count;
static int saves;

const char *config_get(const char *section, const char *key,
                       const char *fallback) {
  assert(strcmp(section, "audiobook") == 0);
  for (int i = 0; i < stub_count; i++) {
    if (strcmp(stub_key[i], key) == 0) {
      return stub_value[i];
    }
  }
  return fallback;
}

void config_set(const char *section, const char *key, const char *value) {
  assert(strcmp(section, "audiobook") == 0);
  for (int i = 0; i < stub_count; i++) {
    if (strcmp(stub_key[i], key) == 0) {
      snprintf(stub_value[i], sizeof(stub_value[i]), "%s", value);
      return;
    }
  }
  assert(stub_count < STUB_MAX);
  snprintf(stub_key[stub_count], sizeof(stub_key[0]), "%s", key);
  snprintf(stub_value[stub_count], sizeof(stub_value[0]), "%s", value);
  stub_count++;
}

bool config_save(void) {
  saves++;
  return true;
}

// The engine side, recorded: which override was last put on it.
static enum {
  EQ_CALL_NONE,
  EQ_CALL_BANDS,
  EQ_CALL_BYPASS,
  EQ_CALL_CLEAR
} eq_call;
static int eq_call_bands[EQ_BANDS];
static const eq_preset_t vocal = {"eq_vocal",
                                  {-2, -2, -1, 2, 4, 4, 3, 1, 0, -1}};
static const int voice[EQ_BANDS] = {3, 2, 1, 0, 4, 5, 1, 0, -1, -2};

void eq_override_bands(const int gain_db[EQ_BANDS]) {
  eq_call = EQ_CALL_BANDS;
  memcpy(eq_call_bands, gain_db, sizeof(eq_call_bands));
}
void eq_override_bypass(void) { eq_call = EQ_CALL_BYPASS; }
void eq_override_clear(void) { eq_call = EQ_CALL_CLEAR; }
bool eq_override_active(void) {
  return eq_call == EQ_CALL_BANDS || eq_call == EQ_CALL_BYPASS;
}
bool eq_preset_read(const char *name, int gain_db_out[EQ_BANDS]) {
  if (strcmp(name, "Voice") != 0) {
    return false;
  }
  memcpy(gain_db_out, voice, sizeof(voice));
  return true;
}
const eq_preset_t *eq_default_preset_by_name(const char *name) {
  return strcmp(name, "eq_vocal") == 0 ? &vocal : NULL;
}

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

static void eq_setting(void) {
  stub_count = 0;
  char preset[64];
  assert(audiobook_eq_mode() == AUDIOBOOK_EQ_MUSIC);
  assert(strcmp(audiobook_eq_preset(preset, sizeof(preset)), "") == 0);

  saves = 0;
  audiobook_set_eq(AUDIOBOOK_EQ_PRESET, "user:Voice");
  assert(saves == 1);
  assert(audiobook_eq_mode() == AUDIOBOOK_EQ_PRESET);
  assert(strcmp(audiobook_eq_preset(preset, sizeof(preset)), "user:Voice") ==
         0);

  config_set("audiobook", "eq_mode", "loud");
  assert(audiobook_eq_mode() == AUDIOBOOK_EQ_MUSIC);
}

static void eq_apply(void) {
  stub_count = 0;

  // Same as music: nothing over the music curve.
  eq_call = EQ_CALL_NONE;
  assert(audiobook_apply_eq(true) && eq_call == EQ_CALL_CLEAR);

  audiobook_set_eq(AUDIOBOOK_EQ_OFF, "");
  assert(audiobook_apply_eq(true) && eq_call == EQ_CALL_BYPASS);
  assert(eq_override_active());

  // Not a book: the override comes off whatever the setting says.
  assert(audiobook_apply_eq(false) && eq_call == EQ_CALL_CLEAR);
  assert(audiobook_eq_mode() == AUDIOBOOK_EQ_OFF);

  audiobook_set_eq(AUDIOBOOK_EQ_PRESET, "eq_vocal");
  assert(audiobook_apply_eq(true) && eq_call == EQ_CALL_BANDS);
  assert(memcmp(eq_call_bands, vocal.gain_db, sizeof(eq_call_bands)) == 0);

  audiobook_set_eq(AUDIOBOOK_EQ_PRESET, "user:Voice");
  assert(audiobook_apply_eq(true) && eq_call == EQ_CALL_BANDS);
  assert(memcmp(eq_call_bands, voice, sizeof(eq_call_bands)) == 0);

  // A user preset deleted since: back to the music curve, setting and all.
  audiobook_set_eq(AUDIOBOOK_EQ_PRESET, "user:gone");
  saves = 0;
  assert(!audiobook_apply_eq(true));
  assert(eq_call == EQ_CALL_CLEAR);
  assert(audiobook_eq_mode() == AUDIOBOOK_EQ_MUSIC);
  assert(saves == 1);
  assert(audiobook_apply_eq(true) && eq_call == EQ_CALL_CLEAR);
}

int main(void) {
  chapters_next_prev();
  parts_next_prev();
  parts_win_over_chapters();
  one_part_uses_chapters();
  no_units();
  eq_setting();
  eq_apply();
  return 0;
}

#include "../sonix-player/src/system/playback/audiobook.c"
#include <assert.h>

// What audiobook.c links against.
int audiobookdb_parts_for_each(const char *book,
                               bool (*cb)(const char *, const char *, double, void *),
                               void *user) {
  return 0;
}
const char *tr(const char *key) { return key; }

static int position_writes;
static int position_queues;
static int finished_calls;
static char saved_book[512];
static char saved_file[512];
static char finished_book[512];
static double saved_seconds;
static void set_parts(int count, int index);

void audiobookdb_save_position(const char *book, const char *file, double seconds) {
	position_writes++;
	snprintf(saved_book, sizeof(saved_book), "%s", book);
	snprintf(saved_file, sizeof(saved_file), "%s", file);
	saved_seconds = seconds;
}

void audiobookdb_queue_position(const char *book, const char *file, double seconds) {
	position_queues++;
	snprintf(saved_book, sizeof(saved_book), "%s", book);
	snprintf(saved_file, sizeof(saved_file), "%s", file);
	saved_seconds = seconds;
}

void audiobookdb_mark_finished(const char *book) {
	finished_calls++;
	snprintf(finished_book, sizeof(finished_book), "%s", book);
}

static void seeks_stay_short_of_eof(void) {
	assert(audiobook_clamp_seek(60, 60) == 59);
	assert(audiobook_clamp_seek(59.8, 60) == 59);
	assert(audiobook_clamp_seek(55, 60) == 55);
	assert(audiobook_clamp_seek(-5, 60) == 0);
	assert(audiobook_clamp_seek(0.5, 0.5) == 0);
	assert(audiobook_clamp_seek(5, 0) == 5);
}

static void late_position_is_kept(void) {
	audiobook_track_changed(NULL);
	audiobook_track_changed("/other.m4b");
	position_writes = position_queues = finished_calls = 0;
	audiobook_note_position(50, 60, true);
	assert(position_writes == 1 && position_queues == 0);
	assert(saved_seconds == 50);
	assert(strcmp(saved_book, "/other.m4b") == 0);
	assert(strcmp(saved_file, "/other.m4b") == 0);
	assert(finished_calls == 0);
}

static void natural_finish_starts_over(void) {
	audiobook_track_changed(NULL);
	audiobook_track_changed("/other.m4b");
	position_writes = position_queues = finished_calls = 0;
	audiobook_note_position(50, 60, true);
	audiobook_note_finished();
	assert(finished_calls == 1);
	assert(strcmp(finished_book, "/other.m4b") == 0);
	assert(position_writes == 2 && position_queues == 0);
	assert(saved_seconds == 0);
	assert(strcmp(saved_book, "/other.m4b") == 0);
	assert(strcmp(saved_file, "/other.m4b") == 0);
	audiobook_note_finished();
	assert(finished_calls == 1 && position_writes == 2);
}

static void folder_finish_starts_at_first_part(void) {
	audiobook_track_changed(NULL);
	audiobook_track_changed("/book/2.mp3");
	set_parts(3, 2);
	position_writes = position_queues = finished_calls = 0;
	audiobook_note_finished();
	assert(finished_calls == 1);
	assert(strcmp(finished_book, "/book") == 0);
	assert(position_writes == 1 && position_queues == 0);
	assert(saved_seconds == 0);
	assert(strcmp(saved_book, "/book") == 0);
	assert(strcmp(saved_file, "/book/0.mp3") == 0);
	audiobook_note_finished();
	audiobook_note_position(0, 60, true);
	assert(finished_calls == 1 && position_writes == 1);
	audiobook_track_changed("/song.mp3");
	audiobook_note_finished();
	assert(finished_calls == 1 && position_writes == 1);
}

static void checkpoints_do_not_finish(void) {
	audiobook_track_changed(NULL);
	audiobook_track_changed("/book/2.mp3");
	set_parts(3, 2);
	position_writes = position_queues = finished_calls = 0;
	audiobook_note_position(50, 60, false);
	assert(position_queues == 1 && position_writes == 0);
	assert(saved_seconds == 50 && finished_calls == 0);
	assert(strcmp(saved_file, "/book/2.mp3") == 0);
	audiobook_note_position(55, 60, false);
	assert(position_queues == 1);
	audiobook_note_position(55, 60, true);
	assert(position_writes == 1 && saved_seconds == 55);
	audiobook_note_position(60, 60, true);
	assert(position_writes == 2 && saved_seconds == 60);
	assert(finished_calls == 0);
}

// The database side of the per-book speed: "/book" has 1500 stored, the rest
// none. Each write is recorded and read back on the next load.
static int get_speed_calls;
static int set_speed_calls;
static char set_speed_book[512];
static int set_speed_value;
static int other_speed;
static int folder_speed = 1500;

int audiobookdb_get_speed(const char *book_path) {
	get_speed_calls++;
	return strcmp(book_path, "/book") == 0 ? folder_speed : other_speed;
}
void audiobookdb_set_speed(const char *book_path, int permille) {
	set_speed_calls++;
	snprintf(set_speed_book, sizeof(set_speed_book), "%s", book_path);
	set_speed_value = permille;
	if (strcmp(book_path, "/book") == 0) {
		folder_speed = permille;
	} else {
		other_speed = permille;
	}
}

// Two parts of the folder book "/book", a single-file book, and nothing else.
bool audiobookdb_book_for_file(const char *file_path, char *book_out,
                               size_t book_size) {
  const char *book = NULL;
  if (strncmp(file_path, "/book/", 6) == 0) {
    book = "/book";
  } else if (strcmp(file_path, "/other.m4b") == 0) {
    book = file_path;
  }
  snprintf(book_out, book_size, "%s", book ? book : "");
  return book != NULL;
}
mp4_file_t *mp4_open(const char *path) { return NULL; }
int id3chap_read(const char *path, id3chap_t *out, int max) { return 0; }
int vorbischap_read(const char *path, vorbischap_t *out, int max) { return 0; }

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

long config_get_int(const char *section, const char *key, long fallback) {
  const char *value = config_get(section, key, NULL);
  return value ? strtol(value, NULL, 10) : fallback;
}

void config_set_int(const char *section, const char *key, long value) {
  char text[32];
  snprintf(text, sizeof(text), "%ld", value);
  config_set(section, key, text);
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
bool eq_preset_read(const char *name, int gain_db_out[EQ_BANDS],
                    bool *missing) {
  // "busy" stands for a card that cannot be read just now.
  *missing = strcmp(name, "busy") != 0;
  if (strcmp(name, "Voice") != 0) {
    return false;
  }
  *missing = false;
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
  assert(!audiobook_take_eq_fallback());

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
  assert(audiobook_take_eq_fallback());
  assert(!audiobook_take_eq_fallback());

  // A preset that cannot be read just now: plain music for this track, the
  // setting kept, nothing to tell.
  audiobook_set_eq(AUDIOBOOK_EQ_PRESET, "user:busy");
  saves = 0;
  eq_call = EQ_CALL_NONE;
  assert(audiobook_apply_eq(true) && eq_call == EQ_CALL_CLEAR);
  assert(audiobook_eq_mode() == AUDIOBOOK_EQ_PRESET && saves == 0);
  assert(!audiobook_take_eq_fallback());

  // An unknown built-in tag is gone just the same.
  audiobook_set_eq(AUDIOBOOK_EQ_PRESET, "eq_bogus");
  assert(!audiobook_apply_eq(true) && eq_call == EQ_CALL_CLEAR);
  assert(audiobook_eq_mode() == AUDIOBOOK_EQ_MUSIC);
  assert(audiobook_take_eq_fallback());
  assert(!audiobook_take_eq_fallback());
}

static void speed_per_book(void) {
	stub_count = 0;
	audiobook_track_changed(NULL);
	assert(audiobook_speed_permille() == AUDIOBOOK_SPEED_NORMAL);

	// Nothing loaded: the default alone.
	audiobook_set_speed_permille(1250);
	assert(set_speed_calls == 0);
	assert(config_get_int("audiobook", "speed", 0) == 1250);
	assert(audiobook_speed_permille() == 1250);

	// A book with a speed of its own plays at it, and another part of the same
	// book does not ask again.
	audiobook_track_changed("/book/0.mp3");
	assert(get_speed_calls == 1);
	assert(audiobook_speed_permille() == 1500);
	audiobook_track_changed("/book/1.mp3");
	assert(get_speed_calls == 1);
	assert(audiobook_speed_permille() == 1500);

	// A pick is the book's and the new default.
	audiobook_set_speed_permille(750);
	assert(set_speed_calls == 1 && set_speed_value == 750);
	assert(strcmp(set_speed_book, "/book") == 0);
	assert(config_get_int("audiobook", "speed", 0) == 750);
	assert(audiobook_speed_permille() == 750);

	// First listen pins the default, even after another book changes it.
	audiobook_track_changed("/other.m4b");
	assert(get_speed_calls == 2);
	assert(audiobook_speed_permille() == 750);
	assert(set_speed_calls == 2 && set_speed_value == 750);
	assert(strcmp(set_speed_book, "/other.m4b") == 0);
	audiobook_track_changed("/book/0.mp3");
	audiobook_set_speed_permille(1500);
	audiobook_track_changed("/other.m4b");
	assert(audiobook_speed_permille() == 750);
	assert(set_speed_calls == 3);

	// A song is no book: the default, and a pick goes nowhere else.
	audiobook_track_changed("/song.mp3");
	assert(audiobook_speed_permille() == 1500);
	audiobook_set_speed_permille(1000);
	assert(set_speed_calls == 3);
	assert(audiobook_speed_permille() == 1000);
}

static int length_writes;
static char length_path[512];
static double length_seconds;
void audiobookdb_set_part_duration(const char *path, double seconds) {
	assert(pthread_mutex_trylock(&lock) == 0);
	pthread_mutex_unlock(&lock);
	length_writes++;
	snprintf(length_path, sizeof(length_path), "%s", path);
	length_seconds = seconds;
}
static void measured_lengths(void) {
	const double stored[] = {0, 100, 100};
	const double measured[] = {80, 100.4, 103};
	for (int i = 0; i < 3; i++) {
		reset();
		snprintf(current_path, sizeof(current_path), "/book/0.mp3");
		add_part(current_path, "First", stored[i], NULL);
		part_index = 0;
		length_writes = 0;
		audiobook_note_length(0);
		audiobook_note_length(measured[i]);
		audiobook_note_length(measured[i]);
		assert(length_writes == (i == 1 ? 0 : 1));
		if (i != 1) {
			assert(length_seconds == measured[i]);
			assert(!strcmp(length_path, current_path));
		}
		assert(audiobook_part_duration(0) == (i == 1 ? 100 : measured[i]));
		assert(audiobook_part_duration(-1) == 0 && audiobook_part_duration(1) == 0);
	}
	// A different measurement in the same load is ignored; reloading re-arms it.
	audiobook_note_length(106);
	assert(length_writes == 1);
	audiobook_track_changed("/book/0.mp3");
	audiobook_note_length(106);
	assert(length_writes == 2 && length_seconds == 106);
	audiobook_track_changed(NULL);
	audiobook_note_length(200);
	assert(length_writes == 2);
	puts("PASS audiobook-length");
}
static void book_progress(void) {
	reset();
	set_parts(3, 1);
	parts[0].duration = parts[2].duration = 600;
	audiobook_progress_t p;
	assert(audiobook_book_progress(300, 1, &p));
	assert(!p.known_total && !p.known_left);
	parts[1].duration = 600;
	assert(audiobook_book_progress(300, 1.5, &p));
	assert(p.known_total && p.known_left);
	assert(fabs(p.fraction - 0.5) < 1e-9 && p.left_seconds == 600);
	parts[0].duration = 0;
	assert(audiobook_book_progress(300, 1, &p));
	assert(!p.known_total && p.known_left && p.left_seconds == 900);
	puts("PASS audiobook-progress-parts");

	reset();
	assert(audiobook_book_progress(30, 1, &p));
	assert(!p.known_total && !p.known_left);
	audiobook_note_length(120);
	assert(audiobook_book_progress(30, 2, &p));
	assert(p.known_total && p.known_left && p.fraction == 0.25 && p.left_seconds == 45);
	assert(audiobook_book_progress(-10, 0, &p) && p.fraction == 0 && p.left_seconds == 120);
	assert(audiobook_book_progress(200, 1, &p) && p.fraction == 1 && p.left_seconds == 0);
	audiobook_track_changed(NULL);
	assert(!audiobook_book_progress(0, 1, &p));
	reset();
	assert(audiobook_book_progress(0, 1, &p) && !p.known_total && !p.known_left);
	puts("PASS audiobook-progress-single");
}

int main(void) {
	book_progress();
	measured_lengths();
  chapters_next_prev();
  parts_next_prev();
  parts_win_over_chapters();
  one_part_uses_chapters();
  no_units();
  eq_setting();
  eq_apply();
  speed_per_book();
	seeks_stay_short_of_eof();
	late_position_is_kept();
	natural_finish_starts_over();
	folder_finish_starts_at_first_part();
	checkpoints_do_not_finish();
  return 0;
}

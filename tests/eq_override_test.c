#include <math.h>
static double rebuild_pow(double base, double exponent);
#define pow rebuild_pow
#include "../sonix-player/src/system/audio/eq.c"
#undef pow
#include <assert.h>
#include <fcntl.h>

// An in-memory config: enough for eq.c, which only reads and writes integers.
#define STUB_MAX 64
static char stub_section[STUB_MAX][32];
static char stub_key[STUB_MAX][32];
static long stub_value[STUB_MAX];
static int stub_count;
static int eq_writes; // config_set_int calls on [eq]

static int stub_find(const char *section, const char *key) {
  for (int i = 0; i < stub_count; i++) {
    if (strcmp(stub_section[i], section) == 0 &&
        strcmp(stub_key[i], key) == 0) {
      return i;
    }
  }
  return -1;
}

long config_get_int(const char *section, const char *key, long fallback) {
  int i = stub_find(section, key);
  return i < 0 ? fallback : stub_value[i];
}

void config_set_int(const char *section, const char *key, long value) {
  if (strcmp(section, "eq") == 0) {
    eq_writes++;
  }
  int i = stub_find(section, key);
  if (i < 0) {
    assert(stub_count < STUB_MAX);
    i = stub_count++;
    snprintf(stub_section[i], sizeof(stub_section[i]), "%s", section);
    snprintf(stub_key[i], sizeof(stub_key[i]), "%s", key);
  }
  stub_value[i] = value;
}

bool config_save(void) { return true; }

static void music_curve(int enabled, int band0) {
  stub_count = 0;
  config_set_int("eq", "enabled", enabled);
  config_set_int("eq", "band0", band0);
  eq_init();
  eq_override_clear();
  eq_writes = 0;
}

// What the audio thread does with the next buffer, with its log line kept off
// the test's output.
static bool next_buffer_runs(void) {
  fflush(stderr);
  int saved = dup(2);
  int null = open("/dev/null", O_WRONLY);
  dup2(null, 2);
  bool runs = process_prepare(64, 2, 44100);
  fflush(stderr);
  dup2(saved, 2);
  close(null);
  close(saved);
  return runs;
}

static void override_wins(void) {
  music_curve(0, 0);
  const int six[EQ_BANDS] = {6, 6, 6, 6, 6, 6, 6, 6, 6, 6};
  unsigned before = settings_version;
  eq_override_bands(six);
  assert(settings_version != before);
  assert(eq_override_active());
  assert(eq_effective_enabled());
  assert(eq_effective_band(0) == 6 && eq_effective_band(9) == 6);

  // The settings still say what the EQ page shows, and nothing was saved.
  assert(eq_get_band(0) == 0);
  assert(!eq_get_enabled());
  assert(eq_writes == 0);

  // The filters, the pre-gain and the early-out all hear the override.
  assert(eq_auto_headroom_tenths() == -60);
  assert(next_buffer_runs());
  assert(chain_active[MSEB_FILTERS]);
  assert(fabsf(pregain_target - 0.5012f) < 0.001f);
}

static void bypass_silences(void) {
  music_curve(1, 5);
  eq_override_bypass();
  assert(eq_override_active());
  assert(!eq_effective_enabled());
  assert(eq_get_enabled() && eq_get_band(0) == 5);
  assert(eq_auto_headroom_tenths() == 0);
  assert(!next_buffer_runs());

  unsigned before = settings_version;
  eq_override_clear();
  assert(settings_version != before);
  assert(!eq_override_active());
  assert(eq_effective_enabled());
  assert(eq_effective_band(0) == 5);
  assert(eq_auto_headroom_tenths() == -50);
  assert(next_buffer_runs());
  assert(eq_writes == 0);
}

// The math call interrupts the real rebuild after it reads its first gain.
static bool change_during_build;
static double rebuild_pow(double base, double exponent) {
	if (change_during_build) {
		change_during_build = false;
		const int cuts[EQ_BANDS] = {-6, -6, -6, -6, -6, -6, -6, -6, -6, -6};
		eq_override_bands(cuts);
	}
	return pow(base, exponent);
}

static void rebuild_retries_changed_override(void) {
	music_curve(0, 0);
	const int boosts[EQ_BANDS] = {6, 6, 6, 6, 6, 6, 6, 6, 6, 6};
	eq_override_bands(boosts);
	unsigned before = settings_version;
	change_during_build = true;
	assert(next_buffer_runs());
	assert(!change_during_build);
	assert(settings_version != before);
	assert(built_version == before);
	for (int i = 0; i < EQ_BANDS; i++) {
		biquad_t expected = {0};
		assert(biquad_set_coeffs(&expected, F_PEAK, eq_band_freq[i], 6, EQ_BAND_Q, 44100));
		assert(chain[MSEB_FILTERS + i].b0 == expected.b0);
	}
	assert(fabsf(pregain_target - 0.5012f) < 0.001f);
	assert(next_buffer_runs());
	assert(built_version == settings_version);
	for (int i = 0; i < EQ_BANDS; i++) {
		biquad_t expected = {0};
		assert(biquad_set_coeffs(&expected, F_PEAK, eq_band_freq[i], -6, EQ_BAND_Q, 44100));
		assert(chain[MSEB_FILTERS + i].b0 == expected.b0);
	}
	assert(fabsf(pregain_target - 1.0f) < 0.001f);
}

static void preset_read(const char *dir) {
  char path[600];
  snprintf(path, sizeof(path), "%s/.local", dir);
  mkdir(path, 0777);
  snprintf(path, sizeof(path), "%s/.local/eq", dir);
  mkdir(path, 0777);
  snprintf(path, sizeof(path), "%s/.local/eq/Voice.ini", dir);
  FILE *f = fopen(path, "w");
  assert(f);
  fputs(
      "# equalizer "
      "preset\nb0=3\nb1=2\nb2=1\nb3=0\nb4=4\nb5=5\nb6=1\nb7=0\nb8=-1\nb9=-2\n",
      f);
  fclose(f);

  music_curve(0, 0);
  eq_presets_set_dir(dir);
  const int want[EQ_BANDS] = {3, 2, 1, 0, 4, 5, 1, 0, -1, -2};
  int out[EQ_BANDS];
  bool missing = true;
  assert(eq_preset_read("Voice", out, &missing) && !missing);
  assert(memcmp(out, want, sizeof(want)) == 0);
  assert(eq_get_band(0) == 0 && eq_writes == 0); // read, not applied
  // Gone for certain: no such file, or a name no file could have.
  assert(!eq_preset_read("Gone", out, &missing) && missing);
  missing = false;
  assert(!eq_preset_read("a/b", out, &missing) && missing);
  assert(!eq_preset_read("Gone", out, NULL));

  // A file that is there but cannot be read is not gone either.
  snprintf(path, sizeof(path), "%s/.local/eq/Locked.ini", dir);
  f = fopen(path, "w");
  assert(f);
  fclose(f);
  chmod(path, 0);
  if (geteuid() != 0) { // root reads it anyway
    missing = true;
    assert(!eq_preset_read("Locked", out, &missing) && !missing);
  }

  // Loading through the same parser still applies the curve.
  assert(eq_preset_load("Voice"));
  assert(eq_get_band(0) == 3 && eq_get_band(9) == -2);

  // No card to look on is not the same as no preset: not missing.
  eq_presets_set_dir("");
  missing = true;
  assert(!eq_preset_read("Voice", out, &missing) && !missing);

  assert(eq_default_preset_by_name("eq_vocal") != NULL);
  assert(eq_default_preset_by_name("eq_vocal")->gain_db[4] == 4);
  assert(eq_default_preset_by_name("nope") == NULL);
  assert(eq_default_preset_by_name(NULL) == NULL);
}

int main(int argc, char **argv) {
  assert(argc == 2);
  override_wins();
  bypass_silences();
	rebuild_retries_changed_override();
  preset_read(argv[1]);
  return 0;
}

#include "audiobook.h"

#include "src/system/library/audiobookdb.h"
#include "src/system/audio/eq.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/decode/mp4.h"
#include "src/system/library/id3chap.h"
#include "src/system/library/vorbischap.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// How often the position is written back while a book is playing. These
// checkpoints go through the background writer; the saves that are forced --
// pause, seek, stop -- are written before returning.
#define SAVE_EVERY_SECONDS 10

// Within this much of the end counts as finished: the last words of a book are
// followed by credits nobody sits through, and a listener who stops there has
// finished it.
#define FINISHED_MARGIN 45.0

typedef struct {
	double start;
	char title[192];
} chapter_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static char current_path[512];
static char current_book[512]; // the file itself, or the folder of a folder book
static bool current_is_book;
static chapter_t *chapters;
static int chapter_count;
static unsigned serial;

// The files of a folder book, in its order, and which of them is loaded.
typedef struct {
	char path[512];
	char title[192];
} part_t;

static part_t *parts;
static int part_count;
static int part_capacity;
static int part_index;

static time_t last_save;
static double last_saved_position;
static bool finished_marked; // this book has already been put on the shelf
static int book_speed;		 // the loaded book's own speed in permille, 0 for none

static void clear_locked(void) {
	free(chapters);
	chapters = NULL;
	chapter_count = 0;
	free(parts);
	parts = NULL;
	part_count = 0;
	part_capacity = 0;
	part_index = -1;
	current_is_book = false;
	current_path[0] = '\0';
	current_book[0] = '\0';
}

// Called with `lock` held, from audiobook_track_changed().
static bool add_part(const char *path, const char *title, void *user) {
	(void)user;
	if (part_count == part_capacity) {
		int grown = part_capacity ? part_capacity * 2 : 16;
		part_t *bigger = realloc(parts, (size_t)grown * sizeof(*bigger));
		if (!bigger) {
			return false;
		}
		parts = bigger;
		part_capacity = grown;
	}
	snprintf(parts[part_count].path, sizeof(parts[0].path), "%s", path);
	snprintf(parts[part_count].title, sizeof(parts[0].title), "%s", title);
	part_count++;
	return true;
}

// The title a chapter is stored under. Both sources leave it empty when the
// file carries no name for it, and a numbered stand-in is better than a blank
// row; the number is the chapter's place in the book, counted from one.
static void name_chapter(char *out, size_t size, const char *title, int index) {
	if (title && title[0]) {
		snprintf(out, size, "%s", title);
		return;
	}
	snprintf(out, size, tr("chapter_n"), index + 1);
}

// The chapters an Opus, Ogg Vorbis or FLAC file keeps in its comments.
static void load_vorbis_chapters_locked(const char *filepath) {
	int count = vorbischap_read(filepath, NULL, 0);
	if (count <= 0) {
		return;
	}
	vorbischap_t *found = calloc((size_t)count, sizeof(*found));
	if (!found) {
		return;
	}
	count = vorbischap_read(filepath, found, count);
	chapters = count > 0 ? calloc((size_t)count, sizeof(*chapters)) : NULL;
	if (chapters) {
		for (int i = 0; i < count; i++) {
			chapters[i].start = found[i].start;
			name_chapter(chapters[i].title, sizeof(chapters[i].title), found[i].title, i);
		}
		chapter_count = count;
	}
	free(found);
}

// Reads the chapter marks out of the container. Done once per book rather than
// held open for its length: an open file handle on the card is what stops the
// card from being unmounted, and the marks are a few kilobytes that never
// change.
static void load_chapters_locked(const char *filepath) {
	mp4_file_t *m = mp4_open(filepath);
	if (!m) {
		// Not an MP4. An audiobook is just as often one long .mp3 with ID3v2
		// CHAP frames in it -- what every tool that splits a book by chapter
		// writes -- and without this those arrive as a four-hour track with no
		// marks at all.
		int count = id3chap_read(filepath, NULL, 0);
		if (count <= 0) {
			load_vorbis_chapters_locked(filepath);
			return;
		}
		int capacity = count < ID3CHAP_MAX_CHAPTERS ? count : ID3CHAP_MAX_CHAPTERS;
		id3chap_t *found = calloc((size_t)capacity, sizeof(*found));
		if (!found) {
			return;
		}
		count = id3chap_read(filepath, found, capacity);
		if (count > capacity) {
			count = capacity;
		}
		if (count > 0) {
			chapters = calloc((size_t)count, sizeof(*chapters));
			if (chapters) {
				for (int i = 0; i < count; i++) {
					chapters[i].start = found[i].start;
					name_chapter(chapters[i].title, sizeof(chapters[i].title), found[i].title, i);
				}
				chapter_count = count;
			}
		}
		free(found);
		return;
	}

	int count = mp4_chapter_count(m);
	if (count > 0) {
		chapters = calloc((size_t)count, sizeof(*chapters));
		if (chapters) {
			for (int i = 0; i < count; i++) {
				const mp4_chapter_t *c = mp4_chapter(m, i);
				chapters[i].start = c->start;
				name_chapter(chapters[i].title, sizeof(chapters[i].title), c->title, i);
			}
			chapter_count = count;
		}
	}
	mp4_close(m);
}

// ---------------------------------------------------------------------------
// How far the two buttons jump
// ---------------------------------------------------------------------------
//
// Read from config at each press rather than cached: it is one lookup in a
// table already in memory, and a cache would be a second place that has to be
// told when the setting changes.

static int clamp_skip(long value) {
	if (value == AUDIOBOOK_SKIP_LONG) {
		return AUDIOBOOK_SKIP_LONG;
	}
	if (value == AUDIOBOOK_SKIP_HUGE) {
		return AUDIOBOOK_SKIP_HUGE;
	}
	return AUDIOBOOK_SKIP_SHORT;
}

int audiobook_skip_back(void) { return clamp_skip(config_get_int("audiobook", "skip_back", AUDIOBOOK_SKIP_SHORT)); }

int audiobook_skip_forward(void) {
	return clamp_skip(config_get_int("audiobook", "skip_forward", AUDIOBOOK_SKIP_SHORT));
}

void audiobook_set_skip_back(int seconds) { config_set_int("audiobook", "skip_back", clamp_skip(seconds)); }

void audiobook_set_skip_forward(int seconds) { config_set_int("audiobook", "skip_forward", clamp_skip(seconds)); }

// The rest of the settings, all the same shape: config is the single copy, and
// nothing here caches it. They are read when something is about to act on
// them, which on this device means a few times a second at worst.

// Except the speed, which a book can have of its own, read once when the book
// loads. Without one the setting applies. A pick is both: the book's from now
// on, and the setting for every book that has never been given one.
static bool speed_valid(long permille) { return permille >= 250 && permille <= 4000; }

int audiobook_speed_permille(void) {
	pthread_mutex_lock(&lock);
	int own = current_is_book ? book_speed : 0;
	pthread_mutex_unlock(&lock);
	if (own) {
		return own;
	}
	long value = config_get_int("audiobook", "speed", AUDIOBOOK_SPEED_NORMAL);
	if (!speed_valid(value)) {
		value = AUDIOBOOK_SPEED_NORMAL;
	}
	return (int)value;
}

double audiobook_speed(void) { return (double)audiobook_speed_permille() / 1000.0; }

void audiobook_set_speed_permille(int permille) {
	if (!speed_valid(permille)) {
		permille = AUDIOBOOK_SPEED_NORMAL;
	}
	config_set_int("audiobook", "speed", permille);

	char book[512] = "";
	pthread_mutex_lock(&lock);
	if (current_is_book) {
		book_speed = permille;
		snprintf(book, sizeof(book), "%s", current_book);
	}
	pthread_mutex_unlock(&lock);
	// Outside `lock`: a write to the card is no reason to hold up the player.
	if (book[0]) {
		audiobookdb_set_speed(book, permille);
	}
}

bool audiobook_stop_at_chapter_end(void) { return config_get_int("audiobook", "stop_chapter_end", 0) != 0; }

void audiobook_set_stop_at_chapter_end(bool on) { config_set_int("audiobook", "stop_chapter_end", on ? 1 : 0); }

bool audiobook_duration_per_chapter(void) { return config_get_int("audiobook", "duration_chapter", 0) != 0; }

void audiobook_set_duration_per_chapter(bool on) { config_set_int("audiobook", "duration_chapter", on ? 1 : 0); }

// The equalizer setting, as words in the file: someone reading config.ini sees
// "preset" and "eq_vocal", not 2. Anything it does not recognise is the music
// curve.
static const char *const EQ_MODE_NAMES[] = {"music", "off", "preset"};

audiobook_eq_mode_t audiobook_eq_mode(void) {
	const char *value = config_get("audiobook", "eq_mode", "music");
	for (int i = 0; i < (int)(sizeof(EQ_MODE_NAMES) / sizeof(EQ_MODE_NAMES[0])); i++) {
		if (value && strcmp(value, EQ_MODE_NAMES[i]) == 0) {
			return (audiobook_eq_mode_t)i;
		}
	}
	return AUDIOBOOK_EQ_MUSIC;
}

const char *audiobook_eq_preset(char *out, size_t size) {
	if (!out || size == 0) {
		return "";
	}
	const char *value = config_get("audiobook", "eq_preset", "");
	snprintf(out, size, "%s", value ? value : "");
	return out;
}

void audiobook_set_eq(audiobook_eq_mode_t mode, const char *preset) {
	if (mode < AUDIOBOOK_EQ_MUSIC || mode > AUDIOBOOK_EQ_PRESET) {
		mode = AUDIOBOOK_EQ_MUSIC;
	}
	config_set("audiobook", "eq_mode", EQ_MODE_NAMES[mode]);
	config_set("audiobook", "eq_preset", preset ? preset : "");
	config_save();
}

static bool eq_fell_back;

bool audiobook_take_eq_fallback(void) {
	bool value = eq_fell_back;
	eq_fell_back = false;
	return value;
}

// Only a book gets the audiobook curve; anything else plays through whatever
// the [eq] settings say. A preset that is gone -- a user preset deleted from
// the card since it was chosen, or a built-in tag this build does not have --
// puts the setting back to the music curve, so the listener is told once
// rather than on every track. One that merely cannot be read now (the card
// busy, or not mounted yet) gets the music curve for this track and keeps the
// setting: the next track asks again.
bool audiobook_apply_eq(bool book_playing) {
	audiobook_eq_mode_t mode = book_playing ? audiobook_eq_mode() : AUDIOBOOK_EQ_MUSIC;
	if (mode == AUDIOBOOK_EQ_MUSIC) {
		eq_override_clear();
		return true;
	}
	if (mode == AUDIOBOOK_EQ_OFF) {
		eq_override_bypass();
		return true;
	}

	char preset[128];
	audiobook_eq_preset(preset, sizeof(preset));
	static const char USER_PREFIX[] = "user:";
	int gain[EQ_BANDS];
	if (strncmp(preset, USER_PREFIX, sizeof(USER_PREFIX) - 1) == 0) {
		bool missing = false;
		if (eq_preset_read(preset + sizeof(USER_PREFIX) - 1, gain, &missing)) {
			eq_override_bands(gain);
			return true;
		}
		if (!missing) {
			eq_override_clear();
			return true;
		}
	} else {
		const eq_preset_t *builtin = eq_default_preset_by_name(preset);
		if (builtin) {
			eq_override_bands(builtin->gain_db);
			return true;
		}
	}

	eq_override_clear();
	audiobook_set_eq(AUDIOBOOK_EQ_MUSIC, "");
	eq_fell_back = true;
	return false;
}

static bool rewind_suppressed;

void audiobook_suppress_rewind_once(void) { rewind_suppressed = true; }

bool audiobook_take_rewind_suppression(void) {
	bool value = rewind_suppressed;
	rewind_suppressed = false;
	return value;
}

bool audiobook_rewind_enabled(void) { return config_get_int("audiobook", "rewind_on_resume", 0) != 0; }

void audiobook_set_rewind_enabled(bool on) { config_set_int("audiobook", "rewind_on_resume", on ? 1 : 0); }

int audiobook_rewind_seconds(void) {
	long value = config_get_int("audiobook", "rewind_seconds", 5);
	if (value != 3 && value != 5 && value != 7 && value != 10) {
		value = 5;
	}
	return (int)value;
}

void audiobook_set_rewind_seconds(int seconds) { config_set_int("audiobook", "rewind_seconds", seconds); }

void audiobook_track_changed(const char *filepath) {
	pthread_mutex_lock(&lock);

	if (filepath && filepath[0] && strcmp(filepath, current_path) == 0) {
		pthread_mutex_unlock(&lock); // same file, nothing to redo
		return;
	}

	char previous_book[512];
	snprintf(previous_book, sizeof(previous_book), "%s", current_book);
	clear_locked();
	serial++;

	if (!filepath || !filepath[0]) {
		pthread_mutex_unlock(&lock);
		return;
	}

	char book[512];
	if (!audiobookdb_book_for_file(filepath, book, sizeof(book))) {
		pthread_mutex_unlock(&lock);
		return;
	}

	snprintf(current_path, sizeof(current_path), "%s", filepath);
	snprintf(current_book, sizeof(current_book), "%s", book);
	current_is_book = true;
	last_save = 0;
	last_saved_position = -1;
	finished_marked = false;

	// Asked once per book, not per part: a folder book is the same book from
	// one file to the next, and a pick while it plays updates both copies.
	if (strcmp(book, previous_book) != 0) {
		int stored = audiobookdb_get_speed(book);
		book_speed = speed_valid(stored) ? stored : 0;
	}

	if (strcmp(book, filepath) != 0) {
		audiobookdb_parts_for_each(book, add_part, NULL);
		for (int i = 0; i < part_count; i++) {
			if (strcmp(parts[i].path, filepath) == 0) {
				part_index = i;
				break;
			}
		}
	}

	load_chapters_locked(filepath);
	printf("audiobook: %s (%d chapters, part %d of %d)\n", filepath, chapter_count, part_index + 1, part_count);

	pthread_mutex_unlock(&lock);
}

bool audiobook_is_playing(void) {
	pthread_mutex_lock(&lock);
	bool value = current_is_book;
	pthread_mutex_unlock(&lock);
	return value;
}

const char *audiobook_current_path(void) { return current_path; }

struct resume_find {
	const char *wanted;
	char first[512];
	bool found;
};

static bool resume_part_cb(const char *path, const char *title, void *user) {
	(void)title;
	struct resume_find *find = user;
	if (!find->first[0]) {
		snprintf(find->first, sizeof(find->first), "%s", path);
	}
	if (find->wanted[0] && strcmp(path, find->wanted) == 0) {
		find->found = true;
		return false;
	}
	return true;
}

int audiobook_part_count(void) {
	pthread_mutex_lock(&lock);
	int value = part_count;
	pthread_mutex_unlock(&lock);
	return value;
}

bool audiobook_part(int index, char *title_out, size_t title_size, char *path_out, size_t path_size) {
	pthread_mutex_lock(&lock);
	bool ok = index >= 0 && index < part_count;
	if (ok) {
		if (title_out && title_size) {
			snprintf(title_out, title_size, "%s", parts[index].title);
		}
		if (path_out && path_size) {
			snprintf(path_out, path_size, "%s", parts[index].path);
		}
	}
	pthread_mutex_unlock(&lock);
	return ok;
}

int audiobook_part_current(void) {
	pthread_mutex_lock(&lock);
	int value = part_count > 0 ? part_index : -1;
	pthread_mutex_unlock(&lock);
	return value;
}

bool audiobook_has_next_part(void) {
	pthread_mutex_lock(&lock);
	bool value = current_is_book && part_index >= 0 && part_index + 1 < part_count;
	pthread_mutex_unlock(&lock);
	return value;
}

bool audiobook_resume_point(const char *book, char *file_out, size_t file_size, double *seconds_out) {
	if (file_out && file_size) {
		file_out[0] = '\0';
	}
	if (seconds_out) {
		*seconds_out = 0;
	}
	if (!book || !book[0] || !file_out || !file_size) {
		return false;
	}

	char file[512];
	double seconds = 0;
	bool saved = audiobookdb_get_position(book, file, sizeof(file), &seconds);

	if (!audiobookdb_is_folder_book(book)) {
		snprintf(file_out, file_size, "%s", book);
		if (seconds_out && saved && (!file[0] || strcmp(file, book) == 0) && seconds > 0) {
			*seconds_out = seconds;
		}
		return true;
	}

	// A folder book comes back in the file it was left in, while that file is
	// still one of its parts; otherwise from its first.
	struct resume_find find = {saved ? file : "", "", false};
	audiobookdb_parts_for_each(book, resume_part_cb, &find);
	if (!find.first[0]) {
		return false;
	}
	if (find.found) {
		snprintf(file_out, file_size, "%s", file);
		if (seconds_out && seconds > 0) {
			*seconds_out = seconds;
		}
	} else {
		snprintf(file_out, file_size, "%s", find.first);
	}
	return true;
}

int audiobook_chapter_count(void) {
	pthread_mutex_lock(&lock);
	int value = chapter_count;
	pthread_mutex_unlock(&lock);
	return value;
}

bool audiobook_chapter(int index, char *title_out, size_t title_size, double *start_out) {
	pthread_mutex_lock(&lock);
	bool ok = index >= 0 && index < chapter_count;
	if (ok) {
		if (title_out && title_size) {
			snprintf(title_out, title_size, "%s", chapters[index].title);
		}
		if (start_out) {
			*start_out = chapters[index].start;
		}
	}
	pthread_mutex_unlock(&lock);
	return ok;
}

static int chapter_at_locked(double seconds) {
	int found = -1;
	for (int i = 0; i < chapter_count; i++) {
		if (chapters[i].start <= seconds + 0.001) {
			found = i;
		} else {
			break;
		}
	}
	return found;
}

int audiobook_chapter_at(double seconds) {
	pthread_mutex_lock(&lock);
	int found = chapter_at_locked(seconds);
	pthread_mutex_unlock(&lock);
	return found;
}

// Parts are the units of a folder book; the chapters inside one of its files
// are not offered as well. Called with `lock` held.
static bool units_are_parts_locked(void) { return part_count > 1; }

static int unit_count_locked(void) { return units_are_parts_locked() ? part_count : chapter_count; }

int audiobook_unit_count(void) {
	pthread_mutex_lock(&lock);
	int value = unit_count_locked();
	pthread_mutex_unlock(&lock);
	return value;
}

int audiobook_unit_current(double seconds) {
	pthread_mutex_lock(&lock);
	int value = units_are_parts_locked() ? part_index : chapter_at_locked(seconds);
	pthread_mutex_unlock(&lock);
	return value;
}

bool audiobook_unit_title(int index, char *out, size_t size) {
	pthread_mutex_lock(&lock);
	bool ok = index >= 0 && index < unit_count_locked();
	if (ok && out && size) {
		snprintf(out, size, "%s", units_are_parts_locked() ? parts[index].title : chapters[index].title);
	}
	pthread_mutex_unlock(&lock);
	return ok;
}

bool audiobook_step_target(double seconds, int direction, audiobook_step_t *out) {
	audiobook_step_t target = {AUDIOBOOK_STEP_NONE, 0, -1};
	pthread_mutex_lock(&lock);
	bool by_part = units_are_parts_locked();
	int count = unit_count_locked();
	int current = by_part ? part_index : chapter_at_locked(seconds);
	bool ok = count > 0 && (!by_part || current >= 0);
	if (ok && direction > 0) {
		if (current + 1 < count) {
			if (by_part) {
				target.kind = AUDIOBOOK_STEP_PART;
				target.part = current + 1;
			} else {
				target.kind = AUDIOBOOK_STEP_SEEK;
				target.seconds = chapters[current + 1].start;
			}
		}
	} else if (ok) {
		double into = by_part || current < 0 ? seconds : seconds - chapters[current].start;
		double start = by_part || current < 0 ? 0 : chapters[current].start;
		if (into > AUDIOBOOK_STEP_RESTART_SECONDS || current <= 0) {
			target.kind = AUDIOBOOK_STEP_SEEK;
			target.seconds = start;
		} else if (by_part) {
			target.kind = AUDIOBOOK_STEP_PART;
			target.part = current - 1;
		} else {
			target.kind = AUDIOBOOK_STEP_SEEK;
			target.seconds = chapters[current - 1].start;
		}
	}
	pthread_mutex_unlock(&lock);
	if (out) {
		*out = target;
	}
	return ok;
}

unsigned audiobook_serial(void) { return serial; }

double audiobook_saved_position(const char *filepath) {
	if (!filepath || !filepath[0]) {
		return 0;
	}

	char book[512];
	if (!audiobookdb_book_for_file(filepath, book, sizeof(book))) {
		return 0;
	}

	char file[512];
	double seconds = 0;
	if (!audiobookdb_get_position(book, file, sizeof(file), &seconds)) {
		return 0;
	}
	if (file[0] && strcmp(file, filepath) != 0) {
		return 0; // the book was left in a different file of the same set
	}
	return seconds > 0 ? seconds : 0;
}

void audiobook_note_position(double seconds, double total, bool force) {
	pthread_mutex_lock(&lock);
	if (!current_is_book || seconds < 0) {
		pthread_mutex_unlock(&lock);
		return;
	}

	// A book heard to the end goes onto the "Finished" shelf and its position
	// goes back to zero. Both halves matter: keeping the position would drop
	// anyone returning to it into its last few seconds, and zeroing it without
	// the shelf mark would make a finished book look like one never opened.
	//
	// In a folder book only the end of the last part is the end of the book,
	// and the book starts over from its first part.
	bool last_part = part_count == 0 || part_index == part_count - 1;
	char resume_file[512];
	snprintf(resume_file, sizeof(resume_file), "%s", current_path);
	if (last_part && total > FINISHED_MARGIN && seconds >= total - FINISHED_MARGIN) {
		seconds = 0;
		force = true;
		if (part_count > 0) {
			snprintf(resume_file, sizeof(resume_file), "%s", parts[0].path);
		}
		if (!finished_marked) {
			finished_marked = true;
			char done[512];
			snprintf(done, sizeof(done), "%s", current_book);
			pthread_mutex_unlock(&lock);
			audiobookdb_mark_finished(done);
			printf("audiobook: finished %s\n", done);
			pthread_mutex_lock(&lock);
		}
	}

	time_t now = time(NULL);
	bool due = force || last_save == 0 || (now - last_save) >= SAVE_EVERY_SECONDS;
	// Nothing moved: a paused book being polled must not keep rewriting the
	// same row onto the card.
	if (!due || (last_saved_position >= 0 && seconds == last_saved_position)) {
		pthread_mutex_unlock(&lock);
		return;
	}

	char book[512];
	snprintf(book, sizeof(book), "%s", current_book);
	last_save = now;
	last_saved_position = seconds;
	pthread_mutex_unlock(&lock);

	if (force) {
		audiobookdb_save_position(book, resume_file, seconds);
	} else {
		audiobookdb_queue_position(book, resume_file, seconds);
	}
}

bool audiobook_current_book(char *book_out, size_t book_size, char *file_out, size_t file_size) {
	pthread_mutex_lock(&lock);
	bool ok = current_is_book;
	if (ok) {
		if (book_out && book_size) {
			snprintf(book_out, book_size, "%s", current_book);
		}
		if (file_out && file_size) {
			snprintf(file_out, file_size, "%s", current_path);
		}
	}
	pthread_mutex_unlock(&lock);
	return ok;
}

void audiobook_place_label(double seconds, char *out, size_t size) {
	if (!out || !size) {
		return;
	}
	out[0] = '\0';
	int chapter = audiobook_chapter_at(seconds);
	if (chapter >= 0 && audiobook_chapter(chapter, out, size, NULL)) {
		return;
	}
	pthread_mutex_lock(&lock);
	if (part_index >= 0 && part_index < part_count) {
		snprintf(out, size, "%s", parts[part_index].title);
	}
	pthread_mutex_unlock(&lock);
}

#include "audiobook.h"

#include "src/system/library/audiobookdb.h"
#include "src/system/audio/eq.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/decode/decode.h"
#include "src/system/decode/mp4.h"
#include "src/system/library/id3chap.h"
#include "src/system/library/vorbischap.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// How often the position is written back while a book is playing. These
// checkpoints go through the background writer; the saves that are forced --
// pause, seek, stop -- are written before returning.
#define SAVE_EVERY_SECONDS 10

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
	double duration;
} part_t;

static part_t *parts;
static int part_count;
static int part_capacity;
static int part_index;
static bool length_noted;
static double file_length;
// Physical worker lifetime, also used by card close to wait for decoder_close.
// audiobook_working_out() reports only work belonging to the loaded book.
static bool lengths_working;
static bool lengths_done;
static char lengths_book[512];
static bool lengths_cancel;
static pthread_cond_t lengths_idle;
static pthread_once_t lengths_idle_once = PTHREAD_ONCE_INIT;

static void lengths_idle_init(void) {
	pthread_condattr_t attr;
	pthread_condattr_init(&attr);
	pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
	pthread_cond_init(&lengths_idle, &attr);
	pthread_condattr_destroy(&attr);
}

static time_t last_save;
static double last_saved_position;
static bool finished_marked; // this book has already been put on the shelf
static int book_speed;		 // the loaded book's own speed in permille, 0 for none

double audiobook_clamp_seek(double target, double length) {
	if (length > 0 && target > length - 1.0) {
		target = length - 1.0;
	}
	return target < 0 ? 0 : target;
}

static void clear_locked(void) {
	lengths_done = false;
	length_noted = false;
	file_length = 0;
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
static bool add_part(const char *path, const char *title, double duration, void *user) {
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
	parts[part_count].duration = duration;
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
// loads. First listen pins the default. A pick is both: the book's from now
// on, and the setting for every book that has never been heard.
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

int audiobook_rewind_for_break(double break_seconds, int short_s, int long_s) {
	if (!(break_seconds >= 300))
		return short_s;
	if (break_seconds >= 3600)
		return long_s;
	return (int)(short_s + (long_s - short_s) * ((break_seconds - 300) / 3300) + 0.5);
}

int audiobook_rewind_long_seconds(void) {
	long value = config_get_int("audiobook", "rewind_long_seconds", 30);
	if (value != 15 && value != 30 && value != 45 && value != 60)
		value = 30;
	return (int)value;
}

void audiobook_set_rewind_long_seconds(int seconds) { config_set_int("audiobook", "rewind_long_seconds", seconds); }

void audiobook_track_changed(const char *filepath) {
	pthread_mutex_lock(&lock);
	length_noted = false;

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
	int pin_speed = 0;
	if (strcmp(book, previous_book) != 0) {
		int stored = audiobookdb_get_speed(book);
		if (!speed_valid(stored)) {
			long value = config_get_int("audiobook", "speed", AUDIOBOOK_SPEED_NORMAL);
			stored = speed_valid(value) ? (int)value : AUDIOBOOK_SPEED_NORMAL;
			pin_speed = stored;
		}
		book_speed = stored;
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
	if (pin_speed) {
		audiobookdb_set_speed(book, pin_speed);
	}
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

static bool resume_part_cb(const char *path, const char *title, double duration, void *user) {
	(void)duration;
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

static bool lengths_missing_locked(void) {
	if (!current_is_book)
		return false;
	for (int i = 0; i < part_count; i++) {
		if (parts[i].duration <= 0)
			return true;
	}
	return false;
}

bool audiobook_lengths_missing(void) {
	pthread_mutex_lock(&lock);
	bool missing = lengths_missing_locked();
	pthread_mutex_unlock(&lock);
	return missing;
}

bool audiobook_working_out(void) {
	pthread_mutex_lock(&lock);
	bool working = lengths_working && !lengths_cancel && current_is_book && !strcmp(current_book, lengths_book);
	pthread_mutex_unlock(&lock);
	return working;
}

bool audiobook_take_lengths_done(void) {
	pthread_mutex_lock(&lock);
	bool done = lengths_done;
	lengths_done = false;
	pthread_mutex_unlock(&lock);
	return done;
}

typedef struct {
	char book[512];
	unsigned card;
	int count;
	part_t parts[];
} lengths_job_t;

static bool lengths_current_locked(const lengths_job_t *job) { return !lengths_cancel && current_is_book && !strcmp(current_book, job->book) && audiobookdb_card_epoch() == job->card; }

static void lengths_cancel_and_wait(unsigned ms) {
	pthread_once(&lengths_idle_once, lengths_idle_init);
	struct timespec until;
	clock_gettime(CLOCK_MONOTONIC, &until);
	until.tv_sec += ms / 1000;
	until.tv_nsec += (long)(ms % 1000) * 1000000;
	if (until.tv_nsec >= 1000000000) {
		until.tv_sec++;
		until.tv_nsec -= 1000000000;
	}
	pthread_mutex_lock(&lock);
	lengths_cancel = true;
	while (lengths_working) {
		if (pthread_cond_timedwait(&lengths_idle, &lock, &until) != 0)
			break;
	}
	pthread_mutex_unlock(&lock);
}

static void *lengths_thread_func(void *arg) {
	lengths_job_t *job = arg;
	thread_be_background("book lengths");
	bool changed = false;
	for (int i = 0; i < job->count; i++) {
		pthread_mutex_lock(&lock);
		bool valid = lengths_current_locked(job);
		pthread_mutex_unlock(&lock);
		if (!valid)
			break;
		if (job->parts[i].duration > 0)
			continue;

		const char *path = job->parts[i].path;
		decoder_t *dec = decoder_open_info(path, decode_detect_format(path));
		pthread_mutex_lock(&lock);
		valid = lengths_current_locked(job);
		pthread_mutex_unlock(&lock);
		double seconds = 0;
		if (dec) {
			if (valid) {
				int rate = decoder_sample_rate(dec);
				if (rate > 0)
					seconds = (double)decoder_total_pcm_frames(dec) / rate;
			}
			decoder_close(dec);
		}
		pthread_mutex_lock(&lock);
		valid = lengths_current_locked(job);
		pthread_mutex_unlock(&lock);
		if (!valid)
			break;
		if (!isfinite(seconds) || seconds <= 0)
			continue;
		// The card check also happens under db_lock, beside the write, so a
		// close/open between this check and the write cannot cross cards.
		if (!audiobookdb_set_part_duration_on_card(path, seconds, job->card))
			continue;
		pthread_mutex_lock(&lock);
		if (lengths_current_locked(job)) {
			for (int k = 0; k < part_count; k++) {
				if (!strcmp(parts[k].path, path) && parts[k].duration <= 0)
					parts[k].duration = seconds;
			}
			changed = true;
		}
		pthread_mutex_unlock(&lock);
	}
	pthread_mutex_lock(&lock);
	if (lengths_current_locked(job)) {
		lengths_done = !lengths_missing_locked();
		if (changed)
			serial++;
	}
	lengths_working = false;
	pthread_cond_broadcast(&lengths_idle);
	pthread_mutex_unlock(&lock);
	free(job);
	return NULL;
}

void audiobook_work_out_lengths(void) {
	pthread_once(&lengths_idle_once, lengths_idle_init);
	pthread_mutex_lock(&lock);
	// A tap during an old book's decoder open is dropped; the entry stays for the next tap.
	if (lengths_working || !lengths_missing_locked()) {
		pthread_mutex_unlock(&lock);
		return;
	}
	int count = part_count;
	lengths_job_t *job = calloc(1, sizeof(*job) + (size_t)count * sizeof(*parts));
	if (!job) {
		pthread_mutex_unlock(&lock);
		return;
	}
	snprintf(job->book, sizeof(job->book), "%s", current_book);
	job->card = audiobookdb_card_epoch();
	job->count = count;
	memcpy(job->parts, parts, (size_t)count * sizeof(*parts));
	// Register while holding our lock: close's callback cannot miss a worker
	// between admission and pthread_create. The callback runs without db_lock.
	if (!audiobookdb_register_card_user(lengths_cancel_and_wait, job->card)) {
		free(job);
		pthread_mutex_unlock(&lock);
		return;
	}
	snprintf(lengths_book, sizeof(lengths_book), "%s", job->book);
	lengths_cancel = false;
	lengths_done = false;
	lengths_working = true;
	pthread_t thread;
	int rc = pthread_create(&thread, NULL, lengths_thread_func, job);
	if (rc != 0) {
		lengths_working = false;
		free(job);
	} else {
		pthread_detach(thread);
	}
	pthread_mutex_unlock(&lock);
}

double audiobook_part_duration(int index) {
	pthread_mutex_lock(&lock);
	double duration = index >= 0 && index < part_count ? parts[index].duration : 0;
	pthread_mutex_unlock(&lock);
	return duration;
}

void audiobook_note_length(double total) {
	// Loads happen on the GUI thread, so the first valid total after a load
	// belongs to the loaded part.
	if (!isfinite(total) || total <= 0)
		return;
	char path[sizeof(current_path)] = "";
	double seconds = 0;
	pthread_mutex_lock(&lock);
	if (current_is_book && !length_noted) {
		length_noted = true;
		if (part_index >= 0 && part_index < part_count) {
			double known = parts[part_index].duration;
			if (known <= 0 || fabs(known - total) > 1) {
				parts[part_index].duration = total;
				snprintf(path, sizeof(path), "%s", current_path);
				seconds = total;
			}
		} else if (part_count == 0) {
			file_length = total;
		}
	}
	pthread_mutex_unlock(&lock);
	if (path[0])
		audiobookdb_set_part_duration(path, seconds);
}

bool audiobook_book_progress(double seconds, double speed, audiobook_progress_t *out) {
	if (!out)
		return false;
	*out = (audiobook_progress_t){0};
	pthread_mutex_lock(&lock);
	if (!current_is_book) {
		pthread_mutex_unlock(&lock);
		return false;
	}
	int index = part_index;
	if (part_count > 0 && (index < 0 || index >= part_count)) {
		pthread_mutex_unlock(&lock);
		return true;
	}
	int count = index >= 0 && index < part_count ? part_count : 1;
	double *durations = malloc((size_t)count * sizeof(*durations));
	if (!durations) {
		pthread_mutex_unlock(&lock);
		return true;
	}
	for (int i = 0; i < count; i++)
		durations[i] = index >= 0 ? parts[i].duration : file_length;
	if (index < 0)
		index = 0;
	pthread_mutex_unlock(&lock);

	out->known_total = out->known_left = true;
	double total = 0, before = 0, left = 0;
	if (!isfinite(seconds) || seconds < 0)
		seconds = 0;
	if (!isfinite(speed) || speed <= 0)
		speed = 1;
	for (int i = 0; i < count; i++) {
		double duration = durations[i];
		if (!isfinite(duration) || duration <= 0) {
			out->known_total = false;
			if (i >= index)
				out->known_left = false;
			continue;
		}
		total += duration;
		if (i < index)
			before += duration;
		else if (i == index)
			left += duration > seconds ? duration - seconds : 0;
		else
			left += duration;
	}
	if (out->known_total) {
		double current = seconds < durations[index] ? seconds : durations[index];
		out->fraction = (before + current) / total;
	}
	if (out->known_left)
		out->left_seconds = left / speed;
	free(durations);
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

unsigned audiobook_serial(void) {
	pthread_mutex_lock(&lock);
	unsigned value = serial;
	pthread_mutex_unlock(&lock);
	return value;
}

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

// Only the natural end of the last part puts a book on the "Finished" shelf.
// Its next listen starts at zero, in the first part of a folder book. Position
// checkpoints, pauses and deliberate moves keep their place even near the end.
void audiobook_note_finished(void) {
	pthread_mutex_lock(&lock);
	if (!current_is_book || finished_marked) {
		pthread_mutex_unlock(&lock);
		return;
	}

	char book[512];
	char resume_file[512];
	snprintf(book, sizeof(book), "%s", current_book);
	snprintf(resume_file, sizeof(resume_file), "%s", part_count > 0 ? parts[0].path : current_path);
	finished_marked = true;
	last_save = time(NULL);
	last_saved_position = 0;
	pthread_mutex_unlock(&lock);

	audiobookdb_save_position(book, resume_file, 0);
	audiobookdb_mark_finished(book);
	printf("audiobook: finished %s\n", book);
}

void audiobook_note_position(double seconds, double total, bool force) {
	(void)total;
	pthread_mutex_lock(&lock);
	if (!current_is_book || seconds < 0) {
		pthread_mutex_unlock(&lock);
		return;
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
	char resume_file[512];
	snprintf(book, sizeof(book), "%s", current_book);
	snprintf(resume_file, sizeof(resume_file), "%s", current_path);
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

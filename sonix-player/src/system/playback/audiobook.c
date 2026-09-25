#include "audiobook.h"

#include "src/system/library/audiobookdb.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/decode/mp4.h"
#include "src/system/library/id3chap.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// How often the position is written back while a book is playing.
#define SAVE_EVERY_SECONDS 10

// Within this much of the end counts as finished: the last words of a book are
// followed by credits nobody sits through, and a listener who stops there has
// finished it.
#define FINISHED_MARGIN 15.0

typedef struct {
	double start;
	char title[192];
} chapter_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static char current_path[512];
static bool current_is_book;
static chapter_t *chapters;
static int chapter_count;
static unsigned serial;

static time_t last_save;
static double last_saved_position;
static bool finished_marked; // this book has already been put on the shelf

static void clear_locked(void) {
	free(chapters);
	chapters = NULL;
	chapter_count = 0;
	current_is_book = false;
	current_path[0] = '\0';
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
			return;
		}
		id3chap_t *found = calloc((size_t)count, sizeof(*found));
		if (!found) {
			return;
		}
		count = id3chap_read(filepath, found, count);
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

int audiobook_speed_permille(void) {
	long value = config_get_int("audiobook", "speed", AUDIOBOOK_SPEED_NORMAL);
	if (value < 250 || value > 4000) {
		value = AUDIOBOOK_SPEED_NORMAL;
	}
	return (int)value;
}

double audiobook_speed(void) { return (double)audiobook_speed_permille() / 1000.0; }

void audiobook_set_speed_permille(int permille) {
	if (permille < 250 || permille > 4000) {
		permille = AUDIOBOOK_SPEED_NORMAL;
	}
	config_set_int("audiobook", "speed", permille);
}

bool audiobook_stop_at_chapter_end(void) { return config_get_int("audiobook", "stop_chapter_end", 0) != 0; }

void audiobook_set_stop_at_chapter_end(bool on) { config_set_int("audiobook", "stop_chapter_end", on ? 1 : 0); }

bool audiobook_duration_per_chapter(void) { return config_get_int("audiobook", "duration_chapter", 0) != 0; }

void audiobook_set_duration_per_chapter(bool on) { config_set_int("audiobook", "duration_chapter", on ? 1 : 0); }

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
	current_is_book = true;
	last_save = 0;
	last_saved_position = -1;
	finished_marked = false;

	load_chapters_locked(filepath);
	printf("audiobook: %s (%d chapters)\n", filepath, chapter_count);

	pthread_mutex_unlock(&lock);
}

bool audiobook_is_playing(void) {
	pthread_mutex_lock(&lock);
	bool value = current_is_book;
	pthread_mutex_unlock(&lock);
	return value;
}

const char *audiobook_current_path(void) { return current_path; }

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

int audiobook_chapter_at(double seconds) {
	pthread_mutex_lock(&lock);
	int found = -1;
	for (int i = 0; i < chapter_count; i++) {
		if (chapters[i].start <= seconds + 0.001) {
			found = i;
		} else {
			break;
		}
	}
	pthread_mutex_unlock(&lock);
	return found;
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
	if (total > FINISHED_MARGIN && seconds >= total - FINISHED_MARGIN) {
		seconds = 0;
		force = true;
		if (!finished_marked) {
			finished_marked = true;
			char done[512];
			snprintf(done, sizeof(done), "%s", current_path);
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

	char path[512];
	snprintf(path, sizeof(path), "%s", current_path);
	last_save = now;
	last_saved_position = seconds;
	pthread_mutex_unlock(&lock);

	char book[512];
	if (audiobookdb_book_for_file(path, book, sizeof(book))) {
		audiobookdb_save_position(book, path, seconds);
	}
}

#ifndef AUDIOBOOK_H
#define AUDIOBOOK_H

#include <stdbool.h>
#include <stddef.h>

// What the player needs to know about a book while it is playing.
//
// The audiobook *index* is audiobookdb.h -- what is on the card and where the
// listener got to in each one. This is the other half: whether the thing
// loaded right now is one of them, what its chapters are, and keeping the
// remembered position up to date.
//
// The question "is this an audiobook" is answered from the path, by asking the
// index, and not from a flag set when playback started. A flag has to be
// maintained through every route a track can change by -- the queue, the next
// button, a restore at boot, a file tapped in the browser -- and the first one
// missed leaves the player wearing audiobook controls over a song.

// How far the two transport buttons jump, in seconds. Each is set on its own
// (Settings -> Audiobooks -> Change controls) because going back thirty
// seconds to catch a sentence again and going forward ten to step over a pause
// are not the same size of move.
//
// Ten, thirty or sixty seconds -- the three the icons draw.
#define AUDIOBOOK_SKIP_SHORT 10
#define AUDIOBOOK_SKIP_LONG 30
#define AUDIOBOOK_SKIP_HUGE 60

int audiobook_skip_back(void);
int audiobook_skip_forward(void);
void audiobook_set_skip_back(int seconds);
void audiobook_set_skip_forward(int seconds);

// ---------------------------------------------------------------------------
// The rest of the audiobook settings
// ---------------------------------------------------------------------------

// Playback speed. Stored in thousandths so it survives a config file that only
// carries integers; 1000 is normal. The pitch does not move -- see speed.h.
#define AUDIOBOOK_SPEED_NORMAL 1000
double audiobook_speed(void);
void audiobook_set_speed_permille(int permille);
int audiobook_speed_permille(void);

// Stop at chapter end: playback pauses where the current chapter ends instead
// of running into the next one. Does nothing on a book with no chapter marks
// -- there is no end to stop at.
bool audiobook_stop_at_chapter_end(void);
void audiobook_set_stop_at_chapter_end(bool on);

// Show duration: what the progress bar and its two clocks stand for on a book
// with chapter marks. False (the default) is the whole file; true is the
// chapter being listened to, from 0:00 to the chapter's own length. A book
// with no marks shows the whole file either way.
bool audiobook_duration_per_chapter(void);
void audiobook_set_duration_per_chapter(bool on);

// Equalizer: the graphic EQ curve a book plays through. "Same as music" (the
// default) leaves the [eq] settings in charge; "off" bypasses the graphic EQ;
// a preset is one of the built-ins by its tr tag or one of the user's own as
// "user:<name>". Only the graphic EQ: the parametric, MSEB and the rest stay
// as the listener set them.
typedef enum { AUDIOBOOK_EQ_MUSIC = 0, AUDIOBOOK_EQ_OFF, AUDIOBOOK_EQ_PRESET } audiobook_eq_mode_t;
audiobook_eq_mode_t audiobook_eq_mode(void);						 // [audiobook] eq_mode = music|off|preset
const char *audiobook_eq_preset(char *out, size_t size);			 // [audiobook] eq_preset: "eq_vocal" or "user:<name>"
void audiobook_set_eq(audiobook_eq_mode_t mode, const char *preset); // saves both, config_save()
// Puts the chosen curve on the engine for a playing book, or clears it.
// Called on every track change and when the setting changes. Returns false
// when a user preset named by the setting is missing (the caller toasts).
// A preset that is only unreadable for now (the card busy or not mounted) is
// not missing: that track plays the music curve and the setting stays.
bool audiobook_apply_eq(bool book_playing);
// True once after audiobook_apply_eq() put a missing preset's setting back to
// "Same as music", then false until it happens again. The track change that
// does it has no toasts of its own; the GUI asks this and tells the listener.
bool audiobook_take_eq_fallback(void);

// The sleep timer is one of the three in sleeptimer.h, which keeps the
// audiobook one under the same [audiobook] keys. "Stop at the end of the
// chapter" is the switch above, not a timer length.

// Rewind after pause: pressing play after a pause winds back a few seconds, so
// the sentence that was interrupted is heard whole.
//
// Suppression is set when playback was stopped by the player itself -- at the
// end of a chapter, or by the sleep timer -- and consumed by the next resume.
// Without it, stop-at-chapter-end and rewind-after-pause together make a trap:
// the book stops on the boundary, play winds back five seconds into the
// chapter just finished, and five seconds later it stops on the same boundary
// again, forever.
void audiobook_suppress_rewind_once(void);
bool audiobook_take_rewind_suppression(void);

bool audiobook_rewind_enabled(void);
void audiobook_set_rewind_enabled(bool on);
int audiobook_rewind_seconds(void);
void audiobook_set_rewind_seconds(int seconds);

// Called whenever the loaded track changes: works out whether it is a book and
// reads its chapter list if it is. Cheap for anything that is not one (a single
// database lookup) and safe to call with the same path twice.
void audiobook_track_changed(const char *filepath);

bool audiobook_is_playing(void);
const char *audiobook_current_path(void);

// The parts of the folder book playing now: its files, one per chapter, in
// order. Zero parts for a book in one file. `path_out` is the file.
int audiobook_part_count(void);
bool audiobook_part(int index, char *title_out, size_t title_size, char *path_out, size_t path_size);
// Which part is loaded, or -1 for a book in one file.
int audiobook_part_current(void);
// Whether the book goes on into another file when this one ends.
bool audiobook_has_next_part(void);

// Where to start `book` (a file, or a folder book's folder): the file and the
// second it was left at. The book's first file from its beginning when it was
// never opened, was finished, or was left in a file that has gone. False when
// the book has no file to play.
bool audiobook_resume_point(const char *book, char *file_out, size_t file_size, double *seconds_out);

// The chapters of the book playing now. Zero when it has none -- plenty of
// books are a single unmarked file, and the interface has to cope with that
// rather than inventing chapters.
int audiobook_chapter_count(void);
// Copies chapter `index` out. Borrowed pointers would race with a track
// change, so the title is copied into the caller's buffer.
bool audiobook_chapter(int index, char *title_out, size_t title_size, double *start_out);
// Which chapter contains `seconds`, or -1 when the book has no chapters.
int audiobook_chapter_at(double seconds);

// The units the chapter row and the chapter list count: a folder book's
// parts, else the file's embedded chapters. Zero when the book has neither.
int audiobook_unit_count(void);
// Which unit holds `seconds` of the loaded file: the current part for a
// folder book, the chapter at `seconds` otherwise. -1 with no units.
int audiobook_unit_current(double seconds);
// Title of unit `index` (chapter title or part title). False out of range.
bool audiobook_unit_title(int index, char *out, size_t size);

// Where a step between units lands. SEEK moves within the loaded file; PART
// loads another file of a folder book from its start; NONE stays put.
typedef enum { AUDIOBOOK_STEP_NONE, AUDIOBOOK_STEP_SEEK, AUDIOBOOK_STEP_PART } audiobook_step_kind_t;
typedef struct {
	audiobook_step_kind_t kind;
	double seconds; // SEEK: where to seek in the loaded file
	int part;		// PART: which part to load, from its start
} audiobook_step_t;
// Going back further into a unit than this returns to its start; within it,
// to the unit before.
#define AUDIOBOOK_STEP_RESTART_SECONDS 3.0
// The target of a step by `direction` (+1 next, -1 previous) from `seconds`.
// False when the book has no units. Next from the last unit is NONE; previous
// from the first unit restarts it.
bool audiobook_step_target(double seconds, int direction, audiobook_step_t *out);

// Bumped whenever the loaded book (or its chapter list) changes, so a page can
// redraw only when there is something new.
unsigned audiobook_serial(void);

// Where this book was left last time, in seconds. 0 when it is new, when it
// is not a book, when it was heard to the end -- a finished book starts over,
// and is listed as finished so that fact is not simply lost -- or when `filepath`
// is a part of a folder book other than the one it was left in.
double audiobook_saved_position(const char *filepath);

// Notes how far into the book playback has got. Throttled: a write every ten
// seconds while playing is enough to lose no more than that, and an SD card
// does not want a database write every half second. Those checkpoints go to a
// background writer; `force` writes now, before returning -- used when
// playback pauses or seeks, when the track changes, and on the way out.
//
// `total` is the book's length, and it is what makes a finished book start
// over instead of resuming into its own last seconds; pass 0 when it is not
// known yet.
void audiobook_note_position(double seconds, double total, bool force);

// The book loaded now and the file of it playing. False when no book is.
bool audiobook_current_book(char *book_out, size_t book_size, char *file_out, size_t file_size);

// Where `seconds` falls in the loaded book, as a bookmark's label: the
// chapter's title, else the part's, else "".
void audiobook_place_label(double seconds, char *out, size_t size);

#endif /* AUDIOBOOK_H */

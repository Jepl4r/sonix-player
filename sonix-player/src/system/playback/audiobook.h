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

// The chapters of the book playing now. Zero when it has none -- plenty of
// books are a single unmarked file, and the interface has to cope with that
// rather than inventing chapters.
int audiobook_chapter_count(void);
// Copies chapter `index` out. Borrowed pointers would race with a track
// change, so the title is copied into the caller's buffer.
bool audiobook_chapter(int index, char *title_out, size_t title_size, double *start_out);
// Which chapter contains `seconds`, or -1 when the book has no chapters.
int audiobook_chapter_at(double seconds);

// Bumped whenever the loaded book (or its chapter list) changes, so a page can
// redraw only when there is something new.
unsigned audiobook_serial(void);

// Where this book was left last time, in seconds. 0 when it is new, when it
// is not a book, or when it was heard to the end -- a finished book starts
// over, and is listed as finished so that fact is not simply lost.
double audiobook_saved_position(const char *filepath);

// Notes how far into the book playback has got. Throttled: a write every ten
// seconds while playing is enough to lose no more than that, and an SD card
// does not want a database write every half second. `force` writes now --
// used when playback pauses, when the track changes, and on the way out.
//
// `total` is the book's length, and it is what makes a finished book start
// over instead of resuming into its own last seconds; pass 0 when it is not
// known yet.
void audiobook_note_position(double seconds, double total, bool force);

#endif /* AUDIOBOOK_H */

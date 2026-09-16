#ifndef SLEEPTIMER_H
#define SLEEPTIMER_H

#include <stdbool.h>

// The sleep timers: the player stops by itself after a while.
//
// Three of them, one per kind of listening, because the answer is not the same
// for all three. Half an hour is a reasonable way to fall asleep to music and a
// poor way to fall asleep to a book, and somebody who sets one for podcasts at
// bedtime does not want it counting down over an album the next morning. Each
// keeps its own switch and its own length, in its own corner of the config.
//
// One rule decides when any of them counts. The countdown starts fresh at every
// transition into playing and runs only while something of that kind is
// playing -- so pausing and starting again gives a full stretch back, and so
// does pressing play after the timer has stopped the music. There is no
// "resume where the countdown was": somebody who presses play is awake, and a
// timer that gave them the four minutes left over from last time would be a
// timer that had stopped doing its job.
//
// Which kind is playing is not this file's business to work out: the player
// knows, and tells it. Radio, the Game Boy and the Bluetooth receiver are none
// of the three -- there is no pause to give them -- so the player simply never
// names them.
//
// Each counts down against the monotonic clock rather than by adding up ticks,
// so a slow tick with the screen dark loses nothing: the tick only asks what
// time it is now.
typedef enum {
	SLEEPTIMER_MUSIC = 0, // the card, Tidal and Qobuz: one transport, one timer
	SLEEPTIMER_AUDIOBOOK,
	SLEEPTIMER_PODCAST,
	SLEEPTIMER_COUNT,
} sleeptimer_kind_t;

// Reads all three out of the config.
void sleeptimer_init(void);

bool sleeptimer_enabled(sleeptimer_kind_t kind);
void sleeptimer_set_enabled(sleeptimer_kind_t kind, bool on);

// How long a stretch is, in minutes. Zero means the switch is on but no time
// has been chosen, which arms nothing -- the wheels sitting at 00:00 are not a
// request to stop immediately.
int sleeptimer_minutes(sleeptimer_kind_t kind);
void sleeptimer_set_minutes(sleeptimer_kind_t kind, int minutes);

// Which kind is playing, or SLEEPTIMER_COUNT for none of them. Told from the
// one place in the player that sees every change of playback state; naming a
// kind stops whichever other one was counting.
void sleeptimer_note_playing(sleeptimer_kind_t kind);

// True while that stretch is actually counting down: the switch is on, a time
// is set, and that kind is playing.
bool sleeptimer_running(sleeptimer_kind_t kind);

// True for any of the three, for a caller that only wants to know whether its
// tick is worth running.
bool sleeptimer_any_running(void);

// True exactly once, on the tick that kind's time runs out. The caller stops
// the music: this file has no business knowing how.
bool sleeptimer_expired(sleeptimer_kind_t kind);

// What is left of that stretch, in seconds, or 0 when it is not counting.
int sleeptimer_remaining(sleeptimer_kind_t kind);

#endif /* SLEEPTIMER_H */

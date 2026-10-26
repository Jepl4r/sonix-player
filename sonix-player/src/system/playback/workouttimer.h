#ifndef WORKOUTTIMER_H
#define WORKOUTTIMER_H

#include <stdbool.h>

#include "src/system/audio/cue.h"

// The workout timer: a countdown that sounds a cue when it runs out, laid over
// whatever is playing (cue.h), for the rest between two sets.
//
// It is not a sleep timer and shares none of its rules. Nothing stops: the
// music carries on under the beep. It counts from the moment it is started,
// playing or not, because the rest it measures does not care whether the
// music is paused. And it runs once: having sounded, it is off, and the next
// set starts it again -- from the control centre, or from a side button mapped
// to it (keymap.h), which is how it is meant to be used with the player in a
// pocket. Starting it while it counts starts it over from the full length.
//
// The countdown runs on a thread of its own against the monotonic clock, so a
// dark screen, which slows the interface's timers to a crawl, does not make the
// beep late; and power.c will not suspend the player under a running one.
//
// The length and the sound are kept in [workout]; whether it is counting is
// not kept anywhere, since a timer that survived a restart would sound at a
// moment that no longer means anything.

#define WORKOUTTIMER_MIN_SECONDS 1
#define WORKOUTTIMER_MAX_SECONDS (59 * 60 + 59)

// Reads the config and starts the thread. Once, at startup.
void workouttimer_init(void);

// The length, in seconds, never under WORKOUTTIMER_MIN_SECONDS: a rest of no
// length is no rest, so nothing can set one, a hand-edited config included.
int workouttimer_seconds(void);
void workouttimer_set_seconds(int seconds);

cue_sound_t workouttimer_sound(void);
void workouttimer_set_sound(cue_sound_t sound);

// Any thread. Starts it, or starts it over, at the chosen length, with a short
// blip to say so. False only when its thread could not be made.
bool workouttimer_start(void);

// Any thread. Stops it without a sound.
void workouttimer_stop(void);

bool workouttimer_running(void);

// What is left, in whole seconds rounded up; 0 when it is not counting.
int workouttimer_remaining(void);

// Told of every change: started, stopped, and run out (`fired`). Called on
// whichever thread made the change -- the button thread, the timer's own --
// so the listener must not touch the interface directly.
void workouttimer_set_listener(void (*cb)(bool fired));

#endif /* WORKOUTTIMER_H */

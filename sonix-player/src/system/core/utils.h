#ifndef UTILS_H
#define UTILS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

// Square root, in integers, digit by digit. No libm and no FPU: these are
// called per pixel while a finger is on the screen (the halo under the record
// in Cover Flow, the rounded ends of the waveform's bars) and per column while
// a track is being measured, all three on threads that must not stall the
// interface.
//
// Two widths because the difference is real on a 32-bit core: the 64-bit one
// takes twice the rounds and 64-bit arithmetic for each, so the narrow one is
// what the drawing loops call and the wide one is for sums of squares over a
// whole track.
//
// The prefix is not decoration: libopus ships its own global `isqrt32`
// (mathops.o), and the target build links libopus.a, so the plain name is a
// duplicate definition at link time. Anything added here that sounds like
// arithmetic wants a name of its own.
uint32_t utils_isqrt32(uint32_t v);
uint64_t utils_isqrt64(uint64_t v);

char *read_file_content(const char *filename);
bool file_matches(const char *filename, const char *expected);

// Case-insensitive check of whether `name` ends with `ext` (e.g. ".wav").
bool has_extension(const char *name, const char *ext);

// Returns the size in bytes of the given file, or -1 on failure.
long get_file_size(const char *filepath);

int formatDoubleSeconds(double total_seconds, char *buffer, size_t max_len);
void formatDoubleProgress(double current_secs, double total_secs, char *buffer, size_t max_len);

// Marks the calling thread as background work. The X1600E has one core, so any
// worker at the default priority competes with the interface thread and a JPEG
// decode or a directory scan reads as the browser freezing. SCHED_IDLE lets
// these threads run only when the interface has nothing to do: the work takes
// longer, the screen never stalls. Call it first thing in the thread function.
void thread_be_background(const char *name);

// The two levels above, for a thread that has to move between them while it
// runs. Silent, because it is called per job and the level is a consequence of
// something else rather than an event of its own.
//
// `above_idle` true is nice 10, false is SCHED_IDLE: roughly a tenth of a busy
// core against almost none of it.
void thread_set_background_level(bool above_idle);

// The same idea for a thread the interface is actually waiting on: below the
// interface, but not starved by it. An idle-class thread gets under half a
// percent of a core somebody else is keeping busy, which is too little for work
// something is waiting on.
void thread_be_low_priority(const char *name);

// And the other end of the scale, for a thread that carries audio: SCHED_RR at
// `priority`, the same policy and the same 10 the playback thread uses. Only for
// work with a hard deadline -- a PCM that has to be drained before the buffer
// behind it fills -- because on one core everything else waits behind it.
// Failing is not fatal; the host build has no privilege for it.
void thread_be_realtime(const char *name, int priority);

// Gives the calling thread its own stack for signal handlers to run on, so a
// stack overflow reaches the crash handler instead of killing the process
// silently. thread_be_background() already does this; call it directly from a
// thread that stays at normal priority but recurses.
//
// It also notes where this thread's stack begins and ends, for the report below.
void thread_signal_stack(void);

// The calling thread's stack, as noted by thread_signal_stack(). False on a
// thread that never called it, or where the system would not say.
//
// For the crash handler: asking the system at the moment of the fault means
// reading /proc and allocating, which a signal handler must not do and a thread
// whose stack has just run out cannot do. `low` is the furthest the stack can
// grow, `high` is where it started.
bool thread_stack_bounds(uintptr_t *low, uintptr_t *high);

// A pthread_cond_timedwait deadline `ms` from now, on CLOCK_REALTIME.
//
// Worth a helper because getting it wrong fails silently. This is a 32-bit
// platform, so `long` and therefore `timespec.tv_nsec` holds at most
// 2,147,483,647. The obvious
//
//     deadline.tv_nsec += (long)POLL_MS * 1000000L;
//
// overflows for any poll of two seconds or more, giving a negative tv_nsec that
// pthread_cond_timedwait rejects with EINVAL immediately. The wait then does not
// wait and the worker spins on a full core, with no symptom but the battery.
//
// The arithmetic here is done in 64 bits and normalised afterwards.
void deadline_in_ms(struct timespec *out, unsigned int ms);

// How many bytes the C heap has handed out and not had back, and how many it
// holds from the system in total.
//
// The half of the player's memory that /proc cannot break down: the resident
// set says how much is anonymous, but not how much of that is the heap.
// Everything LVGL allocates -- every object, style, label and decoded image on
// every page -- goes through malloc, so watching this while walking the
// interface is what tells a level from a growth. The Processes page shows it.
//
// Zero when the C library does not offer the figures.
void heap_usage(size_t *in_use, size_t *from_system);

// ---------------------------------------------------------------------------
// Buffers that live as long as a page and then have to go away
//
// malloc() is the wrong tool for a megabyte that one screen needs and nobody
// else ever wants. free() gives the bytes back to the C library, not to the
// kernel: the heap only ever shrinks from its top, and a chunk under the mmap
// threshold never leaves it at all. Cover Flow's six faces are 88 KB each,
// which is under that threshold, so on a 56 MB device closing the page gives
// back nothing: the resident set keeps the buffers after they are freed.
//
// The threshold does not settle it either. glibc raises it on its own: the
// first large block that comes back tells it to keep blocks that size on the
// heap from then on, so a buffer that was mapped the first time is part of the
// heap the second. A page-sized, page-lifetime buffer therefore asks the
// kernel directly.
//
// `size` at big_free() is the one passed to big_alloc(); the rounding to whole
// pages happens in both places and agrees.
void *big_alloc(size_t size);
void big_free(void *ptr, size_t size);

// ---------------------------------------------------------------------------
// The breadcrumb
//
// The last thing the player was doing, kept in a fixed buffer so a signal
// handler can print it. The crash handler covers SIGSEGV and its relatives, but
// a process killed from outside never reaches it, and then the only thing
// anyone can say afterwards is which file was under the needle -- so it is
// written down as the scan goes.
//
// Cheap on purpose: one bounded copy, no lock, no allocation. A reader in a
// signal handler can catch a half-written path, which is still worth more than
// nothing.
// ---------------------------------------------------------------------------

void crumb_set(const char *what);

// The current breadcrumb, or "" if nothing has been noted. The pointer is to
// the fixed buffer and stays valid; safe to call from a signal handler.
const char *crumb_get(void);

// The track being loaded, and the file whose artwork is being decoded right
// now. Kept apart from the scan's breadcrumb above and from each other, because
// three threads write them and a single buffer would leave only the last one.
//
// With no scan running the scan's breadcrumb is empty, and the log would
// otherwise say nothing about what the player was doing. The artwork one is set
// when a decode starts and cleared when it ends, so finding it filled means the
// crash landed inside a decode rather than merely near one.
void crumb_set_track(const char *path);
const char *crumb_track(void);

void crumb_set_artwork(const char *path);
const char *crumb_artwork(void);

#endif

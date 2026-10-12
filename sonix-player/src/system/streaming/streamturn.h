#ifndef STREAMTURN_H
#define STREAMTURN_H

#include "src/system/net/http.h"

#include <pthread.h>
#include <stdbool.h>

// The download turn: one download at a time, across all services.
//
// Prefetch calls the start path for upcoming tracks, and that returns as soon
// as there is enough material, leaving a thread to fetch the rest, so four
// calls in a row would mean five concurrent downloads. On a 24-bit/176.4 kHz
// track -- around 190 MB, needing 5 Mbit/s just to keep up -- that splits the
// bandwidth five ways and gives the card five write streams to serve while
// playback reads from it.
//
// The turn belongs to the device and not to a service: one network, one card,
// one core. Separate counters in qobuzcache and tidalcache would allow exactly
// what the rule prevents -- a Qobuz track and a Tidal track downloading at once
// because neither knows about the other, which happens with a mixed queue or
// when one service's prefetch starts while the other is still finishing.

// Takes the turn, waiting until it is free. Call from a worker thread.
void streamturn_acquire(void);

// Releases it and wakes a waiter.
void streamturn_release(void);

// Waits until nothing is downloading, without taking the turn. For work that
// should only run ahead when it disturbs nobody.
void streamturn_wait_idle(void);

// ---------------------------------------------------------------------------
// Abandoning every download, whoever owns it
//
// The turn is shared, so abandoning has to be shared too. A service that could
// only cancel its own downloads would queue behind another service's hi-res
// track -- minutes of it -- and a tap would do nothing at all, with no message,
// for the length of another track.
//
// So each cache registers its own abandon function here, and whoever is about
// to ask for the turn calls them all: whatever is downloading gives way to what
// the user just asked for, whichever service it belongs to.
typedef void (*streamturn_abandon_fn)(void);
void streamturn_register_abandon(streamturn_abandon_fn fn);

// Abandons everything in flight, for every service.
void streamturn_abandon_all(void);

// ---------------------------------------------------------------------------
// How much is buffered before playback starts
//
// Shared, for the same reason as the turn: they answer "how many seconds of
// music must be in hand so playback does not catch up with the download", and
// that does not depend on which service is sending the bytes. Two copies would
// drift apart.
// ---------------------------------------------------------------------------

// How much to wait for before starting the decoder. Enough for the format
// headers (a FLAC STREAMINFO fits in a few kilobytes) plus margin so playback
// does not immediately catch up with the download.
#define STREAM_PREBUFFER (512 * 1024)

// The first chunk, timed to measure network speed.
#define STREAM_PROBE (256 * 1024)

// Prefetch depth in seconds of music: floor and ceiling.
//
// A byte count means nothing: half a megabyte is twelve seconds of MP3 and one
// second of 24/176.4. Hence seconds.
//
// The floor covers the network's first breath, and the fact that a measurement
// taken over one chunk can arrive all at once from the socket buffer and say
// nothing.
//
// The ceiling exists because the never-stall formula -- hold enough to cover
// the network deficit through to the end of the track -- answers "almost the
// whole file" on a network slower than the track, which on a hundred megabytes
// of hi-res is minutes of waiting before the first note. Refill is clean
// (audio.c stops, stocks up and resumes, always interruptible), so this
// behaves like a streaming app instead: start with a few seconds in hand and
// pause briefly to reload if the network cannot keep up.
#define STREAM_HEAD_SECS 3.0
#define STREAM_HEAD_MAX_SECS 12.0

// ---------------------------------------------------------------------------
// Picking a download up where a dropped connection left it
//
// A Wi-Fi hiccup halfway through a 150 MB hi-res track used to cost the whole
// track: the download ended "cut short", the marker stayed, and the next play
// started again from byte 0. Now the cache reconnects asking for the rest
// (Range: bytes=<have>-) and carries on appending to the same open file, so
// the decoder reading it as it grows sees nothing but a pause. A server that
// ignores the Range answers 200 with everything, and the part already held is
// read past rather than rewritten (see http_stream_skip).
//
// Shared by the three caches because the delicate part is the same in each:
// the stream being replaced is the one an abandon may be shutting down at that
// very moment, so it is taken out of view before it is closed and put back
// only once the new connection exists, and the generation is checked after it
// is back in view -- the abandon bumps the generation before it wakes, so one
// of the two always catches it.
// ---------------------------------------------------------------------------

// Drops tolerated per download, counted from the last stretch of good
// progress (see STREAM_RESUME_GOOD_BYTES); the pauses between attempts double
// from one second, so four of them give a network up to fifteen seconds to come
// back.
#define STREAM_RESUME_ATTEMPTS 4

// This much downloaded since the last resume earns the attempts back: a long
// download on a flaky network may drop many times, each recovered from.
#define STREAM_RESUME_GOOD_BYTES (1024 * 1024)

typedef struct {
	http_stream_t *stream;		   // the download's stream, reopened in place
	pthread_mutex_t *active_lock;  // the cache's lock for *active_stream
	http_stream_t **active_stream; // where abandoners look for a socket to wake
	unsigned (*generation)(void);  // the cache's current generation
	unsigned started_generation;   // the download's own
	const char *url;			   // NULL or empty: resuming is not possible
	long have;					   // bytes already in the file
	long total;					   // bytes expected, 0 when unknown
	int timeout_secs;
	const char *who; // for the log: "qobuzcache: 1234"
} streamturn_resume_t;

// Called by a download loop whose read failed (or ended short of `total`).
// Reconnects and leaves r->stream positioned at byte r->have and published as
// the active stream again. `attempts` is the download's own counter, bumped
// here. False when the download was abandoned meanwhile, the attempts are
// used up, or the server now describes a file of a different size; the
// stream is closed then and the caller ends the download as before. Blocks:
// worker threads only.
bool streamturn_resume(const streamturn_resume_t *r, int *attempts);

#endif /* STREAMTURN_H */

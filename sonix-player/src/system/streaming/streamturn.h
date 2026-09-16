#ifndef STREAMTURN_H
#define STREAMTURN_H

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

#endif /* STREAMTURN_H */

#ifndef GROWFILE_H
#define GROWFILE_H

#include <stdbool.h>
#include <stddef.h>

// Reading a file while someone else is still writing it.
//
// Needed for Qobuz playback: the track downloads to the card and must start
// immediately, not once the download has finished. The decoder is the usual one
// (dr_flac, dr_mp3) opening a path as usual -- except that instead of the
// default read functions it gets these, which on reaching the end wait for the
// next piece instead of answering "done".
//
// The writer announces the file (growfile_announce), reports how much has
// arrived (growfile_progress) and closes at the end (growfile_finish). The
// reader opens with growfile_open: if nobody announced that path, the file is
// an ordinary file and nothing is waited for, so opening a track already in the
// cache costs no more than a plain fopen.
//
// This module deliberately knows nothing about Qobuz: it knows about growing
// files. qobuzcache.c ties one end and decode.c the other, and neither depends
// on the other.

// How many downloads can be in flight together. Two suffice (the playing one
// and the prefetch), four leave margin.
#define GROWFILE_MAX 4

// How long a piece is waited for while seeking inside the track. Far less than
// while reading: reading goes in order and the missing piece is arriving right
// now, whereas a seek can ask for a point that arrives minutes later, and there
// it is better to say no quickly.
#define GROWFILE_SEEK_WAIT_MS 300

// --- writer side ----------------------------------------------------------

// `total` is the final size when the server stated it, 0 when it did not.
void growfile_announce(const char *path, long total);
void growfile_progress(const char *path, long done);

// End of the download: `ok` false means it died halfway, and waiting readers
// must stop waiting instead of hanging there.
void growfile_finish(const char *path, bool ok);

// True while that path is being written.
bool growfile_is_growing(const char *path);

// True if any file being written starts with `prefix`. Needed by whoever
// sweeps the directory: deleting the file being downloaded (or its cover, or
// its tags) while it is playing is the fastest way to break everything.
bool growfile_prefix_is_growing(const char *prefix);

// --- reader side ----------------------------------------------------------

// Registers the question "is there a reason to stop waiting?". The waits in
// growfile_read and growfile_seek ask it every tenth of a second: when the
// answer is yes -- a stop, a pause, another track queued -- the wait gives up
// at once and the read returns short.
//
// Without it the audio thread can stay locked in here for up to twenty seconds
// with a command already pressed, and every other track waits for the thread to
// come free.
void growfile_set_reader_abort_cb(bool (*cb)(void));

typedef struct growfile growfile_t;

// Opens for reading. NULL if the file cannot be opened.
growfile_t *growfile_open(const char *path);
void growfile_close(growfile_t *g);

// Reads up to `bytes`. If the file is still growing and the bytes are not there
// yet, waits for them. Returns less than `bytes` (or 0) only when the file has
// really ended or the download died.
size_t growfile_read(growfile_t *g, void *out, size_t bytes);

// Seeks. `origin` 0 = from the start, 1 = from the current position. Waits if
// the destination has not arrived yet. False when it cannot be reached.
bool growfile_seek(growfile_t *g, long offset, int origin);

// The current position, for decoders that ask for it.
long growfile_tell(growfile_t *g);

// True (once: reading clears it) if since the last call a read had to wait for
// a piece not yet downloaded. This is how playback discovers it has reached the
// edge of the download: from outside, a read that waits and then fills is
// indistinguishable from a normal one.
bool growfile_take_starved(growfile_t *g);

// How much has arrived and how much there will be in total, for the reader.
// False when that file is no longer growing (it is all there) or when the
// server never stated the final size.
//
// It serves one important purpose: inside a downloading file one cannot seek
// past what has arrived. A seek must know that before trying, otherwise it
// waits for bytes that arrive minutes later -- and the waiter is the audio
// thread.
bool growfile_span(growfile_t *g, long *done, long *total);

#endif /* GROWFILE_H */

#ifndef YANDEXWAVE_H
#define YANDEXWAVE_H

#include <stdbool.h>

#include "src/system/streaming/yandex.h"

// My Wave kept going: what a station needs from the player besides its first
// batch -- the next batch before the queue runs dry, and word of what was
// played, skipped and finished -- whatever page is on the screen, with the
// screen off, after a track ended on its own or was skipped.
//
// Why a module of its own rather than the page's worker: in 1.2.5 the request
// for more sat in the page worker's single "pending" slot, and anything else
// the page asked for meanwhile -- the fetch of a track playback had just
// reached, a list opened -- took the slot over. The request was gone without
// a word, and the flag that said "asked" was only ever cleared by its answer,
// so the station was never asked again and the queue ran out after the first
// batch or two. A failed request was retried only when the next track
// started, and not at all on the last one. Here the requests have their own
// queue on their own thread, nothing replaces them, and a failure is retried
// on a timer (5, 10, 20, 40, then every 60 s) for as long as the wave plays.
//
// The page owns the queue (what the player plays) and tells this module what
// it holds; this module owns the conversation with the station. No LVGL here:
// the UI 2.0 pages can drive it the same way.
//
// Thread contract: every yandexwave_* call is made on the GUI thread (but
// yandexwave_batch_done, which may come from anywhere). The network runs on
// the module's own worker, started on first use; the batch callback is
// called on that worker and must bounce to the GUI with gui_post() and answer
// with yandexwave_batch_done() there.

// The most a batch can bring (the service sends five or so).
#define YANDEXWAVE_BATCH_MAX 20

// A new batch for the queue. `tracks` belongs to the module and is valid only
// during the call: copy what is needed. `restarted`: the session had lapsed
// and a new one gave these.
typedef void (*yandexwave_batch_cb)(const yandex_track_t *tracks, int count, bool restarted, void *user);

// The page has queued a new wave: `first` is the first batch (already in the
// queue). Forgets the previous wave's bookkeeping.
void yandexwave_begin(const yandex_track_t *first, int count, yandexwave_batch_cb batch, void *user);

// The wave is no longer what plays (another list started, signed out): the
// track that was playing is reported as skipped, and nothing more is asked.
void yandexwave_end(void);
bool yandexwave_running(void);

// Once a second while the wave is the queue. `current` is the wave track
// loaded now (NULL when what plays is not one of the wave's), `playing`
// whether it is sounding, `remaining` how many tracks are queued after it in
// playback order. Reports a change of track (trackStarted, and trackFinished
// or skip for the one before, with the seconds actually heard) and asks for
// the next batch when two tracks or fewer remain.
void yandexwave_watch(const yandex_track_t *current, bool playing, int remaining);

// The batch last handed to the callback has been dealt with on the GUI thread
// (`appended` tracks went into the queue, 0 when it was dropped or lost).
// Until this comes no other batch is asked for: the queue's length is only
// right once the batch is in it. Any thread.
void yandexwave_batch_done(int appended);

// A request for tracks is out: the network is wanted (the page's watch keeps
// the Wi-Fi from being parked under it). Any thread.
bool yandexwave_requesting(void);

// A dislike of the playing track: returns the seconds it was heard (the
// dislike carries them) and keeps the skip that follows from being reported
// a second time.
int yandexwave_disliked(const char *track_id);

#endif /* YANDEXWAVE_H */

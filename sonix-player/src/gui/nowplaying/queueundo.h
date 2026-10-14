#ifndef QUEUEUNDO_H
#define QUEUEUNDO_H

#include <stdbool.h>

// ---------------------------------------------------------------------------
// "Undo" for every change to the queue (Sonix UI 2.0, principle 6)
//
// A change to the queue -- a track sent to play next or to the end, one taken
// out, the order shuffled, "Up next" cleared -- happens at once and is said in
// a toast with "Undo"; undoing puts the queue back exactly as it was (the
// entries, the order, where playback is, the "play next" marks). The handoff's
// snapshot-before-the-action rule, on top of playlist_snapshot_take().
//
// The pattern, for any page that changes the queue itself:
//
//     queue_undo_begin();                    // before the change
//     bool changed = playlist_remove(...);   // the change
//     if (changed) {
//         device_state_queue_changed();
//         queue_undo_commit(tr("..."));      // the toast, with "Undo"
//     } else {
//         queue_undo_cancel();
//     }
//
// trackmenu_queue_paths() -- "Play next" and "Add to end" from any list,
// service page or sheet -- already does all of this, so a page that queues
// through it gets the toast with "Undo" without doing anything.
//
// One snapshot at a time: a new begin() replaces the last one, and the toast
// of an earlier change loses its "Undo" when a later one takes its place, as
// any toast replaced by another does. The snapshot is freed once its toast
// has gone (queue_undo_tick() from the player's poll), so a queue the size of
// a library is held for the few seconds the toast is up and no longer.
//
// After an undo the player and the queue page are told
// (player_queue_changed(), trackmenu_queue_refresh()) and the queue is written
// down to the card again (device_state_queue_changed()).
//
// UI thread only.
// ---------------------------------------------------------------------------

// Takes the snapshot: for a library list a copy of the row ids (4 bytes a
// track), for a queue of paths a copy of each path -- not taken past 2000 of
// them (QUEUE_UNDO_MAX_PATHS), and the toast then comes without "Undo".
void queue_undo_begin(void);

// The change happened: the toast with `text` (shown as given, translated
// already) and "Undo". With no snapshot (the queue was empty before, or out of
// memory) the toast comes without "Undo".
void queue_undo_commit(const char *text);

// The change did not happen: the snapshot goes.
void queue_undo_cancel(void);

// Frees the snapshot once nothing can undo it any more. From the player's
// poll; cheap.
void queue_undo_tick(void);

// True while a snapshot is held (for tests and the report).
bool queue_undo_pending(void);

#endif /* QUEUEUNDO_H */

#ifndef QUEUEUNDO_H
#define QUEUEUNDO_H

#include <stdbool.h>

#include "src/system/playback/playlist.h"

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
// Additions -- "Play next" and "Add to the end" from any list, service page or
// sheet -- go through queue_undo_add_paths() and then queue_undo_commit():
// trackmenu_queue_paths(), listacts_queue() (the library lists) and
// svcui_queue() (the streaming pages) are the three doors, each with its own
// wording, and all of them end here, so every change to the queue has the same
// "Undo". This is the only undo of the queue: there is no other to keep in step.
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

// Queues `paths` (playlist_queue_paths) after taking what "Undo" needs: the
// snapshot, or -- for a queue too long for one -- a note of just this
// addition, so even a queue of thousands can take back what was added to it.
// The queue is written down to the card, the player and the queue page are
// told. Returns how many went in; on 0 the reason has been said ("already
// playing" for "Play next" on the playing track, else "could not add") and
// nothing is left to commit. Otherwise follow with queue_undo_commit(text).
int queue_undo_add_paths(const char *const *paths, int count, playlist_queue_where_t where);

// True while a snapshot or a note is held (for tests and the report).
bool queue_undo_pending(void);

#endif /* QUEUEUNDO_H */

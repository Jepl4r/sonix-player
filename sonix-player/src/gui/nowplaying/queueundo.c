#include "queueundo.h"

#include <stdint.h>
#include <stdio.h>

#include "src/gui/nowplaying/player.h"
#include "src/gui/nowplaying/trackmenu.h"
#include "src/gui/shell/sx_toast.h"
#include "src/gui/shell/gui.h"
#include "src/system/core/lang.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"

#include <string.h>

static playlist_snapshot_t *snap;

// An addition to a queue too long for a snapshot: a note of just what went in
// (playlist_queue_paths_undoable), which costs nothing whatever the queue's
// size. Only queue_undo_add_paths() makes one.
static playlist_queue_undo_t added;

// Which change the snapshot belongs to. The toast keeps the number it was
// shown with, so an "Undo" from a toast whose snapshot has since been replaced
// does nothing instead of undoing somebody else's change.
static uintptr_t generation;

static void drop(void) {
	playlist_snapshot_free(snap);
	snap = NULL;
	added.valid = false;
}

// A queue of paths copies each of them into the snapshot (a library list
// copies only its row ids): a list of thousands queued from a streaming
// service is half a megabyte for the few seconds of a toast. Past this the
// change is made all the same and said without "Undo" -- except an addition,
// which is taken back from its note instead (queue_undo_add_paths).
#define QUEUE_UNDO_MAX_PATHS 2000

void queue_undo_begin(void) {
	drop();
	generation++;
	if (playlist_appended_count() > QUEUE_UNDO_MAX_PATHS) {
		return;
	}
	snap = playlist_snapshot_take();
}

void queue_undo_cancel(void) { drop(); }

bool queue_undo_pending(void) { return snap != NULL || added.valid; }

static void undo_cb(void *user) {
	if ((uintptr_t)user != generation || (!snap && !added.valid)) {
		return;
	}
	if (snap) {
		playlist_snapshot_t *taken = snap;
		snap = NULL;
		device_state_t state;
		device_state_get(&state);
		if (!playlist_snapshot_restore(taken, state.current_file[0] ? state.current_file : NULL)) {
			return;
		}
	} else {
		playlist_queue_undo_t note = added;
		added.valid = false;
		if (!playlist_queue_undo(&note)) {
			// Something else changed the queue in the meantime (one of the
			// added tracks came up, another change): taking these out now
			// could take the wrong ones.
			sx_toast(tr("lists_undo_too_late"));
			return;
		}
	}
	printf("queue: undone (%d tracks)\n", playlist_count());
	device_state_queue_changed();
	player_queue_changed();
	trackmenu_queue_refresh();
}

void queue_undo_commit(const char *text) {
	if (!snap && !added.valid) {
		sx_toast(text ? text : "");
		return;
	}
	sx_toast_undo(text ? text : "", undo_cb, (void *)generation);
}

void queue_undo_tick(void) {
	if ((snap || added.valid) && !sx_toast_showing()) {
		drop();
	}
}

int queue_undo_add_paths(const char *const *paths, int count, playlist_queue_where_t where) {
	queue_undo_begin();
	int queued = 0;
	if (paths && count > 0) {
		queued = snap ? playlist_queue_paths(paths, count, where)
					  : playlist_queue_paths_undoable(paths, count, where, &added);
	}
	if (queued <= 0) {
		queue_undo_cancel();
		// "Play next" on the track that is playing is the one refusal with a
		// reason worth more than "could not": it is already where it would go.
		char playing[512];
		int at = playlist_current_index();
		if (where == PLAYLIST_QUEUE_NEXT && count == 1 && paths && paths[0] && at >= 0 &&
			playlist_path_at(at, playing, sizeof(playing)) && strcmp(playing, paths[0]) == 0) {
			sx_toast(tr("queue_already_playing"));
		} else {
			gui_notify_popup("playlist_cannot_add_the_tracks");
		}
		return 0;
	}
	// Written down now: the mirror on disk is otherwise refreshed only when a
	// track is loaded, and a power-off before that loses the addition. One
	// write for the whole set, whatever its size.
	device_state_queue_changed();
	player_queue_changed();
	trackmenu_queue_refresh();
	return queued;
}

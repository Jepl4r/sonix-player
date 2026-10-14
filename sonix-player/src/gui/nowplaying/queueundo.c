#include "queueundo.h"

#include <stdint.h>
#include <stdio.h>

#include "src/gui/nowplaying/player.h"
#include "src/gui/nowplaying/trackmenu.h"
#include "src/gui/shell/sx_toast.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"

static playlist_snapshot_t *snap;

// Which change the snapshot belongs to. The toast keeps the number it was
// shown with, so an "Undo" from a toast whose snapshot has since been replaced
// does nothing instead of undoing somebody else's change.
static uintptr_t generation;

static void drop(void) {
	playlist_snapshot_free(snap);
	snap = NULL;
}

void queue_undo_begin(void) {
	drop();
	generation++;
	snap = playlist_snapshot_take();
}

void queue_undo_cancel(void) { drop(); }

bool queue_undo_pending(void) { return snap != NULL; }

static void undo_cb(void *user) {
	if ((uintptr_t)user != generation || !snap) {
		return;
	}
	playlist_snapshot_t *taken = snap;
	snap = NULL;

	device_state_t state;
	device_state_get(&state);
	if (!playlist_snapshot_restore(taken, state.current_file[0] ? state.current_file : NULL)) {
		return;
	}
	fprintf(stderr, "queue: undone (%d tracks)\n", playlist_count());
	device_state_queue_changed();
	player_queue_changed();
	trackmenu_queue_refresh();
}

void queue_undo_commit(const char *text) {
	if (!snap) {
		sx_toast(text ? text : "");
		return;
	}
	sx_toast_undo(text ? text : "", undo_cb, (void *)generation);
}

void queue_undo_tick(void) {
	if (snap && !sx_toast_showing()) {
		drop();
	}
}

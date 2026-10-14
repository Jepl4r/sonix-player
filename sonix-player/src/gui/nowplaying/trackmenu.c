#include "trackmenu.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/library/medialist.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/library/playlistpage.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/popover.h"
#include "src/gui/settings/musicsettings.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/npicons.h"
#include "src/gui/nowplaying/queueundo.h"
#include "src/gui/shell/sx_frame.h"
#include "src/gui/shell/sx_mini.h"
#include "src/gui/shell/sx_palette.h"
#include "src/gui/shell/sx_row.h"
#include "src/gui/shell/sx_sheet.h"
#include "src/gui/shell/sx_styles.h"
#include "src/gui/shell/sx_toast.h"
#include "src/gui/shell/sx_tokens.h"
#include "src/gui/shell/topbar.h"
#include "src/system/library/covertone.h"
#include "src/system/library/covertonefan.h"
#include "src/system/playback/audiobook.h"
#include "src/system/remote/dlna.h"
#include "src/system/audio/audio.h"
#include "src/system/audio/replaygain.h"
#include "src/system/decode/decode.h"
#include "src/system/library/metadata.h"
#include "src/system/playback/device_state.h"
#include "src/system/core/lang.h"
#include "src/gui/streaming/podcastcard.h"
#include "src/gui/streaming/podcastpage.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/provider.h"
#include "src/system/library/library.h"
#include "src/system/playback/playlist.h"
#include "src/system/core/utils.h"

lv_obj_t *queue_screen;
lv_obj_t *details_screen;

// ---------------------------------------------------------------------------
// The queue (Sonix UI 2.0, the handoff's "Очередь")
//
// A compact list in three runs, each under its heading: СЕЙЧАС ИГРАЕТ (the
// playing track), ДАЛЕЕ · N (what is still to come, in the order it will
// play) and УЖЕ ИГРАЛИ · N (what has played, oldest first). The rows are the
// 2.0 row (sx_row, compact: 64 px, 84 with Large rows): the album's
// thumbnail -- its tone plaque until there is one -- the title, the artist
// and the length; the playing one on the surface with the equaliser.
//
// Gestures, the handoff's matrix for a queue row: a tap plays it (and the
// page stays: the mini player says what plays), a drag to the left takes it
// out ("✕ Remove", not the playing one) with "Undo" in the toast, a hold
// opens its sheet (play now, move to next, up, down, show the album,
// remove). The header's ⋮: save as a playlist, shuffle what is left, clear
// "Up next" -- each with "Undo" (queueundo.h).
//
// What plays outside the queue (the handoff's `ext`): a station, AirPlay or
// the Bluetooth receiver play beside it, so the queue is all there, with a
// note on top instead of the playing run, and its next run starting at the
// track it will go back to. A book (or a phone's track over DLNA) took the
// queue's place, and the music queue it interrupted is kept aside
// (device_state_parked()): that is the one listed, and a tap on it goes back
// to it. Nothing ever played (the idle state): "Nothing is playing", and the
// one button, "Shuffle all music".
//
// Windowed, as every long list here: the queue may be a whole library. The
// runs have fixed row heights, so where any entry sits is arithmetic
// (queue_row_y), and a pool of QUEUE_POOL rows is bound to whatever is in
// view as the list scrolls. A bind is three indexed lookups (names, length,
// album) and, when the album has one, its 56 px thumbnail read into the row's
// own buffer (6 KB, allocated on first use and freed when the page is left).
//
// UI thread.
// ---------------------------------------------------------------------------

#define QUEUE_POOL 16
#define QUEUE_GROUP_H 44 // the handoff: 14 above, the 16 px heading, 6 below
#define QUEUE_PAD_BOTTOM 24

typedef enum {
	QUEUE_MODE_IDLE,   // nothing has played: the empty state
	QUEUE_MODE_QUEUE,  // the queue, its track playing
	QUEUE_MODE_BESIDE, // something plays beside the queue: a station, AirPlay, the receiver
	QUEUE_MODE_PARKED, // a book or the phone took its place: the parked queue
} queue_mode_t;

typedef struct {
	lv_obj_t *row;
	int index;		// the entry bound, -1 for none
	int64_t album;	// the album whose thumbnail is in `buf`, 0 for none
	uint8_t *buf;	// COVERTONE_THUMB_BYTES(56), on first use
	lv_image_dsc_t dsc;
} queue_row_t;

static lv_obj_t *queue_title;
static lv_obj_t *queue_more_btn;
static lv_obj_t *queue_list; // the scrollable viewport
static lv_obj_t *queue_body; // as tall as the whole list; the rows sit on it
static lv_obj_t *queue_heads[3];
static lv_obj_t *queue_note_out;   // "Now playing X -- outside the queue"
static lv_obj_t *queue_note_empty; // "Nothing next"
static lv_obj_t *queue_idle;	   // the idle state
static queue_row_t queue_rows[QUEUE_POOL];

// The layout, as queue_layout() last worked it out.
static queue_mode_t queue_mode;
static int queue_total;		  // entries of the list shown
static int queue_cur;		  // the entry shown as playing; -1 when none is
static int queue_next_first;  // the first entry of the "Up next" run
static int queue_next_count;
static int queue_played_count; // entries 0 .. queue_played_count - 1
static int queue_pitch;		  // a row and the gap under it
static int32_t queue_y_cur, queue_y_next, queue_y_played;
static unsigned queue_seen_revision;

static void queue_layout(void);
static void queue_window_update(void);
static int32_t queue_row_y(int index);

static bool queue_page_up(void) { return queue_screen && lv_screen_active() == queue_screen; }

// The name a row, a sheet and a toast give entry `index`.
static bool queue_path_at(int index, char *out, size_t size) {
	if (queue_mode == QUEUE_MODE_PARKED) {
		return device_state_parked_path(index, out, size);
	}
	return playlist_path_at(index, out, size);
}

static void queue_names_of(const char *path, char *title, size_t title_size, char *artist, size_t artist_size) {
	title[0] = '\0';
	artist[0] = '\0';
	if (library_track_names(path, title, title_size, artist, artist_size) && title[0]) {
		return;
	}
	// Not in the library: a streamed track keeps its tags in the sidecar
	// beside the cache file, and a file from outside the music its own.
	struct stat st;
	if (stat(path, &st) == 0) {
		song_metadata_t meta;
		metadata_read(path, &meta);
		if (meta.title[0]) {
			snprintf(title, title_size, "%s", meta.title);
			snprintf(artist, artist_size, "%s", metadata_shown_artist(&meta));
			return;
		}
	}
	const char *slash = strrchr(path, '/');
	snprintf(title, title_size, "%s", slash ? slash + 1 : path);
	char *dot = strrchr(title, '.');
	if (dot && dot != title) {
		*dot = '\0';
	}
}

static void queue_title_of(int index, char *out, size_t size) {
	char path[512];
	out[0] = '\0';
	if (queue_path_at(index, path, sizeof(path))) {
		char title[256];
		char artist[256];
		queue_names_of(path, title, sizeof(title), artist, sizeof(artist));
		snprintf(out, size, "%s", title);
	}
}

// ---------------------------------------------------------------------------
// The rows
// ---------------------------------------------------------------------------

static int queue_user_index(void *user) { return (int)(intptr_t)user - 1; }

// Whether entry `index` may be taken out: never the playing track, and
// nothing of a parked queue (it is not the queue now).
static bool queue_removable(int index) {
	return (queue_mode == QUEUE_MODE_QUEUE || queue_mode == QUEUE_MODE_BESIDE) &&
		   index != playlist_current_index();
}

// The album's 56 px thumbnail on the row, or its tone plaque until there is
// one (covertone.h, "List rows").
static void queue_row_cover(queue_row_t *q, const char *path, const char *title) {
	char album[512];
	int64_t id = 0;
	char shown[256] = "";
	if (library_track_album_value(path, album, sizeof(album)) && album[0]) {
		id = covertone_album_id(album);
		library_album_title(album, shown, sizeof(shown));
	}
	float hue = 0, chroma = 0, light = 0;
	if (!id || !covertone_for_album(id, &hue, &chroma, &light)) {
		hue = covertone_hash_hue(shown[0] ? shown : title);
	}
	lv_color_t tone = sx_tone(SX_T_COVER_TONE, hue);
	if (id && id == q->album) {
		return; // that thumbnail is up already
	}
	// Off the buffer before it is written to.
	sx_row_set_thumb(q->row, NULL, tone);
	q->album = 0;
	if (!id) {
		return;
	}
	if (!q->buf) {
		q->buf = malloc(COVERTONE_THUMB_BYTES(COVERTONE_THUMB_SMALL));
		if (!q->buf) {
			return;
		}
	}
	if (covertone_thumb(id, COVERTONE_THUMB_SMALL, &q->dsc, q->buf, COVERTONE_THUMB_BYTES(COVERTONE_THUMB_SMALL))) {
		q->album = id;
		sx_row_set_thumb(q->row, &q->dsc, tone);
	} else {
		covertone_request(id); // the listener brings it (queue_covertone_changed)
	}
}

static void queue_row_bind(queue_row_t *q, int index) {
	q->index = index;
	char path[512];
	if (index < 0 || !queue_path_at(index, path, sizeof(path))) {
		q->index = -1;
		lv_obj_set_hidden(q->row, true);
		return;
	}
	char title[256];
	char artist[256];
	queue_names_of(path, title, sizeof(title), artist, sizeof(artist));

	char length[16] = "";
	uint32_t ms = 0;
	library_track_quality_length(path, &ms);
	if (ms > 0) {
		formatDoubleSeconds(ms / 1000.0, length, sizeof(length));
	}

	sx_row_set_user(q->row, (void *)(intptr_t)(index + 1));
	sx_row_set_title(q->row, title);
	// A "play next" still to come says so: why it is where it is, and that a
	// change of shuffle will not move it (1.2.5).
	bool queued = queue_mode == QUEUE_MODE_QUEUE && playlist_is_queued_next(index);
	sx_row_set_subtitle(q->row, artist, queued ? tr("queue_badge_next") : NULL, length[0] ? length : NULL);
	sx_row_set_playing(q->row, index == queue_cur);
	sx_row_set_swipes(q->row, false, queue_removable(index));
	queue_row_cover(q, path, title);

	lv_obj_set_y(q->row, queue_row_y(index));
	lv_obj_set_hidden(q->row, false);
}

// Where entry `index` sits on the body, -1 when it is not listed.
static int32_t queue_row_y(int index) {
	if (index < 0) {
		return -1;
	}
	if (index == queue_cur) {
		return queue_y_cur;
	}
	if (index >= queue_next_first && index < queue_next_first + queue_next_count) {
		return queue_y_next + (int32_t)(index - queue_next_first) * queue_pitch;
	}
	if (index < queue_played_count) {
		return queue_y_played + (int32_t)index * queue_pitch;
	}
	return -1;
}

// The entries of a run of `count` rows starting at `y0` that are within
// [top, bottom], into `want`.
static int queue_run_visible(int32_t y0, int first, int count, int32_t top, int32_t bottom, int *want, int n,
							 int max) {
	if (count <= 0 || y0 + (int32_t)count * queue_pitch < top || y0 > bottom) {
		return n;
	}
	int from = top > y0 ? (int)((top - y0) / queue_pitch) : 0;
	int to = (int)((bottom - y0) / queue_pitch);
	if (to > count - 1) {
		to = count - 1;
	}
	for (int k = from; k <= to && n < max; k++) {
		want[n++] = first + k;
	}
	return n;
}

static void queue_window_update(void) {
	if (!queue_list) {
		return;
	}
	int want[QUEUE_POOL];
	int n = 0;
	if (queue_mode != QUEUE_MODE_IDLE && queue_pitch > 0) {
		int32_t top = lv_obj_get_scroll_y(queue_list) - queue_pitch;
		int32_t bottom = top + lv_obj_get_height(queue_list) + 2 * queue_pitch;
		if (queue_cur >= 0) {
			n = queue_run_visible(queue_y_cur, queue_cur, 1, top, bottom, want, n, QUEUE_POOL);
		}
		n = queue_run_visible(queue_y_next, queue_next_first, queue_next_count, top, bottom, want, n, QUEUE_POOL);
		n = queue_run_visible(queue_y_played, 0, queue_played_count, top, bottom, want, n, QUEUE_POOL);
	}

	// Rows already showing a wanted entry keep it; the rest are free.
	bool kept[QUEUE_POOL] = {false};
	bool placed[QUEUE_POOL] = {false};
	for (int i = 0; i < QUEUE_POOL; i++) {
		queue_row_t *q = &queue_rows[i];
		for (int k = 0; k < n && q->index >= 0; k++) {
			if (!placed[k] && want[k] == q->index) {
				placed[k] = true;
				kept[i] = true;
				break;
			}
		}
	}
	int free_row = 0;
	for (int k = 0; k < n; k++) {
		if (placed[k]) {
			continue;
		}
		while (free_row < QUEUE_POOL && kept[free_row]) {
			free_row++;
		}
		if (free_row >= QUEUE_POOL) {
			break;
		}
		kept[free_row] = true;
		queue_row_bind(&queue_rows[free_row], want[k]);
	}
	for (int i = 0; i < QUEUE_POOL; i++) {
		if (!kept[i] && queue_rows[i].index != -1) {
			queue_rows[i].index = -1;
			lv_obj_set_hidden(queue_rows[i].row, true);
		}
	}
}

// Every row bound again where it is: the entries under them changed.
static void queue_rebind_all(void) {
	for (int i = 0; i < QUEUE_POOL; i++) {
		queue_rows[i].index = -1;
		lv_obj_set_hidden(queue_rows[i].row, true);
	}
	queue_window_update();
}

static void queue_scroll_cb(lv_event_t *e) {
	(void)e;
	queue_window_update();
}

// ---------------------------------------------------------------------------
// The layout: which runs, where
// ---------------------------------------------------------------------------

static void queue_head_place(int which, int32_t y, const char *text) {
	lv_label_set_text(queue_heads[which], text);
	lv_obj_set_y(queue_heads[which], y);
	lv_obj_set_hidden(queue_heads[which], false);
}

static int32_t queue_note_place(lv_obj_t *note, int32_t y, const char *text) {
	lv_label_set_text(note, text);
	lv_obj_set_y(note, y);
	lv_obj_set_hidden(note, false);
	lv_obj_update_layout(note);
	return lv_obj_get_height(note);
}

static void queue_layout(void) {
	if (!queue_list) {
		return;
	}
	queue_seen_revision = playlist_revision();
	queue_pitch = sx_row_height(true) + SX_ROW_GAP;

	char outside[256] = "";
	bool beside = false;
	int parked_count = 0, parked_at = -1;
	bool parked = device_state_parked(&parked_count, &parked_at);
	if (player_outside_title(outside, sizeof(outside))) {
		beside = !audiobook_is_playing() && !dlna_owns_playback();
	}

	int count = playlist_count();
	int current = playlist_current_index();
	if (outside[0] && !beside) {
		queue_mode = QUEUE_MODE_PARKED;
		queue_total = parked ? parked_count : 0;
		queue_cur = -1;
		queue_next_first = parked_at >= 0 ? parked_at : 0;
	} else if (outside[0]) {
		queue_mode = QUEUE_MODE_BESIDE;
		queue_total = count;
		queue_cur = -1;
		queue_next_first = current >= 0 ? current : 0; // where the queue goes back to
	} else if (count <= 0 && !sx_mini_has_track()) {
		queue_mode = QUEUE_MODE_IDLE;
		queue_total = 0;
		queue_cur = -1;
		queue_next_first = 0;
	} else {
		queue_mode = QUEUE_MODE_QUEUE;
		queue_total = count > 0 ? count : 0;
		queue_cur = current >= 0 && current < queue_total ? current : -1;
		queue_next_first = queue_cur + 1;
	}
	if (queue_next_first > queue_total) {
		queue_next_first = queue_total;
	}
	queue_next_count = queue_total - queue_next_first;
	queue_played_count = queue_mode == QUEUE_MODE_QUEUE ? (queue_cur > 0 ? queue_cur : 0) : queue_next_first;

	for (int i = 0; i < 3; i++) {
		lv_obj_set_hidden(queue_heads[i], true);
	}
	lv_obj_set_hidden(queue_note_out, true);
	lv_obj_set_hidden(queue_note_empty, true);
	lv_obj_set_hidden(queue_idle, queue_mode != QUEUE_MODE_IDLE);
	// Nothing to save, shuffle or clear in a queue that is not the queue.
	bool menu = queue_mode == QUEUE_MODE_QUEUE && queue_total > 0;
	lv_obj_set_hidden(queue_more_btn, !menu);

	int32_t y = 0;
	char text[320];
	if (queue_mode == QUEUE_MODE_IDLE) {
		lv_obj_set_height(queue_body, 1);
		queue_rebind_all();
		return;
	}
	if (queue_mode == QUEUE_MODE_QUEUE) {
		if (queue_cur >= 0) {
			queue_head_place(0, y, tr("queue_group_now"));
			y += QUEUE_GROUP_H;
			queue_y_cur = y;
			y += queue_pitch;
		}
	} else {
		snprintf(text, sizeof(text), tr("queue_outside_fmt"), outside);
		y += queue_note_place(queue_note_out, y + 8, text) + 8;
	}

	snprintf(text, sizeof(text), tr("queue_group_next_fmt"), queue_next_count);
	queue_head_place(1, y, text);
	y += QUEUE_GROUP_H;
	queue_y_next = y;
	if (queue_next_count > 0) {
		y += (int32_t)queue_next_count * queue_pitch;
	} else {
		y += queue_note_place(queue_note_empty, y, tr("queue_next_empty"));
	}

	if (queue_played_count > 0) {
		snprintf(text, sizeof(text), tr("queue_group_played_fmt"), queue_played_count);
		queue_head_place(2, y, text);
		y += QUEUE_GROUP_H;
		queue_y_played = y;
		y += (int32_t)queue_played_count * queue_pitch;
	}
	lv_obj_set_height(queue_body, y + QUEUE_PAD_BOTTOM);
	lv_obj_update_layout(queue_list);
	queue_rebind_all();
}

// The layout again, one tick later: from inside a row's own gesture, which
// must finish on the objects it started on.
static void queue_layout_async_cb(void *user) {
	(void)user;
	if (queue_page_up()) {
		queue_layout();
	}
}

static void queue_layout_soon(void) { lv_async_call(queue_layout_async_cb, NULL); }

void trackmenu_queue_refresh(void) {
	if (queue_page_up()) {
		queue_layout();
	}
}

// The track changed under the page: the runs move round it.
void trackmenu_notify_now_playing(void) { trackmenu_queue_refresh(); }

// covertone has thumbnails or tones it did not have (covertonefan.h): the
// rows still on their plaque look again.
static void queue_covertone_changed(void) {
	if (!queue_page_up()) {
		return;
	}
	for (int i = 0; i < QUEUE_POOL; i++) {
		queue_row_t *q = &queue_rows[i];
		if (q->index >= 0 && q->album == 0) {
			char path[512];
			char title[256], artist[256];
			if (queue_path_at(q->index, path, sizeof(path))) {
				queue_names_of(path, title, sizeof(title), artist, sizeof(artist));
				queue_row_cover(q, path, title);
			}
		}
	}
}

// ---------------------------------------------------------------------------
// What the rows do
// ---------------------------------------------------------------------------

// Plays entry `index` of the list shown. The page stays (the owner's rule for
// 2.0: playing never pulls the player up; the mini player says what plays).
static void queue_play(int index) {
	bool ok;
	if (queue_mode == QUEUE_MODE_PARKED) {
		ok = device_state_resume_parked(index);
	} else {
		ok = device_state_play_queue_index(index);
	}
	if (ok) {
		player_refresh_now_playing(); // which comes back here (trackmenu_notify_now_playing)
	}
}

static void queue_row_tap(lv_obj_t *row, void *user) {
	(void)row;
	queue_play(queue_user_index(user));
}

static void queue_remove_index(int index) {
	if (!queue_removable(index)) {
		return;
	}
	static char text[320];
	char title[256];
	queue_title_of(index, title, sizeof(title));
	queue_undo_begin();
	if (!playlist_remove(&index, 1)) {
		queue_undo_cancel();
		return;
	}
	// Written down now, as an addition is: the mirror on the card is
	// otherwise refreshed only when a track is loaded.
	device_state_queue_changed();
	player_queue_changed();
	snprintf(text, sizeof(text), tr("queue_removed_fmt"), title);
	queue_undo_commit(text);
	queue_layout_soon();
}

static void queue_row_swiped_out(lv_obj_t *row, void *user) {
	(void)row;
	queue_remove_index(queue_user_index(user));
}

// The row's sheet.
static int queue_sheet_index;

static void queue_sheet_play(void *user) {
	(void)user;
	queue_play(queue_sheet_index);
}

static void queue_sheet_next(void *user) {
	(void)user;
	static char text[320];
	char title[256];
	queue_title_of(queue_sheet_index, title, sizeof(title));
	queue_undo_begin();
	if (!playlist_move_next(queue_sheet_index)) {
		queue_undo_cancel();
		return;
	}
	device_state_queue_changed();
	player_queue_changed();
	snprintf(text, sizeof(text), tr("queue_moved_next_fmt"), title);
	queue_undo_commit(text);
	queue_layout();
}

// One place up or down, as 1.2.5's arrows did: among what is still to come.
static void queue_sheet_move(void *user) {
	int delta = (int)(intptr_t)user;
	if (!playlist_move(queue_sheet_index, delta)) {
		return;
	}
	device_state_queue_changed();
	player_queue_changed();
	queue_layout();
}

static void queue_sheet_remove(void *user) {
	(void)user;
	queue_remove_index(queue_sheet_index);
}

static void queue_sheet_album(void *user) {
	(void)user;
	char path[512];
	char album[512];
	if (!queue_path_at(queue_sheet_index, path, sizeof(path)) ||
		!library_track_album_value(path, album, sizeof(album)) || !album[0]) {
		gui_notify_popup("trackmenu_no_album");
		return;
	}
	medialist_open(album, LIBRARY_LIST_TRACKS, LIBRARY_FILTER_ALBUM, album);
}

static void queue_row_hold(lv_obj_t *row, void *user) {
	(void)row;
	int index = queue_user_index(user);
	queue_sheet_index = index;
	char path[512];
	if (!queue_path_at(index, path, sizeof(path))) {
		return;
	}
	static char title[256];
	static char sub[128];
	char artist[256];
	queue_names_of(path, title, sizeof(title), artist, sizeof(artist));
	snprintf(sub, sizeof(sub), tr("queue_position_fmt"), index + 1, queue_total);

	char album[512];
	bool has_album = library_track_album_value(path, album, sizeof(album)) && album[0];
	bool parked = queue_mode == QUEUE_MODE_PARKED;
	bool playing = index == queue_cur;
	int current = playlist_current_index();
	bool upcoming = !parked && index > current;

	sx_sheet_item_t items[6];
	int n = 0;
	if (!playing) {
		items[n++] = (sx_sheet_item_t){tr("queue_play_now"), &npicon_play, false, false, queue_sheet_play, NULL};
	}
	if (!parked && !playing && index != current + 1) {
		items[n++] =
			(sx_sheet_item_t){tr("queue_move_next"), &npicon_play_next, false, false, queue_sheet_next, NULL};
	}
	if (upcoming && index - 1 > current) {
		items[n++] = (sx_sheet_item_t){tr("queue_move_up"), &npicon_move_up, false, false, queue_sheet_move,
									   (void *)(intptr_t)-1};
	}
	if (upcoming && index + 1 < queue_total) {
		items[n++] = (sx_sheet_item_t){tr("queue_move_down"), &npicon_move_down, false, false, queue_sheet_move,
									   (void *)(intptr_t)1};
	}
	if (has_album) {
		items[n++] = (sx_sheet_item_t){tr("show_album"), &npicon_album, false, false, queue_sheet_album, NULL};
	}
	if (queue_removable(index)) {
		items[n++] =
			(sx_sheet_item_t){tr("queue_remove"), &npicon_remove, false, true, queue_sheet_remove, NULL};
	}
	if (n > 0) {
		sx_sheet_menu(title, sub, items, n);
	}
}

// ---------------------------------------------------------------------------
// The header's ⋮
// ---------------------------------------------------------------------------

// The queue as it would play, into the playlist picker -- the same picker, and
// so the same writer, "Add to playlist" uses: the user chooses a playlist or
// makes a new one there. Streamed tracks are left out: their files are a
// cache's and are gone a few albums later, which is also why the queue mirror
// on the card never holds them.
#define QUEUE_SAVE_MAX 2000

static void queue_save_cb(void *user) {
	(void)user;
	int total = playlist_count();
	int want = total < QUEUE_SAVE_MAX ? total : QUEUE_SAVE_MAX;
	if (want <= 0) {
		return;
	}
	char **paths = calloc((size_t)want, sizeof(*paths));
	if (!paths) {
		return;
	}
	int n = 0;
	for (int i = 0; i < want; i++) {
		char path[512];
		if (!playlist_path_at(i, path, sizeof(path)) || stream_provider_owns(path) ||
			podcastcache_is_episode(path)) {
			continue;
		}
		paths[n] = strdup(path);
		if (!paths[n]) {
			break;
		}
		n++;
	}
	if (n == 0) {
		gui_notify_popup("trackmenu_queue_nothing_to_save");
	} else {
		if (total > QUEUE_SAVE_MAX) {
			// Said, not done silently: "All tracks" as the queue is a library,
			// and a playlist of a library is not what anybody saving a queue
			// has in mind.
			char text[160];
			snprintf(text, sizeof(text), tr("trackmenu_queue_save_capped"), QUEUE_SAVE_MAX);
			toast_plain(text);
		}
		// The picker goes back to this page when it is done: the queue is
		// where the user was, and where the result can be seen.
		playlistpage_add_tracks((const char *const *)paths, n);
	}
	for (int i = 0; i < n; i++) {
		free(paths[i]);
	}
	free(paths);
}

static void queue_shuffle_rest_cb(void *user) {
	(void)user;
	queue_undo_begin();
	if (!playlist_shuffle_upcoming()) {
		queue_undo_cancel();
		return;
	}
	device_state_queue_changed();
	player_queue_changed();
	queue_undo_commit(tr("queue_shuffled"));
	queue_layout();
}

// "Clear Up next": no question asked first any more -- the toast's "Undo" is
// the way back (principle 6: a confirmation only for what cannot be undone).
static void queue_clear_cb(void *user) {
	(void)user;
	queue_undo_begin();
	if (!playlist_clear_upcoming()) {
		queue_undo_cancel();
		return;
	}
	device_state_queue_changed();
	player_queue_changed();
	queue_undo_commit(tr("trackmenu_queue_cleared"));
	queue_layout();
}

static void queue_more_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	static char sub[64];
	snprintf(sub, sizeof(sub), tr("queue_count_fmt"), playlist_count());
	bool upcoming = playlist_current_index() >= 0 && playlist_current_index() + 1 < playlist_count();
	bool shufflable = playlist_current_index() >= 0 && playlist_current_index() + 2 < playlist_count();
	sx_sheet_item_t items[3];
	int n = 0;
	items[n++] = (sx_sheet_item_t){tr("queue_save_as_playlist"), &npicon_add_playlist, false, false, queue_save_cb, NULL};
	if (shufflable) {
		items[n++] =
			(sx_sheet_item_t){tr("queue_shuffle_rest"), &npicon_shuffle, false, false, queue_shuffle_rest_cb, NULL};
	}
	if (upcoming) {
		items[n++] = (sx_sheet_item_t){tr("queue_clear_next"), &npicon_clear, false, true, queue_clear_cb, NULL};
	}
	sx_sheet_menu(tr("trackmenu_queue"), sub, items, n);
}

static void queue_shuffle_all_cb(lv_event_t *e) {
	(void)e;
	if (player_shuffle_all_music()) {
		queue_layout();
	}
}

// ---------------------------------------------------------------------------
// Queueing from the menus
// ---------------------------------------------------------------------------

int trackmenu_queue_paths(const char *const *paths, int count, playlist_queue_where_t where) {
	// The snapshot first, so the toast can offer "Undo" (queueundo.h).
	queue_undo_begin();
	int queued = playlist_queue_paths(paths, count, where);
	if (queued > 0) {
		// Written down now: the mirror on disk is otherwise refreshed only when
		// a track is loaded, and a power-off before that loses the addition.
		// One write for the whole set, whatever its size.
		device_state_queue_changed();
		player_queue_changed();
		trackmenu_queue_refresh();
		static char text[320];
		if (queued == 1 && paths && paths[0]) {
			char title[256], artist[256];
			queue_names_of(paths[0], title, sizeof(title), artist, sizeof(artist));
			snprintf(text, sizeof(text),
					 tr(where == PLAYLIST_QUEUE_NEXT ? "queue_next_named_fmt" : "queue_end_named_fmt"), title);
		} else {
			snprintf(text, sizeof(text), "%s",
					 tr(where == PLAYLIST_QUEUE_NEXT ? "queue_will_play_next" : "queue_added_to_end"));
		}
		queue_undo_commit(text);
	} else {
		queue_undo_cancel();
		gui_notify_popup("playlist_cannot_add_the_tracks");
	}
	return queued;
}

static trackmenu_queue_chosen_cb choose_cb;
static void *choose_user;

static void choose_picked(void *user) {
	trackmenu_queue_chosen_cb cb = choose_cb;
	choose_cb = NULL;
	if (cb) {
		cb((playlist_queue_where_t)(intptr_t)user, choose_user);
	}
}

void trackmenu_queue_choose(lv_obj_t *anchor, trackmenu_queue_chosen_cb chosen, void *user) {
	if (!anchor || !chosen) {
		return;
	}
	static const popover_item_t ITEMS[] = {
		{"queue_play_next", choose_picked, (void *)(intptr_t)PLAYLIST_QUEUE_NEXT},
		{"queue_add_to_end", choose_picked, (void *)(intptr_t)PLAYLIST_QUEUE_END},
	};
	choose_cb = chosen;
	choose_user = user;
	popover_show(anchor, ITEMS, 2);
}

// ---------------------------------------------------------------------------
// The page
// ---------------------------------------------------------------------------

static void queue_loaded_cb(lv_event_t *e) {
	(void)e;
	queue_layout();
	// From the top: the playing run is the first thing on it.
	lv_obj_scroll_to_y(queue_list, 0, LV_ANIM_OFF);
	queue_window_update();
}

// The thumbnails' buffers go with the page: 6 KB a row is not memory to keep
// for a page nobody is looking at.
static void queue_unloaded_cb(lv_event_t *e) {
	(void)e;
	for (int i = 0; i < QUEUE_POOL; i++) {
		queue_row_t *q = &queue_rows[i];
		sx_row_set_thumb(q->row, NULL, sx_pal()->cover_tone);
		q->album = 0;
		q->index = -1;
		lv_obj_set_hidden(q->row, true);
		free(q->buf);
		q->buf = NULL;
	}
}

static lv_obj_t *queue_label(lv_obj_t *parent, const lv_font_t *font, bool secondary) {
	lv_obj_t *label = lv_label_create(parent);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_add_style(label, secondary ? &sx_style_text2 : &sx_style_text, 0);
	return label;
}

static void build_queue_page(gui_config_t *cfg) {
	lv_obj_add_style(queue_screen, &theme_style_screen, 0);
	queue_title = settingsrow_title(queue_screen, cfg, "trackmenu_queue");

	queue_more_btn = lv_btn_create(queue_screen);
	lv_obj_remove_style_all(queue_more_btn);
	lv_obj_set_size(queue_more_btn, SX_HEADER_BTN, SX_HEADER_BTN);
	lv_obj_add_style(queue_more_btn, &sx_style_press, LV_STATE_PRESSED);
	lv_obj_set_style_radius(queue_more_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_align(queue_more_btn, LV_ALIGN_TOP_RIGHT, 0, SX_STATUSBAR_H + (SX_HEADER_H - SX_HEADER_BTN) / 2);
	lv_obj_add_event_cb(queue_more_btn, queue_more_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *more_icon = lv_image_create(queue_more_btn);
	lv_image_set_src(more_icon, &icon_ellipsis_vertical);
	lv_obj_add_style(more_icon, &sx_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(more_icon, LV_OPA_COVER, 0);
	lv_obj_center(more_icon);
	settingsrow_title_corner_slots(queue_title, cfg, 1);

	int content_top = settingsrow_content_top(cfg);
	queue_list = lv_obj_create(queue_screen);
	lv_obj_remove_style_all(queue_list);
	lv_obj_set_size(queue_list, cfg->screen_width, cfg->screen_height - content_top);
	lv_obj_align(queue_list, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_pad_hor(queue_list, SX_BODY_PAD_L, 0);
	lv_obj_set_scroll_dir(queue_list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(queue_list, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_event_cb(queue_list, queue_scroll_cb, LV_EVENT_SCROLL, NULL);
	// The last rows clear the mini player.
	sx_frame_reserve_bottom(queue_list);

	int width = cfg->screen_width - 2 * SX_BODY_PAD_L;
	queue_body = lv_obj_create(queue_list);
	lv_obj_remove_style_all(queue_body);
	lv_obj_set_size(queue_body, width, 1);
	lv_obj_set_scrollable(queue_body, false);
	lv_obj_set_event_bubble(queue_body, true);

	// The three headings: the handoff's 16 px mono capitals, a little apart.
	for (int i = 0; i < 3; i++) {
		lv_obj_t *head = queue_label(queue_body, &font_ui_17, true);
		lv_obj_set_width(head, width);
		lv_obj_set_style_text_letter_space(head, 1, 0);
		lv_obj_set_style_pad_top(head, 14, 0);
		lv_obj_set_style_pad_left(head, 6, 0);
		lv_label_set_long_mode(head, LV_LABEL_LONG_DOT);
		lv_obj_set_hidden(head, true);
		queue_heads[i] = head;
	}
	lv_obj_t *const notes[2] = {queue_label(queue_body, &font_ui_20, true), queue_label(queue_body, &font_ui_20, true)};
	for (int i = 0; i < 2; i++) {
		lv_obj_set_width(notes[i], width);
		lv_label_set_long_mode(notes[i], LV_LABEL_LONG_WRAP);
		lv_obj_set_style_pad_left(notes[i], 6, 0);
		lv_obj_set_style_pad_right(notes[i], 6, 0);
		lv_obj_set_style_pad_top(notes[i], 4, 0);
		lv_obj_set_style_pad_bottom(notes[i], 12, 0);
		lv_obj_set_style_text_line_space(notes[i], 6, 0);
		lv_obj_set_hidden(notes[i], true);
	}
	queue_note_out = notes[0];
	queue_note_empty = notes[1];

	sx_row_handlers_t handlers = {
		.on_tap = queue_row_tap,
		.on_long_press = queue_row_hold,
		.on_swipe_end = queue_row_swiped_out,
		.end_is_remove = true,
	};
	for (int i = 0; i < QUEUE_POOL; i++) {
		queue_row_t *q = &queue_rows[i];
		q->row = sx_row_create(queue_body, &handlers, true);
		lv_obj_set_width(q->row, width);
		lv_obj_set_pos(q->row, 0, 0);
		lv_obj_set_hidden(q->row, true);
		q->index = -1;
	}

	// The idle state: nothing has ever played.
	queue_idle = lv_obj_create(queue_list);
	lv_obj_remove_style_all(queue_idle);
	lv_obj_set_size(queue_idle, width, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_top(queue_idle, 90, 0);
	lv_obj_set_style_pad_hor(queue_idle, 16, 0);
	lv_obj_set_flex_flow(queue_idle, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(queue_idle, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(queue_idle, false);
	lv_obj_set_hidden(queue_idle, true);

	lv_obj_t *disc = lv_obj_create(queue_idle);
	lv_obj_remove_style_all(disc);
	lv_obj_set_size(disc, 84, 84);
	lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
	lv_obj_add_style(disc, &sx_style_surface, 0);
	lv_obj_t *glyph = lv_image_create(disc);
	lv_image_set_src(glyph, &npicon_queue);
	lv_obj_add_style(glyph, &sx_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(glyph, LV_OPA_COVER, 0);
	lv_obj_center(glyph);

	lv_obj_t *head = queue_label(queue_idle, &font_ui_30_bold, false);
	lv_label_set_text(head, tr("queue_idle_title"));
	lv_obj_set_style_pad_top(head, 20, 0);
	lv_obj_t *text = queue_label(queue_idle, &font_ui_22, true);
	lv_label_set_text(text, tr("queue_idle_text"));
	lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(text, lv_pct(100));
	lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_pad_top(text, 8, 0);

	lv_obj_t *button = lv_btn_create(queue_idle);
	lv_obj_remove_style_all(button);
	lv_obj_set_height(button, 68);
	lv_obj_set_style_pad_hor(button, 56, 0);
	lv_obj_set_style_margin_top(button, 28, 0);
	lv_obj_set_style_radius(button, 34, 0);
	lv_obj_add_style(button, &sx_style_accent, 0);
	lv_obj_add_style(button, &sx_style_press, LV_STATE_PRESSED);
	lv_obj_add_event_cb(button, queue_shuffle_all_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *button_text = lv_label_create(button);
	lv_label_set_text(button_text, tr("queue_shuffle_all"));
	lv_obj_set_style_text_font(button_text, &font_ui_24_bold, 0);
	lv_obj_center(button_text);
	lv_obj_set_width(button, LV_SIZE_CONTENT);

	switcher_attach_back_gesture(queue_list);
	lv_obj_add_event_cb(queue_screen, queue_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(queue_screen, queue_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	covertonefan_add(queue_covertone_changed);
}

static lv_obj_t *details_card;
static lv_obj_t *details_scroll;

// ---------------------------------------------------------------------------
// Details: everything known about the playing file.
// ---------------------------------------------------------------------------

static void details_add_row(const char *name, const char *value) {
	if (!value || !value[0]) {
		return; // an empty field is noise, not information
	}

	lv_obj_t *row = lv_obj_create(details_card);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(row, 0, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_set_style_pad_gap(row, 2, 0);
	lv_obj_set_scrollable(row, false);
	// Presses over the text must climb to the scroll surface, or the swipe
	// gestures can never start on the card's content.
	lv_obj_set_event_bubble(row, true);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);

	lv_obj_t *name_label = lv_label_create(row);
	lv_label_set_text(name_label, tr(name));
	lv_obj_add_style(name_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(name_label, &font_ui_18, 0);

	lv_obj_t *value_label = lv_label_create(row);
	lv_label_set_text(value_label, value);
	lv_label_set_long_mode(value_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(value_label, lv_pct(100));
	lv_obj_add_style(value_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(value_label, &font_ui_24, 0);
}

// Which file the page describes: a specific one (opened from a library
// list's popover) or, when empty, whatever is playing right now (opened from
// the player's own menu).
static char details_path[512];

// The common tail of both variants: tags, format, size, path.
// `codec` is the name the decoder reports ("MP3", "AAC", "ALAC"...), empty when
// it has none; `kbps` is the bitrate the file declares, 0 when it declares
// none; `lossy` says the format discards information. See decode.h: for a lossy
// file the bit/kHz pair describes the PCM leaving the decoder, not what was
// encoded.
// One ReplayGain figure from the tags: the correction, and the peak it was
// measured against when the file gives one.
static void details_add_gain(const char *name, float gain_db, float peak) {
	char buffer[64];
	if (peak > 0) {
		snprintf(buffer, sizeof(buffer), tr("trackmenu_rg_gain_peak"), gain_db, peak);
	} else {
		snprintf(buffer, sizeof(buffer), "%+.2f dB", gain_db);
	}
	details_add_row(name, buffer);
}

// `playing` adds what the player is applying to this file right now, which
// only exists for the track being played.
static void details_fill(const char *file, const song_metadata_t *tags, int bits, double sample_rate,
						 int channels, double duration_secs, const char *codec, int kbps, bool lossy,
						 bool playing) {
	char buffer[64];

	const char *slash = strrchr(file, '/');
	details_add_row("trackmenu_title", tags->title[0] ? tags->title : (slash ? slash + 1 : file));
	details_add_row("trackmenu_artist", tags->artist);
	details_add_row("trackmenu_album_artist", tags->album_artist);
	details_add_row("trackmenu_album", tags->album);
	details_add_row("genre", tags->genre);

	if (tags->year > 0) {
		snprintf(buffer, sizeof(buffer), "%d", tags->year);
		details_add_row("trackmenu_year", buffer);
	}
	if (tags->track_number > 0) {
		snprintf(buffer, sizeof(buffer), "%d", tags->track_number);
		details_add_row("track", buffer);
	}
	if (tags->disc_number > 0) {
		snprintf(buffer, sizeof(buffer), "%d", tags->disc_number);
		details_add_row("trackmenu_disc_number", buffer);
	}

	// A lossy file is described by its bitrate, not by bits and kHz: those two
	// numbers belong to the PCM leaving the decoder and are identical for a
	// 96 kbps AAC and a 320 kbps one. Lossless files keep the usual pair.
	if (lossy) {
		if (kbps > 0 && codec && codec[0]) {
			snprintf(buffer, sizeof(buffer), "%d kbps %s", kbps, codec);
		} else if (kbps > 0) {
			snprintf(buffer, sizeof(buffer), "%d kbps", kbps);
		} else {
			snprintf(buffer, sizeof(buffer), "%s", codec && codec[0] ? codec : "");
		}
		if (buffer[0]) {
			details_add_row("trackmenu_format", buffer);
		}
	} else if (sample_rate > 0) {
		snprintf(buffer, sizeof(buffer), tr("trackmenu_bit_khz_channels"), bits > 0 ? bits : 16,
				 sample_rate / 1000.0, channels);
		details_add_row("trackmenu_format", buffer);
	}

	// A separate bitrate row, only for local lossless tracks: size over
	// duration, which is how dense the file is. Lossy files already carry it in
	// the row above, and for a streamed track the cache file may be half
	// downloaded, making the number meaningless.
	if (!lossy && !stream_provider_owns(file) && duration_secs > 0.5) {
		struct stat st_kbps;
		if (stat(file, &st_kbps) == 0 && st_kbps.st_size > 0) {
			int avg = (int)((double)st_kbps.st_size * 8.0 / duration_secs / 1000.0 + 0.5);
			snprintf(buffer, sizeof(buffer), "%d kbps", avg);
			details_add_row("trackmenu_bitrate", buffer);
		}
	}

	if (tags->has_track_gain) {
		details_add_gain("trackmenu_rg_track", tags->track_gain_db, tags->track_peak);
	}
	if (tags->has_album_gain) {
		details_add_gain("trackmenu_rg_album", tags->album_gain_db, tags->album_peak);
	}
	// The applied figure can differ from both tags: it is the one the mode
	// picks, lowered when the peak would clip, and capped at +12 dB.
	if (playing && (tags->has_track_gain || tags->has_album_gain)) {
		if (replaygain_mode() == REPLAYGAIN_OFF) {
			details_add_row("trackmenu_rg_applied", tr("trackmenu_rg_off"));
		} else {
			snprintf(buffer, sizeof(buffer), "%+.2f dB", replaygain_current_db());
			details_add_row("trackmenu_rg_applied", buffer);
		}
	}

	// No size row for a streamed track: it would be the size of the cache file,
	// which may still be downloading -- a number that grows on screen by itself
	// and describes the download, not the track.
	struct stat st;
	if (!stream_provider_owns(file) && stat(file, &st) == 0) {
		snprintf(buffer, sizeof(buffer), "%.1f MB", st.st_size / (1024.0 * 1024.0));
		details_add_row("trackmenu_size", buffer);
	}

	// The path, except for a streamed track: that lives in a cache directory
	// nobody chose, under a numeric name, and showing it implies a file of the
	// user's own that does not exist. The source service is shown instead.
	const stream_provider_t *service = stream_provider_for_path(file);
	if (service) {
		details_add_row("trackmenu_source", service->id);
	} else {
		details_add_row("trackmenu_file", file);
	}
}

static void details_rebuild(void) {
	lv_obj_clean(details_card);

	if (details_path[0]) {
		// An arbitrary library track: tags read from the file itself, format
		// from the decoder's header (opened and closed without decoding).
		song_metadata_t tags;
		metadata_read(details_path, &tags);

		int bits = 0, channels = 0;
		double sample_rate = 0;
		decode_format_t format = decode_detect_format(details_path);
		double duration_secs = 0;
		char codec[16] = "";
		int kbps = 0;
		bool lossy = false;
		decoder_t *dec = format != DECODE_FORMAT_UNKNOWN ? decoder_open_info(details_path, format) : NULL;
		if (dec) {
			bits = decoder_source_bits(dec);
			channels = decoder_channels(dec);
			sample_rate = decoder_sample_rate(dec);
			snprintf(codec, sizeof(codec), "%s", decoder_codec_name(dec));
			kbps = decoder_bitrate_kbps(dec);
			lossy = decoder_is_lossy(dec);
			uint64_t frames = decoder_total_pcm_frames(dec);
			if (sample_rate > 0 && frames > 0) {
				duration_secs = (double)frames / sample_rate;
			}
			decoder_close(dec);
		}

		// A lossy file that declares no bitrate: use the real average, which can
		// be computed here because the whole file is present (streamed tracks
		// never reach this branch).
		if (lossy && kbps <= 0 && duration_secs > 0.5) {
			struct stat st_lossy;
			if (stat(details_path, &st_lossy) == 0 && st_lossy.st_size > 0) {
				kbps = (int)((double)st_lossy.st_size * 8.0 / duration_secs / 1000.0 + 0.5);
			}
		}

		details_fill(details_path, &tags, bits, sample_rate, channels, duration_secs, codec, kbps, lossy,
					 false);
		return;
	}

	device_state_t state;
	device_state_get(&state);

	if (!state.current_file[0]) {
		details_add_row("trackmenu_state", tr("trackmenu_nothing_is_playing"));
		return;
	}

	// The stream, as it is actually being played.
	char codec[16] = "";
	audio_get_stream_codec(codec, sizeof(codec));
	bool lossy = audio_stream_is_lossy();
	int kbps = audio_get_stream_bitrate_kbps();

	// As in the player: when the file declares no bitrate, fall back to the real
	// average, but only for a local file -- a streamed one may still be half
	// downloaded and the number would climb by itself.
	if (lossy && kbps <= 0 && !stream_provider_owns(state.current_file) &&
		state.progress_total_secs > 0.5) {
		struct stat st_lossy;
		if (stat(state.current_file, &st_lossy) == 0 && st_lossy.st_size > 0) {
			kbps = (int)((double)st_lossy.st_size * 8.0 / state.progress_total_secs / 1000.0 + 0.5);
		}
	}

	details_fill(state.current_file, &state.metadata, audio_get_stream_bits(), state.stream_sample_rate,
				 state.stream_channels, state.progress_total_secs, codec, kbps, lossy, true);
}

static void details_loaded_cb(lv_event_t *e) {
	(void)e;
	details_rebuild();

	// Always start at the top. The page is built once and refilled on every
	// open, so the scroll container otherwise keeps the position from last time
	// and the card for another track opens halfway down. The first row is the
	// title, which is what should be visible on entry.
	//
	// After details_rebuild(), not before: that is what sets the content height,
	// and LVGL clamps a scroll past the bottom of whatever was there before.
	if (details_scroll) {
		lv_obj_scroll_to_y(details_scroll, 0, LV_ANIM_OFF);
	}
}

static void build_details_page(gui_config_t *cfg) {
	lv_obj_add_style(details_screen, &theme_style_screen, 0);
	settingsrow_title(details_screen, cfg, "details");

	int content_top = settingsrow_content_top(cfg);

	lv_obj_t *scroll = lv_obj_create(details_screen);
	lv_obj_set_size(scroll, cfg->screen_width, cfg->screen_height - content_top);
	lv_obj_align(scroll, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(scroll, 0, 0);
	lv_obj_set_style_border_width(scroll, 0, 0);
	lv_obj_set_style_radius(scroll, 0, 0);
	lv_obj_set_style_pad_hor(scroll, cfg->padding, 0);
	lv_obj_set_style_pad_ver(scroll, 0, 0);
	lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
	details_scroll = scroll;

	details_card = lv_obj_create(scroll);
	lv_obj_set_width(details_card, lv_pct(100));
	lv_obj_set_height(details_card, LV_SIZE_CONTENT);
	lv_obj_add_style(details_card, &theme_style_card, 0);
	lv_obj_set_style_radius(details_card, 12, 0);
	lv_obj_set_style_border_width(details_card, 0, 0);
	lv_obj_set_style_shadow_width(details_card, 0, 0);
	lv_obj_set_style_pad_all(details_card, 18, 0);
	lv_obj_set_style_pad_gap(details_card, 14, 0);
	lv_obj_set_scrollable(details_card, false);
	lv_obj_set_event_bubble(details_card, true);
	lv_obj_set_flex_flow(details_card, LV_FLEX_FLOW_COLUMN);

	// Deliberately not player_sheet_attach_drag(): this page is opened from the
	// player, so dragging it sideways back into the player was one gesture
	// meaning two contradictory things. Removing details_screen from gui.c's
	// swipe list was not enough on its own -- the drag was attached here, to the
	// scroll container, and that is what kept working.
	switcher_attach_back_gesture(scroll);
	lv_obj_add_event_cb(details_screen, details_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}

// ---------------------------------------------------------------------------
// The player's ⋮ (Sonix UI 2.0, the handoff's "Лист плеера")
//
// A bottom sheet with the playing track's title and artist on top, and:
// stop after this track, stop after this album, the sleep timer (its own
// sheet: off, 15, 30, 60 minutes, at the end of the track), add to a
// playlist, show the album, show the artist -- the handoff's five -- and what
// the 1.2.5 menu had besides: shuffle (the repeat button cycles repeat only
// now), the lyrics (which a swipe on the artwork used to pull in), the queue,
// the details page and the music settings.
//
// What is offered follows what plays: a podcast's episode has its podcast in
// place of the album and no artist page; a station, AirPlay or a phone over
// DLNA play outside the queue and keep only the timer, the queue and the
// settings (a book has its own menu, player.c).
// ---------------------------------------------------------------------------

static void open_queue_cb(void *user) {
	(void)user;
	trackmenu_open_queue();
}

// The same page without going through the menu: the player's "Up next" chip,
// and its episodes button on a podcast -- a podcast's episodes are exactly the
// queue.
void trackmenu_open_queue(void) {
	player_sheet_close(false);
	switch_screen(queue_screen);
	switcher_set_player_return(queue_screen);
}

// "Stop after this track / this album": one-shot, cleared when it fires (see
// device_state_set_stop_after()); choosing the one already set clears it. The
// toast says it, with "Undo". "At the end of the track" in the sleep timer is
// the same switch.
static void stop_after_undo(void *user) {
	device_state_set_stop_after((device_state_stop_after_t)(intptr_t)user);
	topbar_refresh_playback(); // the moon
}

static void stop_after_pick(void *user) {
	device_state_stop_after_t want = (device_state_stop_after_t)(intptr_t)user;
	device_state_stop_after_t was = device_state_stop_after();
	if (was == want) {
		want = DEVICE_STATE_STOP_AFTER_NONE;
	}
	device_state_set_stop_after(want);
	topbar_refresh_playback();
	sx_toast_undo(tr(want == DEVICE_STATE_STOP_AFTER_TRACK	 ? "trackmenu_stop_after_track_set"
					 : want == DEVICE_STATE_STOP_AFTER_ALBUM ? "trackmenu_stop_after_album_set"
															 : "trackmenu_stop_after_cleared"),
				  stop_after_undo, (void *)(intptr_t)was);
}

static void sleep_cb(void *user) {
	(void)user;
	player_sleep_sheet_open();
}

static void shuffle_cb(void *user) {
	(void)user;
	player_shuffle_toggle();
}

static void lyrics_cb(void *user) {
	(void)user;
	player_lyrics_toggle();
}

static void open_details_cb(void *user) {
	(void)user;
	details_path[0] = '\0'; // the playing track
	player_sheet_close(false);
	switch_screen(details_screen);
	switcher_set_player_return(details_screen);
}

// "Add to playlist" for the track the player has loaded: the same picker the
// library lists open, on the playing file.
static void add_to_playlist_cb(void *user) {
	(void)user;
	device_state_t state;
	device_state_get(&state);
	if (!state.current_file[0]) {
		return;
	}
	player_sheet_close(false);
	playlistpage_add_track(state.current_file);
	switcher_set_player_return(playlistpage_screen);
}

// "Show album": the record the playing track came off. Backing out of it
// returns to the player, like everything else this menu opens.
static void open_album_cb(void *user) {
	(void)user;

	device_state_t state;
	device_state_get(&state);
	if (!state.current_file[0]) {
		return;
	}

	// Podcasts come first, because there is no album to show: the other episodes
	// are what is wanted. The feed id was kept by the sidecar file when the
	// episode was downloaded; the "album" title alone would not do, because two
	// podcasts can share a name and the catalogue is queried by id.
	long long feed_id = 0;
	if (podcastcache_feed_id(state.current_file, &feed_id)) {
		device_state_t now;
		device_state_get(&now);
		player_sheet_close(false);
		if (!podcastpage_open_feed(feed_id, now.metadata.album)) {
			gui_notify_popup("trackmenu_podcast_failed");
			return;
		}
		switcher_set_player_return(podcastcard_feed_screen);
		return;
	}

	// A streamed track is not in the local library, so its album is not looked
	// up in the database: it is reopened on the service it came from, using the
	// id the sidecar file kept when the track was downloaded. The name is not
	// enough, since different albums share a title.
	const stream_provider_t *service = stream_provider_for_path(state.current_file);
	char service_album[64];
	if (service && service->album_id && service->gui.open_album &&
		service->album_id(state.current_file, service_album, sizeof(service_album))) {
		char title[256];
		if (!library_track_album(state.current_file, title, sizeof(title)) || !title[0]) {
			device_state_t now;
			device_state_get(&now);
			snprintf(title, sizeof(title), "%s", now.metadata.album);
		}
		player_sheet_close(false);
		lv_obj_t *back_to = service->gui.open_album(service_album, title);
		if (!back_to) {
			gui_notify_popup(service->album_failed_key);
			return;
		}
		switcher_set_player_return(back_to);
		return;
	}

	char album[256];
	if (!library_track_album_value(state.current_file, album, sizeof(album)) || !album[0]) {
		gui_notify_popup("trackmenu_no_album");
		return;
	}

	player_sheet_close(false);
	medialist_open(album, LIBRARY_LIST_TRACKS, LIBRARY_FILTER_ALBUM, album);
	// Over a track list the album takes that list's place on the same page,
	// and the page brings the list back with the player over it itself.
	if (!medialist_step_back_to_player()) {
		switcher_set_player_return(medialist_tracks_screen);
	}
}

// "Show artist": the page of whoever the playing track is filed under -- the
// album artist, or the performer on a compilation. Only the library knows
// that, so a track it has not indexed (a stream, a file played from outside the
// card's music) gets a note instead of an empty page.
static bool open_artist(bool quiet) {
	device_state_t state;
	device_state_get(&state);
	if (!state.current_file[0]) {
		return false;
	}

	char name[256];
	if (!library_track_artist_page(state.current_file, name, sizeof(name), NULL)) {
		if (!quiet) {
			gui_notify_popup("trackmenu_no_artist");
		}
		return false;
	}

	player_sheet_close(false);
	medialist_open_track_artist(state.current_file);
	// The artist's records or tracks, whichever screen that is: backing out
	// of it returns to the player, like everything else this menu opens.
	if (!medialist_step_back_to_player()) {
		switcher_set_player_return(lv_screen_active());
	}
	return true;
}

static void open_artist_cb(void *user) {
	(void)user;
	open_artist(false);
}

bool trackmenu_show_artist(void) { return open_artist(true); }

static void open_settings_cb(void *user) {
	(void)user;
	player_sheet_close(false);
	switch_screen(musicsettings_screen);
	switcher_set_player_return(musicsettings_screen);
}

// Public: the details page for a specific file, from a library list's menu.
// NOT from the player, so backing out returns to the list, not the sheet.
void trackmenu_details_open(const char *path) {
	if (!path || !path[0]) {
		return;
	}
	snprintf(details_path, sizeof(details_path), "%s", path);
	switch_screen(details_screen);
	switcher_set_player_return(NULL);
}

void trackmenu_open(lv_obj_t *anchor) {
	(void)anchor; // the sheet comes up from the bottom, wherever the button is

	device_state_t state;
	device_state_get(&state);
	bool podcast = !state.live && state.current_file[0] && podcastcache_is_episode(state.current_file);
	char outside[256];
	bool ext = player_outside_title(outside, sizeof(outside));
	bool loaded = state.live || state.current_file[0];
	device_state_stop_after_t stop = device_state_stop_after();

	// The title and the artist, as the panel shows them.
	static char title[256];
	static char sub[256];
	const char *slash = strrchr(state.current_file, '/');
	snprintf(title, sizeof(title), "%.255s",
			 ext && outside[0] ? outside
			 : state.metadata.title[0] ? state.metadata.title
									   : (slash ? slash + 1 : state.current_file));
	snprintf(sub, sizeof(sub), "%s", state.live ? state.metadata.artist : metadata_shown_artist(&state.metadata));

	sx_sheet_item_t items[12];
	int n = 0;
	if (!ext && !state.live && loaded) {
		items[n++] = (sx_sheet_item_t){tr("np_stop_after_track"), &npicon_stop_after,
									   stop == DEVICE_STATE_STOP_AFTER_TRACK, false, stop_after_pick,
									   (void *)(intptr_t)DEVICE_STATE_STOP_AFTER_TRACK};
	}
	items[n++] = (sx_sheet_item_t){player_sleep_label(), &npicon_sleep, false, false, sleep_cb, NULL};
	if (!ext && loaded) {
		items[n++] = (sx_sheet_item_t){tr("add_to_playlist"), &npicon_add_playlist, false, false, add_to_playlist_cb,
									   NULL};
		items[n++] = (sx_sheet_item_t){tr(podcast ? "trackmenu_show_podcast" : "show_album"), &npicon_album, false,
									   false, open_album_cb, NULL};
		if (!podcast) {
			items[n++] = (sx_sheet_item_t){tr("show_artist"), &npicon_artist, false, false, open_artist_cb, NULL};
			items[n++] =
				(sx_sheet_item_t){tr("np_shuffle"), &npicon_shuffle, player_shuffle_on(), false, shuffle_cb, NULL};
		}
		if (player_lyrics_available()) {
			items[n++] =
				(sx_sheet_item_t){tr("np_lyrics"), &npicon_lyrics, player_lyrics_shown(), false, lyrics_cb, NULL};
		}
	}
	items[n++] = (sx_sheet_item_t){tr("trackmenu_queue"), &npicon_queue, false, false, open_queue_cb, NULL};
	if (loaded && !state.live) {
		items[n++] = (sx_sheet_item_t){tr("details"), &npicon_details, false, false, open_details_cb, NULL};
	}
	items[n++] = (sx_sheet_item_t){tr("settings"), &npicon_settings, false, false, open_settings_cb, NULL};

	sx_sheet_menu(loaded ? title : tr("trackmenu_queue"), loaded ? sub : NULL, items, n);
}

void trackmenu_init(gui_config_t *cfg) {
	build_queue_page(cfg);
	build_details_page(cfg);
}

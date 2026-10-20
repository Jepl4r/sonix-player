#include "src/gui/library/geniusui.h"
#include "src/gui/library/playlistpage.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/spinner.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/device/power.h"
#include "src/system/library/genius.h"
#include "src/system/library/library.h"
#include "src/system/library/playlists.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"

// What the finished mix is for. A mix started from a track's own menu is
// played; the one the Playlist page asks for is written to the card as a
// playlist and leaves the decoder alone.
typedef enum {
	GENIUS_MODE_PLAY,
	GENIUS_MODE_PLAYLIST,
} genius_mode_t;

typedef struct {
	char seed_path[512];
	// The language's word for the feature, read on the GUI thread: the worker
	// below writes the playlist and cannot reach the language table.
	char prefix[64];
	genius_mode_t mode;
	unsigned random;
	atomic_bool cancel;
	atomic_bool delivery_failed;
	geniusdb_snapshot_t *snapshot;
	genius_mix_t mix;
	genius_result_t result;
	// Where the mix landed, for the mode that writes one.
	char playlist_name[256];
	uint64_t playlist_session;
} genius_job_t;

// GUI thread owns the widgets and job pointer. The worker owns the result
// until gui_post succeeds, or delivery_failed hands it to the polling timer.
static struct {
	genius_job_t *job;
	lv_obj_t *overlay;
	lv_timer_t *timer;
} genius_ui;

static void genius_begin(const char *seed_path, genius_mode_t mode, const char *prefix);

static void genius_finish(void *user) {
	genius_job_t *job = user;
	lv_timer_delete(genius_ui.timer);
	genius_ui.timer = NULL;
	lv_obj_delete(genius_ui.overlay);
	genius_ui.overlay = NULL;
	genius_ui.job = NULL;
	power_hold_screen_on(false);
	if (atomic_load(&job->cancel) || (job->snapshot && !geniusdb_snapshot_current(job->snapshot)) || !gui_card_available() ||
		(job->mode == GENIUS_MODE_PLAYLIST && job->playlist_session && library_session() != job->playlist_session)) {
		job->result = GENIUS_CANCELLED;
	}
	// Cancelled while it was being written, or the card went away under it:
	// remove the unwanted playlist only while its session is still open.
	if (job->mode == GENIUS_MODE_PLAYLIST && job->result != GENIUS_OK && job->playlist_name[0]) {
		playlists_delete_session(job->playlist_name, job->playlist_session);
		job->playlist_name[0] = '\0';
	}
	if (job->result == GENIUS_OK && job->mode == GENIUS_MODE_PLAYLIST) {
		// The mix is a playlist on the card now. What is playing is none of
		// this mode's business: the user asked for a playlist, so they are
		// handed the playlist -- its own list of tracks, the same screen
		// tapping its row would open.
		printf("genius: mix saved as '%s'\n", job->playlist_name);
		toast_success("playlist_tracks_added");
		playlistpage_open_named(job->playlist_name);
	} else if (job->result == GENIUS_OK) {
		// The mix's sequence is loaded as an ordered queue, so next and prev
		// follow it and device_state persists it as it stands.
		//
		// Starting a mix from the track already playing is queue replacement,
		// not a restart: the file on the decoder is left alone.
		device_state_t state;
		device_state_get(&state);
		// ...and only when it is playing. The loaded track stays reported while
		// the player is stopped, and leaving the decoder alone then would
		// replace the queue and never start anything.
		if (job->mix.count > 0 && state.status == AUDIO_STATUS_PLAYING && state.current_file[0] &&
			strcmp(state.current_file, job->mix.paths[0]) == 0) {
			// The seed is already on the decoder. Replace the queue around it
			// rather than loading it again: a reload restarts the file and
			// re-applies a position, which is what makes the bar jump.
			playlist_load_paths_ordered(job->mix.paths, job->mix.count, 0);
			device_state_queue_changed();
		} else {
			device_state_play_list_ordered(job->mix.paths, job->mix.count, 0);
		}
		player_refresh_now_playing();
		switch_screen(player_screen);
	} else {
		const char *message;
		switch (job->result) {
		case GENIUS_NO_DATA:
			message = "smartmix_no_data";
			break;
		case GENIUS_TOO_FEW:
			message = "smartmix_too_few";
			break;
		case GENIUS_BUDGET:
			message = "smartmix_budget";
			break;
		case GENIUS_CANCELLED:
			message = "smartmix_cancelled";
			break;
		default:
			message = "smartmix_failed";
			break;
		}
		gui_notify_popup(message);
	}
	genius_mix_free(&job->mix);
	geniusdb_free_snapshot(job->snapshot);
	free(job);
}

static void genius_poll(lv_timer_t *timer) {
	(void)timer;
	if (genius_ui.job && atomic_load(&genius_ui.job->delivery_failed)) {
		genius_finish(genius_ui.job);
	}
}

static void *genius_build(void *user) {
	genius_job_t *job = user;
	thread_be_low_priority("smart mix");
	geniusdb_result_t read = geniusdb_load_snapshot(&job->snapshot, &job->cancel);
	if (read == GENIUSDB_OK) {
		job->result = genius_mix_build(job->snapshot, job->seed_path, job->random, &job->cancel, &job->mix);
		// Writing the playlist belongs to this thread: it reads tags off the
		// card for every entry. A mix that cannot be written is reported as a
		// failure, and the playlist it half made is taken back off the card
		// inside the save.
		if (job->result == GENIUS_OK && job->mode == GENIUS_MODE_PLAYLIST &&
			!playlists_create_from_seed(job->prefix, job->seed_path, (const char *const *)job->mix.paths,
										job->mix.count, job->playlist_name, sizeof(job->playlist_name), &job->playlist_session)) {
			job->result = GENIUS_INVALID;
		}
	} else if (read == GENIUSDB_UNAVAILABLE) {
		job->result = GENIUS_NO_DATA;
	} else if (read == GENIUSDB_LIMIT) {
		job->result = GENIUS_BUDGET;
	} else if (read == GENIUSDB_CANCELLED) {
		job->result = GENIUS_CANCELLED;
	} else {
		job->result = GENIUS_INVALID;
	}
	if (!gui_post(genius_finish, job)) {
		// The GUI timer owns delivery after a full post queue. The worker
		// touches no widget, and the progress card still has a completion.
		fprintf(stderr, "genius: GUI post queue is full\n");
		atomic_store(&job->delivery_failed, true);
	}
	return NULL;
}

static void genius_cancel(lv_event_t *e) {
	(void)e;
	if (genius_ui.job) {
		atomic_store(&genius_ui.job->cancel, true);
	}
}

static void genius_cancel_button(lv_obj_t *parent) {
	lv_obj_t *row = lv_btn_create(parent);
	lv_obj_set_size(row, lv_pct(100), 76);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, 12, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, 16, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_add_event_cb(row, genius_cancel, LV_EVENT_CLICKED, NULL);

	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text(label, tr("cancel"));
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_obj_center(label);
}

bool geniusui_available(void) { return gui_card_available() && geniusdb_available(); }

void geniusui_start(const char *seed_path) { genius_begin(seed_path, GENIUS_MODE_PLAY, NULL); }

void geniusui_create_playlist(const char *seed_path) {
	// The name is composed here, on the GUI thread: the worker that writes the
	// playlist has no way to ask the language table what the feature is called.
	genius_begin(seed_path, GENIUS_MODE_PLAYLIST, tr("smartmix_playlist_prefix"));
}

static void genius_begin(const char *seed_path, genius_mode_t mode, const char *prefix) {
	if (genius_ui.job) {
		return;
	}
	if (!geniusui_available() || !seed_path || !seed_path[0]) {
		fprintf(stderr, "genius: no data for the selected track\n");
		gui_notify_popup("smartmix_no_data");
		return;
	}
	genius_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		fprintf(stderr, "genius: cannot allocate background job\n");
		gui_notify_popup("smartmix_failed");
		return;
	}
	snprintf(job->seed_path, sizeof(job->seed_path), "%s", seed_path);
	snprintf(job->prefix, sizeof(job->prefix), "%s", prefix ? prefix : "");
	job->mode = mode;
	job->random = lv_rand(0, INT32_MAX);
	atomic_init(&job->cancel, false);
	atomic_init(&job->delivery_failed, false);
	genius_ui.job = job;
	power_hold_screen_on(true);
	genius_ui.overlay = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(genius_ui.overlay);
	lv_obj_set_size(genius_ui.overlay, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_color(genius_ui.overlay, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(genius_ui.overlay, LV_OPA_60, 0);
	lv_obj_set_clickable(genius_ui.overlay, true);
	lv_obj_set_scrollable(genius_ui.overlay, false);
	lv_obj_t *card = lv_obj_create(genius_ui.overlay);
	lv_obj_set_size(card, 380, LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_screen, 0);
	lv_obj_set_style_pad_all(card, 20, 0);
	lv_obj_set_style_pad_gap(card, 20, 0);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_center(card);
	lv_obj_t *spinner = spinner_create(card, &icon_loader_big);
	lv_obj_set_size(spinner, 48, 48);
	lv_obj_t *label = lv_label_create(card);
	lv_label_set_text(label, tr("smartmix_building"));
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_obj_add_style(label, &theme_style_text, 0);
	genius_cancel_button(card);
	genius_ui.timer = lv_timer_create(genius_poll, 100, NULL);
	pthread_t worker;
	int error = pthread_create(&worker, NULL, genius_build, job);
	if (error) {
		fprintf(stderr, "genius: cannot start worker: %s\n", strerror(error));
		job->result = GENIUS_INVALID;
		genius_finish(job);
		return;
	}
	pthread_detach(worker);
}

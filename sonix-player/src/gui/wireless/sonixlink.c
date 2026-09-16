#include "sonixlink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/system/audio/alsa-controls.h"
#include "src/system/audio/audio.h"
#include "src/system/playback/device_state.h"
#include "src/system/core/lang.h"
#include "src/system/library/library.h"
#include "src/system/playback/playlist.h"
#include "src/system/remote/sonixlink.h"
#include "src/system/net/wifi.h"

lv_obj_t *sonixlink_screen;

// The pump is not the page's timer: commands arrive while the page is closed,
// and that is when a remote control has to work at all.
#define PUMP_MS 200
#define PUMP_WAITING_MS 1000
#define PUMP_IDLE_MS 2000
#define PAGE_POLL_MS 500

#define SL_OFF_HELP "sonixlink_off_note"

// Where a scan asked for from the app starts, as the Library page uses it.
static const char *sd_root;

static lv_obj_t *toggle;
static lv_obj_t *glyph;
static lv_obj_t *status_label;
static lv_obj_t *wifi_row;
static lv_timer_t *page_timer;
static lv_timer_t *pump_timer;

// ---------------------------------------------------------------------------

static void hide(lv_obj_t *obj) { lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN); }
static void show(lv_obj_t *obj) { lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN); }

static bool switch_is_on(void) { return lv_obj_has_state(toggle, LV_STATE_CHECKED); }

static bool wifi_is_connected(void) {
	wifi_status_t status;
	wifi_get_status(&status);
	return status.state == WIFI_STATE_CONNECTED && status.ip[0] != '\0';
}

// ---------------------------------------------------------------------------
// the pump: the player's state out, the app's commands in
// ---------------------------------------------------------------------------

static int mode_to_wire(playback_mode_t mode) {
	switch (mode) {
	case PLAYBACK_MODE_REPEAT_ALL:
		return SONIXLINK_MODE_REPEAT_ALL;
	case PLAYBACK_MODE_REPEAT_ONE:
		return SONIXLINK_MODE_REPEAT_ONE;
	case PLAYBACK_MODE_SHUFFLE:
		return SONIXLINK_MODE_SHUFFLE;
	case PLAYBACK_MODE_SHUFFLE_REPEAT:
		return SONIXLINK_MODE_SHUFFLE_REPEAT;
	case PLAYBACK_MODE_NORMAL:
	default:
		return SONIXLINK_MODE_NORMAL;
	}
}

static void wire_to_mode(int wire) {
	switch (wire) {
	case SONIXLINK_MODE_REPEAT_ALL:
		playlist_set_mode(PLAYBACK_MODE_REPEAT_ALL);
		break;
	case SONIXLINK_MODE_REPEAT_ONE:
		playlist_set_mode(PLAYBACK_MODE_REPEAT_ONE);
		break;
	case SONIXLINK_MODE_SHUFFLE:
		playlist_set_mode(PLAYBACK_MODE_SHUFFLE);
		break;
	case SONIXLINK_MODE_SHUFFLE_REPEAT:
		playlist_set_mode(PLAYBACK_MODE_SHUFFLE_REPEAT);
		break;
	case SONIXLINK_MODE_NORMAL:
	default:
		playlist_set_mode(PLAYBACK_MODE_NORMAL);
		break;
	}
}

// Whether the track playing now is starred. The answer is remembered by path:
// this runs five times a second while a phone is watching, and asking the
// database every time would take the library lock for nothing.
static bool current_is_favourite(const char *path) {
	static char known_path[512];
	static bool known_answer;

	if (!path || !path[0]) {
		known_path[0] = '\0';
		return false;
	}
	if (strcmp(known_path, path) != 0) {
		snprintf(known_path, sizeof(known_path), "%s", path);
		known_answer = library_fav_contains(path);
	}
	return known_answer;
}

// After a star is set or cleared the remembered answer is wrong: forget it.
static void forget_favourite_memory(void) { current_is_favourite(NULL); }

// The window of the queue the phone is shown, refreshed only when it moves.
// Sending it on every pump would mean copying two hundred paths five times a
// second for a page that is usually not even open.
static void publish_queue_window(void) {
	static unsigned last_generation = (unsigned)-1;
	static int last_position = -2;
	static int last_count = -1;

	int count = playlist_count();
	int position = playlist_current_index();
	unsigned generation = playlist_revision();

	if (generation == last_generation && position == last_position && count == last_count) {
		return;
	}
	last_generation = generation;
	last_position = position;
	last_count = count;

	int first = position > 0 ? position - 20 : 0;
	if (first < 0) {
		first = 0;
	}
	if (first + SONIXLINK_QUEUE_WINDOW > count) {
		first = count - SONIXLINK_QUEUE_WINDOW;
		if (first < 0) {
			first = 0;
		}
	}

	// A hundred kilobytes of paths, borrowed for the length of this call. Held
	// as static storage it would sit in the resident set of every run of the
	// player, for a page most people never open.
	enum { PATH_MAX_HERE = 512 };
	char *paths = malloc((size_t)SONIXLINK_QUEUE_WINDOW * PATH_MAX_HERE);
	const char **pointers = malloc(sizeof(char *) * SONIXLINK_QUEUE_WINDOW);
	if (!paths || !pointers) {
		free(paths);
		free(pointers);
		return;
	}

	int written = 0;
	for (int i = 0; i < SONIXLINK_QUEUE_WINDOW && first + i < count; i++) {
		char *slot = paths + (size_t)written * PATH_MAX_HERE;
		if (!playlist_path_at(first + i, slot, PATH_MAX_HERE)) {
			slot[0] = '\0';
		}
		pointers[written] = slot;
		written++;
	}
	sonixlink_publish_queue(count, position, first, pointers, written);
	free(paths);
	free(pointers);
}

static void publish_state(void) {
	device_state_t d;
	device_state_get(&d);

	sonixlink_state_t s;
	memset(&s, 0, sizeof(s));

	switch (d.status) {
	case AUDIO_STATUS_PLAYING:
		s.play_state = 1;
		break;
	case AUDIO_STATUS_PAUSED:
		s.play_state = 2;
		break;
	default:
		s.play_state = 0;
		break;
	}

	s.play_mode = mode_to_wire(playlist_get_mode());
	s.volume = get_volume_percent();
	s.progress_secs = (unsigned)d.progress_current_secs;
	s.duration_secs = (unsigned)d.progress_total_secs;

	snprintf(s.title, sizeof(s.title), "%s", d.metadata.title);
	snprintf(s.artist, sizeof(s.artist), "%s", d.metadata.artist);
	snprintf(s.album, sizeof(s.album), "%s", d.metadata.album);
	snprintf(s.path, sizeof(s.path), "%s", d.current_file);

	s.sample_rate = (unsigned)(d.stream_sample_rate > 0 ? d.stream_sample_rate : 0);
	s.bits = (uint8_t)audio_get_stream_bits();
	s.bitrate = (unsigned)audio_get_stream_bitrate_kbps() * 1000u;
	s.lossless = !audio_stream_is_lossy();

	s.battery_percent = atoi(d.battery_percent);
	s.charging = d.battery_charging;

	// While a scan runs the app watches the counter climb; at rest what it
	// wants is how much the library holds.
	s.scanning = library_scan_running();
	s.scan_count = (unsigned)library_scan_found();
	s.track_count = (unsigned)(s.scanning ? 0 : library_track_count());
	s.favourite = current_is_favourite(s.path);

	s.queue_position = playlist_current_index();
	s.queue_count = playlist_count();

	// The app wears the player's own accent.
	lv_color_t accent = theme()->accent;
	snprintf(s.accent, sizeof(s.accent), "#%02x%02x%02x", accent.red, accent.green, accent.blue);

	sonixlink_publish(&s);
	publish_queue_window();
}

static void apply_command(const sonixlink_command_t *cmd) {
	switch (cmd->kind) {
	case SONIXLINK_CMD_PLAY:
		if (audio_get_status() != AUDIO_STATUS_PLAYING) {
			player_key_play_pause();
		}
		break;
	case SONIXLINK_CMD_PAUSE:
		if (audio_get_status() == AUDIO_STATUS_PLAYING) {
			player_key_play_pause();
		}
		break;
	case SONIXLINK_CMD_TOGGLE:
		player_key_play_pause();
		break;
	case SONIXLINK_CMD_STOP:
		device_state_stop();
		player_refresh_now_playing();
		break;
	case SONIXLINK_CMD_NEXT:
		player_key_next();
		break;
	case SONIXLINK_CMD_PREV:
		player_key_prev();
		break;
	case SONIXLINK_CMD_SEEK:
		device_state_seek((double)cmd->arg);
		break;
	case SONIXLINK_CMD_VOLUME:
		set_volume_percent(cmd->arg);
		gui_notify_volume(get_volume_percent());
		break;
	case SONIXLINK_CMD_MODE:
		wire_to_mode(cmd->arg);
		player_refresh_now_playing();
		break;
	case SONIXLINK_CMD_PLAY_PATH: {
		// The app names a track by the path it read out of the index. It is
		// checked before it is played: the app's copy of the index can outlive
		// a rescan, and a path that has gone is not something to start.
		char path[512];
		snprintf(path, sizeof(path), "%s", cmd->path);
		if (!path[0] || access(path, R_OK) != 0) {
			printf("sonixlink: the app asks for a track that is not there: %s\n", path);
			break;
		}

		// The queue becomes the list the track was picked from, not the whole
		// library: tapping a track inside a playlist of four hundred has to give
		// a queue of four hundred.
		library_list_t kind = LIBRARY_LIST_TRACKS;
		library_filter_t filter = LIBRARY_FILTER_NONE;
		library_order_t order = LIBRARY_ORDER_DEFAULT;
		const char *value = cmd->value[0] ? cmd->value : NULL;

		switch (cmd->list) {
		case SONIXLINK_LIST_ALBUM:
			filter = LIBRARY_FILTER_ALBUM;
			break;
		case SONIXLINK_LIST_ARTIST:
			filter = LIBRARY_FILTER_ARTIST;
			// An artist's tracks read as their records, one after another, the
			// same as on the player's own artist page.
			order = LIBRARY_ORDER_ALBUM;
			break;
		case SONIXLINK_LIST_ALBUM_ARTIST:
			filter = LIBRARY_FILTER_ALBUM_ARTIST;
			order = LIBRARY_ORDER_ALBUM;
			break;
		case SONIXLINK_LIST_GENRE:
			filter = LIBRARY_FILTER_GENRE;
			break;
		case SONIXLINK_LIST_FAVOURITES:
			kind = LIBRARY_LIST_FAVOURITES;
			value = NULL;
			break;
		case SONIXLINK_LIST_PLAYLIST:
			kind = LIBRARY_LIST_PLAYLIST;
			break;
		case SONIXLINK_LIST_QUEUE:
			// Already in the queue: move to it instead of building another.
			for (int i = 0, n = playlist_count(); i < n; i++) {
				char at[512];
				if (playlist_path_at(i, at, sizeof(at)) && strcmp(at, path) == 0) {
					if (device_state_play_queue_index(i)) {
						player_refresh_now_playing();
						goto play_path_done;
					}
					break;
				}
			}
			break;
		case SONIXLINK_LIST_ALL:
		default:
			break;
		}

		// A filtered list with nothing to filter on would quietly become the
		// whole library, which is the very thing this is here to avoid.
		if (filter != LIBRARY_FILTER_NONE && !value) {
			filter = LIBRARY_FILTER_NONE;
		}
		if (kind == LIBRARY_LIST_PLAYLIST && !value) {
			kind = LIBRARY_LIST_TRACKS;
		}

		library_index_t *ix = library_index_open(kind, filter, value, order, false);
		int at = ix ? library_index_find_path(ix, path) : -1;

		// The handle is given away by device_state_play_index() whether it
		// succeeds or not, so once that is called it must not be touched again.
		bool started = false;
		if (ix && at >= 0) {
			started = device_state_play_index(ix, at);
		} else if (ix) {
			library_index_close(ix);
		}
		if (!started) {
			device_state_play_file(path);
		}
		player_refresh_now_playing();
	play_path_done:
		break;
	}
	case SONIXLINK_CMD_SCAN:
		if (library_scan_running()) {
			printf("sonixlink: scan already running\n");
		} else if (!sd_root || !sd_root[0]) {
			printf("sonixlink: cannot scan, no card\n");
		} else if (!library_scan_start(sd_root)) {
			printf("sonixlink: scan refused by library_scan_start(%s)\n", sd_root);
		} else {
			printf("sonixlink: scan started on %s\n", sd_root);
		}
		break;
	case SONIXLINK_CMD_FAVOURITE: {
		if (!cmd->path[0]) {
			break;
		}
		bool starred = library_fav_contains(cmd->path);
		bool want = cmd->arg < 0 ? !starred : (cmd->arg != 0);
		if (want != starred) {
			// The name and artist are stored at star time so the favourites
			// list draws without a join. For the track playing now they are in
			// hand; for any other one the index has them.
			device_state_t d;
			device_state_get(&d);
			const char *name = "";
			const char *artist = "";
			if (strcmp(d.current_file, cmd->path) == 0) {
				name = d.metadata.title;
				artist = d.metadata.artist;
			}
			library_fav_toggle(cmd->path, name, artist);
			forget_favourite_memory();
			player_refresh_now_playing();
		}
		break;
	}
	case SONIXLINK_CMD_QUEUE_INDEX:
		if (device_state_play_queue_index(cmd->arg)) {
			player_refresh_now_playing();
		}
		break;
	case SONIXLINK_CMD_NONE:
	default:
		break;
	}
}

// The rate the current state calls for: fast enough that the app sees a
// progress bar move, slow while nothing is listening, slower still when the
// service is off -- which is nearly always.
static int pump_period(void) {
	if (!sonixlink_get_enabled()) {
		return PUMP_IDLE_MS;
	}
	return sonixlink_is_connected() ? PUMP_MS : PUMP_WAITING_MS;
}

static void pump_cb(lv_timer_t *timer) {
	lv_timer_set_period(timer, pump_period());
	if (!sonixlink_get_enabled()) {
		return;
	}

	sonixlink_command_t cmd;
	while (sonixlink_take_command(&cmd)) {
		apply_command(&cmd);
	}
	publish_state();
}

static void pump_pace(void) {
	if (pump_timer) {
		lv_timer_set_period(pump_timer, pump_period());
	}
}

// ---------------------------------------------------------------------------
// the page
// ---------------------------------------------------------------------------

// Two states and nothing else, the way the DAC page reads: the picture says
// what this is, the line under it says whether a phone is there.
static void refresh(void) {
	if (!wifi_is_connected()) {
		lv_label_set_text(status_label, tr("enable_wi_fi_first"));
		lv_obj_set_style_text_color(status_label, theme()->text_primary, 0);
		lv_obj_add_state(toggle, LV_STATE_DISABLED);
		show(wifi_row);
		return;
	}
	lv_obj_remove_local_style_prop(status_label, LV_STYLE_TEXT_COLOR, 0);
	lv_obj_remove_state(toggle, LV_STATE_DISABLED);
	hide(wifi_row);

	if (!switch_is_on()) {
		lv_label_set_text(status_label, tr(SL_OFF_HELP));
		return;
	}

	lv_label_set_text(status_label, sonixlink_is_connected() ? tr("connected") : tr("sonixlink_waiting"));
}

static void page_poll_cb(lv_timer_t *timer) {
	(void)timer;

	refresh();

	if (!switch_is_on()) {
		return;
	}

	// The network went away under a running service: there is nothing left to
	// be found on.
	if (!wifi_is_connected()) {
		lv_obj_remove_state(toggle, LV_STATE_CHECKED);
		sonixlink_set_enabled(false);
		pump_pace();
		refresh();
	}
}

static void toggle_changed_cb(lv_event_t *e) {
	(void)e;

	bool on = switch_is_on();

	if (on && !wifi_is_connected()) {
		lv_obj_remove_state(toggle, LV_STATE_CHECKED);
		gui_notify_popup("enable_wi_fi_first");
		return;
	}

	sonixlink_set_enabled(on);
	if (on) {
		// The app asks for the state the moment it connects, so the service
		// must not be holding a stale picture when it does.
		publish_state();
	}
	pump_pace();
	refresh();
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;

	if (sonixlink_get_enabled()) {
		lv_obj_add_state(toggle, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(toggle, LV_STATE_CHECKED);
	}

	refresh();
	lv_timer_resume(page_timer);
	lv_timer_ready(page_timer);
}

// Like AirPlay and DLNA, leaving the page does not switch the service off: the
// switch owns that state, the page is only where it lives.
static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	lv_timer_pause(page_timer);
}

static void wifi_row_cb(lv_event_t *e) {
	(void)e;
	switch_screen(wifisettings_screen);
}

// ---------------------------------------------------------------------------

void sonixlink_page_init(gui_config_t *cfg) {
	sd_root = cfg->sd_root_path;

	lv_obj_t *container = settingsrow_page(sonixlink_screen, cfg, "sonixlink");

	settingsrow_toggle(container, "sonixlink", &toggle, toggle_changed_cb);

	glyph = lv_image_create(container);
	lv_image_set_src(glyph, &icon_sonixlink_page);
	lv_obj_set_style_margin_top(glyph, 40, 0);

	status_label = lv_label_create(container);
	lv_obj_set_width(status_label, lv_pct(100));
	lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
	lv_obj_add_style(status_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(status_label, &font_ui_22, 0);
	lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_pad_hor(status_label, 4, 0);
	lv_obj_set_style_margin_top(status_label, 20, 0);

	// The same row as the other network pages: the notice says what is
	// missing, this leads to where it is fixed.
	wifi_row = settingsrow_add(container, "wi_fi_settings", NULL, wifi_row_cb, NULL);
	lv_obj_set_style_margin_top(wifi_row, 16, 0);
	hide(wifi_row);

	page_timer = lv_timer_create(page_poll_cb, PAGE_POLL_MS, NULL);
	lv_timer_pause(page_timer);

	// This one never stops: it is the bridge between the network thread and
	// playback, and the app pressing next while another page is open still has
	// to be served. It only slows down while SonixLink is off.
	pump_timer = lv_timer_create(pump_cb, PUMP_IDLE_MS, NULL);
	pump_pace();

	lv_obj_add_event_cb(sonixlink_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(sonixlink_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

	refresh();
}

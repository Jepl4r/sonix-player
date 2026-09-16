#include "btplayer.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "src/gui/shell/gui.h"
#include "src/system/audio/audio.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/bluetooth/dbuslite.h"
#include "src/system/playback/device_state.h"

#define PLAYER_PATH "/org/mpris/MediaPlayer2"
#define PLAYER_IFACE "org.mpris.MediaPlayer2.Player"
#define PROPS_IFACE "org.freedesktop.DBus.Properties"
#define BLUEZ_NAME "org.bluez"
#define BLUEZ_ADAPTER "/org/bluez/hci0"
#define BLUEZ_MEDIA "org.bluez.Media1"

// How often the worker looks at what the player is doing. Its own status, not
// the Bluetooth transport's: the player already knows, and asking it is one
// read under a mutex. Half a second is well inside what a pair of headphones
// notices and costs nothing.
#define POLL_MS 500

// How long to wait before trying the whole thing again after a failure. Long,
// because a failure here is almost always "bluetoothd is not up yet" and
// hammering it helps nobody.
#define RETRY_MS 3000

static pthread_t worker;
static bool worker_started;
static volatile bool stopping;
static volatile bool registered;

static dbus_conn_t *conn;

// What was last told to bluez, so PropertiesChanged is emitted on a change and
// not on a tick.
static char sent_status[16];
static char sent_track[512];

bool btplayer_registered(void) { return registered; }

// The two roads one AVRCP command can take here, told apart by nothing but how
// close together they arrive.
#define DUPLICATE_WINDOW_MS 150

static pthread_mutex_t command_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t last_command_ms;

static uint32_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000));
}

bool btplayer_command_is_duplicate(void) {
	uint32_t now = now_ms();
	pthread_mutex_lock(&command_lock);
	// A signed difference, so the millisecond counter's wrap every forty-nine
	// days does not read as "a very long time ago".
	bool duplicate = last_command_ms != 0 && (int32_t)(now - last_command_ms) < DUPLICATE_WINDOW_MS;
	last_command_ms = now;
	pthread_mutex_unlock(&command_lock);
	return duplicate;
}

// ---------------------------------------------------------------------------
// What the player is doing, in the words MPRIS uses
// ---------------------------------------------------------------------------

static const char *status_now(void) {
	switch (audio_get_status()) {
	case AUDIO_STATUS_PLAYING:
		return "Playing";
	case AUDIO_STATUS_PAUSED:
		return "Paused";
	default:
		return "Stopped";
	}
}

// The title, artist, album and length of what is loaded. A live stream has no
// file and no length; its station name is the title, which is what a pair of
// headphones should show.
typedef struct {
	char title[256];
	char artist[256];
	char album[256];
	int64_t length_us;
	char id[512]; // what the track is, for noticing a change
} track_t;

static void track_now(track_t *t) {
	memset(t, 0, sizeof(*t));

	device_state_t state;
	device_state_get(&state);

	snprintf(t->title, sizeof(t->title), "%s", state.metadata.title);
	snprintf(t->artist, sizeof(t->artist), "%s",
			 state.metadata.artist[0] ? state.metadata.artist : state.metadata.album_artist);
	snprintf(t->album, sizeof(t->album), "%s", state.metadata.album);

	if (!t->title[0] && state.current_file[0]) {
		const char *slash = strrchr(state.current_file, '/');
		const char *name = slash ? slash + 1 : state.current_file;
		// Truncated on purpose and without complaint: a file name longer than
		// what a pair of headphones can show is still better shown short.
		snprintf(t->title, sizeof(t->title), "%.*s", (int)sizeof(t->title) - 1, name);
	}

	// MPRIS counts in microseconds. A live stream has no length, and zero is
	// how MPRIS says so.
	t->length_us = state.live ? 0 : (int64_t)(state.progress_total_secs * 1000000.0);

	// Only a marker for "has the track changed", so a cut one is still a
	// different string when the track is different.
	const char *source = state.live ? state.metadata.title : state.current_file;
	snprintf(t->id, sizeof(t->id), "%.*s", (int)sizeof(t->id) - 1, source);
}

// The Metadata dictionary. mpris:trackid is an object path and not a string --
// bluez reads it as one, and a string there is a message it rejects.
static void w_dict_metadata(dbus_writer_t *w, const track_t *t) {
	// The entry itself: the key, then a variant holding the dictionary.
	dbus_w_dict_variant_begin(w, "Metadata", "a{sv}");
	dbus_array_t meta;
	dbus_w_array_begin(w, "{sv}", &meta);
	dbus_w_dict_path(w, "mpris:trackid", PLAYER_PATH "/track");
	if (t->length_us > 0) {
		dbus_w_dict_i64(w, "mpris:length", t->length_us);
	}
	if (t->title[0]) {
		dbus_w_dict_string(w, "xesam:title", t->title);
	}
	if (t->artist[0]) {
		const char *one[1] = {t->artist};
		dbus_w_dict_strings(w, "xesam:artist", one, 1);
	}
	if (t->album[0]) {
		dbus_w_dict_string(w, "xesam:album", t->album);
	}
	dbus_w_array_end(w, &meta);
}

// Every property of the player, for Properties.GetAll and for the dictionary
// RegisterPlayer is given.
static void write_all_properties(dbus_writer_t *w) {
	track_t t;
	track_now(&t);

	dbus_array_t a;
	dbus_w_array_begin(w, "{sv}", &a);
	dbus_w_dict_string(w, "PlaybackStatus", status_now());
	dbus_w_dict_string(w, "LoopStatus", "None");
	dbus_w_dict_bool(w, "Shuffle", false);
	dbus_w_dict_bool(w, "CanGoNext", true);
	dbus_w_dict_bool(w, "CanGoPrevious", true);
	dbus_w_dict_bool(w, "CanPlay", true);
	dbus_w_dict_bool(w, "CanPause", true);
	// Seeking is not offered: this player has one, but wiring AVRCP's position
	// to it is a separate job and claiming it would invite commands there is
	// nothing to answer with.
	dbus_w_dict_bool(w, "CanSeek", false);
	dbus_w_dict_bool(w, "CanControl", true);
	w_dict_metadata(w, &t);
	dbus_w_array_end(w, &a);
}

// ---------------------------------------------------------------------------
// Serving the object
// ---------------------------------------------------------------------------

// The transport commands, sent the same way the /dev/input reader sends them:
// onto the GUI thread, where the player's own controls live. Both roads end
// here, so a command cannot mean one thing through one and another through the
// other.
static void run_command(const char *member) {
	if (btplayer_command_is_duplicate()) {
		printf("btplayer: %s also arrived from the input node; leaving it to that one\n", member);
		return;
	}

	audio_status_t status = audio_get_status();

	if (strcmp(member, "Next") == 0) {
		gui_notify_key(GUI_KEY_NEXT);
	} else if (strcmp(member, "Previous") == 0) {
		gui_notify_key(GUI_KEY_PREV);
	} else if (strcmp(member, "PlayPause") == 0) {
		gui_notify_key(GUI_KEY_PLAY_PAUSE);
	} else if (strcmp(member, "Play") == 0) {
		if (status != AUDIO_STATUS_PLAYING) {
			gui_notify_key(GUI_KEY_PLAY_PAUSE);
		}
	} else if (strcmp(member, "Pause") == 0 || strcmp(member, "Stop") == 0) {
		if (status == AUDIO_STATUS_PLAYING) {
			gui_notify_key(GUI_KEY_PLAY_PAUSE);
		}
	}
	printf("btplayer: %s (was %s)\n", member,
		   status == AUDIO_STATUS_PLAYING ? "playing" : (status == AUDIO_STATUS_PAUSED ? "paused" : "stopped"));
}

static void on_call(dbus_conn_t *c, const dbus_msg_t *m, void *user) {
	(void)user;

	if (strcmp(m->interface, PROPS_IFACE) == 0) {
		if (strcmp(m->member, "GetAll") == 0) {
			dbus_writer_t *w = dbus_reply_begin(c, m, "a{sv}");
			write_all_properties(w);
			dbus_reply_send(c);
			return;
		}
		if (strcmp(m->member, "Get") == 0) {
			dbus_reader_t r;
			dbus_reader_init(&r, m);
			char iface[128], prop[128];
			dbus_r_string(&r, iface, sizeof(iface));
			dbus_r_string(&r, prop, sizeof(prop));

			dbus_writer_t *w = dbus_reply_begin(c, m, "v");
			if (strcmp(prop, "PlaybackStatus") == 0) {
				dbus_w_variant_string(w, status_now());
			} else if (strcmp(prop, "LoopStatus") == 0) {
				dbus_w_variant_string(w, "None");
			} else if (strcmp(prop, "CanSeek") == 0 || strcmp(prop, "Shuffle") == 0) {
				dbus_w_variant_bool(w, false);
			} else {
				// CanPlay, CanPause, CanGoNext, CanGoPrevious, CanControl: all
				// true, and anything else this player does not know about is
				// better answered true than left to time out.
				dbus_w_variant_bool(w, true);
			}
			dbus_reply_send(c);
			return;
		}
		if (strcmp(m->member, "Set") == 0) {
			dbus_error(c, m, "org.freedesktop.DBus.Error.PropertyReadOnly",
					   "this player does not accept writes");
			return;
		}
	}

	if (strcmp(m->interface, PLAYER_IFACE) == 0) {
		run_command(m->member);
		dbus_reply_begin(c, m, "");
		dbus_reply_send(c);
		return;
	}

	dbus_error(c, m, "org.freedesktop.DBus.Error.UnknownMethod", m->member);
}

// ---------------------------------------------------------------------------
// Telling bluez when something changes
// ---------------------------------------------------------------------------

static void emit_status(const char *status) {
	dbus_writer_t *w = dbus_signal_begin(conn, PLAYER_PATH, PROPS_IFACE, "PropertiesChanged", "sa{sv}as");
	dbus_w_string(w, PLAYER_IFACE);
	dbus_array_t a;
	dbus_w_array_begin(w, "{sv}", &a);
	dbus_w_dict_string(w, "PlaybackStatus", status);
	dbus_w_array_end(w, &a);
	dbus_array_t none;
	dbus_w_array_begin(w, "s", &none);
	dbus_w_array_end(w, &none);
	dbus_signal_send(conn);
}

static void emit_metadata(const track_t *t) {
	dbus_writer_t *w = dbus_signal_begin(conn, PLAYER_PATH, PROPS_IFACE, "PropertiesChanged", "sa{sv}as");
	dbus_w_string(w, PLAYER_IFACE);
	dbus_array_t a;
	dbus_w_array_begin(w, "{sv}", &a);
	w_dict_metadata(w, t);
	dbus_w_array_end(w, &a);
	dbus_array_t none;
	dbus_w_array_begin(w, "s", &none);
	dbus_w_array_end(w, &none);
	dbus_signal_send(conn);
}

// ---------------------------------------------------------------------------
// Coming up, and staying up
// ---------------------------------------------------------------------------

static bool register_with_bluez(void) {
	dbus_writer_t *w =
		dbus_call_begin(conn, BLUEZ_NAME, BLUEZ_ADAPTER, BLUEZ_MEDIA, "RegisterPlayer", "oa{sv}");
	dbus_w_path(w, PLAYER_PATH);
	write_all_properties(w);

	char err[DBUS_NAME_MAX];
	if (dbus_call_send(conn, 5000, err, sizeof(err))) {
		return true;
	}

	// Said out loud rather than swallowed: the shape of the dictionary bluez
	// will accept is the one thing here that cannot be checked away from the
	// device, so the error name is the evidence for the next attempt.
	fprintf(stderr, "btplayer: RegisterPlayer refused (%s)\n", err[0] ? err : "no reply");
	return false;
}

static void teardown(void) {
	if (!conn) {
		return;
	}
	if (registered && dbus_alive(conn)) {
		dbus_writer_t *w = dbus_call_begin(conn, BLUEZ_NAME, BLUEZ_ADAPTER, BLUEZ_MEDIA, "UnregisterPlayer", "o");
		dbus_w_path(w, PLAYER_PATH);
		dbus_call_send(conn, 2000, NULL, 0);
	}
	dbus_disconnect(conn);
	conn = NULL;
	registered = false;
	sent_status[0] = '\0';
	sent_track[0] = '\0';
}

static void *worker_func(void *unused) {
	(void)unused;

	while (!stopping) {
		if (!bluetooth_get_enabled()) {
			// With the radio off there is no bluetoothd to register with, and
			// holding a connection open buys nothing.
			if (conn) {
				teardown();
			}
			usleep(RETRY_MS * 1000);
			continue;
		}

		if (conn && !dbus_alive(conn)) {
			fprintf(stderr, "btplayer: the bus dropped; reconnecting\n");
			teardown();
		}

		if (!conn) {
			conn = dbus_connect_system(NULL, on_call, NULL);
			if (!conn) {
				usleep(RETRY_MS * 1000);
				continue;
			}
			registered = register_with_bluez();
			if (!registered) {
				teardown();
				usleep(RETRY_MS * 1000);
				continue;
			}
			printf("btplayer: registered with bluez on %s\n", BLUEZ_ADAPTER);
		}

		// What changed since last time, and only that.
		const char *status = status_now();
		if (strcmp(status, sent_status) != 0) {
			snprintf(sent_status, sizeof(sent_status), "%s", status);
			emit_status(status);
			printf("btplayer: status -> %s\n", status);
		}

		track_t t;
		track_now(&t);
		if (strcmp(t.id, sent_track) != 0) {
			snprintf(sent_track, sizeof(sent_track), "%s", t.id);
			emit_metadata(&t);
		}

		usleep(POLL_MS * 1000);
	}

	teardown();
	return NULL;
}

void btplayer_init(void) {
	if (worker_started) {
		return;
	}
	stopping = false;
	if (pthread_create(&worker, NULL, worker_func, NULL) != 0) {
		fprintf(stderr, "btplayer: no thread; the player will not be registered\n");
		return;
	}
	worker_started = true;
	printf("thread: btplayer runs in the background\n");
}

void btplayer_stop(void) {
	if (!worker_started) {
		return;
	}
	stopping = true;
	pthread_join(worker, NULL);
	worker_started = false;
}

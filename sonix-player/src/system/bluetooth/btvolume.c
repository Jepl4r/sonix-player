#include "btvolume.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "src/gui/shell/gui.h"
#include "src/system/audio/alsa-controls.h"
#include "src/system/bluetooth/dbuslite.h"
#include "src/system/core/utils.h"

#define BLUEALSA_SERVICE "org.bluealsa"
#define PCM_INTERFACE "org.bluealsa.PCM1"
#define PROPERTIES_INTERFACE "org.freedesktop.DBus.Properties"

#define CALL_TIMEOUT_MS 3000
#define RETRY_MS 3000	 // between attempts to reach bluealsa on the bus
#define TICK_MS 250		 // while waiting for the output to move over
#define IDLE_WAIT_MS 600 // with nothing to bridge

#define BT_VOLUME_MAX 127
#define PATH_MAX_LEN 160

// How long to wait, after the stream appears, before taking the level it
// declares at face value.
//
// A freshly created A2DP transport starts at maximum: bluealsa sets
// volume_init_level, which is 0 dB, that is 127 of 127. The headphones' own
// level arrives just afterwards as an AVRCP volume-changed, which becomes a
// PropertiesChanged. Reading once, immediately, would read bluealsa's starting
// value and not the headphones'.
#define ADOPT_SETTLE_MS 2500

// Where the worker is with the stream it has been given.
typedef enum {
	BTV_IDLE,  // no stream
	BTV_SETUP, // SoftVolume still to be written
	BTV_ADOPT, // synchronised: the headphones' own level still to be read
	BTV_READY
} btv_state_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static pthread_t worker;
static bool worker_running;
static bool stopping;

static char want_path[PATH_MAX_LEN]; // the stream to bridge, "" for none
static char have_path[PATH_MAX_LEN]; // the one the worker has set up
static bool want_sync = true;
static bool have_sync = true;
static bool bus_reset;			// bluealsa restarted, or moved a property underneath
static int pending_local = -1;	// a percent the player wants the stream to be at
static int pending_remote = -1; // a 0..127 that arrived over the bus
static int last_bt = -1;		// what the stream holds, as far as this side knows
static bool path_live;			// a connection and a stream: writes can go over the bus
static bool bus_broken;			// the stream is there but does not answer about its volume

// The stream whose level has already been adopted. Adoption happens once per
// connection, not once per change on the bus: bluealsa resets SoftVolume its
// own way whenever the PCM is closed and reopened -- that is, at every track
// change without gapless -- and re-reading the level on that would raise the
// volume slider on screen.
static char adopted_path[PATH_MAX_LEN];

static uint32_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000));
}

static int clamp_int(int v, int low, int high) { return v < low ? low : (v > high ? high : v); }

static int local_to_bt(int percent) { return (clamp_int(percent, 0, 100) * BT_VOLUME_MAX + 50) / 100; }

static int bt_to_local(int volume) { return (clamp_int(volume, 0, BT_VOLUME_MAX) * 100 + 63) / BT_VOLUME_MAX; }

// ---------------------------------------------------------------------------
// The calls
// ---------------------------------------------------------------------------

static bool set_property_bool(dbus_conn_t *c, const char *path, const char *name, bool value) {
	dbus_writer_t *w = dbus_call_begin(c, BLUEALSA_SERVICE, path, PROPERTIES_INTERFACE, "Set", "ssv");
	dbus_w_string(w, PCM_INTERFACE);
	dbus_w_string(w, name);
	dbus_w_variant_bool(w, value);

	char err[DBUS_NAME_MAX];
	if (!dbus_call_send(c, CALL_TIMEOUT_MS, err, sizeof(err))) {
		fprintf(stderr, "btvolume: %s not written (%s)\n", name, err[0] ? err : "no reply");
		return false;
	}
	return true;
}

// Both channels at the same level and neither muted. Balance is a separate
// control and stays out of this one.
static bool set_stream_volume(dbus_conn_t *c, const char *path, int volume) {
	uint16_t both = (uint16_t)((clamp_int(volume, 0, BT_VOLUME_MAX) << 8) | clamp_int(volume, 0, BT_VOLUME_MAX));

	dbus_writer_t *w = dbus_call_begin(c, BLUEALSA_SERVICE, path, PROPERTIES_INTERFACE, "Set", "ssv");
	dbus_w_string(w, PCM_INTERFACE);
	dbus_w_string(w, "Volume");
	dbus_w_variant_u16(w, both);

	char err[DBUS_NAME_MAX];
	if (!dbus_call_send(c, CALL_TIMEOUT_MS, err, sizeof(err))) {
		fprintf(stderr, "btvolume: volume not written (%s)\n", err[0] ? err : "no reply");
		return false;
	}
	return true;
}

// The left channel's level, which is the one this side tracks: the two are
// always written together.
static bool get_stream_volume(dbus_conn_t *c, const char *path, int *out) {
	dbus_writer_t *w = dbus_call_begin(c, BLUEALSA_SERVICE, path, PROPERTIES_INTERFACE, "Get", "ss");
	dbus_w_string(w, PCM_INTERFACE);
	dbus_w_string(w, "Volume");

	char err[DBUS_NAME_MAX];
	if (!dbus_call_send(c, CALL_TIMEOUT_MS, err, sizeof(err))) {
		fprintf(stderr, "btvolume: volume not read (%s)\n", err[0] ? err : "no reply");
		return false;
	}

	dbus_reader_t r;
	dbus_reply_reader(c, &r);
	char sig[16];
	uint16_t both = 0;
	if (!dbus_r_signature(&r, sig, sizeof(sig)) || sig[0] != 'q' || !dbus_r_u16(&r, &both)) {
		fprintf(stderr, "btvolume: the reply was not a volume (signature '%s')\n", sig);
		return false;
	}
	*out = (both >> 8) & 0x7f; // the top bit of the byte is the mute switch
	return true;
}

// ---------------------------------------------------------------------------
// What arrives on its own
// ---------------------------------------------------------------------------

static void handle_properties_changed(const dbus_msg_t *m) {
	pthread_mutex_lock(&lock);
	bool ours = have_path[0] && strcmp(m->path, have_path) == 0;
	bool soft_wanted = !have_sync;
	pthread_mutex_unlock(&lock);
	if (!ours) {
		return;
	}

	dbus_reader_t r;
	dbus_reader_init(&r, m);
	char interface[80];
	if (!dbus_r_string(&r, interface, sizeof(interface)) || strcmp(interface, PCM_INTERFACE) != 0) {
		return;
	}

	dbus_array_iter_t it;
	if (!dbus_r_array_begin(&r, "{sv}", &it)) {
		return;
	}
	while (dbus_r_array_more(&r, &it)) {
		char key[64];
		char sig[16];
		if (!dbus_r_string(&r, key, sizeof(key)) || !dbus_r_signature(&r, sig, sizeof(sig))) {
			return;
		}

		if (strcmp(key, "Volume") == 0 && sig[0] == 'q') {
			uint16_t both = 0;
			if (!dbus_r_u16(&r, &both)) {
				return;
			}
			int left = (both >> 8) & 0x7f;
			pthread_mutex_lock(&lock);
			// A write from this side comes back as a change like any other.
			// Recognising it by the value is what keeps the two sides from
			// pushing each other around for ever.
			if (left != last_bt) {
				last_bt = left;
				pending_remote = left;
				pthread_cond_signal(&wake);
			}
			pthread_mutex_unlock(&lock);
		} else if (strcmp(key, "SoftVolume") == 0 && sig[0] == 'b') {
			bool soft = false;
			if (!dbus_r_bool(&r, &soft)) {
				return;
			}
			if (soft != soft_wanted) {
				pthread_mutex_lock(&lock);
				bus_reset = true; // something else moved it: write it again
				pthread_cond_signal(&wake);
				pthread_mutex_unlock(&lock);
			}
		} else if (!dbus_r_skip(&r, sig)) {
			return;
		}
	}
}

static void on_signal(dbus_conn_t *c, const dbus_msg_t *m, void *user) {
	(void)c;
	(void)user;

	if (strcmp(m->member, "PropertiesChanged") == 0) {
		handle_properties_changed(m);
		return;
	}
	if (strcmp(m->member, "NameOwnerChanged") == 0) {
		// bluealsa has gone or come back, and with it every stream object it
		// exported. Whatever was written to the old one means nothing.
		pthread_mutex_lock(&lock);
		bus_reset = true;
		last_bt = -1;
		adopted_path[0] = '\0'; // a new transport: the level has to be taken again
		pthread_cond_signal(&wake);
		pthread_mutex_unlock(&lock);
	}
}

// ---------------------------------------------------------------------------
// The worker
// ---------------------------------------------------------------------------

static dbus_conn_t *open_bus(void) {
	dbus_conn_t *c = dbus_connect_system(NULL, NULL, NULL);
	if (!c) {
		return NULL;
	}
	dbus_set_signal_handler(c, on_signal, NULL);
	dbus_add_match(c, "type='signal',sender='" BLUEALSA_SERVICE "',interface='" PROPERTIES_INTERFACE
					  "',member='PropertiesChanged'");
	dbus_add_match(c, "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
					  "member='NameOwnerChanged',arg0='" BLUEALSA_SERVICE "'");
	printf("btvolume: connected to the system bus\n");
	return c;
}

// gui_notify_volume() raises the volume slider over the player: the right
// answer to a level that changes and a nuisance for one that does not. An
// adoption that lands on the number already shown is not a change.
static void apply_local(int percent) {
	if (percent == get_volume_percent()) {
		return;
	}
	set_volume_percent(percent);
	gui_notify_volume(percent);
}

static void *btvolume_worker(void *unused) {
	(void)unused;
	thread_be_background("btvolume");

	dbus_conn_t *conn = NULL;
	btv_state_t state = BTV_IDLE;
	uint32_t last_try = 0;

	// The adoption in progress: when to stop waiting, and the best level seen
	// so far.
	uint32_t adopt_deadline = 0;
	int adopt_seen = -1;

	for (;;) {
		char path[PATH_MAX_LEN];
		bool sync_on;
		int local, remote;
		bool reset;

		pthread_mutex_lock(&lock);
		if (stopping) {
			pthread_mutex_unlock(&lock);
			break;
		}
		snprintf(path, sizeof(path), "%s", want_path);
		sync_on = want_sync;
		local = pending_local;
		remote = pending_remote;
		reset = bus_reset;
		pending_local = -1;
		pending_remote = -1;
		bus_reset = false;
		// A different stream, or synchronisation just switched on, are the two
		// cases where the headphones' level has to be taken again from scratch.
		// A bus_reset on its own is not: there the stream is the same as before.
		if (strcmp(path, have_path) != 0 || sync_on != have_sync) {
			adopted_path[0] = '\0';
		}
		if (strcmp(path, have_path) != 0 || sync_on != have_sync || reset) {
			snprintf(have_path, sizeof(have_path), "%s", path);
			have_sync = sync_on;
			last_bt = -1;
			bus_broken = false;
			state = path[0] ? BTV_SETUP : BTV_IDLE;
			local = -1; // whatever was queued belonged to the old arrangement
			remote = -1;
		}
		bool broken = bus_broken;
		pthread_mutex_unlock(&lock);

		unsigned wait_ms = IDLE_WAIT_MS;

		if (conn && !dbus_alive(conn)) {
			dbus_disconnect(conn);
			conn = NULL;
			state = path[0] ? BTV_SETUP : BTV_IDLE;
		}

		if (!path[0]) {
			// Nothing connected: the socket and its reader thread are not worth
			// holding open for a device that may never come back.
			if (conn) {
				dbus_disconnect(conn);
				conn = NULL;
			}
			state = BTV_IDLE;
		} else if (!conn) {
			if ((uint32_t)(now_ms() - last_try) >= RETRY_MS || last_try == 0) {
				last_try = now_ms();
				conn = open_bus();
				if (conn) {
					state = BTV_SETUP;
				}
			}
			// The long wait is for a failed attempt, not a successful one: as soon
			// as the connection exists the setup has to happen at once. Sleeping
			// here means the headphones announce their level -- the AVRCP
			// volume-changed arrives right after the transport comes up -- while
			// have_path is still empty, so the notification is discarded and the
			// only value left on the stream is bluealsa's starting one, the
			// maximum.
			wait_ms = conn ? 0 : RETRY_MS;
		} else if (broken) {
			// The stream is on the bus but did not answer about its volume, so
			// this is not the interface it was taken for. Nothing more is tried
			// here: bluetooth.c falls back to bluealsa-cli, and a reconnection
			// or a restart of bluealsa gives this another go.
			wait_ms = RETRY_MS;
		} else {
			if (state == BTV_SETUP) {
				// Synchronised means bluealsa hands the level to the headphones
				// as an AVRCP absolute volume; separate means it attenuates the
				// stream here and they never hear about it.
				set_property_bool(conn, have_path, "SoftVolume", !sync_on);
				printf("btvolume: %s for %s\n", sync_on ? "native volume (synchronised)" : "software volume (separate)",
					   have_path);

				pthread_mutex_lock(&lock);
				bool already_adopted = strcmp(adopted_path, have_path) == 0;
				pthread_mutex_unlock(&lock);

				if (sync_on && !already_adopted) {
					state = BTV_ADOPT;
					adopt_deadline = 0;
					adopt_seen = -1;
				} else {
					// Nothing to adopt: either the volume is separate, or this
					// stream has already been adopted and what counts now is the
					// player's level, which has to be put back on the stream.
					state = BTV_READY;
					local = get_volume_percent();
				}
			}

			if (state == BTV_ADOPT) {
				// Not before the output has actually moved over: the level read
				// here is applied through the player's volume, and until the
				// swap has happened that level belongs to the jack's profile.
				if (!volume_profile_is_bluetooth()) {
					wait_ms = TICK_MS;
				} else if (adopt_deadline == 0) {
						// The first read serves two purposes: knowing what is on
						// the stream now, and telling this side's own writes from
						// the headphones'. It is not applied yet.
					int volume = 0;
					if (get_stream_volume(conn, have_path, &volume)) {
						pthread_mutex_lock(&lock);
						last_bt = volume;
						pthread_mutex_unlock(&lock);
						adopt_seen = volume;
						adopt_deadline = now_ms() + ADOPT_SETTLE_MS;
						wait_ms = TICK_MS;
					} else {
						pthread_mutex_lock(&lock);
						bus_broken = true;
						pthread_mutex_unlock(&lock);
						state = BTV_READY;
						local = -1;
					}
				} else if (local >= 0) {
					// The volume was turned by hand during the wait: the hand
					// beats the adoption.
					printf("btvolume: volume turned by hand; not adopting the headphones' level\n");
					state = BTV_READY;
				} else if (remote >= 0) {
					// The headphones have said their level: this is the value
					// the wait was for.
					adopt_seen = remote;
					printf("btvolume: the headphones are at %d/127; the player follows them\n", adopt_seen);
					apply_local(bt_to_local(adopt_seen));
					state = BTV_READY;
					local = -1; // the adopted level is the level; nothing to push back
				} else if ((int32_t)(now_ms() - adopt_deadline) >= 0) {
					// No notification in time. What the stream declares is not
					// the headphones' level: it is bluealsa's starting value,
					// which is the maximum. Taking it at face value would turn
					// the volume to full scale in the face of someone who has
					// just put the headphones on. The player's level wins
					// instead, and is written to the stream.
					printf("btvolume: the headphones did not say their level (the stream is at %d/127); "
						   "keeping the player's\n",
						   adopt_seen);
					state = BTV_READY;
					local = get_volume_percent();
				} else {
					wait_ms = TICK_MS;
				}

				if (state == BTV_READY) {
					pthread_mutex_lock(&lock);
					snprintf(adopted_path, sizeof(adopted_path), "%s", have_path);
					pthread_mutex_unlock(&lock);
				}
			}

			if (state == BTV_READY) {
				if (remote >= 0 && sync_on) {
					printf("btvolume: the headphones moved the volume -> %d/127\n", remote);
					apply_local(bt_to_local(remote));
					local = -1;
				}
				if (local >= 0) {
					int wanted = local_to_bt(local);
					pthread_mutex_lock(&lock);
					// A level already reading as this percent needs no
					// correction. The two scales do not map one to one -- 27 of
					// the 128 Bluetooth steps come back as a percent that
					// converts to a different step -- so without this the two
					// sides would nudge each other by one for ever.
					bool move = last_bt < 0 || (wanted != last_bt && bt_to_local(last_bt) != local);
					pthread_mutex_unlock(&lock);
					if (move) {
						// Written down before the call, not after: the change
						// comes back as a PropertiesChanged that can beat the
						// reply here, and a value the signal handler does not
						// recognise as ours is taken for the headphones'.
						pthread_mutex_lock(&lock);
						int previous = last_bt;
						last_bt = wanted;
						pthread_mutex_unlock(&lock);

						if (!set_stream_volume(conn, have_path, wanted)) {
							pthread_mutex_lock(&lock);
							if (last_bt == wanted) {
								last_bt = previous;
							}
							bus_broken = true;
							pthread_mutex_unlock(&lock);
						}
					}
				}
			}
		}

		pthread_mutex_lock(&lock);
		path_live = conn != NULL && have_path[0] && state != BTV_IDLE && !bus_broken;
		bool work = stopping || pending_local >= 0 || pending_remote >= 0 || bus_reset ||
					strcmp(want_path, have_path) != 0 || want_sync != have_sync;
		if (!work) {
			struct timespec deadline;
			deadline_in_ms(&deadline, wait_ms);
			pthread_cond_timedwait(&wake, &lock, &deadline);
		}
		pthread_mutex_unlock(&lock);
	}

	if (conn) {
		dbus_disconnect(conn);
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------

void btvolume_init(void) {
	if (worker_running) {
		return;
	}
	if (pthread_create(&worker, NULL, btvolume_worker, NULL) != 0) {
		fprintf(stderr, "btvolume: could not start the thread\n");
		return;
	}
	worker_running = true;
}

void btvolume_stop(void) {
	if (!worker_running) {
		return;
	}
	pthread_mutex_lock(&lock);
	stopping = true;
	pthread_cond_signal(&wake);
	pthread_mutex_unlock(&lock);
	pthread_join(worker, NULL);
	worker_running = false;
	stopping = false;
}

void btvolume_set_device(const char *mac) {
	char path[PATH_MAX_LEN] = "";
	if (mac && mac[0]) {
		char address[64];
		snprintf(address, sizeof(address), "%s", mac);
		for (char *p = address; *p; p++) {
			if (*p == ':' || *p == '-') {
				*p = '_';
			}
		}
		snprintf(path, sizeof(path), "/org/bluealsa/hci0/dev_%s/a2dpsrc/sink", address);
	}

	pthread_mutex_lock(&lock);
	if (strcmp(path, want_path) != 0) {
		snprintf(want_path, sizeof(want_path), "%s", path);
		pthread_cond_signal(&wake);
	}
	pthread_mutex_unlock(&lock);
}

void btvolume_set_sync(bool on) {
	pthread_mutex_lock(&lock);
	if (on != want_sync) {
		want_sync = on;
		pthread_cond_signal(&wake);
	}
	pthread_mutex_unlock(&lock);
}

void btvolume_notify_local(int percent) {
	pthread_mutex_lock(&lock);
	pending_local = clamp_int(percent, 0, 100);
	pthread_cond_signal(&wake);
	pthread_mutex_unlock(&lock);
}

bool btvolume_available(void) {
	pthread_mutex_lock(&lock);
	bool live = path_live;
	pthread_mutex_unlock(&lock);
	return live;
}

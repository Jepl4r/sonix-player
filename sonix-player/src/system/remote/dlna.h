#ifndef DLNA_H
#define DLNA_H

#include <stdbool.h>

// DLNA, done the way the original firmware does it, which here too is not what
// one would expect.
//
// The player does not speak UPnP. /usr/bin/dmrd does: a gmediarender rebuilt by
// HiBy that sits on the rootfs next to shairport, announces itself on the
// network as a renderer and receives the phone's commands. The player is only
// that process's audio output, and the two talk over two UNIX sockets, one per
// direction:
//
//   phone --UPnP--> dmrd
//                        |
//         "set_uri:<url>", "play@<pos>", "pause", "stop", "seek@<s>",
//         "set_meta:title:<...>", "get_position_info", "set_volume@<n>"
//                        |
//                        v
//                /data/dmr_streamer   <-- this process is the server
//                        |
//                   the player downloads the url and plays it
//                        |
//                /data/dmr_control    <-- dmrd is the server
//                        ^
//         "play", "pause", "stop", "next", "previous",
//         "set_position@<pos>@<dur>", "set_duration@<dur>", "play_finish",
//         "exit"
//
// dmrd does not send audio: it sends a URL. The phone serves the track over its
// own HTTP, and the player downloads and decodes it, which is why part of this
// module closely resembles the Qobuz cache. The file lands on the card and
// playback starts while it arrives (decode/growfile.c), so the right decoder,
// the progress bar, the equalizer and seeking all work unchanged.
//
// dmrd is not started here but by sys_server, with "DLNA:TURN_ON:<name>": it
// runs `ifconfig lo up` and `dmrd -f "<name>" &`, then watches
// /data/dlna_result.txt until "dlna_success_init" appears. The name is the one
// the device gives for itself, from /usr/resource/bt_name -- the same one
// Bluetooth and AirPlay use.
//
// Two consequences worth stating. The /data/dmr_streamer socket must already be
// listening when dmrd starts, because dmrd is the one connecting. And the
// player owns the output the whole time: what arrives over DLNA is a track like
// any other, so local playback stops -- not from a device conflict, but because
// one output cannot carry two things.
//
// As with AirPlay, the switch is the switch and the page is only where it
// lives: once on it stays on after leaving the page, the only sensible reading
// of a receiver, whose job is to be found by someone else.

#define DLNA_TEXT_MAX 256
#define DLNA_URL_MAX 1024
#define DLNA_PATH_MAX 512

// True when the firmware ships dmrd and the daemon that starts it.
bool dlna_available(void);

// Starts listening on the socket and asks sys_server to start dmrd (or stop
// it). Returns immediately: the work runs on a thread, because sys_server runs
// its scripts inside a blocking system() call.
void dlna_set_enabled(bool on);
bool dlna_get_enabled(void);

// True once dmrd is actually up and the socket is being served. There is a gap
// between the switch and this: sys_server queues the request and waits for dmrd
// to report that it started.
bool dlna_running(void);

// The name the device appears under in the phone's list.
const char *dlna_name(void);

typedef struct {
	bool connected;	   // a controller has sent something
	bool fetching;	   // the track is downloading
	char title[DLNA_TEXT_MAX];
	char artist[DLNA_TEXT_MAX];
	char album[DLNA_TEXT_MAX];
	char error[DLNA_TEXT_MAX]; // empty when all is well
} dlna_state_t;

// The current state. Safe to call from the UI thread: copied under the lock.
void dlna_get_state(dlna_state_t *out);

// Bumped on every change, so a page repaints when there is something new
// instead of on every tick.
unsigned dlna_serial(void);

// ---------------------------------------------------------------------------
// The bridge to the UI thread
//
// Commands arrive on a network thread, but acting on them means touching
// playback and with it the player, which is LVGL's territory. So the network
// thread posts them here and whoever owns the UI collects them.
// ---------------------------------------------------------------------------

typedef enum {
	DLNA_CMD_NONE = 0,
	DLNA_CMD_PLAY,	 // `path` is ready to play, from `position`
	DLNA_CMD_PAUSE,
	DLNA_CMD_RESUME,
	DLNA_CMD_STOP,
	DLNA_CMD_SEEK,
	DLNA_CMD_VOLUME, // `volume` as a percentage
} dlna_cmd_kind_t;

typedef struct {
	dlna_cmd_kind_t kind;
	double position;
	int volume;
	char path[DLNA_PATH_MAX];
} dlna_command_t;

// Collects the next command, if any. UI thread only.
bool dlna_take_command(dlna_command_t *out);

// Which file DLNA is playing, and who owns playback.
//
// The UI calls dlna_report_playing() as soon as it has handed the file to
// playback: from then on the loaded track belongs to DLNA, the position must be
// reported to dmrd and the end must be announced. dlna_report_stopped() is the
// opposite -- anything else taking over the player (a local track, radio,
// another album) takes playback away from DLNA, and nothing is reported from
// then on.
void dlna_report_playing(const char *path);
void dlna_report_stopped(void);
bool dlna_owns_playback(void);

// True if that path is a file downloaded by DLNA, that is, one inside its cache
// directory. For callers deciding who a file belongs to.
bool dlna_owns_path(const char *path);

// Position within the track, its duration and the volume: the controller draws
// its own bar and slider from these. Called from the UI tick, the only place
// that can read them without surprises; both dmrd's "get_position_info" and the
// heartbeat sent towards it are answered from here.
void dlna_report_progress(int position_secs, int duration_secs, int volume);

// The track ended on its own: telling the controller usually makes it send the
// next one straight away.
void dlna_notify_finished(void);

// Next/previous pressed here while DLNA is playing has no local queue to move
// through: the queue is on the phone. These ask it.
void dlna_request_next(void);
void dlna_request_prev(void);

// Where downloaded tracks go, under the card root. With no card DLNA still
// turns on but has nowhere to put the track, and says so.
#define DLNACACHE_DIR ".local/dlna-cache"
void dlna_set_root(const char *sd_root);

// Empties the cache. Call on the way out -- shutdown, reboot, firmware update,
// factory reset -- exactly as for the Qobuz and Tidal caches and for the same
// reason: these are transient tracks sent by the phone to be heard now, not
// files the user put on the card and expects to find again. Left alone they sit
// in a hidden folder eating space with nothing to show for it.
void dlna_clear_on_exit(void);

#endif /* DLNA_H */

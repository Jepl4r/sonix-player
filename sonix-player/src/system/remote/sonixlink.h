#ifndef SONIXLINK_H
#define SONIXLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// SonixLink: the player, answered over the network by a phone.
//
// This protocol belongs to the project, not to the vendor. No bespoke framing,
// no length-prefixed records, no binary type signatures: it is HTTP with JSON
// bodies, and the library is not paged over the wire at all -- the phone
// downloads the index file once and queries it with its own SQLite. Everything
// that goes wrong is then visible in a browser.
//
//     GET  /api/info               what this player is, and how old its index
//     GET  /api/db                 the index file itself
//     GET  /api/covers             the player's own thumbnail cache, as a file
//     GET  /api/state              what is playing, volume, mode, battery
//     GET  /api/queue              the playback queue around the current track
//     GET  /api/favourites         the starred tracks, as they stand now
//     GET  /api/cover?path=...     the artwork beside a track, when there is one
//     GET  /api/art?path=...       the same track's artwork whole, tag or file,
//                                  undecoded: the phone decodes it
//     POST /api/command?do=...     play, pause, next, prev, seek, volume, mode,
//                                  favourite, scan
//
// Found two ways, so neither has to be perfect: a DNS-SD record for
// `_sonixlink._tcp`, which Android resolves by itself, and a plain UDP
// announcement for anything that would rather listen for one.
//
// Threading. One worker thread owns every socket and never blocks on the
// database for long. The player's state reaches it through sonixlink_publish()
// and the phone's commands leave through sonixlink_take_command(), pumped by
// the interface -- the same shape dlna.c uses.

#define SONIXLINK_TEXT_MAX 256
#define SONIXLINK_PATH_MAX 512

// The port the interface shows and the phone connects to.
#define SONIXLINK_PORT 7800

// What the player is doing. Filled in by the interface and handed over whole.
typedef struct {
	int play_state; // 0 stopped, 1 playing, 2 paused
	int play_mode;	// see sonixlink_mode_t
	int volume;		// 0..100
	unsigned progress_secs;
	unsigned duration_secs;

	char title[SONIXLINK_TEXT_MAX];
	char artist[SONIXLINK_TEXT_MAX];
	char album[SONIXLINK_TEXT_MAX];
	char path[SONIXLINK_PATH_MAX];

	unsigned sample_rate;
	unsigned bitrate;
	uint8_t bits;
	bool lossless;

	int battery_percent;
	bool charging;

	bool favourite; // the track playing now is starred

	// The player's accent colour as "#rrggbb", so the app can wear the same one.
	char accent[8];

	// Where playback sits in the queue, and how long the queue is. Two numbers
	// rather than a queue request: the now-playing screen wants "4 di 12" once a
	// second, and asking for the whole window to read two integers would be
	// twenty kilobytes a second for a label.
	int queue_position; // -1 when there is no queue
	int queue_count;

	bool scanning;
	unsigned scan_count;
	unsigned track_count;
} sonixlink_state_t;

// The five orders the player understands, in the order the phone cycles them.
typedef enum {
	SONIXLINK_MODE_NORMAL = 0,	 // play through the queue and stop
	SONIXLINK_MODE_REPEAT_ALL,	 // loop the queue
	SONIXLINK_MODE_REPEAT_ONE,	 // loop this track
	SONIXLINK_MODE_SHUFFLE,		 // random, once through
	SONIXLINK_MODE_SHUFFLE_REPEAT, // the same random order, over and over
} sonixlink_mode_t;

typedef enum {
	SONIXLINK_CMD_NONE = 0,
	SONIXLINK_CMD_PLAY,
	SONIXLINK_CMD_PAUSE,
	SONIXLINK_CMD_TOGGLE,
	SONIXLINK_CMD_STOP,
	SONIXLINK_CMD_NEXT,
	SONIXLINK_CMD_PREV,
	SONIXLINK_CMD_SEEK,		// arg: seconds from the start
	SONIXLINK_CMD_VOLUME,	// arg: 0..100
	SONIXLINK_CMD_MODE,		// arg: sonixlink_mode_t
	SONIXLINK_CMD_PLAY_PATH, // the track in the command's own `path`
	SONIXLINK_CMD_SCAN,
	SONIXLINK_CMD_FAVOURITE, // arg: 1 star, 0 unstar, -1 toggle; `path` says which
	SONIXLINK_CMD_QUEUE_INDEX, // arg: entry of the current queue to jump to
} sonixlink_command_kind_t;

// Which list a track was started from, so the queue becomes that list and not
// the whole library: starting a track inside a playlist must not queue every
// track in the index.
typedef enum {
	SONIXLINK_LIST_ALL = 0, // every track in the index
	SONIXLINK_LIST_ALBUM,
	SONIXLINK_LIST_ARTIST,
	SONIXLINK_LIST_ALBUM_ARTIST,
	SONIXLINK_LIST_GENRE,
	SONIXLINK_LIST_FAVOURITES,
	SONIXLINK_LIST_PLAYLIST, // `value` is the playlist's name, without M3U_
	SONIXLINK_LIST_QUEUE,	 // the queue as it stands: do not rebuild it
} sonixlink_list_t;

typedef struct {
	sonixlink_command_kind_t kind;
	int arg;
	// The track a command is about, empty for the ones that are not about one.
	// Carried here rather than in a single shared slot so two path commands in
	// the same pump cannot end up pointing at the same file.
	char path[SONIXLINK_PATH_MAX];

	// For SONIXLINK_CMD_PLAY_PATH: the list the track was picked from, and what
	// narrows it (the album's name, the artist's, the playlist's).
	int list;
	char value[SONIXLINK_TEXT_MAX];
} sonixlink_command_t;

// Starts the worker. It sits idle until the switch is on.
void sonixlink_init(void);

// The switch, remembered across reboots in [wireless] sonixlink.
bool sonixlink_get_enabled(void);
void sonixlink_set_enabled(bool on);

// Whether a phone has asked for anything recently, and what to show on the
// page: the address it should be pointed at, and who last spoke.
bool sonixlink_is_connected(void);
void sonixlink_peer(char *out, size_t out_size);
void sonixlink_address(char *out, size_t out_size);

// Where the index file lives, so it can be served. Called once at startup.
void sonixlink_set_db_path(const char *path);

// Where the thumbnail cache lives -- the same file the player fills while its
// own lists are drawn (see src/gui/cover.c). It is sent to the phone as it
// stands: the pictures are already decoded and scaled, and the key is worked
// out from the index the phone already has.
void sonixlink_set_thumbs_path(const char *path);

// The playback queue, as much of it as the phone is shown.
//
// The whole queue is not published: it can be the entire library, and holding a
// second copy of seven thousand paths to answer a page nobody may open is not
// worth the memory. What goes across is a window around the track playing now,
// refreshed when the queue or the position changes.
#define SONIXLINK_QUEUE_WINDOW 200

// `paths` holds `count` entries starting at queue position `first`. `total` is
// how long the queue really is and `position` where playback sits in it.
void sonixlink_publish_queue(int total, int position, int first, const char *const *paths, int count);

// Hands the worker a fresh picture of the player. Call it on a timer.
void sonixlink_publish(const sonixlink_state_t *state);

// Takes one queued command, oldest first. False when there is nothing.
bool sonixlink_take_command(sonixlink_command_t *out);

// The track SONIXLINK_CMD_PLAY_PATH asks for.
void sonixlink_take_path(char *out, size_t out_size);

#endif /* SONIXLINK_H */

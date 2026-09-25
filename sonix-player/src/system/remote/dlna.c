#include "dlna.h"

#include "src/system/core/respath.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "src/system/decode/growfile.h"
#include "src/system/net/http.h"
#include "src/system/device/sysserver.h"
#include "src/system/core/utils.h"
#include "src/system/core/lang.h"

// The two sockets, and which side listens on each. See dlna.h.
#define STREAMER_SOCKET "/data/dmr_streamer"
#define CONTROL_SOCKET "/data/dmr_control"

#define DMRD_BIN "/usr/bin/dmrd"

// Where sys_server listens; it is what starts dmrd. See dlna_available().
#define SYSSERVER_SOCKET_PATH "/var/run/sys_server"

// The same file Bluetooth and AirPlay take their name from: one line,
// "HiBy R3PROII". sys_server passes it to dmrd quoted, so unlike AirPlay the
// spaces must be left as they are.
#define NAME_PATH RESOURCE_DIR "/bt_name"
#define NAME_FALLBACK "HiBy Music"

// One command fits in a single packet, and they are all short: the longest is
// a set_uri carrying a URL.
#define COMMAND_MAX 2048

// The phone is on the same LAN, so it either answers at once or is gone.
#define FETCH_TIMEOUT_SECS 10
#define FETCH_CHUNK 32768

// How much to download before starting the decoder. The phone is one hop away,
// not halfway around the world, so the sizing math qobuzcache needs is not
// required here: half a megabyte covers the headers of any format and keeps
// the decoder from catching up with the download immediately.
#define FETCH_PREBUFFER (512 * 1024)

// How often the current position goes to dmrd. The controller draws its
// progress bar from it; once per second is what the stock firmware does.
#define POSITION_EVERY_MS 1000

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static dlna_state_t state;
static unsigned state_serial;

// The path DLNA is playing, as reported by the UI. Empty when playback belongs
// to someone else.
static char playing_path[DLNA_PATH_MAX];

static char cache_root[DLNA_PATH_MAX];

void dlna_clear_on_exit(void) {
	if (!cache_root[0]) {
		return;
	}
	DIR *d = opendir(cache_root);
	if (!d) {
		return;
	}
	int removed = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') {
			continue;
		}
		char path[DLNA_PATH_MAX + 300];
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", cache_root, e->d_name) >= sizeof(path)) {
			continue;
		}
		if (remove(path) == 0) {
			removed++;
		}
	}
	closedir(d);
	printf("dlna: cache cleared (%d files)\n", removed);
}

static void bump(void) { state_serial++; }

static void set_error(const char *why) {
	pthread_mutex_lock(&state_lock);
	snprintf(state.error, sizeof(state.error), "%s", why ? why : "");
	bump();
	pthread_mutex_unlock(&state_lock);
}

void dlna_get_state(dlna_state_t *out) {
	if (!out) {
		return;
	}
	pthread_mutex_lock(&state_lock);
	*out = state;
	pthread_mutex_unlock(&state_lock);
}

unsigned dlna_serial(void) {
	pthread_mutex_lock(&state_lock);
	unsigned s = state_serial;
	pthread_mutex_unlock(&state_lock);
	return s;
}

// Two access() calls, like airplay_available(), rather than
// sysserver_available(): the page asks on every tick, and that would open a
// connection to the daemon each time -- an unbounded connect() on the UI
// thread. The socket existing is enough: sys_server starts dmrd, and without
// sys_server the socket is not there.
bool dlna_available(void) { return access(DMRD_BIN, X_OK) == 0 && access(SYSSERVER_SOCKET_PATH, F_OK) == 0; }

const char *dlna_name(void) {
	static char name[80];
	if (name[0]) {
		return name;
	}

	FILE *f = fopen(NAME_PATH, "r");
	if (f) {
		if (fgets(name, sizeof(name), f) == NULL) {
			name[0] = '\0';
		}
		fclose(f);
	}

	size_t len = strlen(name);
	while (len > 0 && (unsigned char)name[len - 1] <= ' ') {
		name[--len] = '\0';
	}
	if (name[0] == '\0') {
		snprintf(name, sizeof(name), "%s", NAME_FALLBACK);
	}
	return name;
}

void dlna_set_root(const char *sd_root) {
	pthread_mutex_lock(&state_lock);
	if (sd_root && sd_root[0]) {
		snprintf(cache_root, sizeof(cache_root), "%s/" DLNACACHE_DIR, sd_root);
	} else {
		cache_root[0] = '\0';
	}
	pthread_mutex_unlock(&state_lock);
}

static bool cache_dir(char *out, size_t size) {
	pthread_mutex_lock(&state_lock);
	bool have = cache_root[0] != '\0';
	if (have) {
		snprintf(out, size, "%s", cache_root);
	}
	pthread_mutex_unlock(&state_lock);
	return have;
}

// mkdir -p over the levels needed (.local, then the directory itself).
static bool ensure_dir(const char *dir) {
	char path[DLNA_PATH_MAX];
	snprintf(path, sizeof(path), "%s", dir);
	for (char *p = path + 1; *p; p++) {
		if (*p != '/') {
			continue;
		}
		*p = '\0';
		mkdir(path, 0777);
		*p = '/';
	}
	if (mkdir(path, 0777) != 0 && errno != EEXIST) {
		fprintf(stderr, "dlna: cannot create %s: %s\n", path, strerror(errno));
		return false;
	}
	return true;
}

bool dlna_owns_path(const char *path) {
	if (!path || !path[0]) {
		return false;
	}
	char dir[DLNA_PATH_MAX];
	if (!cache_dir(dir, sizeof(dir))) {
		return false;
	}
	size_t n = strlen(dir);
	return strncmp(path, dir, n) == 0 && path[n] == '/';
}

// ---------------------------------------------------------------------------
// the command queue towards the UI
// ---------------------------------------------------------------------------
//
// Small, and coalescing per kind: a phone sending three seeks in a row while
// its slider is dragged must not cause three seeks.

#define CMD_QUEUE 8

static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static dlna_command_t queue[CMD_QUEUE];
static int queue_head, queue_count;

static void queue_push(const dlna_command_t *cmd) {
	pthread_mutex_lock(&queue_lock);

	// A command of the same kind not yet taken is replaced: the last one wins,
	// not the first.
	for (int i = 0; i < queue_count; i++) {
		dlna_command_t *slot = &queue[(queue_head + i) % CMD_QUEUE];
		if (slot->kind == cmd->kind && cmd->kind != DLNA_CMD_PLAY) {
			*slot = *cmd;
			pthread_mutex_unlock(&queue_lock);
			return;
		}
	}

	if (queue_count == CMD_QUEUE) {
		queue_head = (queue_head + 1) % CMD_QUEUE; // drop the oldest
		queue_count--;
	}
	queue[(queue_head + queue_count) % CMD_QUEUE] = *cmd;
	queue_count++;

	pthread_mutex_unlock(&queue_lock);
}

static void queue_clear(void) {
	pthread_mutex_lock(&queue_lock);
	queue_head = queue_count = 0;
	pthread_mutex_unlock(&queue_lock);
}

bool dlna_take_command(dlna_command_t *out) {
	if (!out) {
		return false;
	}
	pthread_mutex_lock(&queue_lock);
	bool have = queue_count > 0;
	if (have) {
		*out = queue[queue_head];
		queue_head = (queue_head + 1) % CMD_QUEUE;
		queue_count--;
	}
	pthread_mutex_unlock(&queue_lock);
	return have;
}

// ---------------------------------------------------------------------------
// towards dmrd: /data/dmr_control
// ---------------------------------------------------------------------------

// Longest wait for dmrd to accept the connection.
#define CONTROL_CONNECT_MS 1000

// connect() with a deadline: switch the descriptor to non-blocking, wait with
// poll(), then restore the flags.
static int connect_with_timeout(int fd, const struct sockaddr *addr, socklen_t len, int timeout_ms) {
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		return connect(fd, addr, len); // no deadline, but better than nothing
	}

	int rc = connect(fd, addr, len);
	if (rc < 0 && errno == EINPROGRESS) {
		struct pollfd p = {.fd = fd, .events = POLLOUT};
		rc = poll(&p, 1, timeout_ms);
		if (rc <= 0) {
			fcntl(fd, F_SETFL, flags);
			return -1; // timed out, or poll failed
		}
		int err = 0;
		socklen_t errlen = sizeof(err);
		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0 || err != 0) {
			fcntl(fd, F_SETFL, flags);
			return -1;
		}
		rc = 0;
	}

	fcntl(fd, F_SETFL, flags);
	return rc;
}

// Connect, send, close -- one message per connection, like the stock firmware.
// The socket is local: dmrd either answers at once or is not there.
//
// Worker threads only. connect() on a UNIX socket with a full backlog blocks,
// and SO_SNDTIMEO does not bound it, so on the UI thread this would freeze the
// display. Anything that is not a worker thread uses control_post() below.
static void control_send(const char *message) {
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		return;
	}

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", CONTROL_SOCKET);

	struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	// The bounded connect(), not the plain one: SO_SNDTIMEO does not bound it,
	// and on a UNIX socket with a full backlog it waits forever. The heartbeat
	// thread comes through here and service_stop() joins it, so a block here
	// made turning DLNA off never return.
	if (connect_with_timeout(fd, (struct sockaddr *)&addr, sizeof(addr), CONTROL_CONNECT_MS) == 0) {
		// The NUL goes over the wire with the rest: the other side runs sscanf
		// on a C string.
		ssize_t ignored = send(fd, message, strlen(message) + 1, MSG_NOSIGNAL);
		(void)ignored;
	}
	close(fd);
}

// Outbox: callers that cannot afford to block leave the message here and the
// heartbeat thread delivers it.
#define OUTBOX 8
static pthread_mutex_t outbox_lock = PTHREAD_MUTEX_INITIALIZER;
static char outbox[OUTBOX][64];
static int outbox_head, outbox_count;

static void control_post(const char *message) {
	pthread_mutex_lock(&outbox_lock);
	if (outbox_count == OUTBOX) {
		outbox_head = (outbox_head + 1) % OUTBOX; // drop the oldest
		outbox_count--;
	}
	snprintf(outbox[(outbox_head + outbox_count) % OUTBOX], sizeof(outbox[0]), "%s", message);
	outbox_count++;
	pthread_mutex_unlock(&outbox_lock);
}

static void control_flush(void) {
	for (;;) {
		char message[64];
		pthread_mutex_lock(&outbox_lock);
		bool have = outbox_count > 0;
		if (have) {
			snprintf(message, sizeof(message), "%s", outbox[outbox_head]);
			outbox_head = (outbox_head + 1) % OUTBOX;
			outbox_count--;
		}
		pthread_mutex_unlock(&outbox_lock);
		if (!have) {
			return;
		}
		control_send(message);
	}
}

void dlna_notify_finished(void) {
	// Playback has ended here, but do not send "stop": a controller reads
	// that as "the user stopped it" and never advances to the next track. It
	// only needs to hear that this one ended.
	pthread_mutex_lock(&state_lock);
	playing_path[0] = '\0';
	pthread_mutex_unlock(&state_lock);

	control_post("play_finish");
}
void dlna_request_next(void) { control_post("next"); }
void dlna_request_prev(void) { control_post("previous"); }

// ---------------------------------------------------------------------------
// the download
// ---------------------------------------------------------------------------
//
// The phone serves the track over its own HTTP server and hands over the URL.
// It is downloaded to the card and playback starts once there is enough; from
// there on it is a file like any other.

static pthread_mutex_t fetch_lock = PTHREAD_MUTEX_INITIALIZER;
static int fetch_fd = -1;		  // socket of the running download, so it can be woken
static unsigned fetch_generation; // bumped on every new URL: cancels the old one

typedef struct {
	char url[DLNA_URL_MAX];
	char path[DLNA_PATH_MAX];
	double position;
	unsigned generation;
} fetch_job_t;

static unsigned generation_now(void) {
	pthread_mutex_lock(&fetch_lock);
	unsigned g = fetch_generation;
	pthread_mutex_unlock(&fetch_lock);
	return g;
}

// Cuts off the running download, if any: the reader on that socket returns at
// once instead of sitting there until the timeout expires.
static void fetch_abandon(void) {
	pthread_mutex_lock(&fetch_lock);
	fetch_generation++;
	if (fetch_fd >= 0) {
		// shutdown(), not close(): the stream belongs to its own thread, and
		// closing the descriptor here under an in-flight recv() is how the
		// same number ends up handed to another connection.
		shutdown(fetch_fd, SHUT_RDWR);
	}
	// Drop the commands that track already queued, under the same lock as the
	// generation bump: no instant exists between "it is no longer current" and
	// "what it queued is void" for a stale play to slip through (see
	// push_play).
	queue_clear();
	pthread_mutex_unlock(&fetch_lock);
}

// The extension to give the file. The decoder inspects content, but
// decode_detect_format() starts from the name, so the name has to be right.
//
// Writes into a caller buffer rather than a static one: two downloads can
// overlap (an abandoned one keeps running until it notices), and a shared
// buffer means two files each carrying the other's extension.
static void extension_for(const char *content_type, const char *url, char *out, size_t size) {
	static const struct {
		const char *mime;
		const char *ext;
	} MAP[] = {
		// The names DLNA servers actually use for the same thing. An Android
		// phone sends an MP3 as "audio/x-mpeg" or "application/octet-stream"
		// depending on the app, so every spelling needs an entry.
		{"audio/flac", "flac"},
		{"audio/x-flac", "flac"},

		{"audio/mpeg", "mp3"},
		{"audio/x-mpeg", "mp3"},
		{"audio/mp3", "mp3"},
		{"audio/x-mp3", "mp3"},
		{"audio/mpeg3", "mp3"},
		{"audio/x-mpeg-3", "mp3"},

		// AAC inside an MP4 container, and video too: an mp4 pushed from the
		// phone has an audio track like any other, and mp4.c picks that one
		// and ignores the pictures. Video on the phone, sound on the player,
		// with no extra code.
		{"audio/mp4", "m4a"},
		{"audio/x-m4a", "m4a"},
		{"audio/m4a", "m4a"},
		{"audio/mp4a-latm", "m4a"},
		{"video/mp4", "mp4"},
		{"video/x-m4v", "mp4"},
		{"video/quicktime", "mp4"},

		{"audio/wav", "wav"},
		{"audio/x-wav", "wav"},
		{"audio/wave", "wav"},
		{"audio/vnd.wave", "wav"},

		{"audio/ogg", "ogg"},
		{"application/ogg", "ogg"},
		{"audio/vorbis", "ogg"},
		{"audio/x-vorbis+ogg", "ogg"},

		{"audio/opus", "opus"},
		{"audio/x-opus+ogg", "opus"},

		{"audio/x-aiff", "aiff"},
		{"audio/aiff", "aiff"},

		{"audio/x-wavpack", "wv"},
		{"audio/wavpack", "wv"},

		{"audio/x-ape", "ape"},
		{"audio/ape", "ape"},
		{"audio/x-monkeys-audio", "ape"},

		{"audio/dsd", "dsf"},
		{"audio/x-dsd", "dsf"},
		{"audio/x-dsf", "dsf"},
		{"audio/x-dff", "dff"},

		// No audio/aac entry on purpose. That type means raw ADTS AAC, not
		// MP4: calling it .m4a sent mp4.c looking for a container that is not
		// there and the track never started. With no entry, the URL extension
		// or the last-resort branch applies and the decoder can at least try
		// by content.
	};

	if (content_type && content_type[0]) {
		for (unsigned i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++) {
			if (strncasecmp(content_type, MAP[i].mime, strlen(MAP[i].mime)) == 0) {
				snprintf(out, size, "%s", MAP[i].ext);
				return;
			}
		}
	}

	// The declared type says nothing ("application/octet-stream" is common).
	// Fall back to the tail of the URL, up to the first ? or #.
	if (url) {
		const char *end = url + strlen(url);
		for (const char *p = url; *p; p++) {
			if (*p == '?' || *p == '#') {
				end = p;
				break;
			}
		}
		const char *dot = NULL;
		for (const char *p = url; p < end; p++) {
			if (*p == '.') {
				dot = p;
			}
			if (*p == '/') {
				dot = NULL;
			}
		}
		if (dot && end - dot - 1 > 0 && (size_t)(end - dot - 1) < size) {
			size_t n = (size_t)(end - dot - 1);
			memcpy(out, dot + 1, n);
			out[n] = '\0';
			for (size_t i = 0; i < n; i++) {
				if (out[i] >= 'A' && out[i] <= 'Z') {
					out[i] = (char)(out[i] - 'A' + 'a');
				}
			}
			return;
		}
	}

	// Last resort: nearly everything a phone pushes is an MP3, and the decoder
	// still probes by content anyway.
	snprintf(out, size, "mp3");
}

// The cache directory holds one track at a time. Everything goes except the
// file about to be written and the one still playing -- which may well be the
// previous one, if the phone already pushed the next while this one finishes.
static void prune_cache(const char *keep) {
	char dir[DLNA_PATH_MAX];
	if (!cache_dir(dir, sizeof(dir))) {
		return;
	}

	char playing[DLNA_PATH_MAX];
	pthread_mutex_lock(&state_lock);
	snprintf(playing, sizeof(playing), "%s", playing_path);
	pthread_mutex_unlock(&state_lock);

	DIR *d = opendir(dir);
	if (!d) {
		return;
	}
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.') {
			continue;
		}
		char path[DLNA_PATH_MAX + 300];
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		if ((keep && strcmp(path, keep) == 0) || (playing[0] && strcmp(path, playing) == 0)) {
			continue;
		}
		if (growfile_is_growing(path)) {
			continue; // still being written
		}
		remove(path);
	}
	closedir(d);
}

// A counter, not a flag: an abandoned download keeps running until it notices,
// and its exit must not clear the indicator for the one just started.
static int fetching_count;

static void set_fetching(bool on) {
	pthread_mutex_lock(&state_lock);
	fetching_count += on ? 1 : -1;
	if (fetching_count < 0) {
		fetching_count = 0;
	}
	state.fetching = fetching_count > 0;
	bump();
	pthread_mutex_unlock(&state_lock);
}

// The "play this" towards the UI, with the generation check attached: between
// this thread deciding there is enough data and the command entering the
// queue, the phone may already have pushed another track -- and that play,
// landing after the new track's queue_clear, would play the abandoned file
// instead of the new one. The check sits inside the queue lock, so there is no
// window.
static void push_play(const fetch_job_t *job) {
	dlna_command_t cmd = {0};
	cmd.kind = DLNA_CMD_PLAY;
	cmd.position = job->position;
	snprintf(cmd.path, sizeof(cmd.path), "%s", job->path);

	// The lock is held across queue_push, not just for the comparison:
	// fetch_abandon() bumps the generation and clears the queue under the same
	// lock, so either this command lands first (and the clear takes it away) or
	// it never lands. There is nothing in between.
	pthread_mutex_lock(&fetch_lock);
	if (fetch_generation == job->generation) {
		queue_push(&cmd);
	}
	pthread_mutex_unlock(&fetch_lock);
}

static void *fetch_thread(void *arg) {
	thread_be_background("dlna fetch");

	fetch_job_t *job = arg;
	http_stream_t stream;
	FILE *out = NULL;
	long done = 0, total = 0;
	bool announced = false;
	bool started_playing = false;
	bool ok = false;

	set_fetching(true);

	if (!http_stream_open(&stream, job->url, FETCH_TIMEOUT_SECS)) {
		const char *why = http_last_error();
		fprintf(stderr, "dlna: %s does not open%s%s\n", job->url, why && why[0] ? ": " : "", why && why[0] ? why : "");
		set_error(why && why[0] ? why : tr("dlna_download_failed"));
		goto done_label;
	}

	// The final name is only known now: Content-Type says which container the
	// track is in.
	{
		char dir[DLNA_PATH_MAX];
		if (!cache_dir(dir, sizeof(dir))) {
			set_error(tr("dlna_needs_a_microsd_card"));
			http_stream_close(&stream);
			goto done_label;
		}
		if (!ensure_dir(dir)) {
			set_error(tr("card_write_failed"));
			http_stream_close(&stream);
			goto done_label;
		}
		// The generation goes in the name instead of a fixed "track.flac", so
		// that two tracks pushed back to back write to different files: with a
		// single name the new download overwrites what the decoder is still
		// reading.
		char ext[8];
		extension_for(stream.content_type, job->url, ext, sizeof(ext));
		snprintf(job->path, sizeof(job->path), "%.460s/track-%u.%.7s", dir, job->generation, ext);
	}

	total = stream.content_length;

	// Announce before the file exists, prune after. That is what protects the
	// path: prune_cache skips growing files, and between fopen and the
	// announcement there would be a window where this file was neither growing
	// nor playing -- a download starting in that instant would delete it from
	// under this one.
	growfile_announce(job->path, total);
	announced = true;

	// Earlier tracks are pruned only here, not before the announcement: one of
	// them may still have been playing a moment ago.
	prune_cache(job->path);

	out = fopen(job->path, "wb");
	if (!out) {
		fprintf(stderr, "dlna: %s does not open for writing: %s\n", job->path, strerror(errno));
		set_error(errno == ENOSPC ? tr("card_full") : tr("card_write_failed"));
		http_stream_close(&stream);
		goto done_label;
	}

	pthread_mutex_lock(&fetch_lock);
	fetch_fd = stream.fd;
	pthread_mutex_unlock(&fetch_lock);

	for (;;) {
		if (generation_now() != job->generation) {
			break; // the phone already pushed something else
		}

		char buf[FETCH_CHUNK];
		int n = http_stream_read(&stream, buf, (int)sizeof(buf));
		if (n < 0) {
			break;
		}
		if (n == 0) {
			ok = true;
			break;
		}
		if (fwrite(buf, 1, (size_t)n, out) != (size_t)n) {
			fprintf(stderr, "dlna: write failed (card full?)\n");
			set_error(tr("card_full"));
			break;
		}
		// Out of the stdio buffer first, then announce: the reader looks on
		// disk, not in this process's memory.
		fflush(out);
		done += n;
		growfile_progress(job->path, done);

		// Enough to start the decoder, or the whole track if it is shorter
		// than the prebuffer.
		if (!started_playing && (done >= FETCH_PREBUFFER || (total > 0 && done >= total))) {
			started_playing = true;
			push_play(job);
		}
	}

	pthread_mutex_lock(&fetch_lock);
	if (fetch_fd == stream.fd) {
		fetch_fd = -1;
	}
	pthread_mutex_unlock(&fetch_lock);

	http_stream_close(&stream);

	if (ok && total > 0 && done != total) {
		fprintf(stderr, "dlna: cut short at %ld of %ld bytes\n", done, total);
		ok = false;
	}
	if (done == 0) {
		ok = false;
	}

done_label:
	if (out) {
		if (fclose(out) != 0) {
			ok = false;
		}
	}
	if (announced) {
		growfile_finish(job->path, ok);
	}

	// The whole track arrived but never reached the prebuffer threshold: play
	// it now rather than stay silent.
	if (ok && !started_playing) {
		push_play(job);
	}

	set_fetching(false);
	free(job);
	return NULL;
}

// ---------------------------------------------------------------------------
// from dmrd: /data/dmr_streamer
// ---------------------------------------------------------------------------

static int listen_fd = -1;
static pthread_t server_thread;
static bool server_live;
static volatile bool server_stopping;

static char pending_url[DLNA_URL_MAX];

static int make_server(const char *path) {
	unlink(path); // a socket left over from a previous run would fail bind()

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		fprintf(stderr, "dlna: socket(%s): %s\n", path, strerror(errno));
		return -1;
	}

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "dlna: bind(%s): %s\n", path, strerror(errno));
		close(fd);
		return -1;
	}
	if (listen(fd, 16) < 0) {
		fprintf(stderr, "dlna: listen(%s): %s\n", path, strerror(errno));
		close(fd);
		unlink(path);
		return -1;
	}
	chmod(path, 0666);
	return fd;
}

static void reply(int fd, const char *text) {
	ssize_t ignored = send(fd, text, strlen(text) + 1, MSG_NOSIGNAL);
	(void)ignored;
}

// Metadata the phone sent, kept for the page to show.
static void store_meta(const char *key, const char *value) {
	pthread_mutex_lock(&state_lock);
	if (strcmp(key, "title") == 0) {
		snprintf(state.title, sizeof(state.title), "%s", value);
	} else if (strcmp(key, "artist") == 0 || strcmp(key, "creator") == 0) {
		// "creator" always arrives, "artist" only from some controllers:
		// artist wins when present, but must not erase creator.
		if (strcmp(key, "artist") == 0 || state.artist[0] == '\0') {
			snprintf(state.artist, sizeof(state.artist), "%s", value);
		}
	} else if (strcmp(key, "album") == 0) {
		snprintf(state.album, sizeof(state.album), "%s", value);
	}
	state.connected = true;
	bump();
	pthread_mutex_unlock(&state_lock);
}

static void clear_meta(void) {
	pthread_mutex_lock(&state_lock);
	state.title[0] = state.artist[0] = state.album[0] = state.error[0] = '\0';
	state.connected = true;
	bump();
	pthread_mutex_unlock(&state_lock);
}

// Position and duration, for get_position_info and for the heartbeat towards
// dmrd. Filled in by the UI (see dlna_report_progress).
static pthread_mutex_t progress_lock = PTHREAD_MUTEX_INITIALIZER;
static int progress_pos_secs;
static int progress_dur_secs;
static int volume_percent = 50;

void dlna_report_playing(const char *path) {
	pthread_mutex_lock(&state_lock);
	snprintf(playing_path, sizeof(playing_path), "%s", path ? path : "");
	pthread_mutex_unlock(&state_lock);
	if (path && path[0]) {
		control_post("play");
	}
}

void dlna_report_stopped(void) {
	pthread_mutex_lock(&state_lock);
	bool was = playing_path[0] != '\0';
	playing_path[0] = '\0';
	pthread_mutex_unlock(&state_lock);
	if (was) {
		control_post("stop");
	}
}

bool dlna_owns_playback(void) {
	pthread_mutex_lock(&state_lock);
	bool own = playing_path[0] != '\0';
	pthread_mutex_unlock(&state_lock);
	return own;
}

// Called from the UI tick: it is the one that can read audio.c safely and the
// one that knows the volume in effect.
void dlna_report_progress(int position_secs, int duration_secs, int volume) {
	pthread_mutex_lock(&progress_lock);
	progress_pos_secs = position_secs;
	progress_dur_secs = duration_secs;
	volume_percent = volume;
	pthread_mutex_unlock(&progress_lock);
}

static void handle(int fd, const char *cmd) {
	if (strncmp(cmd, "set_uri:", 8) == 0) {
		// Only stored: the download starts on play, because some controllers
		// send the next track's URL long before they want to hear it.
		pthread_mutex_lock(&state_lock);
		snprintf(pending_url, sizeof(pending_url), "%.*s", (int)sizeof(pending_url) - 1, cmd + 8);
		pthread_mutex_unlock(&state_lock);
		clear_meta();
		return;
	}

	if (strncmp(cmd, "set_meta:", 9) == 0) {
		const char *rest = cmd + 9;
		const char *colon = strchr(rest, ':');
		if (colon) {
			char key[32];
			size_t n = (size_t)(colon - rest);
			if (n >= sizeof(key)) {
				n = sizeof(key) - 1;
			}
			memcpy(key, rest, n);
			key[n] = '\0';
			store_meta(key, colon + 1);
		}
		return;
	}

	if (strncmp(cmd, "play", 4) == 0) {
		int position = 0;
		sscanf(cmd, "play@%d", &position);

		char url[DLNA_URL_MAX];
		pthread_mutex_lock(&state_lock);
		snprintf(url, sizeof(url), "%s", pending_url);
		pthread_mutex_unlock(&state_lock);

		if (!url[0]) {
			// No new URL: this is a resume on what is already loaded.
			dlna_command_t resume = {.kind = DLNA_CMD_RESUME};
			queue_push(&resume);
			return;
		}

		// A new URL cancels the running download and the commands it queued.
		fetch_abandon();

		fetch_job_t *job = calloc(1, sizeof(*job));
		if (!job) {
			return;
		}
		snprintf(job->url, sizeof(job->url), "%s", url);
		job->position = position;
		job->generation = generation_now();

		pthread_mutex_lock(&state_lock);
		pending_url[0] = '\0'; // consumed: the next "play" is a resume
		state.connected = true;
		bump();
		pthread_mutex_unlock(&state_lock);

		pthread_t t;
		if (pthread_create(&t, NULL, fetch_thread, job) == 0) {
			pthread_detach(t);
		} else {
			free(job);
		}
		return;
	}

	if (strcmp(cmd, "pause") == 0) {
		dlna_command_t c = {.kind = DLNA_CMD_PAUSE};
		queue_push(&c);
		return;
	}

	if (strcmp(cmd, "stop") == 0) {
		fetch_abandon();
		dlna_command_t c = {.kind = DLNA_CMD_STOP};
		queue_push(&c);
		return;
	}

	if (strncmp(cmd, "seek@", 5) == 0) {
		int secs = 0;
		sscanf(cmd, "seek@%d", &secs);
		dlna_command_t c = {.kind = DLNA_CMD_SEEK, .position = secs};
		queue_push(&c);
		return;
	}

	if (strncmp(cmd, "set_volume@", 11) == 0) {
		int percent = 50;
		sscanf(cmd, "set_volume@%d", &percent);
		dlna_command_t c = {.kind = DLNA_CMD_VOLUME, .volume = percent};
		queue_push(&c);
		return;
	}

	if (strcmp(cmd, "get_volume") == 0) {
		char answer[16];
		pthread_mutex_lock(&progress_lock);
		snprintf(answer, sizeof(answer), "%d", volume_percent);
		pthread_mutex_unlock(&progress_lock);
		reply(fd, answer);
		return;
	}

	if (strcmp(cmd, "get_position_info") == 0) {
		char answer[32];
		pthread_mutex_lock(&progress_lock);
		snprintf(answer, sizeof(answer), "%d@%d", progress_pos_secs, progress_dur_secs);
		pthread_mutex_unlock(&progress_lock);
		reply(fd, answer);
		return;
	}

	// "state@<n>" and everything else: dmrd keeps its own state and this side
	// does not need it. Accepted without a reply, like the stock firmware.
}

static void *server_worker(void *unused) {
	(void)unused;
	thread_be_background("dlna server");

	while (!server_stopping) {
		struct pollfd p = {.fd = listen_fd, .events = POLLIN};
		int ready = poll(&p, 1, 300);
		if (ready <= 0) {
			continue;
		}

		int client = accept(listen_fd, NULL, NULL);
		if (client < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}

		// One connection, one command: that is how dmrd talks.
		//
		// With a timeout, though: the socket is world-accessible, and anyone
		// connecting without sending would hold this loop forever -- and with
		// it the pthread_join in service_stop(), so DLNA could never be turned
		// off.
		struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
		setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

		char buf[COMMAND_MAX];
		ssize_t got = recv(client, buf, sizeof(buf) - 1, 0);
		if (got > 0) {
			buf[got] = '\0';
			// The sender NUL-terminates; if it did not, the line above does.
			printf("dlna: <- %s\n", buf);
			handle(client, buf);
		}
		close(client);
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// the heartbeat towards dmrd
// ---------------------------------------------------------------------------

static pthread_t heartbeat_thread;
static bool heartbeat_live;

// The only place that writes to dmrd. It does two things: drain the outbox the
// UI filled, and once per second report the position within the track.
#define HEARTBEAT_TICK_MS 100

static void *heartbeat_worker(void *unused) {
	(void)unused;
	thread_be_background("dlna position");

	int last_sent = -1;
	int ticks = 0;
	while (!server_stopping) {
		usleep(HEARTBEAT_TICK_MS * 1000);
		if (server_stopping) {
			break;
		}

		control_flush();

		if (++ticks < POSITION_EVERY_MS / HEARTBEAT_TICK_MS) {
			continue;
		}
		ticks = 0;

		if (!dlna_owns_playback()) {
			last_sent = -1;
			continue;
		}

		int pos, dur;
		pthread_mutex_lock(&progress_lock);
		pos = progress_pos_secs;
		dur = progress_dur_secs;
		pthread_mutex_unlock(&progress_lock);

		if (pos == last_sent) {
			continue; // paused: nothing new to report
		}
		last_sent = pos;

		char message[64];
		snprintf(message, sizeof(message), "set_position@%d@%d", pos, dur);
		control_send(message);
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// on and off
// ---------------------------------------------------------------------------

static pthread_mutex_t ctl_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ctl_cond = PTHREAD_COND_INITIALIZER;
static bool ctl_thread_started;
static bool desired_on;
static bool actual_on;
static bool service_up;

static void service_start(void) {
	server_stopping = false;
	queue_clear();

	pthread_mutex_lock(&state_lock);
	memset(&state, 0, sizeof(state));
	state.fetching = fetching_count > 0;
	pending_url[0] = '\0';
	playing_path[0] = '\0';
	bump();
	pthread_mutex_unlock(&state_lock);

	// Listening before sys_server is told to start dmrd: dmrd is the side that
	// connects here, and a socket that does not exist yet means a renderer
	// that announces itself on the network and then plays nothing.
	listen_fd = make_server(STREAMER_SOCKET);
	if (listen_fd < 0) {
		set_error(tr("dlna_socket_failed"));
		return;
	}

	server_live = pthread_create(&server_thread, NULL, server_worker, NULL) == 0;
	heartbeat_live = pthread_create(&heartbeat_thread, NULL, heartbeat_worker, NULL) == 0;

	// The daemon runs `dmrd  -f "%s" &` through a shell, with those quotes real
	// ones -- so a space or a '#' in the name is safe there, and only the four
	// characters a shell still reads inside double quotes are not. A backtick
	// would be a command substitution, running as root.
	char wire[96];
	if (sysserver_safe_quoted(dlna_name(), wire, sizeof(wire))) {
		printf("dlna: the name has characters the daemon shell would read; using \"%s\"\n", wire);
	}

	char command[160];
	snprintf(command, sizeof(command), "DLNA:TURN_ON:%s", wire);

	char answer[64] = {0};
	int rc = sysserver_request_timeout(command, answer, sizeof(answer), 15000);
	printf("dlna: '%s' -> %s\n", command, answer[0] ? answer : "(no answer)");

	if (rc != 0) {
		set_error(tr("dlna_start_failed"));
		return;
	}
	pthread_mutex_lock(&ctl_lock);
	service_up = true;
	pthread_mutex_unlock(&ctl_lock);
}

static void service_stop(void) {
	pthread_mutex_lock(&ctl_lock);
	bool up = service_up;
	pthread_mutex_unlock(&ctl_lock);

	if (up) {
		// Say goodbye to dmrd first: "exit" is the clean way, and the killall
		// sys_server does right after is the safety net.
		control_send("exit");
		char answer[64] = {0};
		sysserver_request("DLNA:TURN_OFF", answer, sizeof(answer));
		printf("dlna: turn off -> %s\n", answer[0] ? answer : "(no answer)");
		pthread_mutex_lock(&ctl_lock);
		service_up = false;
		pthread_mutex_unlock(&ctl_lock);
	}

	fetch_abandon();
	server_stopping = true;

	if (server_live) {
		pthread_join(server_thread, NULL);
		server_live = false;
	}
	if (heartbeat_live) {
		pthread_join(heartbeat_thread, NULL);
		heartbeat_live = false;
	}

	if (listen_fd >= 0) {
		close(listen_fd);
		listen_fd = -1;
	}
	unlink(STREAMER_SOCKET);

	// Again, and not a bare queue_clear(): an abandoned download is detached
	// and may still be between "there is enough" and its queue_push. This
	// bumps the generation and clears together, so what comes later never
	// lands.
	fetch_abandon();

	pthread_mutex_lock(&state_lock);
	memset(&state, 0, sizeof(state));
	state.fetching = fetching_count > 0;
	pending_url[0] = '\0';
	playing_path[0] = '\0';
	bump();
	pthread_mutex_unlock(&state_lock);
}

static void *ctl_worker(void *unused) {
	(void)unused;

	for (;;) {
		pthread_mutex_lock(&ctl_lock);
		while (desired_on == actual_on) {
			pthread_cond_wait(&ctl_cond, &ctl_lock);
		}
		bool want = desired_on;
		pthread_mutex_unlock(&ctl_lock);

		if (want) {
			service_start();
		} else {
			service_stop();
		}

		pthread_mutex_lock(&ctl_lock);
		actual_on = want;
		pthread_mutex_unlock(&ctl_lock);
	}
	return NULL;
}

void dlna_set_enabled(bool on) {
	pthread_mutex_lock(&ctl_lock);

	if (!ctl_thread_started) {
		pthread_t t;
		if (pthread_create(&t, NULL, ctl_worker, NULL) == 0) {
			pthread_detach(t);
			ctl_thread_started = true;
		} else {
			pthread_mutex_unlock(&ctl_lock);
			fprintf(stderr, "dlna: no control thread; DLNA unavailable\n");
			return;
		}
	}

	desired_on = on;
	pthread_cond_signal(&ctl_cond);
	pthread_mutex_unlock(&ctl_lock);
}

bool dlna_get_enabled(void) {
	pthread_mutex_lock(&ctl_lock);
	bool on = desired_on;
	pthread_mutex_unlock(&ctl_lock);
	return on;
}

bool dlna_running(void) {
	pthread_mutex_lock(&ctl_lock);
	bool up = actual_on && service_up;
	pthread_mutex_unlock(&ctl_lock);
	return up;
}

#include "airplay.h"

#include "src/system/core/respath.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "src/system/audio/audio.h"
#include "src/system/device/sysserver.h"

// ---------------------------------------------------------------------------
// The two sockets, and what the firmware puts on them
//
// Almost none of this is what a stock shairport build does; it is the layout of
// /usr/bin/shairport and /usr/bin/hiby_player as they ship.
//
//   PCM  -> /var/run/airplay_socket_server
//           AF_UNIX SOCK_STREAM. Raw interleaved S16_LE, 44100 Hz, stereo, no
//           framing whatsoever -- no header, no length, no timestamps. Writes
//           are one RTP frame's worth at a time, and NOT a fixed size: the
//           rate controller adds or drops one sample per packet, so a write is
//           1404, 1408 or 1412 bytes. shairport connects ONCE, in its init(),
//           and never retries: if nobody is listening at that moment the audio
//           is silently gone for the lifetime of the process. Hence the
//           listeners here are created before sys_server is asked for
//           anything.
//
//   meta -> /var/run/airplay_meta_server
//           AF_UNIX SOCK_STREAM, but one connect + one send + close PER
//           MESSAGE. The message is at most 383 bytes and has one of two
//           shapes:
//               AIRPLAY:TITLE:<decimal length>:<utf-8 text>
//               AIRPLAY:COVER:<path to a file shairport just wrote>
//           and the three flag messages AIRPLAY:CONNECT, AIRPLAY:NO_COVER and
//           AIRPLAY:SERVER_CLOSE carry nothing at all. The artwork is ignored:
//           see handle_meta().
//
// The stock player trusts the declared length and strncpy()s it into a 260
// byte stack buffer. This code takes the shorter of the declared length, what
// actually arrived, and the space available -- same result on every honest
// message, and no smashed stack on a dishonest one.
//
// ---------------------------------------------------------------------------
// Why the audio goes through a ring buffer instead of straight to the DAC
//
// Reads on this socket ARE shairport's clock, and a receiver that stops reading
// stops shairport dead. Its player thread has no sleep, no timer and no
// condvar: it decodes a packet and calls send() on this socket, and the only
// thing pacing that loop is the socket filling up. A reader blocked in
// snd_pcm_writei() waiting for room parks shairport's player thread inside
// send() -- SIGPIPE masked, no SO_SNDTIMEO, blocking fd, so nothing can shake
// it loose.
//
// That only matters once the sender opens a new RTSP connection, which iOS does
// readily at a track change. shairport's SETUP handler then calls
// rtsp_take_player() -> the old connection's exit path -> player_stop() ->
// pthread_join(player_thread). The player thread is in send(), so the join
// never returns, SETUP is never answered, and the phone decides the receiver
// has gone away: one song plays, the next one starts, the device disconnects.
//
// So one thread does nothing but drain the socket into the ring below, and a
// second one feeds the DAC from it.
//
// The ring must NOT simply throw audio away when full. The back-pressure is not
// an accident of the design, it IS shairport's clock: with nothing pushing back
// its player thread free-runs, and buffer_get_frame() answers every call that
// arrives ahead of the network with silence. Fed into a lossy ring, what reaches
// the DAC is real audio shredded with silence.
//
// So a push waits for room and only gives up after three seconds. In ordinary
// listening it waits a few milliseconds and shairport is paced by the DAC. If
// ALSA genuinely wedges the wait expires, the oldest audio goes, and the reader
// carries on -- so the socket is never left unread long enough for shairport's
// SETUP handler to deadlock on it.
// ---------------------------------------------------------------------------

#define PCM_SOCKET "/var/run/airplay_socket_server"
#define META_SOCKET "/var/run/airplay_meta_server"

#define SHAIRPORT_BIN "/usr/bin/shairport"
#define SHAIRPORT_ON "/usr/bin/shairport_on.sh"
#define SHAIRPORT_OFF "/usr/bin/shairport_off.sh"

// Where the firmware keeps the name the device calls itself -- one line,
// "HiBy R3PROII". It is the Bluetooth name and the AirPlay name both; the
// stock player reads this exact file for both.
#define NAME_PATH RESOURCE_DIR "/bt_name"
#define NAME_FALLBACK "HiBy Music"

// shairport is fixed at this and cannot be talked out of it: it refuses any
// stream that is not 16-bit ("only 16-bit samples supported!") and its RAOP
// advertisement offers sr=44100 ss=16 ch=2 and nothing else.
#define AIRPLAY_RATE 44100
#define AIRPLAY_CHANNELS 2
#define AIRPLAY_FRAME_BYTES (AIRPLAY_CHANNELS * 2)

// The stock player opens its output at 32 bits and shifts each 16-bit sample
// up into the top half. Same here: it is the path the hardware is known to
// take on this device, and the DAC is fed the same words it would be fed by
// the firmware.
#define AIRPLAY_OUT_BITS 32

// Wake up often enough that switching AirPlay off feels immediate, rarely
// enough that idle threads cost nothing.
#define POLL_MS 200

// About 190 ms of slack between the socket and the DAC. Small on purpose: with
// back-pressure the ring settles full, so its size is latency.
#define RING_FRAMES 8192
#define RING_BYTES (RING_FRAMES * AIRPLAY_FRAME_BYTES)

// How deep the ALSA buffer is, and why not the usual depth.
//
// The whole AirPlay delay is three pieces:
//
//   ~480 ms  shairport's own pre-roll before it starts playing
//            (`shairport -b 60` in /usr/bin/shairport_on.sh; each unit is one
//            352-frame RTP packet)
//   ~190 ms  the ring above
//   ~200 ms  this
//
// The third is the one worth shrinking. audio_external_begin()'s usual depth --
// eight periods of 4096 frames, about 750 ms -- is sized so that decoding a
// large cover at a track change cannot starve the stream, but with blocking
// writes that buffer settles FULL, so its whole depth is latency.
//
// 200 is shairport-sync's own default for the same thing
// (audio_backend_buffer_desired_length_in_seconds = 0.2), and it is safer here
// than elsewhere because the ring sits in front: a Wi-Fi hiccup eats into that
// before it ever reaches the card.
#define AIRPLAY_ALSA_BUFFER_MS 200

// How long a push waits for room before deciding the output is wedged rather
// than merely busy. Long enough that no ordinary ALSA hiccup reaches it; short
// enough that shairport is never blocked in send() for as long as it takes a
// phone to give up on the receiver.
#define RING_WAIT_MS 3000

// One read, and one write into the DAC. A shade over one RTP packet.
#define CHUNK_BYTES 2048

// ---------------------------------------------------------------------------

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static airplay_state_t state;
static unsigned state_serial;
static int64_t last_audio_ms;

static pthread_mutex_t ctl_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ctl_cond = PTHREAD_COND_INITIALIZER;
static bool ctl_thread_started;
static bool desired_on;
static bool actual_on;

static volatile bool stopping = true;
static pthread_t pcm_thread;
static pthread_t play_thread;
static pthread_t meta_thread;
static bool pcm_thread_live;
static bool play_thread_live;
static bool meta_thread_live;
static int pcm_listen_fd = -1;
static int meta_listen_fd = -1;

// True between accepting the audio socket and losing it. Which, because
// shairport connects once and keeps the socket for its whole life, means "the
// receiver is switched on" and not "a sender is playing" -- see
// airplay_release_output().
static volatile bool stream_live;

// Local playback has asked for the DAC back. Read and cleared by the thread
// that owns the PCM, which is the only one allowed to close it.
static volatile bool release_output;

// Silence after which the output is handed back even though nobody asked. Long
// enough to sit through a gap between two tracks -- releasing there would pop
// the amplifier between every song -- and short enough that a receiver left
// switched on overnight is not holding the DAC awake for nothing.
#define AIRPLAY_IDLE_HANDBACK_MS 5000

static int64_t now_ms(void) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// ---------------------------------------------------------------------------
// state, and who is allowed to touch it
// ---------------------------------------------------------------------------

// End of a session: shairport has gone, or has said it is going. Clearing the
// last track's name keeps it from being shown under "waiting for a
// connection".
static void session_clear(void) {
	pthread_mutex_lock(&state_lock);
	memset(&state, 0, sizeof(state));
	last_audio_ms = 0;
	state_serial++;
	pthread_mutex_unlock(&state_lock);
}

static void state_set_connected(bool connected) {
	pthread_mutex_lock(&state_lock);
	if (state.connected != connected) {
		state.connected = connected;
		state_serial++;
	}
	pthread_mutex_unlock(&state_lock);
}

static void state_set_playing(bool playing) {
	pthread_mutex_lock(&state_lock);
	if (state.playing != playing) {
		state.playing = playing;
		state_serial++;
	}
	if (playing) {
		last_audio_ms = now_ms();
	}
	pthread_mutex_unlock(&state_lock);
}

static void state_set_text(char *field, size_t field_size, const char *value) {
	// Truncation is deliberate and silent: the field is as long as anything
	// worth putting on a 480-pixel screen, and a title that overruns it is
	// still better shown short than not at all.
	size_t len = strlen(value);
	if (len > field_size - 1) {
		len = field_size - 1;
	}

	pthread_mutex_lock(&state_lock);
	if (strlen(field) != len || memcmp(field, value, len) != 0) {
		memcpy(field, value, len);
		field[len] = '\0';
		state_serial++;
	}
	pthread_mutex_unlock(&state_lock);
}

void airplay_get_state(airplay_state_t *out) {
	if (!out) {
		return;
	}
	pthread_mutex_lock(&state_lock);
	*out = state;
	pthread_mutex_unlock(&state_lock);
}

unsigned airplay_serial(void) {
	pthread_mutex_lock(&state_lock);
	unsigned s = state_serial;
	pthread_mutex_unlock(&state_lock);
	return s;
}

int airplay_silent_ms(void) {
	pthread_mutex_lock(&state_lock);
	int64_t last = last_audio_ms;
	pthread_mutex_unlock(&state_lock);

	if (last == 0) {
		return -1; // nothing has ever arrived; not a silence, an absence
	}
	return (int)(now_ms() - last);
}

// ---------------------------------------------------------------------------
// the name
// ---------------------------------------------------------------------------

const char *airplay_name(void) {
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

	// The file ends in a newline, and a trailing space would survive the
	// substitution below as a bare '%' at the end of the shell command.
	size_t len = strlen(name);
	while (len > 0 && (unsigned char)name[len - 1] <= ' ') {
		name[--len] = '\0';
	}

	if (name[0] == '\0') {
		snprintf(name, sizeof(name), "%s", NAME_FALLBACK);
	}
	return name;
}

// sys_server builds its command line with sprintf and hands it to system()
// unquoted -- `/usr/bin/shairport_on.sh HiBy R3PROII` would arrive as two
// arguments and the device would advertise itself as "HiBy". The stock player
// therefore sends the name with spaces replaced by '%', and shairport's own
// -a handler turns them back into spaces before advertising. Neither end is
// optional: sys_server does no substitution at all.
//
// The space is not the only character the shell eats. A '#' after a space
// starts a comment, a '*' is expanded against the working directory, and a ';'
// or a backtick ends the command and starts another one, run as root. What the
// '%' trick does for the space, sysserver_safe_bare() does for the rest --
// after the substitution, so the spaces are already gone by the time it looks.
static void name_for_wire(char *out, size_t out_size) {
	char spaced[128];
	snprintf(spaced, sizeof(spaced), "%s", airplay_name());
	for (char *p = spaced; *p; p++) {
		if (*p == ' ') {
			*p = '%';
		}
	}

	if (sysserver_safe_bare(spaced, out, out_size)) {
		printf("airplay: the name holds characters the daemon's shell would eat; advertising \"%s\"\n", out);
	}
}

bool airplay_available(void) {
	return access(SHAIRPORT_BIN, X_OK) == 0 && access(SHAIRPORT_ON, X_OK) == 0 && access(SHAIRPORT_OFF, X_OK) == 0;
}

// ---------------------------------------------------------------------------
// sockets
// ---------------------------------------------------------------------------

static int make_server(const char *path) {
	unlink(path); // a stale socket file from a crash would make bind() fail

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		fprintf(stderr, "airplay: socket(%s): %s\n", path, strerror(errno));
		return -1;
	}

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "airplay: bind(%s): %s\n", path, strerror(errno));
		close(fd);
		return -1;
	}

	// A deep backlog on purpose. shairport opens a NEW connection for every
	// metadata message and sends five or six of them back to back at each
	// track change -- from the same thread that answers the sender's RTSP
	// requests. A blocking connect() to a full backlog waits forever, and the
	// phone would time out waiting for the reply that connect is holding up.
	if (listen(fd, 32) < 0) {
		fprintf(stderr, "airplay: listen(%s): %s\n", path, strerror(errno));
		close(fd);
		unlink(path);
		return -1;
	}

	// The socket is created by whoever runs the player; shairport runs as the
	// same user in the stock firmware, but a permissive mode costs nothing and
	// saves a silent connect() failure if that ever stops being true.
	chmod(path, 0666);
	return fd;
}

// accept() that gives the stop flag a look every POLL_MS. Returns -1 when
// there is nothing waiting, which the callers treat as "try again".
static int accept_poll(int listen_fd) {
	struct pollfd p = {.fd = listen_fd, .events = POLLIN};
	if (poll(&p, 1, POLL_MS) <= 0) {
		return -1;
	}
	return accept(listen_fd, NULL, NULL);
}

// recv() with the same manners. 0 means the peer went away, -1 means nothing
// arrived this time round.
static int recv_poll(int fd, void *buf, size_t size) {
	struct pollfd p = {.fd = fd, .events = POLLIN};
	int r = poll(&p, 1, POLL_MS);
	if (r <= 0) {
		return -1;
	}
	ssize_t n = recv(fd, buf, size, 0);
	if (n < 0) {
		return (errno == EINTR || errno == EAGAIN) ? -1 : 0;
	}
	return (int)n;
}

// ---------------------------------------------------------------------------
// the ring between the socket and the DAC
// ---------------------------------------------------------------------------

static uint8_t ring[RING_BYTES];
static size_t ring_read; // next byte to play
static size_t ring_fill; // bytes held
static pthread_mutex_t ring_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ring_data = PTHREAD_COND_INITIALIZER;  // something to play
static pthread_cond_t ring_space = PTHREAD_COND_INITIALIZER; // room to write
static size_t dropped_frames;
static int64_t last_drop_log_ms;

// A deadline `ms` from now, for the two timed waits below.
static void deadline_in(struct timespec *out, int ms) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	long usec = tv.tv_usec + (long)ms * 1000;
	out->tv_sec = tv.tv_sec + usec / 1000000;
	out->tv_nsec = (usec % 1000000) * 1000;
}

static void ring_reset(void) {
	pthread_mutex_lock(&ring_lock);
	ring_read = 0;
	ring_fill = 0;
	dropped_frames = 0;
	last_drop_log_ms = 0;
	pthread_mutex_unlock(&ring_lock);
}

// Waits for room, and only stops waiting if the output has plainly stopped
// draining. This wait is the whole point: it is what paces shairport.
static void ring_push(const uint8_t *data, size_t n) {
	if (n > RING_BYTES) {
		data += n - RING_BYTES;
		n = RING_BYTES;
	}

	pthread_mutex_lock(&ring_lock);

	struct timespec until;
	deadline_in(&until, RING_WAIT_MS);
	while (n > RING_BYTES - ring_fill && !stopping) {
		if (pthread_cond_timedwait(&ring_space, &ring_lock, &until) == ETIMEDOUT) {
			break;
		}
	}

	size_t free_bytes = RING_BYTES - ring_fill;
	if (n > free_bytes) {
		size_t drop = n - free_bytes;
		// Whole frames only: dropping an odd number of bytes would swap left
		// and right for the rest of the session.
		drop += (AIRPLAY_FRAME_BYTES - drop % AIRPLAY_FRAME_BYTES) % AIRPLAY_FRAME_BYTES;
		if (drop > ring_fill) {
			drop = ring_fill;
		}
		ring_read = (ring_read + drop) % RING_BYTES;
		ring_fill -= drop;

		// This should now be rare -- it means the DAC did not take a single
		// byte for three seconds -- so it is worth saying, but still not once
		// per packet.
		dropped_frames += drop / AIRPLAY_FRAME_BYTES;
		int64_t now = now_ms();
		if (now - last_drop_log_ms > 2000) {
			last_drop_log_ms = now;
			printf("airplay: the output is behind; %u frames dropped so far\n", (unsigned)dropped_frames);
		}
	}

	size_t write_at = (ring_read + ring_fill) % RING_BYTES;
	size_t first = RING_BYTES - write_at;
	if (first > n) {
		first = n;
	}
	memcpy(ring + write_at, data, first);
	if (n > first) {
		memcpy(ring, data + first, n - first);
	}
	ring_fill += n;

	pthread_cond_signal(&ring_data);
	pthread_mutex_unlock(&ring_lock);
}

// Whole frames only, up to `max`. Waits up to POLL_MS for something to arrive;
// returns 0 when nothing did.
static size_t ring_pop(uint8_t *out, size_t max) {
	pthread_mutex_lock(&ring_lock);

	if (ring_fill < AIRPLAY_FRAME_BYTES && !stopping) {
		struct timespec until;
		deadline_in(&until, POLL_MS);
		pthread_cond_timedwait(&ring_data, &ring_lock, &until);
	}

	size_t n = ring_fill < max ? ring_fill : max;
	n -= n % AIRPLAY_FRAME_BYTES;

	size_t first = RING_BYTES - ring_read;
	if (first > n) {
		first = n;
	}
	memcpy(out, ring + ring_read, first);
	if (n > first) {
		memcpy(out + first, ring, n - first);
	}
	ring_read = (ring_read + n) % RING_BYTES;
	ring_fill -= n;

	if (n > 0) {
		pthread_cond_signal(&ring_space); // the reader may be waiting for this
	}
	pthread_mutex_unlock(&ring_lock);
	return n;
}

// ---------------------------------------------------------------------------
// the socket-draining thread
//
// Its whole job is to never be busy with anything else.
// ---------------------------------------------------------------------------

static void *pcm_worker(void *unused) {
	(void)unused;

	uint8_t buf[CHUNK_BYTES];

	while (!stopping) {
		int fd = accept_poll(pcm_listen_fd);
		if (fd < 0) {
			continue;
		}

		printf("airplay: audio stream connected\n");
		stream_live = true;

		while (!stopping) {
			int n = recv_poll(fd, buf, sizeof(buf));
			if (n == 0) {
				break; // shairport closed, or was killed
			}
			if (n < 0) {
				// A gap. Ordinary between tracks and while a sender is
				// paused, so the device stays open and the page decides what
				// to say about it.
				if (airplay_silent_ms() > 1500) {
					state_set_playing(false);
				}
				continue;
			}

			ring_push(buf, (size_t)n);
			state_set_playing(true);
		}

		stream_live = false;
		close(fd);
		printf("airplay: audio stream closed\n");

		// shairport holds this socket open for its whole life -- it connects
		// once, in init(), and never again -- so losing it means the process
		// is gone, not that a track ended.
		session_clear();
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// the thread that feeds the DAC
// ---------------------------------------------------------------------------

static void *play_worker(void *unused) {
	(void)unused;

	uint8_t chunk[CHUNK_BYTES];
	int32_t wide[CHUNK_BYTES / 2];
	bool device_open = false;
	bool widen = true;

	while (!stopping) {
		size_t n = ring_pop(chunk, sizeof(chunk));

		if (n == 0) {
			// Nothing to play. The DAC goes back when the stream itself is
			// gone, when local playback has asked for it, or when nothing has
			// arrived for long enough that no sender can be mid-song -- but
			// not merely because a track ended, which would pop the amplifier
			// between every song.
			int silent = airplay_silent_ms();
			bool idle = silent >= AIRPLAY_IDLE_HANDBACK_MS;
			if (device_open && (!stream_live || release_output || idle)) {
				printf("airplay: output handed back (%s)\n",
					   !stream_live ? "the stream is gone" : (release_output ? "asked for" : "silent"));
				audio_external_end();
				device_open = false;
			}
			release_output = false;
			continue;
		}

		// Audio again: whoever asked for the device has had their answer, and
		// a request left standing would drop the stream at the next gap.
		release_output = false;

		if (!device_open) {
			// One PCM, one writer. The receiver now runs whether or not its
			// page is open, so a sender can arrive while a local track is
			// playing -- and that track has to give the device up.
			if (audio_get_status() == AUDIO_STATUS_PLAYING) {
				printf("airplay: a sender started; pausing the local track\n");
				audio_pause();
			}

			// 32 bits first, because that is what the stock player asks for
			// and therefore the path this hardware is known to take. If the
			// device will not have it, the samples are already 16-bit and can
			// go out as they came in.
			if (audio_external_begin_latency(AIRPLAY_RATE, AIRPLAY_CHANNELS, AIRPLAY_OUT_BITS,
											 AIRPLAY_ALSA_BUFFER_MS)) {
				widen = true;
			} else if (audio_external_begin_latency(AIRPLAY_RATE, AIRPLAY_CHANNELS, 16, AIRPLAY_ALSA_BUFFER_MS)) {
				widen = false;
				printf("airplay: the output would not take 32 bits; sending 16 straight through\n");
			} else {
				fprintf(stderr, "airplay: could not open the output; dropping what arrives\n");
				continue; // the socket carries on being drained regardless
			}
			device_open = true;
			state_set_connected(true);
		}

		int frames = (int)(n / AIRPLAY_FRAME_BYTES);

		if (widen) {
			const int16_t *src = (const int16_t *)(const void *)chunk;
			for (int i = 0; i < frames * AIRPLAY_CHANNELS; i++) {
				// Left-justified, exactly as the stock player does it: the
				// 16-bit sample becomes the top half of a 32-bit word and the
				// bottom half is zero.
				wide[i] = (int32_t)((uint32_t)(uint16_t)src[i] << 16);
			}
			audio_external_write(wide, frames);
		} else {
			audio_external_write(chunk, frames);
		}
	}

	if (device_open) {
		audio_external_end();
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// the metadata thread
// ---------------------------------------------------------------------------

// "AIRPLAY:TITLE:5:Hello" -> "Hello". The length is decimal ASCII and is the
// byte length of the payload; it is trusted only as far as what actually
// arrived.
static void copy_tagged(const char *msg, size_t msg_len, const char *key, char *out, size_t out_size) {
	const char *p = msg + strlen(key);
	if (*p != ':') {
		return;
	}
	p++;

	char digits[16];
	size_t d = 0;
	while (*p && *p != ':' && d + 1 < sizeof(digits)) {
		if (!isdigit((unsigned char)*p)) {
			return; // not the documented shape; drop it rather than guess
		}
		digits[d++] = *p++;
	}
	digits[d] = '\0';

	if (*p != ':' || d == 0) {
		return;
	}
	p++;

	long declared = strtol(digits, NULL, 10);
	if (declared < 0) {
		return;
	}

	size_t available = msg_len - (size_t)(p - msg);
	size_t len = (size_t)declared;
	if (len > available) {
		len = available;
	}
	if (len > out_size - 1) {
		len = out_size - 1;
	}

	memcpy(out, p, len);
	out[len] = '\0';
}

static void handle_text(const char *msg, size_t msg_len, const char *key, char *field, size_t field_size) {
	char value[AIRPLAY_TEXT_MAX];
	value[0] = '\0';
	copy_tagged(msg, msg_len, key, value, sizeof(value));
	state_set_text(field, field_size, value);
}

static void handle_meta(const char *msg, size_t msg_len) {
	// Prefix order matters: ALBUM would otherwise never be reached, because
	// nothing else shares a prefix with it. Kept in the firmware's own order
	// so a message that matches two keys resolves the same way it would there.
	if (strncmp(msg, "AIRPLAY:ARTIST", 14) == 0) {
		handle_text(msg, msg_len, "AIRPLAY:ARTIST", state.artist, sizeof(state.artist));
	} else if (strncmp(msg, "AIRPLAY:ALBUM", 13) == 0) {
		handle_text(msg, msg_len, "AIRPLAY:ALBUM", state.album, sizeof(state.album));
	} else if (strncmp(msg, "AIRPLAY:GENRE", 13) == 0) {
		handle_text(msg, msg_len, "AIRPLAY:GENRE", state.genre, sizeof(state.genre));
	} else if (strncmp(msg, "AIRPLAY:TITLE", 13) == 0) {
		handle_text(msg, msg_len, "AIRPLAY:TITLE", state.title, sizeof(state.title));
	} else if (strncmp(msg, "AIRPLAY:COVER", 13) == 0) {
		// The artwork is deliberately ignored. shairport writes it to
		// /tmp/cover-<md5>.jpg and then deletes /tmp/*.jpg before every new
		// one, so keeping it means racing that -- and the receiver's screen
		// does not show a picture, so there is nothing to race it for.
	} else if (strncmp(msg, "AIRPLAY:CONNECT", 15) == 0) {
		// Sent once per accepted RTSP connection, not once per session, so
		// duplicates are normal and mean nothing beyond "somebody is there".
		state_set_connected(true);
	} else if (strncmp(msg, "AIRPLAY:NO_COVER", 16) == 0) {
		// ...and likewise nothing to forget.
	} else if (strncmp(msg, "AIRPLAY:OT_CLOSE", 16) == 0) {
		state_set_playing(false);
	} else if (strncmp(msg, "AIRPLAY:SERVER_OPEN", 19) == 0) {
		// Nothing to do -- and nothing in this firmware sends it either.
	} else if (strncmp(msg, "AIRPLAY:SERVER_CLOSE", 20) == 0) {
		// shairport is on its way out: this is the last thing it sends before
		// closing the audio socket.
		session_clear();
	} else {
		printf("airplay: unrecognised metadata '%s'\n", msg);
	}
}

static void *meta_worker(void *unused) {
	(void)unused;

	while (!stopping) {
		int fd = accept_poll(meta_listen_fd);
		if (fd < 0) {
			continue;
		}

		// One connection carries exactly one message and is then closed by
		// the sender -- so a single read is the whole protocol. 384 is
		// shairport's own snprintf() bound; the extra byte holds the
		// terminator it does not send.
		char msg[385];
		memset(msg, 0, sizeof(msg));
		int n = recv_poll(fd, msg, sizeof(msg) - 1);
		close(fd);

		if (n > 0) {
			msg[n] = '\0';
			handle_meta(msg, (size_t)n);
		}
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// starting and stopping the service
//
// All of this runs on the control thread, never on the interface thread:
// sysserver_request() blocks, and shairport takes a moment to come and go.
// ---------------------------------------------------------------------------

static void service_start(void) {
	session_clear();
	ring_reset();
	stream_live = false;
	stopping = false;

	// Bound and listening BEFORE sys_server hears about it. shairport connects
	// once in its init() and never tries again, so losing that race means an
	// AirPlay session that pairs, plays, and is silent.
	pcm_listen_fd = make_server(PCM_SOCKET);
	meta_listen_fd = make_server(META_SOCKET);
	if (pcm_listen_fd < 0 || meta_listen_fd < 0) {
		fprintf(stderr, "airplay: could not open the servers; not starting shairport\n");
		if (pcm_listen_fd >= 0) {
			close(pcm_listen_fd);
			pcm_listen_fd = -1;
			unlink(PCM_SOCKET);
		}
		if (meta_listen_fd >= 0) {
			close(meta_listen_fd);
			meta_listen_fd = -1;
			unlink(META_SOCKET);
		}
		stopping = true;
		return;
	}

	pcm_thread_live = pthread_create(&pcm_thread, NULL, pcm_worker, NULL) == 0;
	play_thread_live = pthread_create(&play_thread, NULL, play_worker, NULL) == 0;
	meta_thread_live = pthread_create(&meta_thread, NULL, meta_worker, NULL) == 0;

	char wire[96];
	name_for_wire(wire, sizeof(wire));

	char command[160];
	snprintf(command, sizeof(command), "SHAIRPORT:TURN_ON:%s", wire);

	char reply[64] = {0};
	sysserver_request(command, reply, sizeof(reply));
	printf("airplay: '%s' -> %s\n", command, reply[0] ? reply : "(no reply)");
}

static void service_stop(void) {
	// Ask first, then tear down. shairport_off.sh sends a plain SIGTERM,
	// which shairport turns into a clean shutdown: its deinit sends
	// AIRPLAY:SERVER_CLOSE and closes the audio socket, so by the time the
	// threads are joined they have usually already noticed.
	char reply[64] = {0};
	sysserver_request("SHAIRPORT:TURN_OFF", reply, sizeof(reply));
	printf("airplay: turn off -> %s\n", reply[0] ? reply : "(no reply)");

	stopping = true;

	pthread_mutex_lock(&ring_lock);
	pthread_cond_broadcast(&ring_data); // let the playback thread out of its wait
	pthread_cond_broadcast(&ring_space); // ...and the reader out of its own
	pthread_mutex_unlock(&ring_lock);

	if (pcm_thread_live) {
		pthread_join(pcm_thread, NULL);
		pcm_thread_live = false;
	}
	if (play_thread_live) {
		pthread_join(play_thread, NULL);
		play_thread_live = false;
	}
	if (meta_thread_live) {
		pthread_join(meta_thread, NULL);
		meta_thread_live = false;
	}

	if (pcm_listen_fd >= 0) {
		close(pcm_listen_fd);
		pcm_listen_fd = -1;
	}
	if (meta_listen_fd >= 0) {
		close(meta_listen_fd);
		meta_listen_fd = -1;
	}
	unlink(PCM_SOCKET);
	unlink(META_SOCKET);

	// Belt and braces: the playback thread hands it back on the way out, but a
	// thread that never started would leave the DAC held open.
	audio_external_end();
	ring_reset();
	session_clear();
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

void airplay_set_enabled(bool on) {
	// Registered here rather than at startup: this is the first moment there is
	// anything to hand back, and it costs nothing to set again.
	audio_set_external_release_cb(airplay_release_output);

	pthread_mutex_lock(&ctl_lock);

	if (!ctl_thread_started) {
		pthread_t t;
		if (pthread_create(&t, NULL, ctl_worker, NULL) == 0) {
			pthread_detach(t);
			ctl_thread_started = true;
		} else {
			pthread_mutex_unlock(&ctl_lock);
			fprintf(stderr, "airplay: no control thread; AirPlay unavailable\n");
			return;
		}
	}

	desired_on = on;
	pthread_cond_signal(&ctl_cond);
	pthread_mutex_unlock(&ctl_lock);
}

bool airplay_get_enabled(void) {
	pthread_mutex_lock(&ctl_lock);
	bool on = desired_on;
	pthread_mutex_unlock(&ctl_lock);
	return on;
}

bool airplay_release_output(void) {
	if (!airplay_running()) {
		return true; // the receiver is not holding it
	}

	// A sender in the middle of a song keeps the output. `playing` is already
	// the "audio arrived in the last second and a half" answer the page shows,
	// which is exactly the question being asked here.
	airplay_state_t now;
	airplay_get_state(&now);
	if (now.playing) {
		return false;
	}

	pthread_mutex_lock(&ring_lock);
	release_output = true;
	pthread_cond_broadcast(&ring_data); // out of its wait, so it lets go now
	pthread_mutex_unlock(&ring_lock);
	return true;
}

bool airplay_running(void) {
	pthread_mutex_lock(&ctl_lock);
	bool up = desired_on && actual_on;
	pthread_mutex_unlock(&ctl_lock);
	return up;
}

// posix_openpt, ptsname_r, pipe2
#define _GNU_SOURCE

#include "btlog.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "src/system/bluetooth/bluetooth.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/core/logging.h"
#include "src/system/core/utils.h"

#define BTLOG_SUBDIR ".local"
#define BTLOG_FILE_NAME "bluetooth.log"
#define BTLOG_OLD_NAME "bluetooth.old.log"

// Past this the file becomes the old one and a new one starts. A failed
// connection is a few hundred lines; this is days of the radio being used with
// the log forgotten on.
#define BTLOG_MAX_BYTES (8L * 1024 * 1024)

#define BTMON_BIN "/usr/bin/btmon"
#define SYSLOG_SOCKET "/dev/log"

// Between the threads that collect lines and the one that writes them: a card
// that stalls for a second must never stall bluetoothd, which waits on its
// syslog() for as long as nobody reads the socket. Lines that find it full
// are counted and the count written instead.
#define RING_SIZE (256 * 1024)

#define LINE_MAX_LEN 512

// btmon pads its packet headers out to the terminal width and cuts the longest
// ones short. Wide enough that nothing is cut; the padding is squeezed out.
#define BTMON_COLUMNS 160

// How often what was written is pushed through to the card.
#define SYNC_EVERY_MS 2000

// A btmon that dies is started again after this, a few times and then not.
#define BTMON_RESTART_MS 5000
#define BTMON_RESTARTS_MAX 5

// A card that will not take the file is asked again after this, not on every
// line.
#define OPEN_RETRY_MS 5000

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------

static pthread_mutex_t control_lock = PTHREAD_MUTEX_INITIALIZER; // start and stop
static bool running;

static pthread_t reader_thread;
static pthread_t writer_thread;
static int wake_pipe[2] = {-1, -1};
static volatile bool reader_stop;

static pthread_mutex_t ring_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ring_cond = PTHREAD_COND_INITIALIZER;
static char ring[RING_SIZE];
static size_t ring_head; // oldest byte
static size_t ring_len;
static unsigned long ring_lost;
static bool accepting; // lines are taken; read by the tap on any thread
static bool writer_stop;
static int stamp_day = -1;

// The file. Held while writing, so a release waits for the write in flight
// and nothing touches the card after it returns.
static pthread_mutex_t file_lock = PTHREAD_MUTEX_INITIALIZER;
static char card_root[PATH_MAX];
static bool card_attached;
static int file_fd = -1;
static off_t file_size;
static long last_open_try_ms = -OPEN_RETRY_MS;
static long last_sync_ms;
static bool unsynced;
static unsigned long away_lines; // dropped while the card was out
static char path_text[PATH_MAX + 32];

static long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// ---------------------------------------------------------------------------
// the ring
// ---------------------------------------------------------------------------

static void ring_put(const char *data, size_t size) {
	size_t tail = (ring_head + ring_len) % RING_SIZE;
	size_t first = RING_SIZE - tail < size ? RING_SIZE - tail : size;
	memcpy(ring + tail, data, first);
	memcpy(ring, data + first, size - first);
	ring_len += size;
}

// One line, stamped the way the player's own log stamps its lines, with a date
// line first whenever the day changes. Never blocks on anything but the
// ring's own lock.
static void push_line(const char *text, size_t len) {
	if (len > LINE_MAX_LEN) {
		len = LINE_MAX_LEN;
	}

	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	time_t seconds = now.tv_sec;
	struct tm local;
	if (!localtime_r(&seconds, &local)) {
		memset(&local, 0, sizeof(local));
	}

	pthread_mutex_lock(&ring_lock);
	if (!accepting) {
		pthread_mutex_unlock(&ring_lock);
		return;
	}

	char day_line[32];
	size_t day_len = 0;
	int day = local.tm_year * 366 + local.tm_yday;
	if (day != stamp_day) {
		day_len = strftime(day_line, sizeof(day_line), "---- %Y-%m-%d ----\n", &local);
	}
	char stamp[24];
	int stamp_len = snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03ld ", local.tm_hour, local.tm_min,
							 local.tm_sec, now.tv_nsec / 1000000L);
	if (stamp_len < 0) {
		stamp_len = 0;
	}

	size_t needed = day_len + (size_t)stamp_len + len + 1;
	if (RING_SIZE - ring_len < needed) {
		ring_lost++;
	} else {
		if (day_len) {
			stamp_day = day;
			ring_put(day_line, day_len);
		}
		ring_put(stamp, (size_t)stamp_len);
		ring_put(text, len);
		ring_put("\n", 1);
		pthread_cond_signal(&ring_cond);
	}
	pthread_mutex_unlock(&ring_lock);
}

static void push_text(const char *text) { push_line(text, strlen(text)); }

// ---------------------------------------------------------------------------
// the file
// ---------------------------------------------------------------------------

static bool file_path(char *out, size_t size, const char *name) {
	int n = snprintf(out, size, "%s/%s/%s", card_root, BTLOG_SUBDIR, name);
	return n > 0 && (size_t)n < size;
}

// Caller holds file_lock.
static void file_close(void) {
	if (file_fd >= 0) {
		if (unsynced) {
			fdatasync(file_fd);
			unsynced = false;
		}
		close(file_fd);
		file_fd = -1;
	}
}

// Caller holds file_lock. Whether the file is open afterwards.
static bool file_open(void) {
	if (file_fd >= 0) {
		return true;
	}
	if (!card_attached || now_ms() - last_open_try_ms < OPEN_RETRY_MS) {
		return false;
	}
	last_open_try_ms = now_ms();

	char folder[PATH_MAX];
	char path[PATH_MAX];
	if (snprintf(folder, sizeof(folder), "%s/%s", card_root, BTLOG_SUBDIR) >= (int)sizeof(folder) ||
		!file_path(path, sizeof(path), BTLOG_FILE_NAME)) {
		return false;
	}
	mkdir(folder, 0755);
	file_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
	if (file_fd < 0) {
		return false;
	}
	struct stat st;
	file_size = fstat(file_fd, &st) == 0 ? st.st_size : 0;
	last_sync_ms = now_ms();
	return true;
}

// Caller holds file_lock and has the file open.
static void file_write_all(const char *data, size_t size) {
	while (size > 0) {
		ssize_t done = write(file_fd, data, size);
		if (done < 0 && errno == EINTR) {
			continue;
		}
		if (done <= 0) {
			return; // the card went away mid-write; the release will follow
		}
		data += done;
		size -= (size_t)done;
		file_size += done;
		unsynced = true;
	}
}

// A note of the file's own: stamped here, since it never went through the ring.
static void file_note(const char *text) {
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	time_t seconds = now.tv_sec;
	struct tm local;
	char line[160];
	int n = 0;
	if (localtime_r(&seconds, &local)) {
		n = snprintf(line, sizeof(line), "%02d:%02d:%02d.%03ld %s\n", local.tm_hour, local.tm_min, local.tm_sec,
					 now.tv_nsec / 1000000L, text);
	}
	if (n > 0 && (size_t)n < sizeof(line)) {
		file_write_all(line, (size_t)n);
	}
}

// Caller holds file_lock and has the file open.
static void file_rotate_if_full(void) {
	if (file_size < BTLOG_MAX_BYTES) {
		return;
	}
	char path[PATH_MAX];
	char old[PATH_MAX];
	if (!file_path(path, sizeof(path), BTLOG_FILE_NAME) || !file_path(old, sizeof(old), BTLOG_OLD_NAME)) {
		return;
	}
	file_close();
	rename(path, old);
	last_open_try_ms = -OPEN_RETRY_MS;
	if (file_open()) {
		file_note("btlog: the previous part is in " BTLOG_OLD_NAME);
	}
}

static unsigned long count_lines(const char *data, size_t size) {
	unsigned long lines = 0;
	for (size_t i = 0; i < size; i++) {
		lines += data[i] == '\n';
	}
	return lines;
}

static void file_write(const char *data, size_t size, unsigned long lost) {
	pthread_mutex_lock(&file_lock);
	if (!file_open()) {
		away_lines += count_lines(data, size) + lost;
		pthread_mutex_unlock(&file_lock);
		return;
	}

	char note[96];
	if (away_lines) {
		snprintf(note, sizeof(note), "btlog: %lu lines lost while the card was away", away_lines);
		file_note(note);
		away_lines = 0;
	}
	if (lost) {
		snprintf(note, sizeof(note), "btlog: %lu lines lost, more than the log could keep up with", lost);
		file_note(note);
	}
	file_write_all(data, size);
	file_rotate_if_full();
	pthread_mutex_unlock(&file_lock);
}

static void file_sync_if_due(bool now) {
	pthread_mutex_lock(&file_lock);
	if (file_fd >= 0 && unsynced && (now || now_ms() - last_sync_ms >= SYNC_EVERY_MS)) {
		fdatasync(file_fd);
		unsynced = false;
		last_sync_ms = now_ms();
	}
	pthread_mutex_unlock(&file_lock);
}

static void *writer_main(void *arg) {
	(void)arg;
	static char out[64 * 1024];

	// Below the interface: the ring absorbs a card that is slow to take the
	// lines, and a log that falls behind loses lines, not the music's turn.
	thread_be_low_priority("btlog writer");

	pthread_mutex_lock(&ring_lock);
	for (;;) {
		if (ring_len == 0 && !ring_lost) {
			if (writer_stop) {
				break;
			}
			struct timespec until;
			clock_gettime(CLOCK_REALTIME, &until);
			until.tv_sec += 1;
			pthread_cond_timedwait(&ring_cond, &ring_lock, &until);
			if (ring_len == 0 && !ring_lost) {
				pthread_mutex_unlock(&ring_lock);
				file_sync_if_due(false);
				pthread_mutex_lock(&ring_lock);
				continue;
			}
		}

		size_t take = ring_len < sizeof(out) ? ring_len : sizeof(out);
		size_t first = RING_SIZE - ring_head < take ? RING_SIZE - ring_head : take;
		memcpy(out, ring + ring_head, first);
		memcpy(out + first, ring, take - first);
		ring_head = (ring_head + take) % RING_SIZE;
		ring_len -= take;
		unsigned long lost = ring_lost;
		ring_lost = 0;
		pthread_mutex_unlock(&ring_lock);

		file_write(out, take, lost);
		file_sync_if_due(false);

		pthread_mutex_lock(&ring_lock);
	}
	pthread_mutex_unlock(&ring_lock);

	file_sync_if_due(true);
	return NULL;
}

// ---------------------------------------------------------------------------
// btmon's output
//
// A packet is a header at the left margin -- "< ACL Data TX: Handle 11 ...",
// "> HCI Event: ..." -- and its decoding indented underneath. What is left
// out is decided per packet:
//
//   - every "Number of Completed Packets" event: the controller acknowledging
//     ACL packets, one per audio packet while streaming, and nothing else;
//   - an ACL packet that decodes to nothing but its channel and raw bytes --
//     the audio, which btmon does not decode without -A, a fragment whose
//     decoding comes with the last piece, or data on a channel opened before
//     btmon started. Kept when the channel line says it is short: signalling
//     is a few bytes, audio hundreds, and a command on a channel btmon never
//     saw opened only shows as bytes;
//   - the audio again, when btmon takes it for signalling. It tells the two
//     AVDTP channels apart by the order it saw them open in, so when the
//     signalling channel predates btmon and the stream is built again under it
//     -- a codec or link-rate change does exactly that -- the new media channel
//     is the first one it knows and every packet on it is "decoded": the RTP
//     header reads as signal 0x20 and up, "AVDTP: Reserved", and the rest of
//     the packet follows as bytes. Under LDAC 990 that is some 180 packets and
//     600 kB of text a second, all of it written to the card: the core never
//     idles and the interface stalls behind it. A long packet whose decoding
//     is a reserved signal is that audio; real signalling is short and never
//     uses a reserved identifier.
//
// Everything else goes through, with the escape sequences of btmon's colours
// taken out and the header padding squeezed.
// ---------------------------------------------------------------------------

// Signalling is never longer than this on a channel btmon cannot decode;
// audio is never this short.
#define SHORT_PAYLOAD_MAX 64

typedef enum {
	BLOCK_PASS,
	BLOCK_HOLD, // an ACL packet whose fate is not known yet
	BLOCK_DROP,
} block_state_t;

typedef struct {
	block_state_t state;
	char held[2][LINE_MAX_LEN + 1]; // the header and the channel line
	int held_count;
	bool held_long; // the packet's channel line said it is longer than signalling ever is
	unsigned long skipped; // packets left out since the last line that went through
	char line[LINE_MAX_LEN + 1]; // the line being read, colours already out
	size_t line_len;
	int escape; // 0 outside an escape sequence, 1 after ESC, 2 inside CSI
} monitor_filter_t;

static monitor_filter_t monitor;

static bool starts_with(const char *s, const char *prefix) { return strncmp(s, prefix, strlen(prefix)) == 0; }

static void monitor_emit(const char *line) {
	if (monitor.skipped) {
		char note[64];
		snprintf(note, sizeof(note), "      (%lu data packets not shown)", monitor.skipped);
		push_text(note);
		monitor.skipped = 0;
	}
	push_text(line);
}

static void monitor_release_held(void) {
	for (int i = 0; i < monitor.held_count; i++) {
		monitor_emit(monitor.held[i]);
	}
	monitor.held_count = 0;
}

static void monitor_hold(const char *line) {
	if (monitor.held_count < 2) {
		snprintf(monitor.held[monitor.held_count++], sizeof(monitor.held[0]), "%s", line);
	}
}

// "Channel: 65 len 2 [PSM 25 mode Basic (0x00)] {chan 0}" -> 2, or -1.
static long channel_payload(const char *line) {
	const char *at = strstr(line, "Channel:");
	if (!at) {
		return -1;
	}
	at = strstr(at, " len ");
	return at ? strtol(at + 5, NULL, 10) : -1;
}

// "        12 01 04 08                     ...." -- raw bytes.
static bool hexdump_line(const char *line) {
	while (*line == ' ') {
		line++;
	}
	bool hex0 = (line[0] >= '0' && line[0] <= '9') || (line[0] >= 'a' && line[0] <= 'f');
	bool hex1 = (line[1] >= '0' && line[1] <= '9') || (line[1] >= 'a' && line[1] <= 'f');
	return hex0 && hex1 && line[2] == ' ' && (line[3] == ' ' || (line[3] && line[4] && line[5] == ' '));
}

// "      AVDTP: Reserved (0x20) Command (0x00) type 0x00 label 8 nosp 0" -- an
// RTP header read as AVDTP signalling. Valid signal identifiers stop at 0x0d.
static bool media_as_signalling(const char *line) {
	while (*line == ' ') {
		line++;
	}
	return starts_with(line, "AVDTP: Reserved (0x");
}

// Runs of three or more spaces down to two, in place.
static void squeeze_padding(char *line) {
	char *out = line;
	int run = 0;
	for (char *in = line; *in; in++) {
		run = *in == ' ' ? run + 1 : 0;
		if (run <= 2) {
			*out++ = *in;
		}
	}
	*out = '\0';
}

static void monitor_line(char *line) {
	if (line[0] == '\0') {
		return;
	}
	if (line[0] != ' ') {
		// A new packet: the one before it is decided now if it was not yet.
		if (monitor.state == BLOCK_HOLD) {
			monitor.skipped++;
		}
		monitor.held_count = 0;
		monitor.held_long = false;

		squeeze_padding(line);
		if (starts_with(line, "> HCI Event: Number of Completed Packets")) {
			monitor.state = BLOCK_DROP;
		} else if (starts_with(line, "< ACL Data") || starts_with(line, "> ACL Data")) {
			monitor.state = BLOCK_HOLD;
			monitor_hold(line);
		} else {
			monitor.state = BLOCK_PASS;
			monitor_emit(line);
		}
		return;
	}

	switch (monitor.state) {
	case BLOCK_DROP:
		return;
	case BLOCK_PASS:
		monitor_emit(line);
		return;
	case BLOCK_HOLD:
		break;
	}

	long payload = channel_payload(line);
	if (payload > SHORT_PAYLOAD_MAX) {
		monitor.held_long = true;
		monitor_hold(line);
		return;
	}
	if (monitor.held_long && media_as_signalling(line)) {
		// Audio that btmon decoded as signalling: left out, bytes and all.
		monitor.state = BLOCK_DROP;
		monitor.held_count = 0;
		monitor.skipped++;
		return;
	}
	if (payload < 0 && hexdump_line(line)) {
		return;
	}
	// Decoded, or short enough to be signalling: the whole packet goes out.
	monitor_release_held();
	monitor.state = BLOCK_PASS;
	monitor_emit(line);
}

// Bytes as they come off the terminal, in any pieces.
static void monitor_feed(const char *data, size_t size) {
	for (size_t i = 0; i < size; i++) {
		unsigned char c = (unsigned char)data[i];
		if (monitor.escape == 1) {
			monitor.escape = c == '[' ? 2 : 0;
			continue;
		}
		if (monitor.escape == 2) {
			if (c >= 0x40 && c <= 0x7e) {
				monitor.escape = 0;
			}
			continue;
		}
		if (c == 0x1b) {
			monitor.escape = 1;
			continue;
		}
		if (c == '\r') {
			continue;
		}
		if (c == '\n') {
			while (monitor.line_len > 0 && monitor.line[monitor.line_len - 1] == ' ') {
				monitor.line_len--; // the padding after a hex dump's text column
			}
			monitor.line[monitor.line_len] = '\0';
			monitor_line(monitor.line);
			monitor.line_len = 0;
			continue;
		}
		if (monitor.line_len < LINE_MAX_LEN) {
			monitor.line[monitor.line_len++] = (char)c;
		}
	}
}

static void monitor_reset(void) { memset(&monitor, 0, sizeof(monitor)); }

// ---------------------------------------------------------------------------
// btmon
// ---------------------------------------------------------------------------

static pid_t btmon_pid = -1;
static int btmon_fd = -1; // the terminal's master side
static long btmon_next_start_ms;
static int btmon_restarts;
static bool btmon_given_up;

static void btmon_start(void) {
	int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (master < 0) {
		push_text("btlog: no pseudo-terminal for btmon; HCI traffic not logged");
		btmon_given_up = true;
		return;
	}
	char name[64];
	int slave = -1;
	if (grantpt(master) == 0 && unlockpt(master) == 0 && ptsname_r(master, name, sizeof(name)) == 0) {
		slave = open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
	}
	if (slave < 0) {
		close(master);
		push_text("btlog: the pseudo-terminal for btmon would not open; HCI traffic not logged");
		btmon_given_up = true;
		return;
	}

	// Plain bytes out: no newline turned into CR LF, nothing echoed back.
	struct termios tio;
	if (tcgetattr(slave, &tio) == 0) {
		tio.c_oflag &= ~(tcflag_t)OPOST;
		tio.c_lflag &= ~(tcflag_t)(ECHO | ICANON | ISIG | IEXTEN);
		tcsetattr(slave, TCSANOW, &tio);
	}
	struct winsize ws = {.ws_row = 50, .ws_col = BTMON_COLUMNS};
	ioctl(slave, TIOCSWINSZ, &ws);

	long open_max = sysconf(_SC_OPEN_MAX);
	if (open_max < 0 || open_max > 4096) {
		open_max = 4096;
	}
	pid_t parent = getpid();

	pid_t pid = fork();
	if (pid < 0) {
		close(slave);
		close(master);
		btmon_next_start_ms = now_ms() + BTMON_RESTART_MS;
		return;
	}
	if (pid == 0) {
		// Dies with the thread that started it: the log being switched off, or
		// the player going away.
		prctl(PR_SET_PDEATHSIG, SIGTERM);
		if (getppid() != parent) {
			_exit(0);
		}
		setsid();
		// Below the interface and the audio. Every packet on the air is a few
		// lines of its text, and a btmon that cannot keep up only loses
		// monitor packets -- the kernel drops what its socket cannot hold --
		// where one at the interface's level takes the core from the player.
		setpriority(PRIO_PROCESS, 0, 10);
		// Away from wherever the player stands, which can be the card.
		if (chdir("/") != 0) {
			_exit(126);
		}
		int devnull = open("/dev/null", O_RDONLY);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
		}
		dup2(slave, STDOUT_FILENO);
		dup2(slave, STDERR_FILENO);
		// Nothing of the player's: an inherited descriptor on the card would
		// keep it busy through every unmount for as long as btmon runs.
		for (int fd = 3; fd < open_max; fd++) {
			close(fd);
		}
		char *argv[] = {(char *)"btmon", (char *)"--no-pager", NULL};
		execv(BTMON_BIN, argv);
		_exit(127);
	}

	close(slave);
	btmon_pid = pid;
	btmon_fd = master;
	monitor_reset();
}

// btmon is gone, or about to be: the terminal said so.
static void btmon_reap(bool asked) {
	if (btmon_fd >= 0) {
		close(btmon_fd);
		btmon_fd = -1;
	}
	if (btmon_pid <= 0) {
		return;
	}

	if (asked) {
		kill(btmon_pid, SIGTERM);
	}
	int status = 0;
	pid_t got = 0;
	for (int waited = 0; waited < 1500; waited += 50) {
		got = waitpid(btmon_pid, &status, WNOHANG);
		if (got != 0) {
			break;
		}
		struct timespec pause = {0, 50 * 1000000L};
		nanosleep(&pause, NULL);
	}
	if (got == 0) {
		kill(btmon_pid, SIGKILL);
		waitpid(btmon_pid, &status, 0);
	}
	btmon_pid = -1;
	if (asked) {
		return;
	}

	if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
		push_text("btlog: there is no " BTMON_BIN " on this firmware; HCI traffic not logged");
		btmon_given_up = true;
		return;
	}
	char note[96];
	if (WIFSIGNALED(status)) {
		snprintf(note, sizeof(note), "btlog: btmon was killed by signal %d", WTERMSIG(status));
	} else {
		snprintf(note, sizeof(note), "btlog: btmon exited with status %d", WEXITSTATUS(status));
	}
	push_text(note);
	if (++btmon_restarts > BTMON_RESTARTS_MAX) {
		push_text("btlog: btmon keeps stopping; HCI traffic no longer logged");
		btmon_given_up = true;
		return;
	}
	btmon_next_start_ms = now_ms() + BTMON_RESTART_MS;
}

// ---------------------------------------------------------------------------
// syslog: bluetoothd and bluealsa
// ---------------------------------------------------------------------------

static int syslog_fd = -1;
static bool syslog_ours; // /dev/log is the socket bound here

static void syslog_open(void) {
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SYSLOG_SOCKET);

	// A syslogd of somebody else's is left alone. A socket file that nobody
	// answers on is this module's own from a player that did not stop cleanly.
	int probe = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (probe >= 0) {
		bool answered = connect(probe, (struct sockaddr *)&addr, sizeof(addr)) == 0;
		close(probe);
		if (answered) {
			push_text("btlog: " SYSLOG_SOCKET " already has a reader; bluetoothd's own lines go there");
			return;
		}
	}

	unlink(SYSLOG_SOCKET);
	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		return;
	}
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		char note[128];
		snprintf(note, sizeof(note), "btlog: cannot listen on " SYSLOG_SOCKET ": %s", strerror(errno));
		close(fd);
		push_text(note);
		return;
	}
	chmod(SYSLOG_SOCKET, 0666);
	int buffer = 256 * 1024;
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
	syslog_fd = fd;
	syslog_ours = true;
}

static void syslog_close(void) {
	if (syslog_fd >= 0) {
		close(syslog_fd);
		syslog_fd = -1;
	}
	if (syslog_ours) {
		unlink(SYSLOG_SOCKET);
		syslog_ours = false;
	}
}

// "<30>Oct  2 23:30:01 bluetoothd[812]: text" -> "bluetoothd[812]: text",
// for the daemons this log is about.
static void syslog_message(char *msg, size_t len) {
	msg[len] = '\0';
	while (len > 0 && (msg[len - 1] == '\n' || msg[len - 1] == '\0')) {
		msg[--len] = '\0';
	}
	char *p = msg;
	if (*p == '<') {
		char *close = strchr(p, '>');
		if (close) {
			p = close + 1;
		}
	}
	if (strlen(p) > 16 && p[3] == ' ' && p[6] == ' ' && p[9] == ':' && p[12] == ':' && p[15] == ' ') {
		p += 16;
	}
	if (starts_with(p, "bluetoothd") || starts_with(p, "bluealsa") || starts_with(p, "dbus")) {
		push_text(p);
	}
}

// ---------------------------------------------------------------------------
// the player's own lines
// ---------------------------------------------------------------------------

static const char *const player_prefixes[] = {
	"bluetooth:", "btstack:", "btreceiver:", "btvolume:", "btplayer:", "btaudio:", "airpods:", "dbus:",
};

static void player_tap(const char *line, size_t len) {
	if (!__atomic_load_n(&accepting, __ATOMIC_RELAXED)) {
		return;
	}
	for (size_t i = 0; i < sizeof(player_prefixes) / sizeof(player_prefixes[0]); i++) {
		size_t plen = strlen(player_prefixes[i]);
		if (len >= plen && memcmp(line, player_prefixes[i], plen) == 0) {
			push_line(line, len);
			return;
		}
	}
}

// ---------------------------------------------------------------------------
// the reader
// ---------------------------------------------------------------------------

static void *reader_main(void *arg) {
	(void)arg;
	char buffer[4096];

	// Named for the watchdog's thread dump. Left at the interface's level:
	// bluetoothd waits in syslog() for as long as this does not read.
	prctl(PR_SET_NAME, "btlog reader", 0, 0, 0);

	syslog_open();

	while (!reader_stop) {
		if (btmon_pid <= 0 && !btmon_given_up && now_ms() >= btmon_next_start_ms) {
			btmon_start();
		}

		struct pollfd fds[3];
		int count = 0;
		fds[count++] = (struct pollfd){.fd = wake_pipe[0], .events = POLLIN};
		int btmon_at = -1;
		int syslog_at = -1;
		if (btmon_fd >= 0) {
			btmon_at = count;
			fds[count++] = (struct pollfd){.fd = btmon_fd, .events = POLLIN};
		}
		if (syslog_fd >= 0) {
			syslog_at = count;
			fds[count++] = (struct pollfd){.fd = syslog_fd, .events = POLLIN};
		}

		if (poll(fds, (nfds_t)count, 1000) < 0) {
			if (errno != EINTR) {
				break;
			}
			continue;
		}

		if (fds[0].revents) {
			while (read(wake_pipe[0], buffer, sizeof(buffer)) > 0) {
			}
		}

		if (btmon_at >= 0 && fds[btmon_at].revents) {
			ssize_t got = read(btmon_fd, buffer, sizeof(buffer));
			if (got > 0) {
				monitor_feed(buffer, (size_t)got);
			} else if (got == 0 || (errno != EINTR && errno != EAGAIN)) {
				btmon_reap(false); // EIO: the last holder of the terminal is gone
			}
		}

		if (syslog_at >= 0 && fds[syslog_at].revents) {
			for (;;) {
				ssize_t got = recv(syslog_fd, buffer, sizeof(buffer) - 1, MSG_DONTWAIT);
				if (got <= 0) {
					break;
				}
				syslog_message(buffer, (size_t)got);
			}
		}
	}

	btmon_reap(true);
	syslog_close();
	return NULL;
}

// ---------------------------------------------------------------------------
// on and off
// ---------------------------------------------------------------------------

static void start_locked(void) {
	if (running) {
		return;
	}
	if (pipe2(wake_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
		fprintf(stderr, "btlog: no pipe: %s\n", strerror(errno));
		return;
	}

	pthread_mutex_lock(&ring_lock);
	ring_head = 0;
	ring_len = 0;
	ring_lost = 0;
	stamp_day = -1;
	writer_stop = false;
	__atomic_store_n(&accepting, true, __ATOMIC_RELAXED);
	pthread_mutex_unlock(&ring_lock);

	reader_stop = false;
	btmon_given_up = false;
	btmon_restarts = 0;
	btmon_next_start_ms = 0;

	push_text("btlog: on -- btmon, bluetoothd and bluealsa through syslog, and the player's own Bluetooth lines");

	if (pthread_create(&writer_thread, NULL, writer_main, NULL) != 0) {
		fprintf(stderr, "btlog: could not start the writer\n");
		__atomic_store_n(&accepting, false, __ATOMIC_RELAXED);
		close(wake_pipe[0]);
		close(wake_pipe[1]);
		wake_pipe[0] = wake_pipe[1] = -1;
		return;
	}
	if (pthread_create(&reader_thread, NULL, reader_main, NULL) != 0) {
		fprintf(stderr, "btlog: could not start the reader\n");
		pthread_mutex_lock(&ring_lock);
		writer_stop = true;
		pthread_cond_signal(&ring_cond);
		pthread_mutex_unlock(&ring_lock);
		pthread_join(writer_thread, NULL);
		__atomic_store_n(&accepting, false, __ATOMIC_RELAXED);
		close(wake_pipe[0]);
		close(wake_pipe[1]);
		wake_pipe[0] = wake_pipe[1] = -1;
		return;
	}
	running = true;
	printf("btlog: Bluetooth log on\n");
}

static void stop_locked(void) {
	if (!running) {
		return;
	}
	running = false;

	reader_stop = true;
	ssize_t woke = write(wake_pipe[1], "x", 1);
	(void)woke;
	pthread_join(reader_thread, NULL);

	push_text("btlog: off");
	pthread_mutex_lock(&ring_lock);
	__atomic_store_n(&accepting, false, __ATOMIC_RELAXED);
	writer_stop = true;
	pthread_cond_signal(&ring_cond);
	pthread_mutex_unlock(&ring_lock);
	pthread_join(writer_thread, NULL);

	close(wake_pipe[0]);
	close(wake_pipe[1]);
	wake_pipe[0] = wake_pipe[1] = -1;

	pthread_mutex_lock(&file_lock);
	file_close();
	away_lines = 0;
	pthread_mutex_unlock(&file_lock);
	printf("btlog: Bluetooth log off\n");
}

bool btlog_enabled(void) { return config_get_bool("bluetooth", "log", false); }

// Not on a development machine: /dev/log and the HCI monitor there are the
// developer's own.
static bool btlog_supported(void) {
#ifdef HOST_BUILD
	return false;
#else
	return true;
#endif
}

void btlog_init(void) {
	logging_set_line_tap(player_tap);
	if (btlog_supported() && btlog_enabled()) {
		pthread_mutex_lock(&control_lock);
		start_locked();
		pthread_mutex_unlock(&control_lock);
		// A bluetoothd that outlived the previous run of the player, or that
		// the firmware started, was not started with -d.
		bluetooth_daemon_debug_on();
	}
}

void btlog_set_enabled(bool enabled) {
	config_set_bool("bluetooth", "log", enabled);
	config_save();
	if (!btlog_supported()) {
		return;
	}
	pthread_mutex_lock(&control_lock);
	if (enabled) {
		start_locked();
	} else {
		stop_locked();
	}
	pthread_mutex_unlock(&control_lock);
	// A bluetoothd started with the log off has its debug lines off; this
	// turns them on without restarting it, which would drop the headphones.
	if (enabled) {
		bluetooth_daemon_debug_on();
	}
}

const char *btlog_path(void) {
	if (!btlog_enabled()) {
		return tr("log_disabled");
	}
	pthread_mutex_lock(&file_lock);
	bool attached = card_attached;
	if (attached) {
		snprintf(path_text, sizeof(path_text), "%s/%s/%s", card_root, BTLOG_SUBDIR, BTLOG_FILE_NAME);
	}
	pthread_mutex_unlock(&file_lock);
	return attached ? path_text : tr("log_card_missing");
}

void btlog_card_release(void) {
	pthread_mutex_lock(&file_lock);
	file_close();
	card_attached = false;
	pthread_mutex_unlock(&file_lock);
}

void btlog_card_attach(const char *sd_root) {
	if (!sd_root || !sd_root[0]) {
		return;
	}
	pthread_mutex_lock(&file_lock);
	if (!card_attached || strcmp(card_root, sd_root) != 0) {
		file_close();
		snprintf(card_root, sizeof(card_root), "%s", sd_root);
		card_attached = true;
		last_open_try_ms = -OPEN_RETRY_MS;
	}
	pthread_mutex_unlock(&file_lock);
}

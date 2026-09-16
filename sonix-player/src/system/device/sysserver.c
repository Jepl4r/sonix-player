#include "sysserver.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "src/system/core/utils.h"

// Where sys_server binds. Taken straight out of the stock binary.
#define SYSSERVER_SOCKET "/var/run/sys_server"

// ...and where it expects to find the client, to push notifications back.
#define SYSCLIENT_SOCKET "/var/run/sys_client"

// The mount path must not hold up startup: if the daemon does not answer
// promptly it is not going to, and the player mounts the card itself instead.
// The radios are the opposite case: sys_server runs bt-adapter, bt-device and
// wpa_cli through a blocking system(), so a scan legitimately takes ten seconds
// and a pairing longer. A two-second socket timeout abandoned commands that
// were about to succeed, hence sysserver_request_timeout().
#define SYSSERVER_TIMEOUT_MS 2000

// MSG_NOSIGNAL keeps a closed connection from killing the process. Defined
// defensively: where libc lacks it, the explicit SIGPIPE handling in main.c is
// the remaining protection.
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

// Whether a daemon reply means the command was accepted.
//
// Every handler in sys_server answers with the bare string "OK" or "FAIL": an
// acknowledgement that the command was accepted, not its outcome. The outcome
// arrives later on the notification socket, in the "<COMMAND>:OK" form. Both
// forms are accepted here -- testing only for ":OK" rejects every direct reply,
// which sends every caller down its fallback path (two pairings, two mounts,
// two of everything).
bool sysserver_reply_ok(const char *reply) {
	if (!reply || !reply[0]) {
		return false;
	}
	if (strstr(reply, "FAIL") != NULL) {
		return false;
	}
	return strncmp(reply, "OK", 2) == 0 || strstr(reply, ":OK") != NULL;
}

// A command as it may be written down.
//
// WIFI:SSID_CONNECT:wlan0:<name>:<passphrase> carries the user's Wi-Fi password
// in clear, and this line went into the log the user then sends on when
// something goes wrong. The passphrase is the last field, and on that path the
// name is guaranteed not to contain a colon, so everything after the fourth one
// is the secret.
static const char *safe_command(const char *command, char *out, size_t out_size) {
	static const char PREFIX[] = "WIFI:SSID_CONNECT:";
	if (strncmp(command, PREFIX, sizeof(PREFIX) - 1) != 0) {
		return command;
	}

	const char *cut = strchr(command + sizeof(PREFIX) - 1, ':'); // past the interface
	cut = cut ? strchr(cut + 1, ':') : NULL;					 // past the name
	if (!cut) {
		return command; // no passphrase field: nothing to hide
	}

	int kept = (int)(cut - command);
	snprintf(out, out_size, "%.*s:***", kept, command);
	return out;
}

// Copies `text`, putting '_' wherever a character would not survive the shell
// the daemon runs its command in. See sysserver.h for which shell, and why a
// name is repaired where a network name is refused.
static bool sanitise(const char *text, char *out, size_t out_size, const char *unsafe) {
	bool changed = false;
	size_t at = 0;

	if (!out || out_size == 0) {
		return false;
	}
	for (const char *p = text ? text : ""; *p && at + 1 < out_size; p++) {
		unsigned char c = (unsigned char)*p;
		// Control characters go too: a newline in the middle of a command line
		// is a second command.
		if (c < ' ' || strchr(unsafe, c) != NULL) {
			out[at++] = '_';
			changed = true;
		} else {
			out[at++] = (char)c;
		}
	}
	out[at] = '\0';
	return changed;
}

// Inside real double quotes the shell still reads four things: the closing
// quote, a backslash, a dollar (expansion) and a backtick (substitution).
// Everything else -- spaces, #, ;, & -- is already just text there.
bool sysserver_safe_quoted(const char *text, char *out, size_t out_size) {
	return sanitise(text, out, out_size, "\"\\$`");
}

// Bare, nothing is text: whitespace splits the argument, # opens a comment,
// and a semicolon or a backtick would run as a command of its own, as root.
bool sysserver_safe_bare(const char *text, char *out, size_t out_size) {
	return sanitise(text, out, out_size, "\"\\$`;&|<>()'*?[]{}#~! \t");
}

static int g_timeout_ms = SYSSERVER_TIMEOUT_MS;

static int sysserver_connect(void) {
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		return -1;
	}
	// Close-on-exec. bluetooth.c starts bluetoothd and bluealsa from a
	// fork of this process, and no descriptor of the player's is marked, so
	// without this line those daemons carry a copy of this socket for the rest
	// of their lives. For the listening one that is also why restarting the
	// player alone is not enough -- the node is recreated while the daemons
	// still hold the old one open. fcntl rather than SOCK_CLOEXEC: it does not
	// depend on which feature macros the target's libc happens to define.
	(void)fcntl(fd, F_SETFD, FD_CLOEXEC);

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, SYSSERVER_SOCKET, sizeof(addr.sun_path) - 1);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}

	// Never block the startup path on a daemon that may be wedged.
	struct timeval tv = {.tv_sec = g_timeout_ms / 1000, .tv_usec = (g_timeout_ms % 1000) * 1000};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	return fd;
}

bool sysserver_available(void) {
	if (getenv("SONIX_NO_SYSSERVER")) {
		return false;
	}

	int fd = sysserver_connect();
	if (fd < 0) {
		return false;
	}
	close(fd);
	return true;
}

// sysserver_request() with a socket timeout other than the default, which is
// restored afterwards.
int sysserver_request_timeout(const char *command, char *reply, size_t reply_size, int timeout_ms) {
	g_timeout_ms = timeout_ms > 0 ? timeout_ms : SYSSERVER_TIMEOUT_MS;
	int rc = sysserver_request(command, reply, reply_size);
	g_timeout_ms = SYSSERVER_TIMEOUT_MS;
	return rc;
}

int sysserver_request(const char *command, char *reply, size_t reply_size) {
	// Callers that do not care about the text still need somewhere for the
	// reply to land: the daemon writes one whether it is read or not.
	char discard[128];
	if (!reply || reply_size < 2) {
		reply = discard;
		reply_size = sizeof(discard);
	}

	if (getenv("SONIX_NO_SYSSERVER")) {
		return -1;
	}

	int fd = sysserver_connect();
	if (fd < 0) {
		return -1;
	}

	// The daemon parses its commands with sscanf on a C string, so the
	// terminator goes over the wire with the rest of it.
	//
	// MSG_NOSIGNAL matters here: without it, a daemon that closes the connection
	// (wrong protocol version, busy) raises SIGPIPE, whose default action kills
	// the process, and on this device a player that dies gets the whole machine
	// rebooted by hiby_player.sh.
	char shown[256];
	const char *safe = safe_command(command, shown, sizeof(shown));

	size_t len = strlen(command) + 1;
	if (send(fd, command, len, MSG_NOSIGNAL) != (ssize_t)len) {
		fprintf(stderr, "sysserver: sending \"%s\" failed: %s\n", safe, strerror(errno));
		close(fd);
		return -1;
	}

	ssize_t got = recv(fd, reply, reply_size - 1, 0);
	close(fd);

	if (got <= 0) {
		fprintf(stderr, "sysserver: no reply to \"%s\": %s\n", safe,
				got == 0 ? "connection closed" : strerror(errno));
		return -1;
	}

	reply[got] = '\0';
	// Replies may be NUL-separated; only the first field matters here.
	printf("sysserver: \"%s\" -> \"%s\"\n", safe, reply);

	return sysserver_reply_ok(reply) ? 0 : -1;
}

int sysserver_mount(const char *device, const char *mount_point) {
	char command[256];
	char reply[256];

	snprintf(command, sizeof(command), "MOUNT:MOUNT:%s %s", device, mount_point);
	return sysserver_request(command, reply, sizeof(reply));
}

int sysserver_umount(const char *mount_point) {
	char command[256];
	char reply[256];

	snprintf(command, sizeof(command), "MOUNT:UMOUNT:%s", mount_point);
	return sysserver_request(command, reply, sizeof(reply));
}

// ---------------------------------------------------------------------------
// The notification socket
// ---------------------------------------------------------------------------

static void (*g_listen_cb)(const char *message);
static int g_listen_fd = -1;

static void *sysserver_listen_thread(void *arg) {
	(void)arg;
	thread_be_background("sysclient");

	int transient_errno = 0;

	for (;;) {
		int client = accept(g_listen_fd, NULL, NULL);
		if (client < 0) {
			if (errno == EINTR) {
				continue;
			}
			// Not every failure here is the end of the socket, and treating
			// them all as one was: this thread is the ONLY writer of the
			// Bluetooth audio state (bluetooth.c, notification_cb), and
			// bluetooth.c latches g_have_notifications the first time one
			// arrives -- so once this returned, the paired-list poll was
			// ignored for ever and the routing froze on whatever it last knew.
			// A connection aborted between the client's connect and this
			// accept, or a moment without descriptors, is not that.
			bool fatal = errno == EBADF || errno == EINVAL || errno == ENOTSOCK || errno == EOPNOTSUPP;
			if (!fatal) {
				if (transient_errno != errno) {
					transient_errno = errno; // said on the first of a run, not once per retry
					fprintf(stderr, "sysclient: accept failed: %s; retrying\n", strerror(errno));
				}
				usleep(100 * 1000);
				continue;
			}
			fprintf(stderr, "sysclient: accept failed for good: %s\n", strerror(errno));
			break;
		}
		transient_errno = 0;

		char buf[1024];
		ssize_t got = recv(client, buf, sizeof(buf) - 1, 0);
		if (got > 0) {
			buf[got] = '\0';
			// The senders terminate their strings; anything past the first NUL
			// is a second message in the same packet.
			ssize_t offset = 0;
			while (offset < got) {
				const char *message = buf + offset;
				size_t length = strlen(message);
				if (length && g_listen_cb) {
					g_listen_cb(message);
				}
				offset += (ssize_t)length + 1;
				if (!length) {
					break;
				}
			}
			// The stock player answers with two bytes before closing; the
			// senders do not read it, but there is no reason to differ.
			ssize_t ignored = send(client, "\0\0", 2, MSG_NOSIGNAL);
			(void)ignored;
		}
		close(client);
	}

	close(g_listen_fd);
	g_listen_fd = -1;
	return NULL;
}

void sysserver_listen(void (*cb)(const char *message)) {
	if (g_listen_fd >= 0 || !cb || getenv("SONIX_NO_SYSSERVER")) {
		return;
	}

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		return;
	}
	// Close-on-exec. bluetooth.c starts bluetoothd and bluealsa from a
	// fork of this process, and no descriptor of the player's is marked, so
	// without this line those daemons carry a copy of this socket for the rest
	// of their lives. For the listening one that is also why restarting the
	// player alone is not enough -- the node is recreated while the daemons
	// still hold the old one open. fcntl rather than SOCK_CLOEXEC: it does not
	// depend on which feature macros the target's libc happens to define.
	(void)fcntl(fd, F_SETFD, FD_CLOEXEC);

	// A previous run (or the stock player) may have left the node behind: a
	// UNIX socket file is not removed when its owner dies, and bind() would
	// fail with EADDRINUSE against a corpse.
	unlink(SYSCLIENT_SOCKET);

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, SYSCLIENT_SOCKET, sizeof(addr.sun_path) - 1);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "sysclient: cannot bind %s: %s\n", SYSCLIENT_SOCKET, strerror(errno));
		close(fd);
		return;
	}
	if (listen(fd, 10) < 0) {
		close(fd);
		return;
	}

	g_listen_cb = cb;
	g_listen_fd = fd;

	pthread_t thread;
	if (pthread_create(&thread, NULL, sysserver_listen_thread, NULL) != 0) {
		close(fd);
		g_listen_fd = -1;
		return;
	}
	pthread_detach(thread);

	printf("sysclient: listening on %s for daemon notifications\n", SYSCLIENT_SOCKET);
}

#include "dbuslite.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

// The two places the system bus socket is found. The address in the
// environment wins when it is set, which is what the bench uses to point this
// at a daemon of its own.
static const char *const SOCKET_PATHS[] = {
	"/var/run/dbus/system_bus_socket",
	"/run/dbus/system_bus_socket",
};

// Incoming messages are read into a buffer that grows to fit them, because one
// of them is not small: ObjectManager.GetManagedObjects on org.bluez returns
// every adapter, every device and every property of each in a single reply, and
// a card with a dozen paired devices puts that well past any fixed size worth
// keeping resident. It starts small and is only ever grown, so a player that
// never asks a big question never pays for one.
#define READ_BUF_START (4 * 1024)
#define READ_BUF_MAX (512 * 1024) // beyond this the peer is not something to talk to
#define WRITE_BUF_MAX (16 * 1024) // a body of ours never approaches it

// ---------------------------------------------------------------------------
// The writer
//
// A body is built into a flat buffer whose offsets are the alignment the
// protocol counts from, so `used` is the only state padding needs.
// ---------------------------------------------------------------------------

struct dbus_writer {
	uint8_t buf[WRITE_BUF_MAX];
	size_t used;
	bool overflow; // past the buffer: the send refuses rather than truncates
};

static void w_pad(dbus_writer_t *w, size_t align) {
	while (w->used % align) {
		if (w->used >= sizeof(w->buf)) {
			w->overflow = true;
			return;
		}
		w->buf[w->used++] = 0;
	}
}

static void w_raw(dbus_writer_t *w, const void *data, size_t len) {
	if (w->used + len > sizeof(w->buf)) {
		w->overflow = true;
		return;
	}
	memcpy(w->buf + w->used, data, len);
	w->used += len;
}

void dbus_w_byte(dbus_writer_t *w, uint8_t v) { w_raw(w, &v, 1); }

void dbus_w_u16(dbus_writer_t *w, uint16_t v) {
	w_pad(w, 2);
	w_raw(w, &v, 2);
}

void dbus_w_u32(dbus_writer_t *w, uint32_t v) {
	w_pad(w, 4);
	w_raw(w, &v, 4);
}

// A boolean travels as a 32-bit word, and only 0 and 1 are legal in it.
void dbus_w_bool(dbus_writer_t *w, bool v) { dbus_w_u32(w, v ? 1u : 0u); }

void dbus_w_i64(dbus_writer_t *w, int64_t v) {
	w_pad(w, 8);
	w_raw(w, &v, 8);
}

// A string is a 32-bit length, the bytes, and a terminator that the length does
// not count. An object path is spelled the same way; only the signature that
// introduces it differs.
static void w_string_body(dbus_writer_t *w, const char *v) {
	if (!v) {
		v = "";
	}
	size_t len = strlen(v);
	dbus_w_u32(w, (uint32_t)len);
	w_raw(w, v, len + 1);
}

void dbus_w_string(dbus_writer_t *w, const char *v) { w_string_body(w, v); }
void dbus_w_path(dbus_writer_t *w, const char *v) { w_string_body(w, v); }

// A signature is the odd one out: a single byte of length, so it needs no
// padding at all.
static void w_signature(dbus_writer_t *w, const char *sig) {
	if (!sig) {
		sig = "";
	}
	size_t len = strlen(sig);
	dbus_w_byte(w, (uint8_t)len);
	w_raw(w, sig, len + 1);
}

// The alignment the first element of an array has to start at. It is the
// element's own, and it is applied AFTER the length word -- the padding between
// the two is not part of the length.
static size_t align_of(const char *sig) {
	switch (sig ? sig[0] : 0) {
	case 'y':
	case 'g':
	case 'v':
		return 1;
	case 'n':
	case 'q':
		return 2;
	case 'x':
	case 't':
	case 'd':
	case '(':
	case '{':
		return 8;
	default:
		return 4; // b, i, u, s, o, a
	}
}

void dbus_w_array_begin(dbus_writer_t *w, const char *elem_sig, dbus_array_t *a) {
	dbus_w_u32(w, 0); // the length, filled in by the end below
	a->len_at = w->used - 4;
	w_pad(w, align_of(elem_sig));
	a->body_at = w->used;
}

void dbus_w_array_end(dbus_writer_t *w, dbus_array_t *a) {
	if (w->overflow || a->len_at + 4 > w->used) {
		return;
	}
	uint32_t len = (uint32_t)(w->used - a->body_at);
	memcpy(w->buf + a->len_at, &len, 4);
}

// {sv}: a dict entry is a struct, so it starts on an eight-byte boundary.
static void w_dict_key(dbus_writer_t *w, const char *key, const char *value_sig) {
	w_pad(w, 8);
	dbus_w_string(w, key);
	w_signature(w, value_sig);
}

void dbus_w_dict_string(dbus_writer_t *w, const char *key, const char *value) {
	w_dict_key(w, key, "s");
	dbus_w_string(w, value);
}

void dbus_w_dict_path(dbus_writer_t *w, const char *key, const char *value) {
	w_dict_key(w, key, "o");
	dbus_w_path(w, value);
}

void dbus_w_dict_bool(dbus_writer_t *w, const char *key, bool value) {
	w_dict_key(w, key, "b");
	dbus_w_bool(w, value);
}

void dbus_w_dict_i64(dbus_writer_t *w, const char *key, int64_t value) {
	w_dict_key(w, key, "x");
	dbus_w_i64(w, value);
}

void dbus_w_dict_strings(dbus_writer_t *w, const char *key, const char *const *values, int count) {
	w_dict_key(w, key, "as");
	dbus_array_t a;
	dbus_w_array_begin(w, "s", &a);
	for (int i = 0; i < count; i++) {
		dbus_w_string(w, values[i]);
	}
	dbus_w_array_end(w, &a);
}

void dbus_w_dict_variant_begin(dbus_writer_t *w, const char *key, const char *value_sig) {
	w_dict_key(w, key, value_sig);
}

void dbus_w_variant_string(dbus_writer_t *w, const char *value) {
	w_signature(w, "s");
	dbus_w_string(w, value);
}

void dbus_w_variant_bool(dbus_writer_t *w, bool value) {
	w_signature(w, "b");
	dbus_w_bool(w, value);
}

void dbus_w_variant_u16(dbus_writer_t *w, uint16_t value) {
	w_signature(w, "q");
	dbus_w_u16(w, value);
}

void dbus_w_variant_u32(dbus_writer_t *w, uint32_t value) {
	w_signature(w, "u");
	dbus_w_u32(w, value);
}

// ---------------------------------------------------------------------------
// The reader
// ---------------------------------------------------------------------------

static uint32_t swap32(uint32_t v) {
	return ((v & 0x000000ffu) << 24) | ((v & 0x0000ff00u) << 8) | ((v & 0x00ff0000u) >> 8) |
		   ((v & 0xff000000u) >> 24);
}

// Whether this machine is the byte order a message says it is in. Worked out
// rather than assumed: the daemon writes its own order, and while in practice
// it is the same machine, a wrong guess here is a silently wrong length.
static bool host_is_big_endian(void) {
	const uint16_t one = 1;
	return ((const uint8_t *)&one)[0] == 0;
}

static void r_pad(dbus_reader_t *r, size_t align) {
	while (r->at % align) {
		if (r->at >= r->len) {
			r->bad = true;
			return;
		}
		r->at++;
	}
}

static uint32_t r_u32(dbus_reader_t *r) {
	r_pad(r, 4);
	if (r->bad || r->at + 4 > r->len) {
		r->bad = true;
		return 0;
	}
	uint32_t v;
	memcpy(&v, r->data + r->at, 4);
	r->at += 4;
	return r->big_endian == host_is_big_endian() ? v : swap32(v);
}

void dbus_reader_init(dbus_reader_t *r, const dbus_msg_t *m) {
	memset(r, 0, sizeof(*r));
	if (m) {
		r->data = m->body;
		r->len = m->body_len;
		r->big_endian = m->big_endian;
	}
}

bool dbus_r_string(dbus_reader_t *r, char *out, size_t out_size) {
	if (out && out_size) {
		out[0] = '\0';
	}
	uint32_t len = r_u32(r);
	if (r->bad || r->at + (size_t)len + 1 > r->len) {
		r->bad = true;
		return false;
	}
	if (out && out_size) {
		size_t take = len < out_size - 1 ? len : out_size - 1;
		memcpy(out, r->data + r->at, take);
		out[take] = '\0';
	}
	r->at += (size_t)len + 1;
	return true;
}

bool dbus_r_signature(dbus_reader_t *r, char *out, size_t out_size) {
	if (out && out_size) {
		out[0] = '\0';
	}
	if (r->bad || r->at >= r->len) {
		r->bad = true;
		return false;
	}
	size_t len = r->data[r->at++];
	if (r->at + len + 1 > r->len) {
		r->bad = true;
		return false;
	}
	if (out && out_size) {
		size_t take = len < out_size - 1 ? len : out_size - 1;
		memcpy(out, r->data + r->at, take);
		out[take] = '\0';
	}
	r->at += len + 1;
	return true;
}

bool dbus_r_byte(dbus_reader_t *r, uint8_t *out) {
	if (r->bad || r->at >= r->len) {
		r->bad = true;
		return false;
	}
	uint8_t v = r->data[r->at++];
	if (out) {
		*out = v;
	}
	return true;
}

bool dbus_r_u16(dbus_reader_t *r, uint16_t *out) {
	r_pad(r, 2);
	if (r->bad || r->at + 2 > r->len) {
		r->bad = true;
		return false;
	}
	uint16_t v;
	memcpy(&v, r->data + r->at, 2);
	r->at += 2;
	if (r->big_endian != host_is_big_endian()) {
		v = (uint16_t)((v >> 8) | (v << 8));
	}
	if (out) {
		*out = v;
	}
	return true;
}

bool dbus_r_u32(dbus_reader_t *r, uint32_t *out) {
	uint32_t v = r_u32(r);
	if (r->bad) {
		return false;
	}
	if (out) {
		*out = v;
	}
	return true;
}

bool dbus_r_bool(dbus_reader_t *r, bool *out) {
	uint32_t v = 0;
	if (!dbus_r_u32(r, &v)) {
		return false;
	}
	if (out) {
		*out = v != 0;
	}
	return true;
}

bool dbus_r_array_begin(dbus_reader_t *r, const char *elem_sig, dbus_array_iter_t *it) {
	uint32_t len = r_u32(r);
	it->align = align_of(elem_sig);
	it->end = 0;
	// The padding to the element's alignment sits after the length word and is
	// not counted by it, so it is stepped over before the end is worked out.
	r_pad(r, it->align);
	if (r->bad || r->at + len > r->len) {
		r->bad = true;
		return false;
	}
	it->end = r->at + len;
	return true;
}

bool dbus_r_array_more(dbus_reader_t *r, dbus_array_iter_t *it) {
	if (r->bad || r->at >= it->end) {
		return false;
	}
	r_pad(r, it->align);
	return !r->bad && r->at < it->end;
}

// How many characters of `sig` make up its first complete type: one for a basic
// type, but a struct or a dictionary entry runs to its own closing bracket and
// an array carries its element type along.
static size_t sig_first_len(const char *sig) {
	size_t n = 0;
	if (!sig) {
		return 0;
	}
	while (sig[n] == 'a') {
		n++;
	}
	if (!sig[n]) {
		return n;
	}
	if (sig[n] == '(' || sig[n] == '{') {
		int depth = 0;
		for (; sig[n]; n++) {
			if (sig[n] == '(' || sig[n] == '{') {
				depth++;
			} else if (sig[n] == ')' || sig[n] == '}') {
				if (--depth == 0) {
					return n + 1;
				}
			}
		}
		return n;
	}
	return n + 1;
}

bool dbus_r_skip(dbus_reader_t *r, const char *sig) {
	if (r->bad) {
		return false;
	}
	if (!sig || !sig[0]) {
		return true;
	}

	switch (sig[0]) {
	case 'y':
		return dbus_r_byte(r, NULL);
	case 'n':
	case 'q':
		return dbus_r_u16(r, NULL);
	case 'b':
	case 'i':
	case 'u':
	case 'h':
		return dbus_r_u32(r, NULL);
	case 'x':
	case 't':
	case 'd':
		r_pad(r, 8);
		if (r->bad || r->at + 8 > r->len) {
			r->bad = true;
			return false;
		}
		r->at += 8;
		return true;
	case 's':
	case 'o':
		return dbus_r_string(r, NULL, 0);
	case 'g':
		return dbus_r_signature(r, NULL, 0);
	case 'v': {
		char inner[64];
		return dbus_r_signature(r, inner, sizeof(inner)) && dbus_r_skip(r, inner);
	}
	case 'a': {
		dbus_array_iter_t it;
		if (!dbus_r_array_begin(r, sig + 1, &it)) {
			return false;
		}
		while (dbus_r_array_more(r, &it)) {
			if (!dbus_r_skip(r, sig + 1)) {
				return false;
			}
		}
		return !r->bad;
	}
	case '(':
	case '{': {
		r_pad(r, 8);
		const char *at = sig + 1;
		const char *close = sig + sig_first_len(sig) - 1;
		while (at < close) {
			size_t n = sig_first_len(at);
			if (n == 0 || at + n > close) {
				r->bad = true;
				return false;
			}
			char one[64];
			size_t take = n < sizeof(one) - 1 ? n : sizeof(one) - 1;
			memcpy(one, at, take);
			one[take] = '\0';
			if (!dbus_r_skip(r, one)) {
				return false;
			}
			at += n;
		}
		return !r->bad;
	}
	default:
		r->bad = true; // a type this client cannot measure: stop rather than guess
		return false;
	}
}

// ---------------------------------------------------------------------------
// The connection
// ---------------------------------------------------------------------------

struct dbus_conn {
	int fd;
	pthread_t reader;
	bool reader_live;
	volatile bool stopping;
	volatile bool alive;

	pthread_mutex_t write_lock; // one message on the wire at a time
	uint32_t next_serial;

	// The message being built. One at a time, under write_lock, which is taken
	// by _begin and released by _send.
	dbus_writer_t writer;
	uint8_t out_type;
	uint32_t out_serial;
	char out_destination[DBUS_NAME_MAX];
	char out_path[DBUS_NAME_MAX];
	char out_interface[DBUS_NAME_MAX];
	char out_member[DBUS_NAME_MAX];
	char out_signature[64];
	uint32_t out_reply_serial;
	char out_error[DBUS_NAME_MAX];

	// The one call waiting for its answer. One is enough: the calls this
	// player makes are made one after another from a single thread.
	pthread_mutex_t reply_lock;
	pthread_cond_t reply_cond;
	uint32_t waiting_serial;
	bool reply_arrived;
	bool reply_ok;
	char reply_error[DBUS_NAME_MAX];
	// The reply's body, copied out: the buffer it was read into belongs to the
	// reader thread and is gone by the time the caller wakes up. Grown to fit,
	// never truncated -- a reply cut in half parses as a reply that ended early,
	// which reads as an empty device list rather than as an error.
	uint8_t *reply_body;
	size_t reply_body_size;
	size_t reply_body_len;
	bool reply_big_endian;

	dbus_method_cb on_call;
	void *user;

	dbus_signal_cb on_signal;
	void *signal_user;

	uint8_t *read_buf;
	size_t read_buf_size;
};

static void set_str(char *dst, size_t size, const char *src) { snprintf(dst, size, "%s", src ? src : ""); }

// Grows *buf to at least `want`, in powers of two. False leaves the old buffer
// untouched, so the caller can drop the message instead of the connection.
static bool grow_buffer(uint8_t **buf, size_t *size, size_t want) {
	if (*size >= want) {
		return true;
	}
	size_t next = *size ? *size : READ_BUF_START;
	while (next < want) {
		next *= 2;
	}
	uint8_t *bigger = realloc(*buf, next);
	if (!bigger) {
		return false;
	}
	*buf = bigger;
	*size = next;
	return true;
}

// --- the socket ---

static int open_socket(const char *path) {
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		return -1;
	}
	// Close-on-exec. bluetooth.c starts bluetoothd, bt-agent and bluealsa from a
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
	if (strlen(path) >= sizeof(addr.sun_path)) {
		close(fd);
		return -1;
	}
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

// MSG_NOSIGNAL keeps a bus that has gone away from killing the process. Defined
// defensively: where libc lacks it, the explicit SIGPIPE handling in main.c is
// the remaining protection.
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static bool write_all(int fd, const void *data, size_t len) {
	const uint8_t *p = data;
	while (len) {
		// send() rather than write(): dbus-daemon exiting under a call in
		// flight would otherwise raise SIGPIPE, and on this device a process
		// that dies takes the whole player with it.
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
		if (n <= 0) {
			if (n < 0 && errno == EINTR) {
				continue;
			}
			return false;
		}
		p += n;
		len -= (size_t)n;
	}
	return true;
}

static bool read_all(int fd, void *data, size_t len) {
	uint8_t *p = data;
	while (len) {
		ssize_t n = read(fd, p, len);
		if (n <= 0) {
			if (n < 0 && errno == EINTR) {
				continue;
			}
			return false;
		}
		p += n;
		len -= (size_t)n;
	}
	return true;
}

// One line of the authentication conversation, which is plain text and ends in
// CRLF -- the only part of D-Bus that is not binary.
static bool read_line(int fd, char *out, size_t out_size) {
	size_t used = 0;
	for (;;) {
		char c;
		if (!read_all(fd, &c, 1)) {
			return false;
		}
		if (c == '\n') {
			while (used && (out[used - 1] == '\r')) {
				used--;
			}
			out[used] = '\0';
			return true;
		}
		if (used + 1 < out_size) {
			out[used++] = c;
		}
	}
}

// EXTERNAL: the credentials are the ones the kernel attaches to the socket, so
// the only thing to send is the uid being claimed -- as decimal digits, hex
// encoded, because the auth conversation carries no raw bytes.
static bool authenticate(int fd) {
	// The leading NUL is not part of any command: it is the byte that has to
	// precede the conversation on a unix socket.
	if (!write_all(fd, "\0", 1)) {
		return false;
	}

	char uid[32];
	int uid_len = snprintf(uid, sizeof(uid), "%u", (unsigned)geteuid());
	char hex[80];
	int used = 0;
	for (int i = 0; i < uid_len && used + 2 < (int)sizeof(hex); i++) {
		used += snprintf(hex + used, sizeof(hex) - (size_t)used, "%02x", (unsigned char)uid[i]);
	}

	char line[256];
	snprintf(line, sizeof(line), "AUTH EXTERNAL %s\r\n", hex);
	if (!write_all(fd, line, strlen(line))) {
		return false;
	}
	if (!read_line(fd, line, sizeof(line)) || strncmp(line, "OK", 2) != 0) {
		fprintf(stderr, "dbus: authentication refused (%s)\n", line);
		return false;
	}
	return write_all(fd, "BEGIN\r\n", 7);
}

// --- sending ---

// The header: fixed part, then the array of fields, then padding to eight
// because the body always starts on that boundary.
static bool send_current(dbus_conn_t *c) {
	if (c->writer.overflow) {
		fprintf(stderr, "dbus: message too big; not sent\n");
		return false;
	}

	dbus_writer_t h;
	memset(&h, 0, sizeof(h));

	dbus_w_byte(&h, host_is_big_endian() ? 'B' : 'l');
	dbus_w_byte(&h, c->out_type);
	dbus_w_byte(&h, 0); // no flags: replies are wanted, and auto-start is not
	dbus_w_byte(&h, 1); // protocol version
	dbus_w_u32(&h, (uint32_t)c->writer.used);
	dbus_w_u32(&h, c->out_serial);

	dbus_array_t fields;
	dbus_w_array_begin(&h, "(yv)", &fields);
	if (c->out_path[0]) {
		w_pad(&h, 8);
		dbus_w_byte(&h, 1);
		w_signature(&h, "o");
		dbus_w_path(&h, c->out_path);
	}
	if (c->out_interface[0]) {
		w_pad(&h, 8);
		dbus_w_byte(&h, 2);
		w_signature(&h, "s");
		dbus_w_string(&h, c->out_interface);
	}
	if (c->out_member[0]) {
		w_pad(&h, 8);
		dbus_w_byte(&h, 3);
		w_signature(&h, "s");
		dbus_w_string(&h, c->out_member);
	}
	if (c->out_error[0]) {
		w_pad(&h, 8);
		dbus_w_byte(&h, 4);
		w_signature(&h, "s");
		dbus_w_string(&h, c->out_error);
	}
	if (c->out_reply_serial) {
		w_pad(&h, 8);
		dbus_w_byte(&h, 5);
		w_signature(&h, "u");
		dbus_w_u32(&h, c->out_reply_serial);
	}
	if (c->out_destination[0]) {
		w_pad(&h, 8);
		dbus_w_byte(&h, 6);
		w_signature(&h, "s");
		dbus_w_string(&h, c->out_destination);
	}
	if (c->out_signature[0]) {
		w_pad(&h, 8);
		dbus_w_byte(&h, 8);
		w_signature(&h, "g");
		w_signature(&h, c->out_signature);
	}
	dbus_w_array_end(&h, &fields);
	w_pad(&h, 8);

	if (h.overflow) {
		fprintf(stderr, "dbus: header too big; not sent\n");
		return false;
	}
	return write_all(c->fd, h.buf, h.used) && (c->writer.used == 0 || write_all(c->fd, c->writer.buf, c->writer.used));
}

// Everything an outgoing message needs, cleared, with the write lock taken. The
// matching send releases it.
static dbus_writer_t *out_begin(dbus_conn_t *c, uint8_t type) {
	pthread_mutex_lock(&c->write_lock);
	memset(&c->writer, 0, sizeof(c->writer));
	c->out_type = type;
	c->out_serial = c->next_serial++;
	if (c->next_serial == 0) {
		c->next_serial = 1; // serial zero is not a serial
	}
	c->out_destination[0] = c->out_path[0] = c->out_interface[0] = '\0';
	c->out_member[0] = c->out_signature[0] = c->out_error[0] = '\0';
	c->out_reply_serial = 0;
	return &c->writer;
}

dbus_writer_t *dbus_call_begin(dbus_conn_t *c, const char *destination, const char *path, const char *interface,
							   const char *member, const char *signature) {
	dbus_writer_t *w = out_begin(c, DBUS_TYPE_METHOD_CALL);
	set_str(c->out_destination, sizeof(c->out_destination), destination);
	set_str(c->out_path, sizeof(c->out_path), path);
	set_str(c->out_interface, sizeof(c->out_interface), interface);
	set_str(c->out_member, sizeof(c->out_member), member);
	set_str(c->out_signature, sizeof(c->out_signature), signature);
	return w;
}

static void deadline_in(struct timespec *ts, int ms) {
	clock_gettime(CLOCK_REALTIME, ts);
	ts->tv_sec += ms / 1000;
	ts->tv_nsec += (long)(ms % 1000) * 1000000L;
	if (ts->tv_nsec >= 1000000000L) {
		ts->tv_sec++;
		ts->tv_nsec -= 1000000000L;
	}
}

bool dbus_call_send(dbus_conn_t *c, int timeout_ms, char *err_out, size_t err_size) {
	if (err_out && err_size) {
		err_out[0] = '\0';
	}

	// Armed before the message goes out, or a reply that beats this thread back
	// would find nobody waiting and be thrown away.
	pthread_mutex_lock(&c->reply_lock);
	c->waiting_serial = c->out_serial;
	c->reply_arrived = false;
	c->reply_ok = false;
	c->reply_error[0] = '\0';
	pthread_mutex_unlock(&c->reply_lock);

	bool sent = send_current(c);
	pthread_mutex_unlock(&c->write_lock);
	if (!sent) {
		return false;
	}

	struct timespec until;
	deadline_in(&until, timeout_ms > 0 ? timeout_ms : 5000);

	pthread_mutex_lock(&c->reply_lock);
	while (!c->reply_arrived && c->alive) {
		if (pthread_cond_timedwait(&c->reply_cond, &c->reply_lock, &until) == ETIMEDOUT) {
			break;
		}
	}
	bool ok = c->reply_arrived && c->reply_ok;
	if (err_out && err_size) {
		snprintf(err_out, err_size, "%s", c->reply_error);
	}
	c->waiting_serial = 0;
	pthread_mutex_unlock(&c->reply_lock);
	return ok;
}

void dbus_reply_reader(dbus_conn_t *c, dbus_reader_t *r) {
	memset(r, 0, sizeof(*r));
	pthread_mutex_lock(&c->reply_lock);
	r->data = c->reply_body;
	r->len = c->reply_body_len;
	r->big_endian = c->reply_big_endian;
	pthread_mutex_unlock(&c->reply_lock);
}

dbus_writer_t *dbus_reply_begin(dbus_conn_t *c, const dbus_msg_t *call, const char *signature) {
	dbus_writer_t *w = out_begin(c, DBUS_TYPE_METHOD_RETURN);
	set_str(c->out_destination, sizeof(c->out_destination), call->sender);
	set_str(c->out_signature, sizeof(c->out_signature), signature);
	c->out_reply_serial = call->serial;
	return w;
}

bool dbus_reply_send(dbus_conn_t *c) {
	bool ok = send_current(c);
	pthread_mutex_unlock(&c->write_lock);
	return ok;
}

bool dbus_error(dbus_conn_t *c, const dbus_msg_t *call, const char *name, const char *message) {
	dbus_writer_t *w = out_begin(c, DBUS_TYPE_ERROR);
	set_str(c->out_destination, sizeof(c->out_destination), call->sender);
	set_str(c->out_error, sizeof(c->out_error), name);
	set_str(c->out_signature, sizeof(c->out_signature), "s");
	c->out_reply_serial = call->serial;
	dbus_w_string(w, message ? message : name);
	bool ok = send_current(c);
	pthread_mutex_unlock(&c->write_lock);
	return ok;
}

dbus_writer_t *dbus_signal_begin(dbus_conn_t *c, const char *path, const char *interface, const char *member,
								 const char *signature) {
	dbus_writer_t *w = out_begin(c, DBUS_TYPE_SIGNAL);
	set_str(c->out_path, sizeof(c->out_path), path);
	set_str(c->out_interface, sizeof(c->out_interface), interface);
	set_str(c->out_member, sizeof(c->out_member), member);
	set_str(c->out_signature, sizeof(c->out_signature), signature);
	return w;
}

bool dbus_signal_send(dbus_conn_t *c) { return dbus_reply_send(c); }

// --- receiving ---

// Pulls the header fields out of one message. The fields are an array of
// (byte, variant), and only the handful this client acts on are kept.
static void parse_fields(dbus_msg_t *m, const uint8_t *data, size_t len, bool big_endian) {
	dbus_reader_t r = {.data = data, .len = len, .at = 0, .big_endian = big_endian, .bad = false};

	while (r.at < r.len && !r.bad) {
		r_pad(&r, 8);
		if (r.bad || r.at >= r.len) {
			break;
		}
		uint8_t code = r.data[r.at++];

		// The field's variant: a signature, then the value it describes.
		if (r.at >= r.len) {
			break;
		}
		uint8_t sig_len = r.data[r.at++];
		if (r.at + (size_t)sig_len + 1 > r.len) {
			break;
		}
		char sig[8] = {0};
		if (sig_len < sizeof(sig)) {
			memcpy(sig, r.data + r.at, sig_len);
		}
		r.at += (size_t)sig_len + 1;

		switch (sig[0]) {
		case 's':
		case 'o': {
			char value[DBUS_NAME_MAX];
			if (!dbus_r_string(&r, value, sizeof(value))) {
				return;
			}
			switch (code) {
			case 1:
				set_str(m->path, sizeof(m->path), value);
				break;
			case 2:
				set_str(m->interface, sizeof(m->interface), value);
				break;
			case 3:
				set_str(m->member, sizeof(m->member), value);
				break;
			case 4:
				set_str(m->signature, sizeof(m->signature), ""); // an error name
				set_str(m->member, sizeof(m->member), value);
				break;
			case 7:
				set_str(m->sender, sizeof(m->sender), value);
				break;
			default:
				break;
			}
			break;
		}
		case 'g': {
			if (r.at >= r.len) {
				return;
			}
			uint8_t n = r.data[r.at++];
			if (r.at + (size_t)n + 1 > r.len) {
				return;
			}
			if (code == 8) {
				size_t take = n < sizeof(m->signature) - 1 ? n : sizeof(m->signature) - 1;
				memcpy(m->signature, r.data + r.at, take);
				m->signature[take] = '\0';
			}
			r.at += (size_t)n + 1;
			break;
		}
		case 'u': {
			uint32_t value = r_u32(&r);
			if (code == 5) {
				m->reply_serial = value;
			}
			break;
		}
		default:
			return; // a type this client does not read: stop rather than guess
		}
	}
}

static void *reader_thread(void *arg) {
	dbus_conn_t *c = arg;

	while (!c->stopping) {
		uint8_t head[16];
		if (!read_all(c->fd, head, sizeof(head))) {
			break;
		}

		bool big_endian = head[0] == 'B';
		bool swap = big_endian != host_is_big_endian();
		uint32_t body_len, serial, fields_len;
		memcpy(&body_len, head + 4, 4);
		memcpy(&serial, head + 8, 4);
		memcpy(&fields_len, head + 12, 4);
		if (swap) {
			body_len = swap32(body_len);
			serial = swap32(serial);
			fields_len = swap32(fields_len);
		}

		// The fields, then the padding that puts the body on an eight-byte
		// boundary. The header is sixteen bytes, so the padding is measured
		// from the end of the fields counted with them.
		size_t fields_end = 16 + (size_t)fields_len;
		size_t body_at = (fields_end + 7) & ~(size_t)7;
		size_t total = body_at - 16 + (size_t)body_len;
		if (total > READ_BUF_MAX) {
			fprintf(stderr, "dbus: %zu byte message, over the limit: closing\n", total);
			break;
		}
		// The buffer is the connection's own, not one for the program: two
		// connections each have a reader thread, and a shared buffer means one
		// thread's message is overwritten while the other is still reading it.
		if (total && !grow_buffer(&c->read_buf, &c->read_buf_size, total)) {
			fprintf(stderr, "dbus: no memory for a %zu byte message: closing\n", total);
			break;
		}
		uint8_t *buf = c->read_buf;
		if (total && !read_all(c->fd, buf, total)) {
			break;
		}

		dbus_msg_t m;
		memset(&m, 0, sizeof(m));
		m.type = head[1];
		m.serial = serial;
		m.big_endian = big_endian;
		parse_fields(&m, buf, fields_len, big_endian);
		m.body = buf + (body_at - 16);
		m.body_len = body_len;

		if (m.type == DBUS_TYPE_METHOD_RETURN || m.type == DBUS_TYPE_ERROR) {
			pthread_mutex_lock(&c->reply_lock);
			if (c->waiting_serial && m.reply_serial == c->waiting_serial) {
				c->reply_arrived = true;
				c->reply_ok = m.type == DBUS_TYPE_METHOD_RETURN;
				set_str(c->reply_error, sizeof(c->reply_error), m.type == DBUS_TYPE_ERROR ? m.member : "");
				if (grow_buffer(&c->reply_body, &c->reply_body_size, m.body_len ? m.body_len : 1)) {
					c->reply_body_len = m.body_len;
					memcpy(c->reply_body, m.body, m.body_len);
				} else {
					c->reply_body_len = 0;
					c->reply_ok = false;
				}
				c->reply_big_endian = m.big_endian;
				pthread_cond_broadcast(&c->reply_cond);
			}
			pthread_mutex_unlock(&c->reply_lock);
			continue;
		}

		if (m.type == DBUS_TYPE_METHOD_CALL && c->on_call) {
			c->on_call(c, &m, c->user);
		} else if (m.type == DBUS_TYPE_SIGNAL && c->on_signal) {
			c->on_signal(c, &m, c->signal_user);
		}
	}

	// Whoever is waiting for a reply is never going to get one.
	pthread_mutex_lock(&c->reply_lock);
	c->alive = false;
	pthread_cond_broadcast(&c->reply_cond);
	pthread_mutex_unlock(&c->reply_lock);
	return NULL;
}

// --- opening and closing ---

dbus_conn_t *dbus_connect_system(const char *address, dbus_method_cb on_call, void *user) {
	int fd = -1;

	// "unix:path=/..." is the only address shape this client understands, and
	// the only one a system bus uses.
	char from_env[256] = {0};
	if (!address) {
		const char *env = getenv("DBUS_SYSTEM_BUS_ADDRESS");
		if (env && env[0]) {
			snprintf(from_env, sizeof(from_env), "%s", env);
			address = from_env;
		}
	}
	if (address) {
		const char *path = strstr(address, "path=");
		if (path) {
			char only[256];
			snprintf(only, sizeof(only), "%s", path + 5);
			char *comma = strchr(only, ',');
			if (comma) {
				*comma = '\0';
			}
			fd = open_socket(only);
		}
	} else {
		for (size_t i = 0; i < sizeof(SOCKET_PATHS) / sizeof(SOCKET_PATHS[0]) && fd < 0; i++) {
			fd = open_socket(SOCKET_PATHS[i]);
		}
	}
	if (fd < 0) {
		return NULL;
	}

	if (!authenticate(fd)) {
		close(fd);
		return NULL;
	}

	dbus_conn_t *c = calloc(1, sizeof(*c));
	if (!c) {
		close(fd);
		return NULL;
	}
	c->fd = fd;
	c->next_serial = 1;
	c->alive = true;
	c->on_call = on_call;
	c->user = user;
	pthread_mutex_init(&c->write_lock, NULL);
	pthread_mutex_init(&c->reply_lock, NULL);
	pthread_cond_init(&c->reply_cond, NULL);

	if (pthread_create(&c->reader, NULL, reader_thread, c) != 0) {
		close(fd);
		free(c);
		return NULL;
	}
	c->reader_live = true;

	// Hello is not a formality: until it is answered the connection has no name
	// and the daemon will not route anything to it.
	dbus_call_begin(c, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "Hello", "");
	if (!dbus_call_send(c, 5000, NULL, 0)) {
		fprintf(stderr, "dbus: no reply to Hello\n");
		dbus_disconnect(c);
		return NULL;
	}
	return c;
}

void dbus_set_signal_handler(dbus_conn_t *c, dbus_signal_cb cb, void *user) {
	if (!c) {
		return;
	}
	c->signal_user = user;
	c->on_signal = cb;
}

bool dbus_add_match(dbus_conn_t *c, const char *rule) {
	dbus_writer_t *w =
		dbus_call_begin(c, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "AddMatch", "s");
	dbus_w_string(w, rule);
	char err[DBUS_NAME_MAX];
	if (!dbus_call_send(c, 5000, err, sizeof(err))) {
		fprintf(stderr, "dbus: AddMatch refused (%s): %s\n", err[0] ? err : "no reply", rule);
		return false;
	}
	return true;
}

void dbus_disconnect(dbus_conn_t *c) {
	if (!c) {
		return;
	}
	c->stopping = true;
	// Shutting the socket down is what takes the reader out of its blocking
	// read; closing alone would leave it on a descriptor number that something
	// else may reuse.
	shutdown(c->fd, SHUT_RDWR);
	if (c->reader_live) {
		pthread_join(c->reader, NULL);
	}
	close(c->fd);
	pthread_mutex_destroy(&c->write_lock);
	pthread_mutex_destroy(&c->reply_lock);
	pthread_cond_destroy(&c->reply_cond);
	free(c->read_buf);
	free(c->reply_body);
	free(c);
}

bool dbus_alive(const dbus_conn_t *c) { return c && c->alive; }

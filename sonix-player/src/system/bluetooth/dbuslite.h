#ifndef DBUSLITE_H
#define DBUSLITE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A small D-Bus client: exactly the part of the protocol this player needs.
//
// It exists because the one thing the player must do over D-Bus -- register a
// media player with bluez so the headphones learn what is being played -- would
// otherwise mean linking libdbus-1, and that library is not in the toolchain's
// sysroot. Bringing it in means either fitting headers and a shared object into
// the cross build or vendoring dbus's own sources (a quarter of a megabyte in
// the binary, and a dozen defines its configure script normally writes). The
// protocol needed here is small enough that writing it is the smaller job.
//
// What it is NOT: a general D-Bus library. There is one pending call at a time,
// no file-descriptor passing, no match rules beyond what is asked for, and the
// type writers cover only what the media player interface uses. Anything else
// belongs in libdbus.
//
// Threading: one reader thread owns the socket's reads and dispatches incoming
// calls; writes are serialised on a mutex, so any thread may reply or emit.

typedef struct dbus_conn dbus_conn_t;

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

#define DBUS_TYPE_METHOD_CALL 1
#define DBUS_TYPE_METHOD_RETURN 2
#define DBUS_TYPE_ERROR 3
#define DBUS_TYPE_SIGNAL 4

// The header fields worth keeping, copied out rather than pointed at: the
// buffer they came from is reused by the next read.
#define DBUS_NAME_MAX 160

typedef struct {
	uint8_t type;
	uint32_t serial;
	uint32_t reply_serial;
	char path[DBUS_NAME_MAX];
	char interface[DBUS_NAME_MAX];
	char member[DBUS_NAME_MAX];
	char sender[DBUS_NAME_MAX];
	char signature[64];
	const uint8_t *body; // into the connection's read buffer, valid for the call
	size_t body_len;
	bool big_endian; // how the body is encoded, for the readers below
} dbus_msg_t;

// ---------------------------------------------------------------------------
// Writing a body
//
// D-Bus is a padded format: every type is written at a multiple of its own
// alignment, counted from the start of the body. Getting that wrong does not
// produce a wrong value, it produces a message the daemon drops and a
// connection it closes, so the padding lives in one place -- here -- and
// callers never think about it.
// ---------------------------------------------------------------------------

typedef struct dbus_writer dbus_writer_t;

void dbus_w_byte(dbus_writer_t *w, uint8_t v);
void dbus_w_bool(dbus_writer_t *w, bool v);
void dbus_w_u16(dbus_writer_t *w, uint16_t v);
void dbus_w_u32(dbus_writer_t *w, uint32_t v);
void dbus_w_i64(dbus_writer_t *w, int64_t v);
void dbus_w_string(dbus_writer_t *w, const char *v);
void dbus_w_path(dbus_writer_t *w, const char *v);

// An array of `elem_sig`. Everything written between begin and end is its
// content; the two must be paired, and they nest.
typedef struct {
	size_t len_at;	// where the length was left blank
	size_t body_at; // where the first element starts
} dbus_array_t;
void dbus_w_array_begin(dbus_writer_t *w, const char *elem_sig, dbus_array_t *a);
void dbus_w_array_end(dbus_writer_t *w, dbus_array_t *a);

// One {sv} entry, for the a{sv} that carries every property in this protocol.
void dbus_w_dict_string(dbus_writer_t *w, const char *key, const char *value);
void dbus_w_dict_path(dbus_writer_t *w, const char *key, const char *value);
void dbus_w_dict_bool(dbus_writer_t *w, const char *key, bool value);
void dbus_w_dict_i64(dbus_writer_t *w, const char *key, int64_t value);
// A variant holding an array of strings, for xesam:artist and its like. `count`
// of zero writes an empty array, which is legal and means "no value".
void dbus_w_dict_strings(dbus_writer_t *w, const char *key, const char *const *values, int count);

// One {sv} entry whose value the caller writes itself, for a variant this file
// has no shorthand for -- Metadata, whose value is another a{sv}. Writes the
// key and the value's signature; whatever is written next is the value.
void dbus_w_dict_variant_begin(dbus_writer_t *w, const char *key, const char *value_sig);

// A bare variant, for the value of Properties.Get and the third argument of
// Properties.Set.
void dbus_w_variant_string(dbus_writer_t *w, const char *value);
void dbus_w_variant_bool(dbus_writer_t *w, bool value);
void dbus_w_variant_u16(dbus_writer_t *w, uint16_t value);
void dbus_w_variant_u32(dbus_writer_t *w, uint32_t value);

// ---------------------------------------------------------------------------
// Reading a body
// ---------------------------------------------------------------------------

typedef struct {
	const uint8_t *data;
	size_t len;
	size_t at;
	bool big_endian;
	bool bad; // something did not fit or did not parse; every read after is a no-op
} dbus_reader_t;

void dbus_reader_init(dbus_reader_t *r, const dbus_msg_t *m);
// Copies the next string into `out`, which may be NULL to step over it. False
// when the body does not hold one.
bool dbus_r_string(dbus_reader_t *r, char *out, size_t out_size);
// A signature ('g'), spelled with a single length byte rather than a word.
bool dbus_r_signature(dbus_reader_t *r, char *out, size_t out_size);
bool dbus_r_byte(dbus_reader_t *r, uint8_t *out);
bool dbus_r_u16(dbus_reader_t *r, uint16_t *out);
bool dbus_r_u32(dbus_reader_t *r, uint32_t *out);
bool dbus_r_bool(dbus_reader_t *r, bool *out);

// Walking an array: begin, then read one element per `more` that answers true.
// `elem_sig` is needed before the first element because an array's contents
// start at the element type's own alignment.
typedef struct {
	size_t end;
	size_t align;
} dbus_array_iter_t;
bool dbus_r_array_begin(dbus_reader_t *r, const char *elem_sig, dbus_array_iter_t *it);
bool dbus_r_array_more(dbus_reader_t *r, dbus_array_iter_t *it);

// Steps over one complete value of `sig`, whatever it is. What makes it
// possible to read the one entry of a dictionary that matters and walk past the
// rest without knowing what they hold.
bool dbus_r_skip(dbus_reader_t *r, const char *sig);

// ---------------------------------------------------------------------------
// The connection
// ---------------------------------------------------------------------------

// Called on the reader thread for every method call addressed to this
// connection. The handler must send exactly one reply -- dbus_reply_send() or
// dbus_error() -- unless the call carried the no-reply flag, which
// dbus_reply_send() then quietly drops.
typedef void (*dbus_method_cb)(dbus_conn_t *c, const dbus_msg_t *m, void *user);

// Called on the reader thread for every signal the bus routes here. It must not
// make a call of its own: the reply would have to be read by the thread that is
// still inside the handler. Record what arrived and let another thread act.
typedef void (*dbus_signal_cb)(dbus_conn_t *c, const dbus_msg_t *m, void *user);

// Opens the system bus, authenticates as this uid and says Hello. NULL when the
// socket is not there or the daemon refuses. `address` may be NULL for the
// usual system bus locations.
dbus_conn_t *dbus_connect_system(const char *address, dbus_method_cb on_call, void *user);

// Signals are dropped until a handler is installed. Set it before the first
// dbus_add_match(), or the answer to the match could arrive with nowhere to go.
void dbus_set_signal_handler(dbus_conn_t *c, dbus_signal_cb cb, void *user);

// Asks the daemon to route the signals a rule describes to this connection.
// Without one, a connection receives no signals at all.
bool dbus_add_match(dbus_conn_t *c, const char *rule);

// Closes the socket and stops the reader thread.
void dbus_disconnect(dbus_conn_t *c);

// True while the reader thread has a live socket. Goes false when the daemon
// exits or the connection is dropped, which is the signal to reconnect.
bool dbus_alive(const dbus_conn_t *c);

// --- making a call ---
//
// Begins a method call and hands back the writer for its body. `signature` is
// the body's signature ("" for no arguments). Every begin must be followed by a
// send.
dbus_writer_t *dbus_call_begin(dbus_conn_t *c, const char *destination, const char *path, const char *interface,
							   const char *member, const char *signature);

// Sends it and waits up to `timeout_ms` for the reply. True when the reply was
// a method return; false on an error reply, a timeout, or a dead connection.
// `err_out` receives the error name when there was one.
bool dbus_call_send(dbus_conn_t *c, int timeout_ms, char *err_out, size_t err_size);

// A reader over the body of the reply the last dbus_call_send() got, for the
// calls whose answer is the point -- Properties.Get. Valid until the next call
// on this connection, and only meaningful after one that returned true. Bodies
// past a few hundred bytes are not kept: nothing read here is that big.
void dbus_reply_reader(dbus_conn_t *c, dbus_reader_t *r);

// --- replying to a call ---

dbus_writer_t *dbus_reply_begin(dbus_conn_t *c, const dbus_msg_t *call, const char *signature);
bool dbus_reply_send(dbus_conn_t *c);

// An error reply, with no body beyond its message.
bool dbus_error(dbus_conn_t *c, const dbus_msg_t *call, const char *name, const char *message);

// --- emitting a signal ---

dbus_writer_t *dbus_signal_begin(dbus_conn_t *c, const char *path, const char *interface, const char *member,
								 const char *signature);
bool dbus_signal_send(dbus_conn_t *c);

#endif /* DBUSLITE_H */

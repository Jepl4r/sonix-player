#define _GNU_SOURCE 1

#include "tls.h"

#include "src/system/core/config.h"

#include "src/system/core/lang.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// ---------------------------------------------------------------------------
// The bits of OpenSSL's headers used here, restated
// ---------------------------------------------------------------------------
//
// The cross toolchain has no openssl/*.h, so the handful of opaque pointers,
// constants and prototypes used here are written out. Every one of them is
// part of OpenSSL's stable ABI and has the same value in 1.1.1 (the target)
// and 3.x (whatever the host happens to have), which is what lets the same
// file be tested on a PC and run on the device.

typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;
typedef struct ssl_method_st SSL_METHOD;

#define SSL_VERIFY_PEER 0x01

// SSL_ctrl() commands. Both are ancient and fixed.
#define SSL_CTRL_SET_TLSEXT_HOSTNAME 55
#define SSL_CTRL_SET_MIN_PROTO_VERSION 123
#define TLSEXT_NAMETYPE_host_name 0
#define TLS1_2_VERSION 0x0303

#define SSL_ERROR_NONE 0
#define SSL_ERROR_SSL 1
#define SSL_ERROR_WANT_READ 2
#define SSL_ERROR_WANT_WRITE 3
#define SSL_ERROR_SYSCALL 5
#define SSL_ERROR_ZERO_RETURN 6

// X509_V_* verification results worth telling apart. The clock ones are not
// exotic on this device: it has an RTC that a flat battery resets, and every
// certificate on the internet is invalid to a machine that thinks it is 1970.
#define X509_V_OK 0
#define X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT 2
#define X509_V_ERR_CERT_NOT_YET_VALID 9
#define X509_V_ERR_CERT_HAS_EXPIRED 10
#define X509_V_ERR_CRL_NOT_YET_VALID 11
#define X509_V_ERR_CRL_HAS_EXPIRED 12
#define X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT 18
#define X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN 19
#define X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY 20
#define X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE 21
#define X509_V_ERR_CERT_UNTRUSTED 27
#define X509_V_ERR_HOSTNAME_MISMATCH 62

struct openssl_api {
	// libssl
	const SSL_METHOD *(*TLS_client_method)(void);
	SSL_CTX *(*SSL_CTX_new)(const SSL_METHOD *);
	void (*SSL_CTX_free)(SSL_CTX *);
	void (*SSL_CTX_set_verify)(SSL_CTX *, int mode, void *cb);
	int (*SSL_CTX_load_verify_locations)(SSL_CTX *, const char *file, const char *path);
	int (*SSL_CTX_set_default_verify_paths)(SSL_CTX *);
	long (*SSL_CTX_ctrl)(SSL_CTX *, int cmd, long larg, void *parg);
	SSL *(*SSL_new)(SSL_CTX *);
	void (*SSL_free)(SSL *);
	int (*SSL_set_fd)(SSL *, int fd);
	long (*SSL_ctrl)(SSL *, int cmd, long larg, void *parg);
	int (*SSL_set1_host)(SSL *, const char *hostname);
	int (*SSL_connect)(SSL *);
	int (*SSL_read)(SSL *, void *buf, int num);
	int (*SSL_write)(SSL *, const void *buf, int num);
	int (*SSL_shutdown)(SSL *);
	int (*SSL_get_error)(const SSL *, int ret);
	long (*SSL_get_verify_result)(const SSL *);

	// libcrypto
	unsigned long (*ERR_get_error)(void);
	void (*ERR_error_string_n)(unsigned long e, char *buf, size_t len);
	const char *(*X509_verify_cert_error_string)(long n);
	const char *(*OpenSSL_version)(int type); // optional; NULL is fine
};

static struct openssl_api api;
static void *lib_ssl;
static void *lib_crypto;

static enum { LOAD_UNTRIED, LOAD_OK, LOAD_FAILED } load_state = LOAD_UNTRIED;
static pthread_mutex_t load_lock = PTHREAD_MUTEX_INITIALIZER;

static SSL_CTX *shared_ctx;
static char ca_source[512];
static char version_text[128];
static char card_root[256];

struct tls_conn {
	SSL *ssl;
};

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

// dlsym with a complaint, so a rootfs with a differently-built OpenSSL fails
// with a name rather than a mystery.
static bool bind_symbol(void *lib, const char *name, void *slot) {
	void *sym = dlsym(lib, name);
	if (!sym) {
		fprintf(stderr, "tls: %s not found in the library\n", name);
		return false;
	}
	memcpy(slot, &sym, sizeof(sym));
	return true;
}

#define BIND(lib, name)                                                                                      \
	if (!bind_symbol((lib), #name, &api.name)) {                                                             \
		return false;                                                                                        \
	}

static bool load_symbols(void) {
	// The target has 1.1; a development host may have 3. The subset used here
	// is identical in both, so take whichever answers.
	static const char *ssl_names[] = {"libssl.so.1.1", "libssl.so.3", "libssl.so", NULL};
	static const char *crypto_names[] = {"libcrypto.so.1.1", "libcrypto.so.3", "libcrypto.so", NULL};

	for (int i = 0; ssl_names[i] && !lib_ssl; i++) {
		lib_ssl = dlopen(ssl_names[i], RTLD_NOW | RTLD_GLOBAL);
	}
	for (int i = 0; crypto_names[i] && !lib_crypto; i++) {
		lib_crypto = dlopen(crypto_names[i], RTLD_NOW | RTLD_GLOBAL);
	}
	if (!lib_ssl || !lib_crypto) {
		fprintf(stderr, "tls: OpenSSL not loadable (%s); https will be unavailable\n", dlerror());
		return false;
	}

	BIND(lib_ssl, TLS_client_method);
	BIND(lib_ssl, SSL_CTX_new);
	BIND(lib_ssl, SSL_CTX_free);
	BIND(lib_ssl, SSL_CTX_set_verify);
	BIND(lib_ssl, SSL_CTX_load_verify_locations);
	BIND(lib_ssl, SSL_CTX_set_default_verify_paths);
	BIND(lib_ssl, SSL_CTX_ctrl);
	BIND(lib_ssl, SSL_new);
	BIND(lib_ssl, SSL_free);
	BIND(lib_ssl, SSL_set_fd);
	BIND(lib_ssl, SSL_ctrl);
	BIND(lib_ssl, SSL_set1_host);
	BIND(lib_ssl, SSL_connect);
	BIND(lib_ssl, SSL_read);
	BIND(lib_ssl, SSL_write);
	BIND(lib_ssl, SSL_shutdown);
	BIND(lib_ssl, SSL_get_error);
	BIND(lib_ssl, SSL_get_verify_result);

	BIND(lib_crypto, ERR_get_error);
	BIND(lib_crypto, ERR_error_string_n);
	BIND(lib_crypto, X509_verify_cert_error_string);

	// Only for the log line, and its name changed between 1.0 and 1.1.
	void *v = dlsym(lib_crypto, "OpenSSL_version");
	memcpy(&api.OpenSSL_version, &v, sizeof(v));

	return true;
}

#undef BIND

static bool file_exists(const char *path) {
	struct stat st;
	return path && path[0] && stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static bool dir_exists(const char *path) {
	struct stat st;
	return path && path[0] && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// Loads trust anchors into `ctx`. Returns false when nothing was found, which
// is a refusal to continue rather than a warning: verifying against an empty
// store rejects every certificate, and the user would see "connection failed"
// with no hint that a file is missing.
static bool load_ca(SSL_CTX *ctx) {
	char candidate[512];

	const char *configured = config_get("network", "ca_bundle", NULL);
	if (configured && configured[0] && file_exists(configured)) {
		if (api.SSL_CTX_load_verify_locations(ctx, configured, NULL) == 1) {
			snprintf(ca_source, sizeof(ca_source), "%s", configured);
			return true;
		}
		fprintf(stderr, "tls: [network] ca_bundle = %s could not be read\n", configured);
	}

	if (card_root[0]) {
		snprintf(candidate, sizeof(candidate), "%s/.local/cacert.pem", card_root);
		if (file_exists(candidate) && api.SSL_CTX_load_verify_locations(ctx, candidate, NULL) == 1) {
			snprintf(ca_source, sizeof(ca_source), "%s", candidate);
			return true;
		}
	}

	// This OpenSSL's own OPENSSLDIR is "/etc/ssl", so these two are where it
	// would look by itself. Naming them explicitly is not redundant: it lets
	// the log say which one answered.
	if (file_exists("/etc/ssl/cert.pem") &&
		api.SSL_CTX_load_verify_locations(ctx, "/etc/ssl/cert.pem", NULL) == 1) {
		snprintf(ca_source, sizeof(ca_source), "/etc/ssl/cert.pem");
		return true;
	}
	if (dir_exists("/etc/ssl/certs") &&
		api.SSL_CTX_load_verify_locations(ctx, NULL, "/etc/ssl/certs") == 1) {
		snprintf(ca_source, sizeof(ca_source), "/etc/ssl/certs/");
		return true;
	}

	// Whatever the library was built to look at, in case it is neither.
	if (api.SSL_CTX_set_default_verify_paths(ctx) == 1) {
		snprintf(ca_source, sizeof(ca_source), "(OpenSSL default paths)");
		return true;
	}

	return false;
}

// Everything one-time: the libraries, the symbols, the context and its trust
// store. Building the context per connection would re-parse the whole PEM
// bundle for every station.
static bool ensure_loaded(void) {
	pthread_mutex_lock(&load_lock);
	if (load_state != LOAD_UNTRIED) {
		pthread_mutex_unlock(&load_lock);
		return load_state == LOAD_OK;
	}
	load_state = LOAD_FAILED;

	if (!load_symbols()) {
		pthread_mutex_unlock(&load_lock);
		return false;
	}

	if (api.OpenSSL_version) {
		const char *v = api.OpenSSL_version(0); // OPENSSL_VERSION
		snprintf(version_text, sizeof(version_text), "%s", v ? v : "OpenSSL");
	} else {
		snprintf(version_text, sizeof(version_text), "OpenSSL");
	}

	shared_ctx = api.SSL_CTX_new(api.TLS_client_method());
	if (!shared_ctx) {
		fprintf(stderr, "tls: SSL_CTX_new failed\n");
		pthread_mutex_unlock(&load_lock);
		return false;
	}

	// TLS 1.2 at the least. 1.0 and 1.1 are dead everywhere that matters, and
	// asking by version rather than by SSL_OP_NO_* flags keeps this correct
	// across OpenSSL versions.
	api.SSL_CTX_ctrl(shared_ctx, SSL_CTRL_SET_MIN_PROTO_VERSION, TLS1_2_VERSION, NULL);

	if (!load_ca(shared_ctx)) {
		fprintf(stderr, "tls: no CA bundle found -- put Mozilla's cacert.pem at\n");
		fprintf(stderr, "tls:   /etc/ssl/cert.pem (firmware) or <card>/.local/cacert.pem\n");
		api.SSL_CTX_free(shared_ctx);
		shared_ctx = NULL;
		pthread_mutex_unlock(&load_lock);
		return false;
	}

	api.SSL_CTX_set_verify(shared_ctx, SSL_VERIFY_PEER, NULL);

	fprintf(stderr, "tls: %s, certificates from %s\n", version_text, ca_source);
	load_state = LOAD_OK;
	pthread_mutex_unlock(&load_lock);
	return true;
}

bool tls_available(void) {
	return ensure_loaded();
}

const char *tls_version(void) {
	return version_text;
}

const char *tls_ca_source(void) {
	return ca_source[0] ? ca_source : NULL;
}

void tls_set_card_root(const char *sd_root) {
	if (sd_root && sd_root[0]) {
		snprintf(card_root, sizeof(card_root), "%s", sd_root);
	} else {
		card_root[0] = '\0';
	}
}

// ---------------------------------------------------------------------------
// Connecting
// ---------------------------------------------------------------------------

// Turns a failed handshake into something a person can act on. The order
// matters: a verification failure is far more informative than the generic
// protocol error that accompanies it, so it is asked about first.
static void explain_failure(SSL *ssl, int ret, char *err, size_t err_size) {
	long verify = api.SSL_get_verify_result(ssl);
	int code = api.SSL_get_error(ssl, ret);

	if (verify != X509_V_OK) {
		switch (verify) {
		case X509_V_ERR_CERT_HAS_EXPIRED:
		case X509_V_ERR_CRL_HAS_EXPIRED:
			snprintf(err, err_size, "%s",
					 tr("tls_cert_expired"));
			return;
		case X509_V_ERR_CERT_NOT_YET_VALID:
		case X509_V_ERR_CRL_NOT_YET_VALID:
			snprintf(err, err_size, "%s",
					 tr("tls_cert_not_yet_valid"));
			return;
		case X509_V_ERR_HOSTNAME_MISMATCH:
			snprintf(err, err_size, "%s", tr("tls_cert_name_mismatch"));
			return;
		case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:
		case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
		case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE:
		case X509_V_ERR_CERT_UNTRUSTED:
			snprintf(err, err_size, "%s",
					 tr("tls_cert_unknown_authority"));
			return;
		case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
		case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:
			snprintf(err, err_size, "%s", tr("tls_cert_self_signed"));
			return;
		default: {
			const char *text = api.X509_verify_cert_error_string(verify);
			snprintf(err, err_size, tr("tls_cert_rejected"), text ? text : tr("tls_reason_unknown"));
			return;
		}
		}
	}

	if (code == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK)) {
		snprintf(err, err_size, "%s", tr("tls_timeout"));
		return;
	}
	if (code == SSL_ERROR_SYSCALL || code == SSL_ERROR_ZERO_RETURN) {
		snprintf(err, err_size, "%s", tr("tls_handshake_closed"));
		return;
	}

	unsigned long e = api.ERR_get_error();
	if (e) {
		char buf[160];
		api.ERR_error_string_n(e, buf, sizeof(buf));
		snprintf(err, err_size, tr("tls_error"), buf);
	} else {
		snprintf(err, err_size, "%s", tr("tls_handshake_failed"));
	}
}

tls_conn_t *tls_client(int fd, const char *hostname, char *err, size_t err_size) {
	if (err && err_size) {
		err[0] = '\0';
	}
	if (fd < 0 || !hostname || !hostname[0]) {
		return NULL;
	}
	if (!ensure_loaded()) {
		if (err) {
			snprintf(err, err_size, "%s", tr("tls_unsupported"));
		}
		return NULL;
	}

	SSL *ssl = api.SSL_new(shared_ctx);
	if (!ssl) {
		if (err) {
			snprintf(err, err_size, "%s", tr("tls_no_memory"));
		}
		return NULL;
	}

	// Two different things that both happen to be the hostname, and leaving
	// out either one is a quiet mistake:
	//
	//   SNI tells a server sharing one address among many sites which
	//   certificate to send. Without it a shared host answers with the wrong
	//   one and verification fails for reasons that look unrelated.
	//
	//   SSL_set1_host is what makes OpenSSL check the name inside the
	//   certificate against the requested one. Without it a valid certificate
	//   for any other domain would sail through, and the whole exercise would
	//   be pointless.
	api.SSL_ctrl(ssl, SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name, (void *)hostname);
	api.SSL_set1_host(ssl, hostname);

	if (api.SSL_set_fd(ssl, fd) != 1) {
		api.SSL_free(ssl);
		if (err) {
			snprintf(err, err_size, "%s", tr("tls_handshake_failed"));
		}
		return NULL;
	}

	int ret = api.SSL_connect(ssl);
	if (ret != 1) {
		if (err) {
			explain_failure(ssl, ret, err, err_size);
		}
		api.SSL_free(ssl);
		return NULL;
	}

	tls_conn_t *c = calloc(1, sizeof(*c));
	if (!c) {
		api.SSL_free(ssl);
		return NULL;
	}
	c->ssl = ssl;
	return c;
}

int tls_read(tls_conn_t *c, void *buf, int len) {
	if (!c || !c->ssl || len <= 0) {
		return -1;
	}
	int n = api.SSL_read(c->ssl, buf, len);
	if (n > 0) {
		return n;
	}

	// A clean close and a torn one are both "the stream ended" to the caller;
	// only a genuine error deserves -1. SSL_ERROR_SYSCALL with no errno is
	// what a server that just drops the connection produces, and treating that
	// as an error rather than an end of stream would turn every station that
	// closes normally into a reported failure.
	int code = api.SSL_get_error(c->ssl, n);
	if (code == SSL_ERROR_ZERO_RETURN) {
		return 0;
	}
	if (code == SSL_ERROR_SYSCALL && errno == 0) {
		return 0;
	}
	return -1;
}

// SSL_write() ends in a plain write() on the socket, and unlike the send() in
// http.c it cannot be given MSG_NOSIGNAL: OpenSSL owns that call. So a peer
// that has gone away raises SIGPIPE, and on this device a process that dies
// takes the whole player with it -- the launcher reboots the machine. The
// protection is main.c's signal(SIGPIPE, SIG_IGN), set before anything else
// starts; reusing this file anywhere without that has to repeat it.
int tls_write(tls_conn_t *c, const void *buf, int len) {
	if (!c || !c->ssl || len <= 0) {
		return -1;
	}
	int n = api.SSL_write(c->ssl, buf, len);
	return n > 0 ? n : -1;
}

void tls_free(tls_conn_t *c) {
	if (!c) {
		return;
	}
	if (c->ssl) {
		// One try, no waiting for the peer's answer. The socket is about to be
		// closed anyway, and a station that has stopped talking must not be
		// able to hold the thread here.
		api.SSL_shutdown(c->ssl);
		api.SSL_free(c->ssl);
	}
	free(c);
}

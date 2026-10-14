#include "cardimport.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/streaming/streamerr.h"

// See cardimport.h.

#define SERVICES_MAX 6

typedef struct {
	const char *service;
	const char *file_name;
	cardimport_validate_fn validate;
	bool pending;  // to be looked for
	bool asked;	   // by the page: an offline outcome is said, not kept quiet
	bool running;
	bool has_last;
	bool undelivered; // nobody was listening yet (an attempt at start-up)
	bool last_asked;
	cardimport_result_t last;
	char last_message[256];
} entry_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static entry_t entries[SERVICES_MAX];
static int entry_count;
static char root[400];
static bool worker_started;
#define LISTENERS_MAX 4
static cardimport_listener_t listeners[LISTENERS_MAX];
static int listener_count;

void cardimport_register(const char *service, const char *file_name, cardimport_validate_fn validate) {
	if (!service || !file_name || !validate || entry_count >= SERVICES_MAX) {
		return;
	}
	pthread_mutex_lock(&lock);
	entries[entry_count++] = (entry_t){.service = service, .file_name = file_name, .validate = validate};
	pthread_mutex_unlock(&lock);
}

void cardimport_add_listener(cardimport_listener_t l) {
	if (!l || listener_count >= LISTENERS_MAX) {
		return;
	}
	listeners[listener_count++] = l;
	// An attempt made at start-up, before the interface was there to hear
	// it: told now, so "signed in as..." is not lost to the boot.
	for (int i = 0; i < entry_count; i++) {
		pthread_mutex_lock(&lock);
		bool tell = entries[i].undelivered && (entries[i].last != CARDIMPORT_OFFLINE || entries[i].last_asked);
		cardimport_result_t result = entries[i].last;
		char message[256];
		snprintf(message, sizeof(message), "%s", entries[i].last_message);
		pthread_mutex_unlock(&lock);
		if (tell) {
			l(entries[i].service, result, message);
		}
	}
}

bool cardimport_last(const char *service, cardimport_result_t *result, char *message, size_t size) {
	bool found = false;
	pthread_mutex_lock(&lock);
	for (int i = 0; i < entry_count && !found; i++) {
		if (service && strcmp(entries[i].service, service) == 0 && entries[i].has_last) {
			found = true;
			if (result) {
				*result = entries[i].last;
			}
			if (message) {
				snprintf(message, size, "%s", entries[i].last_message);
			}
		}
	}
	pthread_mutex_unlock(&lock);
	return found;
}

// The file of an entry on the card: "<name>", else "<name>.txt" (Windows'
// hidden extension). False when neither is there.
static bool find_file(const char *card, const entry_t *e, char *out, size_t size) {
	static const char *const SUFFIXES[] = {"", ".txt"};
	for (size_t i = 0; i < sizeof(SUFFIXES) / sizeof(SUFFIXES[0]); i++) {
		snprintf(out, size, "%s/%s%s", card, e->file_name, SUFFIXES[i]);
		struct stat st;
		if (stat(out, &st) == 0 && S_ISREG(st.st_mode)) {
			return true;
		}
	}
	return false;
}

// The non-empty lines of the file, trimmed: a BOM at its head, "\r", spaces
// and tabs at both ends of a line. -1 when it cannot be read or is too big.
static int read_lines(const char *path, char lines[][CARDIMPORT_LINE_MAX]) {
	struct stat st;
	if (stat(path, &st) != 0 || st.st_size > CARDIMPORT_MAX_BYTES) {
		return -1;
	}
	FILE *f = fopen(path, "rb");
	if (!f) {
		return -1;
	}
	char buf[CARDIMPORT_MAX_BYTES + 1];
	size_t n = fread(buf, 1, CARDIMPORT_MAX_BYTES, f);
	fclose(f);
	buf[n] = '\0';
	char *p = buf;
	if (n >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
		p += 3;
	}
	int count = 0;
	while (*p && count < CARDIMPORT_MAX_LINES) {
		size_t len = strcspn(p, "\n");
		char *next = p[len] ? p + len + 1 : p + len;
		while (len > 0 && (p[len - 1] == '\r' || p[len - 1] == ' ' || p[len - 1] == '\t')) {
			len--;
		}
		while (len > 0 && (*p == ' ' || *p == '\t' || *p == '\r')) {
			p++;
			len--;
		}
		if (len > 0 && len < CARDIMPORT_LINE_MAX) {
			memcpy(lines[count], p, len);
			lines[count][len] = '\0';
			count++;
		}
		p = next;
	}
	// Nothing of the secret stays in this thread's stack.
	memset(buf, 0, sizeof(buf));
	return count;
}

static const char *result_name(cardimport_result_t r) {
	static const char *const NAMES[] = {"accepted", "no network", "refused", "blocked by anti-bot", "invalid"};
	return (unsigned)r < sizeof(NAMES) / sizeof(NAMES[0]) ? NAMES[r] : "?";
}

static void attempt(entry_t *e, const char *card, bool asked) {
	char path[600];
	if (!card[0] || !find_file(card, e, path, sizeof(path))) {
		return;
	}
	char message[256] = "";
	cardimport_result_t result;
	char(*lines)[CARDIMPORT_LINE_MAX] = calloc(CARDIMPORT_MAX_LINES, CARDIMPORT_LINE_MAX);
	int count = lines ? read_lines(path, lines) : -1;
	if (count <= 0) {
		result = CARDIMPORT_INVALID;
	} else if (!streamerr_online()) {
		// No request at all without a network: the file waits for the next
		// occasion, as for a request that went unanswered.
		result = CARDIMPORT_OFFLINE;
	} else {
		result = e->validate((const char(*)[CARDIMPORT_LINE_MAX])lines, count, message, sizeof(message));
	}
	if (lines) {
		memset(lines, 0, (size_t)CARDIMPORT_MAX_LINES * CARDIMPORT_LINE_MAX);
		free(lines);
	}
	const char *file = strrchr(path, '/') + 1;
	printf("cardimport: %s: %s (%d line%s): %s\n", e->service, file, count, count == 1 ? "" : "s",
		   result_name(result));
	// The outcomes decided here, before any service had a word: said with
	// the file's name, so the user knows which file and what happens to it.
	if (!message[0] && result == CARDIMPORT_OFFLINE) {
		snprintf(message, sizeof(message), tr("streaming_import_offline"), file);
	} else if (!message[0] && result == CARDIMPORT_INVALID) {
		snprintf(message, sizeof(message), tr("streaming_import_invalid"), file);
	}
	switch (result) {
	case CARDIMPORT_OK:
		// The account is in internal storage now: the secret leaves the card.
		if (unlink(path) != 0) {
			fprintf(stderr, "cardimport: could not delete %s\n", path);
		}
		sync();
		break;
	case CARDIMPORT_REFUSED:
	case CARDIMPORT_INVALID: {
		char bad[640];
		snprintf(bad, sizeof(bad), "%s.bad", path);
		remove(bad);
		if (rename(path, bad) != 0) {
			fprintf(stderr, "cardimport: could not rename %s\n", path);
		}
		sync();
		break;
	}
	case CARDIMPORT_OFFLINE:
	case CARDIMPORT_BLOCKED:
		break; // the file stays, tried again next time
	}
	// An attempt nobody asked for that only found no network says nothing:
	// a player started with Wi-Fi off would greet the user with it.
	pthread_mutex_lock(&lock);
	e->has_last = true;
	e->undelivered = listener_count == 0;
	e->last_asked = asked;
	e->last = result;
	snprintf(e->last_message, sizeof(e->last_message), "%s", message);
	pthread_mutex_unlock(&lock);
	for (int i = 0; i < listener_count && (result != CARDIMPORT_OFFLINE || asked); i++) {
		listeners[i](e->service, result, message);
	}
}

static void *worker_main(void *arg) {
	(void)arg;
	thread_be_low_priority("cardimport");
	for (;;) {
		pthread_mutex_lock(&lock);
		int index = -1;
		while (index < 0) {
			for (int i = 0; i < entry_count && index < 0; i++) {
				if (entries[i].pending) {
					index = i;
				}
			}
			if (index < 0) {
				pthread_cond_wait(&wake, &lock);
			}
		}
		entry_t *e = &entries[index];
		bool asked = e->asked;
		e->pending = e->asked = false;
		e->running = true;
		char card[400];
		snprintf(card, sizeof(card), "%s", root);
		pthread_mutex_unlock(&lock);

		attempt(e, card, asked);

		pthread_mutex_lock(&lock);
		e->running = false;
		pthread_mutex_unlock(&lock);
	}
	return NULL;
}

// Queues the entries that have a file on the card (`only` NULL: all of them).
// The stat()s are done here, on the caller's thread: two per service, and the
// thread is only started once there is something for it to do.
static void queue_checks(const char *only, bool asked) {
	pthread_mutex_lock(&lock);
	char card[400];
	snprintf(card, sizeof(card), "%s", root);
	bool any = false;
	for (int i = 0; i < entry_count; i++) {
		entry_t *e = &entries[i];
		char path[600];
		if ((only && strcmp(only, e->service) != 0) || !card[0] || !find_file(card, e, path, sizeof(path))) {
			continue;
		}
		if (e->running && !asked) {
			continue; // being tried right now
		}
		e->pending = true;
		e->asked = e->asked || asked;
		any = true;
	}
	if (any && !worker_started) {
		pthread_t th;
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		pthread_attr_setstacksize(&attr, 256 * 1024); // TLS, as the services' workers
		worker_started = pthread_create(&th, &attr, worker_main, NULL) == 0;
		pthread_attr_destroy(&attr);
	}
	if (any) {
		pthread_cond_signal(&wake);
	}
	pthread_mutex_unlock(&lock);
}

void cardimport_set_root(const char *sd_root) {
	pthread_mutex_lock(&lock);
	snprintf(root, sizeof(root), "%s", sd_root ? sd_root : "");
	pthread_mutex_unlock(&lock);
	queue_checks(NULL, false);
}

void cardimport_check(const char *service) { queue_checks(service, true); }

bool cardimport_busy(const char *service) {
	bool busy = false;
	pthread_mutex_lock(&lock);
	for (int i = 0; i < entry_count; i++) {
		if (service && strcmp(entries[i].service, service) == 0) {
			busy = entries[i].pending || entries[i].running;
		}
	}
	pthread_mutex_unlock(&lock);
	return busy;
}

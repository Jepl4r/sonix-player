#include "bootlogo.h"

#include "src/system/core/utils.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// See bootlogo.h for where these numbers come from -- they are the stock
// firmware's own, read out of its init script and its binary, not guesses.
#define MTD_DEVICE "/dev/mtd5"
#define MARKER_OFFSET "0x20000"
#define MARKER_LENGTH 7 // "theme:N"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static volatile bool worker_running;
static volatile int pending_theme; // what the worker should write

int bootlogo_current(void) {
	if (access(MTD_DEVICE, R_OK) != 0) {
		return 0;
	}

	// The stock player reads it the same way, into a temp file: nanddump wants
	// somewhere to put its bytes and will not simply hand them back.
	char cmd[256];
	const char *tmp = "/tmp/.bootlogo_marker";
	snprintf(cmd, sizeof(cmd), "nanddump -q -s %s -l %d %s -a > %s 2>/dev/null", MARKER_OFFSET, MARKER_LENGTH,
			 MTD_DEVICE, tmp);
	if (system(cmd) != 0) {
		return 0;
	}

	FILE *f = fopen(tmp, "rb");
	if (!f) {
		return 0;
	}
	char buf[MARKER_LENGTH + 1] = {0};
	size_t got = fread(buf, 1, MARKER_LENGTH, f);
	fclose(f);
	remove(tmp);

	if (got < MARKER_LENGTH || strncmp(buf, "theme:", 6) != 0) {
		return 0;
	}
	if (buf[6] == '1') {
		return 1;
	}
	if (buf[6] == '2') {
		return 2;
	}
	return 0;
}

static void *write_main(void *arg) {
	(void)arg;
	thread_be_background("boot logo");

	for (;;) {
		int want = pending_theme;

		// Already right: nothing is written. A theme switched back and forth
		// must not cost an erase cycle each time.
		if (bootlogo_current() == want) {
			break;
		}

		char cmd[256];
		snprintf(cmd, sizeof(cmd), "flash_erase -q %s %s 1", MTD_DEVICE, MARKER_OFFSET);
		if (system(cmd) != 0) {
			fprintf(stderr, "bootlogo: flash_erase failed\n");
			break;
		}

		// The stock player's own command, %-256s and all: the C format pads
		// the marker out to 256 columns, and nandwrite's -p fills the rest of
		// the page. The shell drops the padding again when it splits the
		// arguments, which is why only the seven bytes that matter land.
		char marker[32];
		snprintf(marker, sizeof(marker), "theme:%d", want);
		// Its own buffer, because %-256s really does write 256 columns and the
		// rest of the command has to fit after them.
		char write_cmd[512];
		snprintf(write_cmd, sizeof(write_cmd), "printf %-256s | nandwrite -q -s %s -p %s -", marker, MARKER_OFFSET,
				 MTD_DEVICE);
		if (system(write_cmd) != 0) {
			fprintf(stderr, "bootlogo: nandwrite failed\n");
			break;
		}

		printf("bootlogo: theme:%d marker written to %s\n", want, MTD_DEVICE);

		// The theme may have been switched again while the flash was busy.
		if (pending_theme == want) {
			break;
		}
	}

	pthread_mutex_lock(&lock);
	worker_running = false;
	pthread_mutex_unlock(&lock);
	return NULL;
}

void bootlogo_follow_theme(bool dark) {
	// logo1 is the pale image, logo2 the dark one.
	int want = dark ? 2 : 1;

	if (access(MTD_DEVICE, W_OK) != 0) {
		return; // no such flash here (the host build, or a different device)
	}

	pthread_mutex_lock(&lock);
	pending_theme = want;
	if (worker_running) {
		pthread_mutex_unlock(&lock); // the one already running will pick it up
		return;
	}
	worker_running = true;
	pthread_mutex_unlock(&lock);

	pthread_t thread;
	if (pthread_create(&thread, NULL, write_main, NULL) != 0) {
		pthread_mutex_lock(&lock);
		worker_running = false;
		pthread_mutex_unlock(&lock);
		return;
	}
	pthread_detach(thread);
}

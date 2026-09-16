#include "logging.h"

#include <sys/klog.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/core/lang.h"

// Where the log goes when it is on: beside everything else the player writes.
#define LOG_SUBDIR ".local"
#define LOG_FILE_NAME "sonix_player.log"

// The player prints a good deal before the card is mounted: which block device
// it found, what the battery reported, whether the clock survived. That cannot
// go to the card yet and must not go to the device's own storage, so it is held
// until the card turns up. A startup is a couple of kilobytes; anything past
// this is dropped.
#define HELD_MAX (32 * 1024)

// Where the held output waits. It is on tmpfs, so it never touches the card or
// the device's own flash, and it is unlinked the moment it is opened: nothing
// is left behind by a boot that does not finish, and the space goes back to
// the kernel when the descriptor closes.
//
// A real file and not an fmemopen() over a static array, for one reason: an
// fmemopen stream HAS NO DESCRIPTOR. fileno() returns -1, so the dup2 in
// redirect_to() fails with EBADF, stdout is never pointed at it and nothing
// reaches the buffer at all. In the USB path the same failure also leaves
// descriptors 1 and 2 open on the card, which keeps it busy and defeats the
// unmount that logging_suspend_for_usb() exists to allow.
#define EARLY_HELD_PATH "/tmp/sonix-log-early"
#define USB_HELD_PATH "/tmp/sonix-log-usb"

static FILE *early_stream;

static char sd_log_path[512];
static bool on_sd;

// The stream stdout/stderr currently feed. Kept so a switch can close the
// previous one: dup2 copies the descriptor into 1 and 2, but the original stays
// open too, and an fd left open on a file on the card keeps the card busy, so
// the USB export can never unmount it.
static FILE *active_stream;

// The card can go away under the log: the USB export unmounts it, and so does
// pulling it out. While it is gone everything printed is held the same way and
// appended to the file when the card comes back, so the export and hotplug
// diagnostics are not the lines that get lost.
static FILE *usb_stream;
static bool usb_was_on_sd;

const char *logging_path(void) {
	if (on_sd && sd_log_path[0]) {
		return sd_log_path;
	}
	// The setting is on but the file is not open: the card is out, or it came
	// back read-only. Reporting that is not the same as reporting "disabled",
	// which made a card change look like the option had turned itself off.
	if (config_get_bool("system", "log_to_sd", false)) {
		return tr("log_card_missing");
	}
	return tr("log_disabled");
}

// The switch shows the setting, not whether the file happens to be open right
// now. Pulling the card closes the file, and reading that back as the state of
// the option made "Log to microSD" flip itself off and stay off, even though
// nothing had ever written false to the config.
bool logging_to_sd(void) { return config_get_bool("system", "log_to_sd", false); }

// A place to hold output that cannot go where it belongs yet: a real file on
// tmpfs, unlinked at once so it has a name for nobody. NULL when tmpfs will
// not take it, and the caller then falls back to /dev/null.
static FILE *open_held(const char *path) {
	FILE *file = fopen(path, "w+");
	if (!file) {
		return NULL;
	}
	unlink(path);
	return file;
}

// Copies what a held stream collected into `destination` and closes it. The
// copy goes through a small stack buffer rather than one big read: the whole
// point of moving this out of a static array was to stop reserving 32 kB for
// a startup that prints two.
static void drain_held(FILE *held, FILE *destination) {
	if (!held) {
		return;
	}
	fflush(held);
	long size = ftell(held);
	rewind(held);

	if (destination && size > 0) {
		if (size > HELD_MAX) {
			size = HELD_MAX;
		}
		char chunk[512];
		long left = size;
		while (left > 0) {
			size_t want = (size_t)(left < (long)sizeof(chunk) ? left : (long)sizeof(chunk));
			size_t got = fread(chunk, 1, want, held);
			if (got == 0) {
				break;
			}
			fwrite(chunk, 1, got, destination);
			left -= (long)got;
		}
		fflush(destination);
	}
	fclose(held);
}

// Points stdout and stderr at `stream`. Line buffered, so a crash still leaves
// everything up to the last newline behind. Closes the stream they fed before.
//
// False when the descriptors could not be moved, and then nothing is closed:
// closing the previous stream after a failed dup2 would leave stdout writing
// into a descriptor nobody owns any more.
static bool redirect_to(FILE *stream) {
	if (!stream) {
		return false;
	}

	setvbuf(stream, NULL, _IOLBF, 0);
	int fd = fileno(stream);
	if (fd < 0 || dup2(fd, fileno(stdout)) < 0 || dup2(fd, fileno(stderr)) < 0) {
		return false;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IOLBF, 0);

	if (active_stream && active_stream != stream) {
		fclose(active_stream);
	}
	active_stream = stream;
	return true;
}

void logging_init(void) {
	const char *override = getenv("SONIX_LOG");
	if (override && override[0]) {
		FILE *file = fopen(override, "w");
		if (file && redirect_to(file)) {
			on_sd = true;
			snprintf(sd_log_path, sizeof(sd_log_path), "%s", override);
		} else if (file) {
			fclose(file);
		}
		return;
	}

#ifndef HOST_BUILD
	// Everything printed from here is held until the card turns up.
	early_stream = open_held(EARLY_HELD_PATH);
	if (early_stream && !redirect_to(early_stream)) {
		fclose(early_stream);
		early_stream = NULL;
	}
#endif
}

// Stops holding startup output and lets it go, either into the card's log or
// nowhere at all.
static void flush_early(FILE *destination) {
	if (!early_stream) {
		return;
	}

	if (active_stream == early_stream) {
		active_stream = NULL; // drain_held closes it, not redirect_to
	}
	drain_held(early_stream, destination);
	early_stream = NULL;
}

// Opens the card's log and makes it the destination. Returns false if the card
// will not take it.
static bool open_sd_log(void) {
	if (!sd_log_path[0]) {
		return false;
	}

	FILE *file = fopen(sd_log_path, "a");
	if (!file) {
		return false;
	}

	flush_early(file);
	if (!redirect_to(file)) { // closes whatever was feeding stdout, usb_stream included
		fclose(file);
		return false;
	}
	usb_stream = NULL;
	on_sd = true;

	printf("log: writing to %s\n", sd_log_path);
	return true;
}

// Sends output nowhere. /dev/null keeps every printf in the code harmless
// without any of them having to check whether logging is on.
static void open_null_log(void) {
	flush_early(NULL);

	FILE *sink = fopen("/dev/null", "w");
	if (sink) {
		if (redirect_to(sink)) {
			usb_stream = NULL; // redirect_to has just closed it
		} else {
			fclose(sink);
		}
	}
	on_sd = false;
}

void logging_attach_sd(const char *sd_root) {
	if (getenv("SONIX_LOG")) {
		return; // an explicit path wins over everything
	}

	if (sd_root && sd_root[0]) {
		char folder[512];
		if (snprintf(folder, sizeof(folder), "%s/%s", sd_root, LOG_SUBDIR) < (int)sizeof(folder)) {
			mkdir(folder, 0755);
			if (snprintf(sd_log_path, sizeof(sd_log_path), "%s/%s", folder, LOG_FILE_NAME) >=
				(int)sizeof(sd_log_path)) {
				sd_log_path[0] = '\0';
			}
		}
	}

	// If the card has just come back, append the lines buffered while it was
	// away rather than starting fresh and losing the part that explains what
	// happened.
	logging_resume_after_usb();
	if (on_sd) {
		return;
	}

	if (config_get_bool("system", "log_to_sd", false) && open_sd_log()) {
		return;
	}

	open_null_log();
}

// ---------------------------------------------------------------------------
// The USB mass-storage export unmounts the card, and so does pulling it out;
// the log usually lives on the card. See the buffer declared at the top.
// ---------------------------------------------------------------------------

void logging_suspend_for_usb(void) {
	if (!on_sd || usb_stream) {
		// Nothing holding the card, nothing to release, and usb_was_on_sd must
		// not be touched here. Pulling a card fires two matching uevents, so
		// this runs twice: the first call recorded that the log was on the
		// card, the second overwrote that with "no", and the log could then
		// never be brought back.
		return;
	}
	usb_was_on_sd = true;

	printf("log: releasing the card log for the USB export\n");
	fflush(stdout);

	usb_stream = open_held(USB_HELD_PATH);
	if (usb_stream && !redirect_to(usb_stream)) { // closes the file on the card
		fclose(usb_stream);
		usb_stream = NULL;
	}
	if (!usb_stream) {
		open_null_log();
	}
	on_sd = false;
}

void logging_resume_after_usb(void) {
	if (!usb_was_on_sd) {
		return;
	}

	FILE *file = sd_log_path[0] ? fopen(sd_log_path, "a") : NULL;
	if (file) {
		// The held lines go in before the redirect, because draining closes
		// the stream stdout is still pointing at.
		if (active_stream == usb_stream) {
			active_stream = NULL;
		}
		drain_held(usb_stream, file);
		usb_stream = NULL;

		if (!redirect_to(file)) {
			fclose(file);
			open_null_log();
			return;
		}
		on_sd = true;
		usb_was_on_sd = false; // only now: the flag is spent when it has paid off
		printf("log: card log resumed after the USB export\n");
	} else {
		// The card did not come back writable. usb_was_on_sd stays set so a
		// later attempt (a reinsert, or the switch being toggled) can still
		// put the log back on the card.
		if (active_stream == usb_stream) {
			active_stream = NULL;
		}
		drain_held(usb_stream, NULL);
		usb_stream = NULL;
		open_null_log();
	}
}

void logging_set_to_sd(bool enabled) {
	config_set_bool("system", "log_to_sd", enabled);
	config_save();

	if (enabled == on_sd) {
		return;
	}

	if (enabled) {
		if (!open_sd_log()) {
			open_null_log(); // no card, or it would not take the file
		}
		return;
	}

	printf("log: card logging off\n");
	fflush(stdout);
	open_null_log();
}

// ---------------------------------------------------------------------------
// What the kernel says about the previous run
// ---------------------------------------------------------------------------

// Lines worth repeating. The out-of-memory killer announces itself with the
// first three; the last two are what a card that stops answering looks like,
// and that ends a scan just as abruptly.
static const char *const KERNEL_INTERESTING[] = {
	"Out of memory", "oom-kill", "Killed process", "lowmemorykiller",
	"mmc0: ",		 "I/O error", "EXT4-fs error",  "FAT-fs error",
};

static bool kernel_line_interesting(const char *line) {
	for (size_t i = 0; i < sizeof(KERNEL_INTERESTING) / sizeof(KERNEL_INTERESTING[0]); i++) {
		if (strstr(line, KERNEL_INTERESTING[i])) {
			return true;
		}
	}
	return false;
}

// How much of the ring buffer to take, and how many lines to repeat. The buffer
// is a few hundred kilobytes on a desktop and far less here; the cap is on this
// side so a card throwing errors by the thousand cannot fill the player's log
// with them.
#define KERNEL_BUFFER_BYTES 16384
#define KERNEL_LINES_MAX 12

void logging_report_previous_run(void) {
	char *buffer = malloc(KERNEL_BUFFER_BYTES);
	if (!buffer) {
		return;
	}

	// SYSLOG_ACTION_READ_ALL (3): the whole buffer without consuming it, so
	// this can run at every start and nothing else loses the messages.
	int got = klogctl(3, buffer, KERNEL_BUFFER_BYTES - 1);
	if (got <= 0) {
		// Not an error worth a line: a kernel built without the syscall, or a
		// build that reserves it for root, both land here.
		free(buffer);
		return;
	}
	buffer[got] = '\0';

	int said = 0;
	char *line = strtok(buffer, "\n");
	while (line && said < KERNEL_LINES_MAX) {
		if (kernel_line_interesting(line)) {
			if (said == 0) {
				printf("kernel: what the kernel said before this start --\n");
			}
			printf("kernel:   %s\n", line);
			said++;
		}
		line = strtok(NULL, "\n");
	}
	if (said > 0) {
		fflush(stdout);
	}

	free(buffer);
}

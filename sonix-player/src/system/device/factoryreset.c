#include "factoryreset.h"

#include "src/system/audio/audio.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/device/clock.h"
#include "src/system/library/library.h"
#include "src/system/core/logging.h"
#include "src/system/streaming/qobuzcache.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/tidalcache.h"
#include "src/system/remote/dlna.h"
#include "src/system/device/sysserver.h"
#include "src/system/device/system.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// How the original firmware does it, and why that is the only safe way
// ---------------------------------------------------------------------------
//
// The rootfs already contains the two pieces that do all the work:
//
//   /etc/init.d/S39_recovery.recovery   runs at every boot; if the file
//                                       /data/recovery_all exists it reads it,
//                                       runs recovery_all.sh and reboots
//   /usr/bin/recovery_all.sh            deletes /var/lib/bluetooth,
//                                       /var/lib/dbus, /usr/var and then does
//                                       `rm -rf /data/*`
//
// The disassembled original binary does nothing beyond:
//
//     system("echo recovery_all > /data/recovery_all");   // or recovery_data
//     system("reboot");
//
// It leaves a note and reboots. One reason it deletes nothing itself is the
// obvious one: /data is full of files the player has open at that moment,
// starting with its own configuration. The more important reason is that the
// microSD card is mounted under /data, at /data/mnt/sd_0. An `rm -rf /data/*`
// run while the card is mounted would descend into the mount point and erase
// the user's music. The script gets away with it because it runs at S39 while
// the card is mounted by the player at S92: when recovery_all.sh executes,
// /data/mnt/sd_0 is an empty directory.
//
// That is also why this function unmounts the card before rebooting. The
// reboot would unmount it anyway, but the "unmount, write the note, reboot"
// order leaves no window in which a recovery could find anything mounted
// underneath.
//
// The two note variants are `recovery_all` (erase everything) and
// `recovery_data` (keeps usrlocal_media.db, the original firmware's track
// database). This player's database has another name and would not be spared,
// and a factory reset is expected to leave nothing behind: write
// `recovery_all`.
//
// The boot logo is left alone. At this point the original also runs
// `flash_erase /dev/mtd5 0x20000 1`, which clears the theme marker in NAND.
// That is unnecessary here: theme.c rewrites that marker when the theme
// changes, and with the configuration deleted the theme returns to its default
// on first boot by itself. One less write to a flash with a finite erase-cycle
// budget.

#define RECOVERY_FLAG "/data/recovery_all"
#define RECOVERY_FLAG_ALT "/usr/data/recovery_all"
#define RECOVERY_SCRIPT "/usr/bin/recovery_all.sh"
#define RECOVERY_INIT "/etc/init.d/S39_recovery.recovery"

// This player's own configuration. Deleting it here rather than leaving it to
// the script is deliberate: on a rootfs missing the two pieces above, the reset
// still amounts to "settings are back to new", the part the user notices most.
#define CONFIG_FILE "/usr/data/device_config.ini"
// And the reader's, which is a second file for the reason config.h explains.
// Listed here because a reset that leaves every book open at the page it was on
// is not a reset -- and because a file nobody deletes is a file that outlives
// the thing that made it.
#define EBOOK_CONFIG_FILE "/usr/data/ebook_config.ini"

// The two functions below serve only the real procedure, which the host build
// does not have; without this guard the compiler reports them as defined but
// unused.
#ifndef HOST_BUILD

// Unmount with the same fallback ladder as usb.c: the system daemon when it is
// there, busybox when it is not. Failure is not an error, it only means
// something still holds a file open, and the reboot handles it anyway.
static void unmount_card(void) {
	const char *root = storage_sd_root();
	if (!root || !*root) {
		return;
	}

	if (sysserver_available() && sysserver_umount(root) == 0) {
		printf("factoryreset: card unmounted\n");
		return;
	}

	char cmd[512];
	snprintf(cmd, sizeof(cmd), "umount %s", root);
	int rc = system(cmd);
	printf("factoryreset: umount %s -> %d\n", root, rc);
}

// The note for the next boot. /data is a symlink to /usr/data on the device, so
// the two paths are the same file; the first is tried and the second is the
// fallback for the case where the link is missing.
static bool write_flag(void) {
	static const char *const PATHS[] = {RECOVERY_FLAG, RECOVERY_FLAG_ALT};

	for (size_t i = 0; i < sizeof(PATHS) / sizeof(PATHS[0]); i++) {
		FILE *f = fopen(PATHS[i], "w");
		if (!f) {
			continue;
		}
		// The script compares against the exact string `recovery_all`, read
		// with `cat`: the newline `echo` added is harmless (command
		// substitution strips it) but serves no purpose.
		bool ok = fputs("recovery_all", f) >= 0;
		if (fflush(f) != 0) {
			ok = false;
		}
		fclose(f);
		if (ok) {
			printf("factoryreset: %s written\n", PATHS[i]);
			return true;
		}
	}
	return false;
}

#endif /* !HOST_BUILD */

bool factoryreset_supported(void) {
	struct stat st;
	return stat(RECOVERY_SCRIPT, &st) == 0 && stat(RECOVERY_INIT, &st) == 0;
}

void factoryreset_run(void) {
#ifdef HOST_BUILD
	printf("factoryreset: (build host) would delete %s and %s, write the note and reboot\n", CONFIG_FILE,
		   EBOOK_CONFIG_FILE);
#else
	printf("factoryreset: starting the reset\n");

	if (!factoryreset_supported()) {
		// Not a reason to stop: the configuration is deleted anyway and the
		// device restarts with initial settings. Worth reporting, because it
		// explains why Bluetooth pairings and Wi-Fi networks survive the
		// reboot.
		printf("factoryreset: warning, %s or %s are missing: only the configuration will be restored\n",
			   RECOVERY_SCRIPT, RECOVERY_INIT);
	}

	// Everything holding a file open on the card goes down first: the same four
	// as usb.c, which has the same problem, since one open descriptor makes the
	// unmount fail.
	audio_stop();
	library_close();
	audiobookdb_close();
	logging_suspend_for_usb();

	// Streaming caches (tracks and covers) go before the card is unmounted:
	// after unmount_card() those files are unreachable, and a factory reset
	// must not leave streamed tracks behind.
	qobuzcache_clear_on_exit();
	tidalcache_clear_on_exit();
	podcastcache_clear_on_exit();
	dlna_clear_on_exit();

	// Push the time into the RTC as on every other exit path, otherwise the
	// reboot comes back with whatever the RTC remembered.
	clock_shutdown();

	// The card goes down before the note is written. See the long note at the
	// top of the file: this is where the procedure could do real damage, and
	// this line is what prevents it.
	unmount_card();

	// This player's configuration goes now, without waiting for the reboot.
	static const char *const OURS[] = {CONFIG_FILE, EBOOK_CONFIG_FILE};
	for (size_t i = 0; i < sizeof(OURS) / sizeof(OURS[0]); i++) {
		if (unlink(OURS[i]) == 0) {
			printf("factoryreset: %s deleted\n", OURS[i]);
		}
	}

	if (!write_flag()) {
		printf("factoryreset: cannot write the note for the reboot\n");
	}

	sync();
	sleep(1);

	int rc = system("reboot");
	(void)rc;

	// As in firmware_update_start(), there is no way back from here. The
	// configuration is gone and the note is written; returning to the settings
	// page would leave the device in a state that no longer matches anything it
	// displays.
	sleep(10);
	reboot(RB_AUTOBOOT);
	for (;;) {
		sleep(1);
	}
#endif
}

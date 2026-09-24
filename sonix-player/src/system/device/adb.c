#include "adb.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/device/system.h"
#include "src/system/device/usb.h"

// The two init scripts, and the file whose presence decides between them --
// exactly the test /etc/init.d/T90adb makes.
#define ADB_SCRIPT_ANDROID_USB "/etc/init.d/adb/S310adb"
#define ADB_SCRIPT_CONFIGFS "/etc/init.d/adb/S440adb"
#define ANDROID_USB_FUNCTIONS "/sys/class/android_usb/android0/functions"

static const char *adb_script(void) {
	return (access(ANDROID_USB_FUNCTIONS, F_OK) == 0) ? ADB_SCRIPT_ANDROID_USB : ADB_SCRIPT_CONFIGFS;
}

// True when a pid names a process that has already exited and is only waiting
// to be collected.
//
// A dead child keeps its entry in /proc, name and all, until somebody waits
// for it, so matching on the name alone answers "running" for a process that
// is gone. That is not a corner case here: adbd is started from this file and
// the switch is read back from adb_is_running(), so a leftover entry turns the
// switch back on the moment it is turned off, and leaves usb.c believing ADB
// owns the controller.
//
// The state is the field after the command in /proc/<pid>/stat, and the
// command is in parentheses and may contain one itself -- hence the last
// bracket rather than the first.
static bool process_is_zombie(const char *pid) {
	char path[320];
	snprintf(path, sizeof(path), "/proc/%.32s/stat", pid);

	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}

	char line[512];
	bool zombie = false;
	if (fgets(line, sizeof(line), f)) {
		const char *close = strrchr(line, ')');
		if (close && close[1] == ' ') {
			zombie = (close[2] == 'Z');
		}
	}
	fclose(f);
	return zombie;
}

// True if any process is running under this name. Reads /proc rather than
// shelling out to pidof, which would mean another fork.
static bool process_running(const char *name) {
	DIR *proc = opendir("/proc");
	if (!proc) {
		return false;
	}

	bool found = false;
	struct dirent *de;

	while (!found && (de = readdir(proc)) != NULL) {
		if (de->d_name[0] < '0' || de->d_name[0] > '9') {
			continue; // not a pid
		}

		char path[320];
		snprintf(path, sizeof(path), "/proc/%s/comm", de->d_name);

		FILE *f = fopen(path, "r");
		if (!f) {
			continue;
		}

		char comm[64] = "";
		if (fgets(comm, sizeof(comm), f)) {
			comm[strcspn(comm, "\r\n")] = '\0';
			found = (strcmp(comm, name) == 0);
		}
		fclose(f);

		// The second read costs one open, and only for the entry that matched.
		if (found && process_is_zombie(de->d_name)) {
			found = false;
		}
	}

	closedir(proc);
	return found;
}

bool adb_is_running(void) { return process_running("adbd"); }

// Runs "<script> start|stop" in a child. fork+exec rather than system(): it
// keeps the shell out of it and, more to the point, the child is reaped here
// instead of becoming a zombie for the rest of the session.
static bool run_script(const char *script, const char *action) {
	if (access(script, F_OK) != 0) {
		fprintf(stderr, "adb: %s is not on this firmware\n", script);
		return false;
	}

	pid_t pid = fork();
	if (pid < 0) {
		perror("adb: fork");
		return false;
	}

	if (pid == 0) {
		execl("/bin/sh", "sh", script, action, (char *)NULL);
		_exit(127); // exec failed; nothing here is worth reporting up
	}

	// The scripts background the daemon themselves, so this returns quickly.
	int status = 0;
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR) {
			break;
		}
	}

	printf("adb: %s %s -> exit %d\n", script, action, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	return true;
}

// ---------------------------------------------------------------------------
// Starting ADB without the firmware script
//
// /etc/init.d/adb/S440adb refuses outright when the USB configfs is already
// there:
//
//     if [ -d /sys/kernel/config/usb_gadget ]; then
//         echo "Usage: usb configfs already mounted"
//         exit 1
//     fi
//
// That guard assumes nothing else builds a gadget. This player builds one at
// startup for the card reader (usb.c, which mounts the configfs to do it), so
// the configfs is always "already mounted" from the script's point of view:
// the switch goes on and adbd never appears.
//
// The gadget is therefore built here instead, step for step as the script
// builds it, minus that guard. Every step is idempotent, so a second attempt
// after a half-finished one costs nothing.
// ---------------------------------------------------------------------------

#define ADB_GADGET "/sys/kernel/config/usb_gadget/adb_demo"
#define ADB_FFS_DIR "/dev/usb-ffs/adb"

// True once the player owns the USB configfs, which is as soon as the card
// reader's gadget exists, and is exactly when the firmware's script gives up.
static bool configfs_is_ours(void) { return access("/sys/kernel/config/usb_gadget", F_OK) == 0; }

static bool adb_gadget_start(void) {
	char cmd[2048];

	// The same descriptors the firmware's own script writes.
	snprintf(cmd, sizeof(cmd),
			 "[ -d /sys/kernel/config/usb_gadget ] || mount -t configfs none /sys/kernel/config; "
			 "if [ ! -d %s ]; then "
			 "mkdir %s && cd %s && "
			 "echo 0x18d1 > idVendor; "
			 "echo 0xd002 > idProduct; "
			 "echo 0x200 > bcdUSB; "
			 "echo 0x100 > bcdDevice; "
			 "mkdir strings/0x409; "
			 "echo ingenic > strings/0x409/manufacturer; "
			 "echo composite-adb > strings/0x409/product; "
			 "echo dev > strings/0x409/serialnumber; "
			 "mkdir configs/c.1; "
			 "echo 120 > configs/c.1/MaxPower; "
			 "mkdir configs/c.1/strings/0x409; "
			 "echo adb > configs/c.1/strings/0x409/configuration; "
			 "fi; "
			 "cd %s && "
			 "[ -d functions/ffs.adb ] || mkdir functions/ffs.adb; "
			 "[ -e configs/c.1/ffs.adb ] || ln -s functions/ffs.adb configs/c.1; "
			 "mkdir -p %s; "
			 "grep -q ' %s ' /proc/mounts || mount -t functionfs adb %s",
			 ADB_GADGET, ADB_GADGET, ADB_GADGET, ADB_GADGET, ADB_FFS_DIR, ADB_FFS_DIR, ADB_FFS_DIR);

	int rc = system(cmd);
	printf("adb: gadget setup -> %d\n", rc);

	// adbd writes the functionfs descriptors, and the gadget can only be bound to
	// the controller once they exist: start adbd, wait for it, then bind. The
	// firmware leaves that bind to adbserver.sh, so nothing does it when the
	// daemon is started directly.
	// Forked twice, and the middle one is waited for right here. adbd must not
	// be a child of the player: nothing here collects it, so the entry a killed
	// adbd leaves behind still carries the name process_running() reads. The
	// second fork hands adbd to init, which does collect it, and there is
	// nothing left to mistake for a running daemon.
	//
	// setsid() goes in the middle child, so adbd ends up in a session it does
	// not lead: it outlives the player and takes none of its signals, and
	// cannot pick up a controlling terminal either.
	pid_t pid = fork();
	if (pid < 0) {
		perror("adb: fork");
		return false;
	}
	if (pid == 0) {
		setsid();
		if (fork() == 0) {
			execl("/usr/bin/adbd", "adbd", (char *)NULL);
			execlp("adbd", "adbd", (char *)NULL);
			_exit(127);
		}
		_exit(0);
	}
	while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
	}

	for (int i = 0; i < 20 && !adb_is_running(); i++) {
		usleep(100 * 1000);
	}

	rc = system("grep -q '[a-zA-Z0-9]' " ADB_GADGET "/UDC 2>/dev/null || "
				"echo $(ls /sys/class/udc/ | head -n1) > " ADB_GADGET "/UDC");
	printf("adb: gadget bound -> %d, adbd %s\n", rc, adb_is_running() ? "running" : "not running");

	return adb_is_running();
}

// Waits for adbd to be gone. True when it is.
static bool wait_until_adbd_gone(int ms) {
	for (int waited = 0; waited < ms; waited += 100) {
		if (!adb_is_running()) {
			return true;
		}
		usleep(100 * 1000);
	}
	return !adb_is_running();
}

// Taking ADB down, in the order the pieces will actually let go.
//
// The order is the whole of this function, and getting it wrong leaves the
// device with neither ADB nor the card reader until it restarts. Unmounting the
// functionfs in the same breath as asking adbd to stop fails as busy -- adbd
// still holds the descriptors -- and leaves adbd wedged on a functionfs pulled
// apart underneath it: never dying, so the switch keeps reading "running", and
// never letting go of the USB controller, so the card reader has nothing to
// bind to.
//
// So: unbind the controller first, which stops the traffic; then ask adbd to
// go, and wait for it, and insist if it does not; and only once it is really
// gone take the descriptors apart.
static void adb_gadget_stop(void) {
	// 1. Off the controller. Nothing is being carried over USB after this.
	int rc = system("[ -d " ADB_GADGET " ] && echo '' > " ADB_GADGET "/UDC 2>/dev/null");
	(void)rc;

	// 2. Ask, then insist.
	rc = system("killall adbserver.sh 2>/dev/null; killall adbd 2>/dev/null");
	(void)rc;
	if (!wait_until_adbd_gone(2000)) {
		fprintf(stderr, "adb: adbd did not exit on its own, killing it\n");
		rc = system("killall -9 adbd 2>/dev/null");
		(void)rc;
		wait_until_adbd_gone(2000);
	}

	// 3. Now nobody holds the descriptors. The lazy unmount is the fallback
	// for a kernel thread that has not finished letting go: without it a
	// stuck mount point would keep the next start from ever working.
	rc = system("umount " ADB_FFS_DIR " 2>/dev/null || umount -l " ADB_FFS_DIR " 2>/dev/null; "
				"if [ -d " ADB_GADGET " ]; then "
				"rm -f " ADB_GADGET "/configs/c.1/ffs.adb; "
				"rmdir " ADB_GADGET "/functions/ffs.adb 2>/dev/null; fi");
	(void)rc;

	printf("adb: off, adbd %s\n", adb_is_running() ? "STILL ALIVE" : "gone");

	// The configfs itself stays mounted on purpose: the card reader's gadget
	// lives in it, and the firmware's stop script tearing the whole thing down
	// is why usb.c has to be able to rebuild from nothing in the first place.
}

// One request at a time. Each tap gets a thread of its own and a request takes
// a second or two, so without this two quick taps would have one thread
// building the gadget while the other tears it down, on a single USB
// controller. Serialised, the second tap lands after the first.
static pthread_mutex_t apply_lock = PTHREAD_MUTEX_INITIALIZER;

static bool adb_apply(bool enabled) {
	pthread_mutex_lock(&apply_lock);

	// One USB controller, one gadget at a time: hand it over before adbd
	// starts, take it back once adbd is gone (see usb.h).
	if (enabled) {
		usb_gadget_yield_to_adb();

		bool started = configfs_is_ours() ? adb_gadget_start()	  // the firmware's script would refuse; see above
										  : run_script(adb_script(), "start");
		pthread_mutex_unlock(&apply_lock);
		return started;
	}

	bool ok;
	if (access(ADB_GADGET, F_OK) == 0) {
		adb_gadget_stop();
		ok = !adb_is_running();
	} else {
		ok = run_script(adb_script(), "stop");
		// The firmware's own script returns before adbd is actually gone.
		wait_until_adbd_gone(2000);
	}
	usb_gadget_reclaim_from_adb();

	// Switching ADB off rearranges the USB gadget, and while a mass-storage
	// export was riding on ADB's gadget that can leave the card unmounted with
	// nobody left to put it back. Ask the storage layer to look.
	storage_recheck_card();

	pthread_mutex_unlock(&apply_lock);
	return ok;
}

// Off the interface thread: bringing ADB up waits for adbd to appear and then
// binds the controller, a second or two of forks and sysfs writes. Inline, that
// froze the interface for the duration, and on this device a stalled interface
// means a watchdog reboot.
static void *adb_apply_thread(void *arg) {
	adb_apply(arg != NULL);
	return NULL;
}

// The switch, as last set: read by usb.c's watcher thread, which must not go
// through the config (it is not made for two threads).
static volatile bool wanted;

bool adb_switched_on(void) { return wanted; }

bool adb_set_enabled(bool enabled) {
	wanted = enabled;
	config_set_bool("system", "adb", enabled);
	config_save();

	pthread_t thread;
	if (pthread_create(&thread, NULL, adb_apply_thread, enabled ? (void *)1 : NULL) != 0) {
		return adb_apply(enabled); // no thread to be had: do it here rather than not at all
	}
	pthread_detach(thread);
	return true;
}

void adb_apply_saved_state(void) {
	if (!config_get_bool("system", "adb", false)) {
		return;
	}
	wanted = true;

	if (adb_is_running()) {
		printf("adb: already running\n");
		return;
	}

	printf("adb: enabled in the settings, starting it\n");
	usb_gadget_yield_to_adb(); // no-op this early (the gadget comes up later)
	if (configfs_is_ours()) {
		adb_gadget_start();
	} else {
		run_script(adb_script(), "start");
	}
}

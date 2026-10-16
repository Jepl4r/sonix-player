#include "rsyncd.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/core/logging.h"

#define RSYNCD_SCRIPT "/etc/init.d/S51rsyncd"
#define ENABLE_FLAG "/usr/data/rsyncd.enabled"

// Copies of the helpers in adb.c: a dead child keeps its /proc entry until
// reaped, so the name alone can lie. rsync --daemon double-forks into init,
// so a zombie check is cheap insurance here too.
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

static bool process_running(const char *name) {
	DIR *proc = opendir("/proc");
	if (!proc) {
		return false;
	}

	bool found = false;
	struct dirent *de;

	while (!found && (de = readdir(proc)) != NULL) {
		if (de->d_name[0] < '0' || de->d_name[0] > '9') {
			continue;
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

		if (found && process_is_zombie(de->d_name)) {
			found = false;
		}
	}

	closedir(proc);
	return found;
}

bool rsyncd_is_running(void) { return process_running("rsync"); }

// Runs "<script> start|stop" in a child (fork+exec, never system()).
static bool run_script(const char *script, const char *action) {
	if (access(script, F_OK) != 0) {
		fprintf(stderr, "rsyncd: %s is not on this firmware\n", script);
		return false;
	}

	pid_t pid = fork();
	if (pid < 0) {
		perror("rsyncd: fork");
		return false;
	}

	if (pid == 0) {
		logging_child_stdio();
		execl("/bin/sh", "sh", script, action, (char *)NULL);
		_exit(127);
	}

	int status = 0;
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR) {
			break;
		}
	}

	printf("rsyncd: %s %s -> exit %d\n", script, action, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	return true;
}

static volatile bool wanted;

bool rsyncd_switched_on(void) { return wanted; }

bool rsyncd_set_enabled(bool enabled) {
	wanted = enabled;
	if (enabled) {
		FILE *f = fopen(ENABLE_FLAG, "w");
		if (f) {
			fclose(f);
		}
		config_set_bool("system", "rsyncd", true);
		config_save();
		return run_script(RSYNCD_SCRIPT, "start");
	}

	bool ok = run_script(RSYNCD_SCRIPT, "stop");
	unlink(ENABLE_FLAG);
	config_set_bool("system", "rsyncd", false);
	config_save();
	return ok;
}

void rsyncd_apply_saved_state(void) {
	if (!config_get_bool("system", "rsyncd", false)) {
		return;
	}
	wanted = true;

	// The flag is what S51rsyncd reads at boot; recreate it if userdata
	// lost it but the config still says on.
	FILE *f = fopen(ENABLE_FLAG, "r");
	if (f) {
		fclose(f);
	} else {
		f = fopen(ENABLE_FLAG, "w");
		if (f) {
			fclose(f);
		}
	}

	if (rsyncd_is_running()) {
		printf("rsyncd: already running\n");
		return;
	}

	printf("rsyncd: enabled in the settings, starting it\n");
	run_script(RSYNCD_SCRIPT, "start");
}

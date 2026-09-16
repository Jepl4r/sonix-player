#include "headset.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "src/system/core/config.h"

// The root the kernel module registers under. The attribute name is the one
// the original firmware binary looks for too.
#define EARPODS_ROOT "/sys/devices/platform/earpods_adc"
#define EARPODS_SWITCH "earpods_adc_sw"

// How deep to descend. On kernel 4.4 the file sits two or three levels below
// the root; the limit exists because sysfs is full of symlinks leading
// elsewhere, and an unbounded walk would follow them all.
#define EARPODS_MAX_DEPTH 4

static char switch_path[512];
static bool switch_searched;

// Searches once and keeps the path: the walk costs a handful of opendir calls,
// and the setting is only toggled by hand and at boot.
static bool find_switch(char *out, size_t out_size, const char *dir, int depth) {
	DIR *d = opendir(dir);
	if (!d) {
		return false;
	}

	bool found = false;
	struct dirent *e;
	while (!found && (e = readdir(d)) != NULL) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
			continue;
		}

		char path[512];
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= sizeof(path)) {
			continue; // longer than the buffer: not the file being looked for
		}

		if (strcmp(e->d_name, EARPODS_SWITCH) == 0) {
			snprintf(out, out_size, "%s", path);
			found = true;
			break;
		}

		if (depth >= EARPODS_MAX_DEPTH) {
			continue;
		}

		// lstat, not stat: sysfs symlinks must not be followed, or the walk
		// ends up in the rest of the device tree.
		struct stat st;
		if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
			found = find_switch(out, out_size, path, depth + 1);
		}
	}

	closedir(d);
	return found;
}

static const char *switch_file(void) {
	if (!switch_searched) {
		switch_searched = true;
		if (find_switch(switch_path, sizeof(switch_path), EARPODS_ROOT, 0)) {
			printf("headset: inline-remote switch at %s\n", switch_path);
		} else {
			switch_path[0] = '\0';
			printf("headset: no %s under %s, the inline buttons are not there\n", EARPODS_SWITCH, EARPODS_ROOT);
		}
	}
	return switch_path[0] ? switch_path : NULL;
}

bool headset_supported(void) { return switch_file() != NULL; }

// The module compares the written string against "on" and treats anything else
// as off; "off" is written for clarity, not because it is recognised.
static void apply(bool enabled) {
	const char *path = switch_file();
	if (!path) {
		return;
	}

	FILE *f = fopen(path, "w");
	if (!f) {
		fprintf(stderr, "headset: %s will not open for writing: %s\n", path, strerror(errno));
		return;
	}
	fputs(enabled ? "on" : "off", f);
	if (fclose(f) != 0) {
		fprintf(stderr, "headset: writing to %s failed: %s\n", path, strerror(errno));
		return;
	}
	printf("headset: inline controls %s\n", enabled ? "on" : "off");
}

bool headset_controls_enabled(void) { return config_get_bool("system", "headset_controls", false); }

void headset_set_controls_enabled(bool enabled) {
	config_set_bool("system", "headset_controls", enabled);
	config_save();
	apply(enabled);
}

void headset_init(void) {
	// Applied even when the option is off: the module keeps whatever was last
	// written to it until it is reloaded, so restarting only the binary would
	// leave the controls enabled from before.
	apply(headset_controls_enabled());
}

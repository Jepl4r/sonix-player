/*
 * Standalone directory-listing diagnostic for the target.
 *
 * The player showing one track out of a folder full of them can have several
 * causes; this reproduces just the directory walk, with no GUI, no LVGL and no
 * audio, and reports exactly what the C library hands back.
 *
 * Build it twice with the cross compiler -- once as the player used to be
 * built, once with large-file support -- and run both on the same folder:
 *
 *   CC=./rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-gcc
 *   $CC -O1 -o lsdiag32 tools/lsdiag.c
 *   $CC -O1 -D_FILE_OFFSET_BITS=64 -o lsdiag64 tools/lsdiag.c
 *   adb push lsdiag32 lsdiag64 /usr/data/mnt/sd_0/
 *   adb shell /usr/data/mnt/sd_0/lsdiag32 /usr/data/mnt/sd_0/Music
 *   adb shell /usr/data/mnt/sd_0/lsdiag64 /usr/data/mnt/sd_0/Music
 *
 * If the first stops early with "Value too large for defined data type" and
 * the second lists everything, the 32-bit readdir() is the culprit and the
 * -D_FILE_OFFSET_BITS=64 in the Makefile is the fix. If both list everything,
 * the directory walk is fine and the problem is elsewhere in the player.
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

static const char *type_name(unsigned char t) {
	switch (t) {
	case DT_REG:
		return "REG";
	case DT_DIR:
		return "DIR";
	case DT_LNK:
		return "LNK";
	case DT_UNKNOWN:
		return "UNKNOWN";
	default:
		return "other";
	}
}

int main(int argc, char **argv) {
	const char *path = (argc > 1) ? argv[1] : ".";

	printf("lsdiag: sizeof(off_t)=%u sizeof(ino_t)=%u\n", (unsigned)sizeof(off_t), (unsigned)sizeof(ino_t));
	printf("lsdiag: listing '%s'\n", path);

	DIR *dir = opendir(path);
	if (!dir) {
		printf("opendir failed: %s\n", strerror(errno));
		return 1;
	}

	unsigned long total = 0, playable = 0, stat_failures = 0;
	struct dirent *de;

	errno = 0;
	while ((de = readdir(dir)) != NULL) {
		total++;

		int show = (total <= 5); // full detail for the first few entries only

		char full[1024];
		snprintf(full, sizeof(full), "%s/%s", path, de->d_name);

		struct stat st;
		int stat_ok = (stat(full, &st) == 0);
		if (!stat_ok) {
			stat_failures++;
			printf("  [%lu] stat failed on '%s': %s\n", total, de->d_name, strerror(errno));
			errno = 0;
		}

		const char *dot = strrchr(de->d_name, '.');
		if (dot && (strcasecmp(dot, ".flac") == 0 || strcasecmp(dot, ".mp3") == 0 || strcasecmp(dot, ".ogg") == 0 ||
					strcasecmp(dot, ".wav") == 0)) {
			playable++;
		}

		if (show) {
			printf("  [%lu] name='%s' d_type=%s d_ino=%llu size=%lld\n", total, de->d_name, type_name(de->d_type),
				   (unsigned long long)de->d_ino, stat_ok ? (long long)st.st_size : -1);
		}
	}

	if (errno != 0) {
		printf("readdir STOPPED after %lu entries: %s (errno %d)\n", total, strerror(errno), errno);
	} else {
		printf("readdir reached the end of the directory cleanly\n");
	}

	closedir(dir);

	printf("total entries: %lu, playable by extension: %lu, stat failures: %lu\n", total, playable, stat_failures);
	return 0;
}

#include "subsonicaccount.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "subsonic.h"

// The account file lives beside the configuration, which is the writable
// partition and already the place where things that survive a reboot live. One
// file per configuration, so a test run with SONIX_CONFIG set never reads the
// device's real password, and the device never reads a test's.
//
// Its shape is the shortest that still says something to a human: a first line
// with the magic, the server and the user, and then the password. The server and
// the user are not secrets -- they are in the configuration, which stays the
// place the settings page writes them -- but carrying them here makes the file
// self-describing: it is obvious which account it belongs to, and a password
// left behind by a previous account cannot be mistaken for this one's.
//
// Nothing is encrypted. See the note in subsonicaccount.h: there is no key
// storage on the device and a compiled-in key is the same secret in a worse
// place. What the file does have is permissions, and a reader that believes
// only a file it owns and only its owner can write.

#define ACCOUNT_MAGIC "SONIXA1"
#define ACCOUNT_SUFFIX ".subsonic-account"

// The largest account file there can be: the magic line, a password of
// SUBSONIC_PASSWORD_MAX, and the line endings. Reading anything larger is
// reading something that is not this file.
#define ACCOUNT_MAX_BYTES 512

// The configuration's own path decides where the file goes. config_init() has
// run by the time this module is used on the device, but the environment is
// still checked first so a host build that has not initialised the store yet
// does not quietly write to /usr/data.
static void account_path(char *out, size_t size) {
	out[0] = '\0';
	const char *cfg = config_path();
	if (!cfg || !cfg[0]) {
		cfg = getenv("SONIX_CONFIG");
	}
	if (!cfg || !cfg[0]) {
		cfg = "/usr/data/device_config.ini";
	}
	int n = snprintf(out, size, "%s%s", cfg, ACCOUNT_SUFFIX);
	if (n <= 0 || (size_t)n >= size) {
		out[0] = '\0';
	}
}

// A single write() can stop short on a card under pressure; the rest of the
// buffer is not lost, it simply has not been written yet.
static bool write_all(int fd, const char *data, size_t len) {
	size_t done = 0;
	while (done < len) {
		ssize_t n = write(fd, data + done, len - done);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		if (n == 0) {
			return false;
		}
		done += (size_t)n;
	}
	return true;
}

bool subsonicaccount_save(const char *server, const char *user, const char *password) {
	if (!server || !user || !password || !server[0] || !user[0] || !password[0]) {
		return false;
	}

	// The password is handed back to subsonic.c in a buffer of this size; one
	// that does not fit there would be stored and then unreadable.
	if (strlen(password) >= SUBSONIC_PASSWORD_MAX) {
		return false;
	}

	// A tab or a newline would be read back as a field boundary or as the end
	// of the file, so the password would change on the next boot.
	if (strpbrk(password, "\t\r\n")) {
		return false;
	}
	if (strpbrk(server, "\t\r\n") || strpbrk(user, "\t\r\n")) {
		return false;
	}

	size_t need = strlen(ACCOUNT_MAGIC) + 1 + strlen(server) + 1 + strlen(user) + 1 + strlen(password) + 1;
	if (need > ACCOUNT_MAX_BYTES) {
		return false;
	}

	char path[600];
	account_path(path, sizeof(path));
	if (!path[0]) {
		return false;
	}

	// A temporary in the same directory, because rename() is only atomic
	// within one filesystem. A battery dying mid-write then leaves the old
	// password or the new one, never a truncated file that reads as no
	// account at all.
	const char *slash = strrchr(path, '/');
	const char *base = slash ? slash + 1 : path;
	char dir[600];
	if (slash) {
		int n = snprintf(dir, sizeof(dir), "%.*s", (int)(slash - path), path);
		if (n <= 0 || (size_t)n >= sizeof(dir)) {
			return false;
		}
	} else {
		snprintf(dir, sizeof(dir), ".");
	}

	char temp[640];
	// The length is checked rather than left to a precision: a truncated
	// template would place the temporary in a directory that is not the one
	// the rename lands in, and rename() would then cross filesystems and fail
	// after the password had been written.
	if (strlen(dir) + strlen(base) + 10 > sizeof(temp)) {
		return false;
	}
	int temp_len = snprintf(temp, sizeof(temp), "%s/.%s.XXXXXX", dir, base);
	if (temp_len <= 0 || (size_t)temp_len >= sizeof(temp)) {
		return false;
	}

	int fd = mkstemp(temp);
	if (fd < 0) {
		fprintf(stderr, "subsonicaccount: cannot create %s: %s\n", temp, strerror(errno));
		return false;
	}

	// mkstemp already creates 0600; the explicit call keeps the mode true even
	// if that promise ever changes.
	if (fchmod(fd, 0600) != 0) {
		close(fd);
		unlink(temp);
		return false;
	}

	char line[ACCOUNT_MAX_BYTES];
	int line_len = snprintf(line, sizeof(line), ACCOUNT_MAGIC "\t%s\t%s\n", server, user);

	bool ok = line_len > 0 && (size_t)line_len < sizeof(line);
	if (ok) {
		ok = write_all(fd, line, (size_t)line_len) && write_all(fd, password, strlen(password)) && write_all(fd, "\n", 1);
	}
	// fsync before the rename: the rename is ordered but the bytes are not, and
	// a file that exists with nothing in it is worse than no file.
	if (ok && fsync(fd) != 0) {
		ok = false;
	}
	if (close(fd) != 0) {
		ok = false;
	}
	if (ok && rename(temp, path) != 0) {
		ok = false;
	}

	if (!ok) {
		unlink(temp);
		fprintf(stderr, "subsonicaccount: the password was not saved to %s\n", path);
	}
	return ok;
}

// What a file has to look like before its contents are believed. A file
// somebody else can write is not evidence of anything, and a symlink would
// make the check pointless by pointing at a file that passes it.
static bool trustworthy(const struct stat *st) {
	if (!S_ISREG(st->st_mode)) {
		return false;
	}
	if (st->st_uid != getuid()) {
		return false;
	}
	if (st->st_mode & (S_IRWXG | S_IRWXO)) {
		return false;
	}
	return st->st_size > 0 && st->st_size <= ACCOUNT_MAX_BYTES;
}

bool subsonicaccount_load(char *out, size_t size) {
	if (!out || size == 0) {
		return false;
	}
	out[0] = '\0';

	char path[600];
	account_path(path, sizeof(path));
	if (!path[0]) {
		return false;
	}

	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) {
		return false; // not there yet: nobody has set an account
	}

	struct stat st;
	if (fstat(fd, &st) != 0 || !trustworthy(&st)) {
		close(fd);
		return false;
	}

	char buf[ACCOUNT_MAX_BYTES + 1];
	size_t got = 0;
	for (;;) {
		ssize_t n = read(fd, buf + got, sizeof(buf) - 1 - got);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			close(fd);
			return false;
		}
		if (n == 0) {
			break;
		}
		got += (size_t)n;
		if (got >= sizeof(buf) - 1) {
			break;
		}
	}
	close(fd);

	// The file is replaced by rename, so an open descriptor keeps the inode it
	// started with. A different size means something appended to it in place.
	if (got != (size_t)st.st_size) {
		return false;
	}
	buf[got] = '\0';

	char *nl = strchr(buf, '\n');
	if (!nl) {
		return false;
	}
	*nl = '\0';

	// The magic line: SONIXA1, a tab, the server, a tab, the user. Anything
	// else is another format and this module does not guess at it.
	const size_t magic_len = strlen(ACCOUNT_MAGIC);
	if (strncmp(buf, ACCOUNT_MAGIC, magic_len) != 0 || buf[magic_len] != '\t') {
		return false;
	}
	const char *server = buf + magic_len + 1;
	const char *tab = strchr(server, '\t');
	if (!tab || tab == server || !tab[1] || strchr(tab + 1, '\t')) {
		return false;
	}

	const char *password = nl + 1;
	size_t len = strlen(password);
	while (len > 0 && (password[len - 1] == '\n' || password[len - 1] == '\r')) {
		len--;
	}
	if (len == 0 || len >= size) {
		return false;
	}
	memcpy(out, password, len);
	out[len] = '\0';
	return true;
}

void subsonicaccount_clear(void) {
	char path[600];
	account_path(path, sizeof(path));
	if (!path[0]) {
		return;
	}

	// Nothing to remove is not an error: clear is called on every sign-out and
	// on the way to a factory reset, and most of those have no file to remove.
	if (unlink(path) != 0 && errno != ENOENT) {
		fprintf(stderr, "subsonicaccount: cannot remove %s: %s\n", path, strerror(errno));
	}
}

#include "subsoniccache.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <pthread.h>

#include "src/system/core/md5.h"
#include "src/system/streaming/subsonic.h"

// The layout is qobuzcache's and tidalcache's, and for the same reasons: the
// file name is a hash of the identity and not of the song, the sidecar says
// whose file it is, the marker says the body has not finished arriving, and the
// directory prefix answers "is this ours" without a table.
//
// The hash covers the server and the user as well as the song id. That is what
// lets two servers -- or two accounts on one server -- be used without either
// finding the other's file, which numbers mean nothing across them: song 42 on
// one server is a different song on the next.
#define CACHE_DIR ".local/subsonic-cache"

// The ceiling that ready() enforces. prune() takes a limit from its caller
// because a setting may be lower; this one is the point past which this module
// will not write whether asked to or not.
#define SUBSONICCACHE_MAX_BYTES (1024ULL * 1024ULL * 1024ULL)

#define IDENTITY_SUFFIX ".identity"
#define PARTIAL_SUFFIX ".incompleto"
#define TEMP_SUFFIX ".part"

#define SUFFIX_MAX 16
#define PRUNE_MAX_ENTRIES 512

static char cache_dir[512];

// How much the directory holds, kept between calls because ready() is asked
// from the GUI and a full scan per frame is not affordable. Invalidated by
// everything that changes the directory; ready() then measures once.
static unsigned long long used_bytes;
static bool used_valid;

// Written by the UI thread and read by the power loop, both through the lock:
// "is a download running" and "does the page want the network" are asked while
// the answer can be changing.
static pthread_mutex_t flag_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile bool fetching_now;
static volatile bool network_wanted;

void subsoniccache_set_root(const char *card_root) {
	if (!card_root || !*card_root) {
		// No card: every call that touches the disk reports "no card" instead
		// of writing somewhere else, and owns() answers no for everything.
		cache_dir[0] = '\0';
		used_valid = false;
		return;
	}

	int n = snprintf(cache_dir, sizeof(cache_dir), "%s/%s", card_root, CACHE_DIR);
	if (n <= 0 || (size_t)n >= sizeof(cache_dir)) {
		cache_dir[0] = '\0';
		used_valid = false;
		return;
	}
	used_valid = false;
}

// mkdir -p over the two levels needed (.local, then the cache directory). The
// card's own filesystem decides the mode: 0777 less the umask, the same as the
// other caches.
static bool ensure_dir(void) {
	if (!cache_dir[0]) {
		return false;
	}

	char path[512];
	snprintf(path, sizeof(path), "%s", cache_dir);
	for (char *p = path + 1; *p; p++) {
		if (*p != '/') {
			continue;
		}
		*p = '\0';
		mkdir(path, 0777);
		*p = '/';
	}
	if (mkdir(path, 0777) != 0 && errno != EEXIST) {
		fprintf(stderr, "subsoniccache: cannot create %s: %s\n", path, strerror(errno));
		return false;
	}
	return true;
}

static bool is_dot_entry(const char *name) { return name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')); }

static bool ends_with(const char *text, const char *tail) {
	size_t text_len = strlen(text);
	size_t tail_len = strlen(tail);
	return text_len >= tail_len && strcmp(text + text_len - tail_len, tail) == 0;
}

// True for a song body: thirty-two lower-case hex digits, a dot, and an
// extension that is not one of this module's own sidecars. Sidecars are never
// pruned or played; they only make sense next to the file they describe.
static bool is_song_name(const char *name) {
	for (int i = 0; i < MD5_HEX_LEN - 1; i++) {
		char c = name[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
			return false;
		}
	}
	if (name[MD5_HEX_LEN - 1] != '.' || !name[MD5_HEX_LEN]) {
		return false;
	}
	return !ends_with(name, IDENTITY_SUFFIX) && !ends_with(name, PARTIAL_SUFFIX) && !ends_with(name, TEMP_SUFFIX);
}

static unsigned long long measure(void) {
	DIR *d = opendir(cache_dir);
	if (!d) {
		return 0;
	}

	unsigned long long total = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (is_dot_entry(e->d_name)) {
			continue;
		}
		char path[640];
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", cache_dir, e->d_name) >= sizeof(path)) {
			continue;
		}
		struct stat st;
		if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
			total += (unsigned long long)st.st_size;
		}
	}
	closedir(d);
	return total;
}

bool subsoniccache_ready(void) {
	if (!cache_dir[0]) {
		return false;
	}
	if (!ensure_dir()) {
		return false;
	}
	if (!used_valid) {
		used_bytes = measure();
		used_valid = true;
	}
	return used_bytes < SUBSONICCACHE_MAX_BYTES;
}

// The hash that names the file: server, user and song id, separated so that
// concatenations cannot collide ("ab" + "c" is not "a" + "bc").
static bool song_hash(const char *song_id, char out[MD5_HEX_LEN]) {
	const char *server = subsonic_server();
	const char *user = subsonic_user();
	if (!server) {
		server = "";
	}
	if (!user) {
		user = "";
	}

	char key[SUBSONIC_URL_MAX + SUBSONIC_USER_MAX + SUBSONIC_ID_MAX + 3];
	int n = snprintf(key, sizeof(key), "%s\n%s\n%s", server, user, song_id);
	if (n <= 0 || (size_t)n >= sizeof(key)) {
		return false;
	}
	md5_hex(key, (size_t)n, out);
	return true;
}

// The extension with no dot, or NULL when the caller passed something that is
// not one. The suffix comes from the server's metadata, so it is checked rather
// than trusted: a slash would make the file land outside the cache directory.
static const char *extension_of(const char *suffix) {
	if (!suffix || !*suffix) {
		return "bin";
	}
	if (*suffix == '.') {
		suffix++;
	}
	if (!*suffix || strlen(suffix) > SUFFIX_MAX) {
		return NULL;
	}
	for (const char *p = suffix; *p; p++) {
		char c = *p;
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
			return NULL;
		}
	}
	return suffix;
}

bool subsoniccache_path(const char *song_id, const char *suffix, char *out, size_t size) {
	if (!out || size == 0) {
		return false;
	}
	out[0] = '\0';
	if (!cache_dir[0] || !song_id || !song_id[0]) {
		return false;
	}

	const char *ext = extension_of(suffix);
	if (!ext) {
		return false;
	}
	char hash[MD5_HEX_LEN];
	if (!song_hash(song_id, hash)) {
		return false;
	}

	int n = snprintf(out, size, "%s/%s.%s", cache_dir, hash, ext);
	if (n <= 0 || (size_t)n >= size) {
		out[0] = '\0';
		return false;
	}
	return true;
}

// True when the song is fully here: one of its bodies exists and carries no
// marker. The suffix is not known to the caller in every case, so the body is
// found by its hashed stem instead of being rebuilt.
bool subsoniccache_has(const char *song_id) {
	if (!cache_dir[0] || !song_id || !song_id[0]) {
		return false;
	}
	char hash[MD5_HEX_LEN];
	if (!song_hash(song_id, hash)) {
		return false;
	}

	DIR *d = opendir(cache_dir);
	if (!d) {
		return false;
	}

	bool found = false;
	struct dirent *e;
	while (!found && (e = readdir(d)) != NULL) {
		if (!is_song_name(e->d_name) || strncmp(e->d_name, hash, MD5_HEX_LEN - 1) != 0) {
			continue;
		}

		char path[640];
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", cache_dir, e->d_name) >= sizeof(path)) {
			continue;
		}
		char marker[672];
		snprintf(marker, sizeof(marker), "%s" PARTIAL_SUFFIX, path);

		struct stat st;
		if (stat(marker, &st) == 0) {
			continue; // interrupted: download it again
		}
		if (stat(path, &st) != 0 || st.st_size == 0) {
			continue;
		}
		found = true;
	}
	closedir(d);
	return found;
}

bool subsoniccache_owns(const char *path) {
	if (!path || !cache_dir[0]) {
		return false;
	}

	size_t len = strlen(cache_dir);
	if (strncmp(path, cache_dir, len) != 0) {
		return false;
	}
	// The boundary matters: ".../subsonic-cache2/x" starts with the directory
	// name without being inside it.
	return path[len] == '\0' || path[len] == '/';
}

static void set_flag(volatile bool *flag, bool on) {
	pthread_mutex_lock(&flag_lock);
	*flag = on;
	pthread_mutex_unlock(&flag_lock);
}

static bool get_flag(volatile bool *flag) {
	pthread_mutex_lock(&flag_lock);
	bool value = *flag;
	pthread_mutex_unlock(&flag_lock);
	return value;
}

bool subsoniccache_fetching(void) { return get_flag(&fetching_now); }

void subsoniccache_set_network_wanted(bool on) { set_flag(&network_wanted, on); }

bool subsoniccache_network_wanted(void) { return get_flag(&network_wanted); }

// The sidecar that says whose file this is. Written through a temporary and a
// rename: a reader that catches half of it would otherwise conclude the file
// belongs to nobody, or worse, to someone else.
static void write_identity(const char *path, const char *song_id) {
	char identity[672];
	snprintf(identity, sizeof(identity), "%s" IDENTITY_SUFFIX, path);

	char temp[688];
	snprintf(temp, sizeof(temp), "%s.tmp", identity);

	FILE *f = fopen(temp, "w");
	if (!f) {
		return;
	}
	const char *server = subsonic_server();
	const char *user = subsonic_user();
	fprintf(f, "server=%s\n", server ? server : "");
	fprintf(f, "user=%s\n", user ? user : "");
	fprintf(f, "song=%s\n", song_id);

	bool ok = fflush(f) == 0;
	if (fclose(f) != 0) {
		ok = false;
	}
	if (!ok || rename(temp, identity) != 0) {
		remove(temp);
	}
}

// The body arrives under a temporary name in the same directory and is
// published with rename(). The alternative -- writing straight to the final
// name and calling the file finished when the last byte lands -- was needed by
// the other two caches because their decoders read the file while it grows.
// Here nothing opens the file until fetch() returns, so the atomic swap is
// better: a reader either sees the whole song or no song.
static bool fetch_body(const char *song_id, const char *path) {
	char part[672];
	snprintf(part, sizeof(part), "%s" TEMP_SUFFIX, path);

	char marker[672];
	snprintf(marker, sizeof(marker), "%s" PARTIAL_SUFFIX, path);

	// The marker before the first byte: if power is lost between this and the
	// download, an orphan marker (which does nothing) beats a file that looks
	// complete and is not. It is removed only after the rename below.
	FILE *f = fopen(marker, "wb");
	if (f) {
		fclose(f);
	}

	// Whatever an earlier attempt left behind is not worth keeping.
	remove(part);

	unsigned long long bytes = 0;
	bool ok = subsonic_download(song_id, part, &bytes);
	if (ok && bytes == 0) {
		ok = false; // the server accepted and sent nothing: not a song
	}
	if (ok && rename(part, path) != 0) {
		ok = false;
	}

	if (!ok) {
		// The marker stays: has() says no and the song is fetched again
		// instead of playing half of it. subsonic_last_error() still holds
		// whatever subsonic_download() put there, including a full card.
		remove(part);
		fprintf(stderr, "subsoniccache: %s was not downloaded\n", song_id);
		return false;
	}

	remove(marker); // complete now
	write_identity(path, song_id);

	used_valid = false;
	printf("subsoniccache: %s complete (%llu KB)\n", song_id, bytes / 1024);
	return true;
}

bool subsoniccache_fetch(const char *song_id, const char *suffix) {
	if (!song_id || !song_id[0] || !cache_dir[0]) {
		return false;
	}
	if (!ensure_dir()) {
		return false;
	}

	// Already fully here: no network, no wait, and no reason to care which
	// extension the earlier fetch happened to use.
	if (subsoniccache_has(song_id)) {
		return true;
	}

	char path[512];
	if (!subsoniccache_path(song_id, suffix, path, sizeof(path))) {
		return false;
	}

	// Make room before opening the destination: with the ceiling reached the
	// download would otherwise stop at ENOSPC and leave a marker for nothing.
	subsoniccache_prune(SUBSONICCACHE_MAX_BYTES);
	if (!subsoniccache_ready()) {
		fprintf(stderr, "subsoniccache: no room for %s\n", song_id);
		return false;
	}

	set_flag(&fetching_now, true);
	bool ok = fetch_body(song_id, path);
	set_flag(&fetching_now, false);
	return ok;
}

typedef struct {
	char name[96];
	unsigned long long size;
	long atime; // last access: what tells which songs are actually listened to
} entry_t;

// Drops the bodies the card has not seen opened for the longest time until the
// directory fits in `limit_bytes`. Ordered by last access, not creation: an
// album downloaded a month ago but played yesterday should stay, one downloaded
// yesterday and never played can go.
//
// Sidecars are never candidates. Deleting an .identity would leave a file
// nobody could identify, and deleting an .incompleto would let a download in
// flight pass for a finished one. A body whose marker or .part exists is in
// flight and is skipped for the same reason.
void subsoniccache_prune(unsigned long long limit_bytes) {
	if (!cache_dir[0] || limit_bytes == 0) {
		return;
	}

	DIR *d = opendir(cache_dir);
	if (!d) {
		return;
	}

	entry_t *entries = malloc(sizeof(*entries) * PRUNE_MAX_ENTRIES);
	if (!entries) {
		closedir(d);
		return; // nothing is deleted; the ceiling is checked again next time
	}

	unsigned long long total = 0;
	int count = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (is_dot_entry(e->d_name)) {
			continue;
		}

		char path[640];
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", cache_dir, e->d_name) >= sizeof(path)) {
			continue;
		}
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
			continue;
		}
		total += (unsigned long long)st.st_size;

		if (!is_song_name(e->d_name) || count >= PRUNE_MAX_ENTRIES) {
			continue;
		}

		char sidecar[672];
		snprintf(sidecar, sizeof(sidecar), "%s" PARTIAL_SUFFIX, path);
		struct stat unused;
		if (stat(sidecar, &unused) == 0) {
			continue;
		}
		snprintf(sidecar, sizeof(sidecar), "%s" TEMP_SUFFIX, path);
		if (stat(sidecar, &unused) == 0) {
			continue;
		}

		// A name longer than the entry can carry is not one this module wrote
		// (the bodies are a hash and an extension), so it is left alone rather
		// than cut to a name that would then be deleted by mistake.
		size_t name_len = strlen(e->d_name);
		if (name_len >= sizeof(entries[count].name)) {
			continue;
		}
		memcpy(entries[count].name, e->d_name, name_len + 1);
		entries[count].size = (unsigned long long)st.st_size;
		entries[count].atime = (long)st.st_atime;
		count++;
	}
	closedir(d);

	if (total <= limit_bytes) {
		free(entries);
		return;
	}

	// Selection sort: at most 512 entries, and only occasionally.
	for (int i = 0; i < count; i++) {
		int oldest = i;
		for (int k = i + 1; k < count; k++) {
			if (entries[k].atime < entries[oldest].atime) {
				oldest = k;
			}
		}
		entry_t tmp = entries[i];
		entries[i] = entries[oldest];
		entries[oldest] = tmp;
	}

	for (int i = 0; i < count && total > limit_bytes; i++) {
		char path[640];
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", cache_dir, entries[i].name) >= sizeof(path)) {
			continue;
		}

		// The sidecar goes with its body, or it would outlive the reason it
		// exists. Its size is counted in `total` too, so it is measured before
		// the deletion and not after it.
		char identity[672];
		snprintf(identity, sizeof(identity), "%s" IDENTITY_SUFFIX, path);
		struct stat st;
		unsigned long long freed = entries[i].size;
		if (stat(identity, &st) == 0 && S_ISREG(st.st_mode)) {
			freed += (unsigned long long)st.st_size;
		}

		if (remove(path) != 0) {
			continue;
		}
		remove(identity);

		total = total > freed ? total - freed : 0;
		printf("subsoniccache: removed %s (%llu KB)\n", entries[i].name, entries[i].size / 1024);
	}

	free(entries);
	used_valid = false;
}

void subsoniccache_clear_on_exit(void) {
	if (!cache_dir[0]) {
		return;
	}

	DIR *d = opendir(cache_dir);
	if (!d) {
		return;
	}

	int removed = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (is_dot_entry(e->d_name)) {
			continue;
		}
		char path[640];
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", cache_dir, e->d_name) >= sizeof(path)) {
			continue;
		}
		if (remove(path) == 0) {
			removed++;
		}
	}
	closedir(d);

	used_bytes = 0;
	used_valid = true;
	printf("subsoniccache: cleared (%d files)\n", removed);
}

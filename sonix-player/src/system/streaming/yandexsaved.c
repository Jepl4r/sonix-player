#include "yandexsaved.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

// See yandexsaved.h.

// One line of the index, split in place: the strings point into `line`.
typedef struct {
	char *line;
	const char *id, *album_id, *title, *artist, *album, *album_artist, *rel;
	int disc, track, duration, year, album_tracks;
} entry_t;

enum { F_ID, F_ALBUM_ID, F_DISC, F_TRACK, F_DURATION, F_YEAR, F_ALBUM_TRACKS, F_TITLE, F_ARTIST, F_ALBUM, F_ALBUM_ARTIST, F_REL, F_COUNT };

#define HEADER "# sonix yandex-saved 1\n"
// The queue's claimed files: the page's list (150) and My Wave's additions.
#define CLAIM_MAX 320

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char root[400];
static bool loaded;
static entry_t *entries;
static int entry_count, entry_cap;

static uint32_t claims[CLAIM_MAX];
static int claim_count;

// ---------------------------------------------------------------------------
// the file
// ---------------------------------------------------------------------------

static void index_path(char *out, size_t size) { snprintf(out, size, "%s/%s", root, YANDEXSAVED_FILE); }

static void absolute(const entry_t *e, char *out, size_t size) { snprintf(out, size, "%s/%s", root, e->rel); }

static void forget_all(void) {
	for (int i = 0; i < entry_count; i++) {
		free(entries[i].line);
	}
	free(entries);
	entries = NULL;
	entry_count = entry_cap = 0;
	loaded = false;
}

// Splits a line (its own copy, taken over) into an entry. False for one that
// is not whole -- a write cut short -- or not ours.
static bool parse(char *line, entry_t *e) {
	line[strcspn(line, "\r\n")] = '\0';
	if (line[0] == '#' || !line[0]) {
		return false;
	}
	const char *f[F_COUNT];
	char *p = line;
	int n = 0;
	while (n < F_COUNT) {
		f[n++] = p;
		char *tab = strchr(p, '\t');
		if (!tab) {
			break;
		}
		*tab = '\0';
		p = tab + 1;
	}
	if (n != F_COUNT || !f[F_ID][0] || !f[F_REL][0]) {
		return false;
	}
	e->line = line;
	e->id = f[F_ID];
	e->album_id = f[F_ALBUM_ID];
	e->disc = atoi(f[F_DISC]);
	e->track = atoi(f[F_TRACK]);
	e->duration = atoi(f[F_DURATION]);
	e->year = atoi(f[F_YEAR]);
	e->album_tracks = atoi(f[F_ALBUM_TRACKS]);
	e->title = f[F_TITLE];
	e->artist = f[F_ARTIST];
	e->album = f[F_ALBUM];
	e->album_artist = f[F_ALBUM_ARTIST];
	e->rel = f[F_REL];
	return true;
}

static bool grow(void) {
	if (entry_count < entry_cap) {
		return true;
	}
	int cap = entry_cap ? entry_cap * 2 : 32;
	entry_t *bigger = realloc(entries, (size_t)cap * sizeof(*entries));
	if (!bigger) {
		return false;
	}
	entries = bigger;
	entry_cap = cap;
	return true;
}

static int find(const char *id) {
	for (int i = 0; i < entry_count; i++) {
		if (strcmp(entries[i].id, id) == 0) {
			return i;
		}
	}
	return -1;
}

// Read on first use after start-up or a card change. Under the lock.
static void ensure_loaded(void) {
	if (loaded || !root[0]) {
		return;
	}
	loaded = true;
	char path[512];
	index_path(path, sizeof(path));
	FILE *f = fopen(path, "r");
	if (!f) {
		return; // nothing saved yet
	}
	char buf[2048];
	while (fgets(buf, sizeof(buf), f)) {
		char *line = strdup(buf);
		entry_t e;
		if (!line || !grow() || !parse(line, &e)) {
			free(line);
			continue;
		}
		// A later line of the same id is a newer save of it.
		int at = find(e.id);
		if (at >= 0) {
			free(entries[at].line);
			entries[at] = e;
		} else {
			entries[entry_count++] = e;
		}
	}
	fclose(f);
	printf("yandexsaved: %d saved tracks in the index\n", entry_count);
}

// The whole index again, written aside and renamed over: after a removal.
static void rewrite(void) {
	char path[512], tmp[530];
	index_path(path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		return;
	}
	bool ok = fputs(HEADER, f) >= 0;
	for (int i = 0; ok && i < entry_count; i++) {
		const entry_t *e = &entries[i];
		ok = fprintf(f, "%s\t%s\t%d\t%d\t%d\t%d\t%d\t%s\t%s\t%s\t%s\t%s\n", e->id, e->album_id, e->disc, e->track, e->duration, e->year, e->album_tracks, e->title, e->artist, e->album, e->album_artist, e->rel) > 0;
	}
	if (fclose(f) != 0 || !ok || rename(tmp, path) != 0) {
		remove(tmp);
	}
}

static void drop(int at) {
	printf("yandexsaved: %s is no longer saved (%s)\n", entries[at].id, entries[at].rel);
	free(entries[at].line);
	memmove(entries + at, entries + at + 1, (size_t)(entry_count - at - 1) * sizeof(*entries));
	entry_count--;
	rewrite();
}

// A field as one line of the file holds it: no tabs, no line breaks.
static void field(char *out, size_t size, const char *in) {
	snprintf(out, size, "%s", in ? in : "");
	for (char *p = out; *p; p++) {
		if (*p == '\t' || *p == '\n' || *p == '\r') {
			*p = ' ';
		}
	}
}

void yandexsaved_set_root(const char *sd_root) {
	pthread_mutex_lock(&lock);
	const char *next = sd_root ? sd_root : "";
	if (strcmp(root, next) != 0) {
		forget_all();
		snprintf(root, sizeof(root), "%s", next);
		claim_count = 0;
	}
	pthread_mutex_unlock(&lock);
}

void yandexsaved_add(const yandex_track_t *t, const char *album_artist, int year, int album_tracks, const char *path) {
	if (!t || !t->id[0] || !path) {
		return;
	}
	pthread_mutex_lock(&lock);
	size_t n = strlen(root);
	if (!n || strncmp(path, root, n) != 0 || path[n] != '/') {
		pthread_mutex_unlock(&lock);
		return;
	}
	ensure_loaded();
	char title[YANDEX_TITLE_MAX], artist[YANDEX_NAME_MAX], album[YANDEX_TITLE_MAX], aa[YANDEX_NAME_MAX], rel[600];
	field(title, sizeof(title), t->title);
	field(artist, sizeof(artist), t->artist);
	field(album, sizeof(album), t->album);
	field(aa, sizeof(aa), album_artist);
	field(rel, sizeof(rel), path + n + 1);
	char buf[2048];
	snprintf(buf, sizeof(buf), "%s\t%s\t%d\t%d\t%d\t%d\t%d\t%s\t%s\t%s\t%s\t%s\n", t->id, t->album_id, t->disc_number, t->track_number, t->duration, year, album_tracks, title, artist, album, aa, rel);

	char *line = strdup(buf);
	entry_t e;
	if (!line || !grow() || !parse(line, &e)) {
		free(line);
		pthread_mutex_unlock(&lock);
		return;
	}
	int at = find(e.id);
	if (at >= 0) {
		free(entries[at].line);
		entries[at] = e;
		rewrite();
	} else {
		entries[entry_count++] = e;
		// Appended: one short write, and the file never half-replaced.
		char file[512];
		index_path(file, sizeof(file));
		struct stat st;
		bool fresh = stat(file, &st) != 0 || st.st_size == 0;
		FILE *f = fopen(file, "a");
		if (f) {
			if (fresh) {
				fputs(HEADER, f);
			}
			fputs(buf, f);
			fclose(f);
		}
	}
	pthread_mutex_unlock(&lock);
}

int yandexsaved_count(void) {
	pthread_mutex_lock(&lock);
	ensure_loaded();
	int count = entry_count;
	pthread_mutex_unlock(&lock);
	return count;
}

bool yandexsaved_has(const char *track_id) {
	if (!track_id || !track_id[0]) {
		return false;
	}
	pthread_mutex_lock(&lock);
	ensure_loaded();
	bool has = find(track_id) >= 0;
	pthread_mutex_unlock(&lock);
	return has;
}

int yandexsaved_album_count(const char *album_id) {
	if (!album_id || !album_id[0]) {
		return 0;
	}
	pthread_mutex_lock(&lock);
	ensure_loaded();
	int count = 0;
	for (int i = 0; i < entry_count; i++) {
		count += strcmp(entries[i].album_id, album_id) == 0;
	}
	pthread_mutex_unlock(&lock);
	return count;
}

// Whether the entry's file is there to be read; drops it when not. Under the
// lock; `at` is gone afterwards when it returns false.
static bool check(int at, char *out, size_t size) {
	char path[1024];
	absolute(&entries[at], path, sizeof(path));
	if (access(path, R_OK) != 0) {
		drop(at);
		return false;
	}
	if (out) {
		snprintf(out, size, "%s", path);
	}
	return true;
}

bool yandexsaved_local(const char *track_id, char *out, size_t size) {
	if (!track_id || !track_id[0]) {
		return false;
	}
	pthread_mutex_lock(&lock);
	ensure_loaded();
	int at = find(track_id);
	bool ok = at >= 0 && check(at, out, size);
	pthread_mutex_unlock(&lock);
	return ok;
}

bool yandexsaved_track_for_path(const char *path, char *id, size_t id_size, char *album_id, size_t album_size) {
	if (id && id_size) {
		id[0] = '\0';
	}
	if (album_id && album_size) {
		album_id[0] = '\0';
	}
	// Every saved file is under ".../Yandex Music/": anything else is not
	// worth the lock.
	if (!path || !strstr(path, "/" YANDEXSAVED_FOLDER "/")) {
		return false;
	}
	pthread_mutex_lock(&lock);
	size_t n = strlen(root);
	bool found = false;
	if (n && strncmp(path, root, n) == 0 && path[n] == '/') {
		ensure_loaded();
		for (int i = 0; i < entry_count; i++) {
			if (strcmp(entries[i].rel, path + n + 1) == 0) {
				if (id && id_size) {
					snprintf(id, id_size, "%s", entries[i].id);
				}
				if (album_id && album_size) {
					snprintf(album_id, album_size, "%s", entries[i].album_id);
				}
				found = true;
				break;
			}
		}
	}
	pthread_mutex_unlock(&lock);
	return found;
}

// ---------------------------------------------------------------------------
// lists for the "Saved on the card" page
// ---------------------------------------------------------------------------

static int album_cmp(const void *a, const void *b) {
	const yandex_album_t *x = a, *y = b;
	int c = strcasecmp(x->artist, y->artist);
	return c ? c : strcasecmp(x->title, y->title);
}

int yandexsaved_albums(yandex_album_t *out, int max) {
	pthread_mutex_lock(&lock);
	ensure_loaded();
	int count = 0;
	for (int i = 0; i < entry_count; i++) {
		const entry_t *e = &entries[i];
		if (!e->album_id[0]) {
			continue;
		}
		int at = -1;
		for (int k = 0; k < count; k++) {
			if (strcmp(out[k].id, e->album_id) == 0) {
				at = k;
				break;
			}
		}
		if (at < 0) {
			if (count >= max) {
				continue;
			}
			at = count++;
			memset(&out[at], 0, sizeof(out[at]));
			snprintf(out[at].id, sizeof(out[at].id), "%s", e->album_id);
			snprintf(out[at].title, sizeof(out[at].title), "%s", e->album);
			snprintf(out[at].artist, sizeof(out[at].artist), "%s", e->album_artist[0] ? e->album_artist : e->artist);
			out[at].year = e->year;
		}
		out[at].track_count++;
	}
	pthread_mutex_unlock(&lock);
	qsort(out, (size_t)count, sizeof(*out), album_cmp);
	return count;
}

static int track_cmp(const void *a, const void *b) {
	const yandex_track_t *x = a, *y = b;
	int dx = x->disc_number ? x->disc_number : 1, dy = y->disc_number ? y->disc_number : 1;
	if (dx != dy) {
		return dx - dy;
	}
	if (x->track_number != y->track_number) {
		return x->track_number - y->track_number;
	}
	return strcasecmp(x->title, y->title);
}

int yandexsaved_tracks(const char *album_id, yandex_track_t *out, int max) {
	const char *want = album_id ? album_id : "";
	pthread_mutex_lock(&lock);
	ensure_loaded();
	int count = 0;
	for (int i = 0; i < entry_count && count < max;) {
		const entry_t *e = &entries[i];
		if (strcmp(e->album_id, want) != 0) {
			i++;
			continue;
		}
		if (!check(i, NULL, 0)) {
			continue; // dropped: the next entry has moved into `i`
		}
		yandex_track_t *t = &out[count++];
		memset(t, 0, sizeof(*t));
		snprintf(t->id, sizeof(t->id), "%s", e->id);
		snprintf(t->album_id, sizeof(t->album_id), "%s", e->album_id);
		snprintf(t->title, sizeof(t->title), "%s", e->title);
		snprintf(t->artist, sizeof(t->artist), "%s", e->artist);
		snprintf(t->album, sizeof(t->album), "%s", e->album);
		t->duration = e->duration;
		t->track_number = e->track;
		t->disc_number = e->disc;
		t->available = true;
		i++;
	}
	pthread_mutex_unlock(&lock);
	qsort(out, (size_t)count, sizeof(*out), track_cmp);
	return count;
}

// ---------------------------------------------------------------------------
// deleting
// ---------------------------------------------------------------------------

// Whether the folder holds nothing but (at most) a cover.jpg.
static bool only_cover_left(const char *folder) {
	DIR *d = opendir(folder);
	if (!d) {
		return false;
	}
	bool only = true;
	struct dirent *de;
	while (only && (de = readdir(d)) != NULL) {
		const char *name = de->d_name;
		if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || strcasecmp(name, "cover.jpg") == 0) {
			continue;
		}
		only = false;
	}
	closedir(d);
	return only;
}

// The album folder, then the artist's, while they are empty -- never above
// "Yandex Music", which is left even when empty (it is where saves go).
static void prune_folders(const char *file) {
	char folder[1024];
	snprintf(folder, sizeof(folder), "%s", file);
	for (int up = 0; up < 2; up++) {
		char *slash = strrchr(folder, '/');
		if (!slash) {
			return;
		}
		*slash = '\0';
		const char *name = strrchr(folder, '/');
		if (!name || strcmp(name + 1, YANDEXSAVED_FOLDER) == 0) {
			return;
		}
		if (up == 0 && only_cover_left(folder)) {
			char cover[1100];
			snprintf(cover, sizeof(cover), "%s/cover.jpg", folder);
			remove(cover);
		}
		if (rmdir(folder) != 0) {
			return;
		}
	}
}

// Under the lock. The entry goes whatever happens to the file: a file that
// cannot be removed (read-only card) is reported, and the next play finds it
// again only if it is still there.
static bool delete_at(int at) {
	char path[1024], tags[1100];
	absolute(&entries[at], path, sizeof(path));
	bool ok = remove(path) == 0 || errno == ENOENT;
	if (!ok) {
		fprintf(stderr, "yandexsaved: cannot remove %s: %s\n", path, strerror(errno));
		return false;
	}
	snprintf(tags, sizeof(tags), "%s.tags", path);
	remove(tags);
	prune_folders(path);
	drop(at);
	return true;
}

bool yandexsaved_delete(const char *track_id) {
	if (!track_id || !track_id[0]) {
		return false;
	}
	pthread_mutex_lock(&lock);
	ensure_loaded();
	int at = find(track_id);
	bool ok = at < 0 || delete_at(at);
	pthread_mutex_unlock(&lock);
	return ok;
}

int yandexsaved_delete_album(const char *album_id) {
	if (!album_id || !album_id[0]) {
		return 0;
	}
	pthread_mutex_lock(&lock);
	ensure_loaded();
	int gone = 0;
	for (int i = 0; i < entry_count;) {
		if (strcmp(entries[i].album_id, album_id) == 0 && delete_at(i)) {
			gone++;
			continue; // the next entry has moved into `i`
		}
		i++;
	}
	pthread_mutex_unlock(&lock);
	return gone;
}

// ---------------------------------------------------------------------------
// the queue's files
//
// Kept as hashes of the paths, not the paths: up to three hundred of them are
// asked about on every redraw of the star and every power check, and four
// bytes each is all a yes/no needs. A clash would make one other file under
// "Yandex Music" look like a Yandex track for as long as the queue lasts.
// ---------------------------------------------------------------------------

static uint32_t path_hash(const char *s) {
	uint32_t h = 2166136261u;
	for (; *s; s++) {
		h = (h ^ (uint8_t)*s) * 16777619u;
	}
	return h;
}

void yandexsaved_claim(const char *const *paths, int count) {
	pthread_mutex_lock(&lock);
	claim_count = 0;
	for (int i = 0; i < count && claim_count < CLAIM_MAX; i++) {
		if (paths[i] && paths[i][0]) {
			claims[claim_count++] = path_hash(paths[i]);
		}
	}
	pthread_mutex_unlock(&lock);
}

bool yandexsaved_claimed(const char *path) {
	if (!path || !strstr(path, "/" YANDEXSAVED_FOLDER "/")) {
		return false;
	}
	uint32_t h = path_hash(path);
	pthread_mutex_lock(&lock);
	bool found = false;
	for (int i = 0; i < claim_count && !found; i++) {
		found = claims[i] == h;
	}
	pthread_mutex_unlock(&lock);
	return found;
}

#ifdef SONIX_ZVUK

// fopencookie
#define _GNU_SOURCE

#include "zvukdl.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/device/system.h"
#include "src/system/device/usb.h"
#include "src/system/library/library.h"
#include "src/system/library/mp4flac.h"
#include "src/system/net/http.h"
#include "src/system/streaming/podcastdl.h"
#include "src/system/streaming/zvukcache.h"

// See zvukdl.h.

#define QUEUE_MAX 300
#define KEEP_BLOCK_MAX (1024 * 1024)

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char root[400];

// ===========================================================================
// the index
// ===========================================================================

// An entry is its line, as in the file, and the two ids as numbers for the
// lookups the rows do on every redraw.
typedef struct {
	uint64_t id, album;
	char *line; // id \t album \t number \t year \t duration \t title \t artist \t album \t album artist \t path
} saved_t;

static saved_t *saved;
static int saved_count, saved_room;
static bool saved_loaded;

enum { F_ID, F_ALBUM, F_NUMBER, F_YEAR, F_DURATION, F_TITLE, F_ARTIST, F_ALBUM_TITLE, F_ALBUM_ARTIST, F_PATH, F_COUNT };

// Field `f` of a line into `out`.
static void field(const char *line, int f, char *out, size_t size) {
	const char *p = line;
	for (int i = 0; i < f && p; i++) {
		p = strchr(p, '\t');
		p = p ? p + 1 : NULL;
	}
	if (!p) {
		out[0] = '\0';
		return;
	}
	snprintf(out, size, "%.*s", (int)strcspn(p, "\t\n"), p);
}

static void index_path(char *out, size_t size) { snprintf(out, size, "%s/" ZVUKDL_INDEX, root); }

// Call with the lock held.
static void index_add_line(char *line) {
	char id[ZVUK_ID_MAX], album[ZVUK_ID_MAX];
	field(line, F_ID, id, sizeof(id));
	field(line, F_ALBUM, album, sizeof(album));
	uint64_t v = strtoull(id, NULL, 10);
	if (!v) {
		free(line);
		return;
	}
	for (int i = 0; i < saved_count; i++) {
		if (saved[i].id == v) { // a newer line replaces an older one
			free(saved[i].line);
			saved[i].line = line;
			saved[i].album = strtoull(album, NULL, 10);
			return;
		}
	}
	if (saved_count == saved_room) {
		int room = saved_room ? saved_room * 2 : 32;
		saved_t *grown = realloc(saved, (size_t)room * sizeof(*grown));
		if (!grown) {
			free(line);
			return;
		}
		saved = grown;
		saved_room = room;
	}
	saved[saved_count++] = (saved_t){v, strtoull(album, NULL, 10), line};
}

static void index_prune(void);

static void index_load(void) {
	if (saved_loaded || !root[0]) {
		return;
	}
	saved_loaded = true;
	char path[500];
	index_path(path, sizeof(path));
	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}
	char buf[2048];
	while (fgets(buf, sizeof(buf), f)) {
		size_t len = strlen(buf);
		if (len == 0 || buf[len - 1] != '\n') {
			continue; // a torn last line
		}
		buf[len - 1] = '\0';
		char *line = strdup(buf);
		if (line) {
			index_add_line(line);
		}
	}
	fclose(f);
	index_prune();
}

// Rewrites the whole file (after a delete). Lock held.
static void index_save(void) {
	char path[500], tmp[520];
	index_path(path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		return;
	}
	for (int i = 0; i < saved_count; i++) {
		fprintf(f, "%s\n", saved[i].line);
	}
	if (fclose(f) != 0 || rename(tmp, path) != 0) {
		remove(tmp);
	}
}

static void index_forget(void) {
	for (int i = 0; i < saved_count; i++) {
		free(saved[i].line);
	}
	free(saved);
	saved = NULL;
	saved_count = saved_room = 0;
	saved_loaded = false;
}

static void clean(const char *in, char *out, size_t size) {
	size_t n = 0;
	for (const char *p = in ? in : ""; *p && n + 1 < size; p++) {
		out[n++] = (*p == '\t' || *p == '\n' || *p == '\r') ? ' ' : *p;
	}
	out[n] = '\0';
}

static void index_add(const zvuk_track_t *t, const char *album_title, const char *album_artist, int year,
					  const char *abs_path) {
	char title[ZVUK_TITLE_MAX], artist[ZVUK_NAME_MAX], album[ZVUK_TITLE_MAX], aa[ZVUK_NAME_MAX];
	clean(t->title, title, sizeof(title));
	clean(t->artist, artist, sizeof(artist));
	clean(album_title, album, sizeof(album));
	clean(album_artist, aa, sizeof(aa));
	size_t rl = strlen(root);
	const char *rel = strncmp(abs_path, root, rl) == 0 && abs_path[rl] == '/' ? abs_path + rl + 1 : abs_path;
	char line[2000];
	snprintf(line, sizeof(line), "%s\t%s\t%d\t%d\t%d\t%s\t%s\t%s\t%s\t%s", t->id, t->album_id, t->track_number, year,
			 t->duration, title, artist, album, aa, rel);
	pthread_mutex_lock(&lock);
	index_load();
	char path[500];
	index_path(path, sizeof(path));
	FILE *f = fopen(path, "a");
	if (f) {
		fprintf(f, "%s\n", line);
		fclose(f);
	}
	char *copy = strdup(line);
	if (copy) {
		index_add_line(copy);
	}
	pthread_mutex_unlock(&lock);
}

static int find(uint64_t id) {
	for (int i = 0; i < saved_count; i++) {
		if (saved[i].id == id) {
			return i;
		}
	}
	return -1;
}

static void abs_of(const saved_t *s, char *out, size_t size) {
	char rel[600];
	field(s->line, F_PATH, rel, sizeof(rel));
	snprintf(out, size, "%s/%s", root, rel);
}

int zvukdl_saved_count(void) {
	pthread_mutex_lock(&lock);
	index_load();
	int n = saved_count;
	pthread_mutex_unlock(&lock);
	return n;
}

bool zvukdl_saved(const char *track_id) {
	uint64_t v = track_id ? strtoull(track_id, NULL, 10) : 0;
	pthread_mutex_lock(&lock);
	index_load();
	bool yes = v && find(v) >= 0;
	pthread_mutex_unlock(&lock);
	return yes;
}

int zvukdl_album_saved_count(const char *album_id) {
	uint64_t v = album_id ? strtoull(album_id, NULL, 10) : 0;
	int n = 0;
	pthread_mutex_lock(&lock);
	index_load();
	for (int i = 0; v && i < saved_count; i++) {
		n += saved[i].album == v;
	}
	pthread_mutex_unlock(&lock);
	return n;
}

static void drop(int i) {
	free(saved[i].line);
	memmove(saved + i, saved + i + 1, (size_t)(saved_count - i - 1) * sizeof(*saved));
	saved_count--;
}

// Drops the entries whose file is gone -- deleted on a computer, or the
// folder moved -- once, as the index is read: the rows' "on the card" and the
// album's "Saved on the card" would claim them until each was played. One
// access() per saved track, once per card. Lock held.
static void index_prune(void) {
	int before = saved_count;
	for (int i = saved_count - 1; i >= 0; i--) {
		char path[1000];
		abs_of(&saved[i], path, sizeof(path));
		if (access(path, R_OK) != 0) {
			drop(i);
		}
	}
	if (saved_count != before) {
		printf("zvukdl: %d saved track(s) no longer on the card, forgotten\n", before - saved_count);
		index_save();
	}
}

bool zvukdl_local(const char *track_id, char *out, size_t size) {
	uint64_t v = track_id ? strtoull(track_id, NULL, 10) : 0;
	pthread_mutex_lock(&lock);
	index_load();
	int i = v ? find(v) : -1;
	char path[1000] = "";
	if (i >= 0) {
		abs_of(&saved[i], path, sizeof(path));
		if (access(path, R_OK) != 0) {
			// Deleted on a computer: the track streams again.
			printf("zvukdl: %s is gone, its entry dropped\n", path);
			drop(i);
			index_save();
			i = -1;
		}
	}
	pthread_mutex_unlock(&lock);
	if (i >= 0 && out) {
		snprintf(out, size, "%s", path);
	}
	return i >= 0;
}

bool zvukdl_track_for_path(const char *path, char *id, size_t id_size, char *album_id, size_t album_size) {
	if (id_size) {
		id[0] = '\0';
	}
	if (album_id && album_size) {
		album_id[0] = '\0';
	}
	size_t rl = strlen(root);
	if (!path || !rl || strncmp(path, root, rl) != 0 || path[rl] != '/') {
		return false;
	}
	bool found = false;
	pthread_mutex_lock(&lock);
	index_load();
	for (int i = 0; i < saved_count && !found; i++) {
		char rel[600];
		field(saved[i].line, F_PATH, rel, sizeof(rel));
		if (strcmp(rel, path + rl + 1) == 0) {
			found = true;
			snprintf(id, id_size, "%llu", (unsigned long long)saved[i].id);
			if (album_id && album_size && saved[i].album) {
				snprintf(album_id, album_size, "%llu", (unsigned long long)saved[i].album);
			}
		}
	}
	pthread_mutex_unlock(&lock);
	return found;
}

static void track_of(const saved_t *s, zvuk_track_t *t) {
	memset(t, 0, sizeof(*t));
	char num[16];
	snprintf(t->id, sizeof(t->id), "%llu", (unsigned long long)s->id);
	if (s->album) {
		snprintf(t->album_id, sizeof(t->album_id), "%llu", (unsigned long long)s->album);
	}
	field(s->line, F_NUMBER, num, sizeof(num));
	t->track_number = atoi(num);
	field(s->line, F_YEAR, num, sizeof(num));
	t->year = atoi(num);
	field(s->line, F_DURATION, num, sizeof(num));
	t->duration = atoi(num);
	field(s->line, F_TITLE, t->title, sizeof(t->title));
	field(s->line, F_ARTIST, t->artist, sizeof(t->artist));
	field(s->line, F_ALBUM_TITLE, t->album, sizeof(t->album));
	t->available = true;
}

int zvukdl_albums(zvuk_album_t *out, int max) {
	int n = 0;
	pthread_mutex_lock(&lock);
	index_load();
	for (int i = 0; i < saved_count; i++) {
		if (!saved[i].album) {
			continue;
		}
		char id[ZVUK_ID_MAX];
		snprintf(id, sizeof(id), "%llu", (unsigned long long)saved[i].album);
		int k = 0;
		while (k < n && strcmp(out[k].id, id) != 0) {
			k++;
		}
		if (k == n) {
			if (n == max) {
				continue;
			}
			memset(&out[n], 0, sizeof(out[n]));
			snprintf(out[n].id, sizeof(out[n].id), "%s", id);
			field(saved[i].line, F_ALBUM_TITLE, out[n].title, sizeof(out[n].title));
			field(saved[i].line, F_ALBUM_ARTIST, out[n].artist, sizeof(out[n].artist));
			char year[8];
			field(saved[i].line, F_YEAR, year, sizeof(year));
			out[n].year = atoi(year);
			n++;
		}
		out[k].track_count++;
	}
	pthread_mutex_unlock(&lock);
	// By artist, then title: a handful of rows, a simple sort.
	for (int i = 1; i < n; i++) {
		for (int j = i; j > 0; j--) {
			int c = strcasecmp(out[j - 1].artist, out[j].artist);
			if (c < 0 || (c == 0 && strcasecmp(out[j - 1].title, out[j].title) <= 0)) {
				break;
			}
			zvuk_album_t tmp = out[j];
			out[j] = out[j - 1];
			out[j - 1] = tmp;
		}
	}
	return n;
}

int zvukdl_tracks(const char *album_id, zvuk_track_t *out, int max) {
	uint64_t v = album_id && album_id[0] ? strtoull(album_id, NULL, 10) : 0;
	int n = 0;
	pthread_mutex_lock(&lock);
	index_load();
	for (int i = 0; i < saved_count && n < max; i++) {
		if (saved[i].album != v) {
			continue;
		}
		char path[1000];
		abs_of(&saved[i], path, sizeof(path));
		if (access(path, R_OK) != 0) {
			continue;
		}
		track_of(&saved[i], &out[n++]);
	}
	pthread_mutex_unlock(&lock);
	for (int i = 1; i < n; i++) {
		for (int j = i; j > 0 && out[j - 1].track_number > out[j].track_number; j--) {
			zvuk_track_t tmp = out[j];
			out[j] = out[j - 1];
			out[j - 1] = tmp;
		}
	}
	return n;
}

// Removes the album's folder when nothing but its cover is left in it, and
// the artist's when that leaves it empty (rmdir refuses anything else).
static void prune_dirs(const char *file) {
	char dir[1000];
	snprintf(dir, sizeof(dir), "%s", file);
	char *slash = strrchr(dir, '/');
	if (!slash) {
		return;
	}
	*slash = '\0';
	DIR *d = opendir(dir);
	int others = 0;
	struct dirent *e;
	while (d && (e = readdir(d)) != NULL) {
		others += strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0 && strcmp(e->d_name, "cover.jpg") != 0;
	}
	if (d) {
		closedir(d);
	}
	if (others > 0) {
		return;
	}
	char cover[1100];
	snprintf(cover, sizeof(cover), "%s/cover.jpg", dir);
	remove(cover);
	if (rmdir(dir) == 0 && (slash = strrchr(dir, '/')) != NULL) {
		*slash = '\0';
		rmdir(dir);
	}
}

static bool delete_at(int i) {
	char path[1000];
	abs_of(&saved[i], path, sizeof(path));
	bool ok = remove(path) == 0 || errno == ENOENT;
	if (ok) {
		drop(i);
	}
	return ok;
}

bool zvukdl_delete(const char *track_id) {
	uint64_t v = track_id ? strtoull(track_id, NULL, 10) : 0;
	pthread_mutex_lock(&lock);
	index_load();
	int i = v ? find(v) : -1;
	char path[1000] = "";
	bool ok = i < 0;
	if (i >= 0) {
		abs_of(&saved[i], path, sizeof(path));
		ok = delete_at(i);
		index_save();
	}
	pthread_mutex_unlock(&lock);
	if (ok && path[0]) {
		prune_dirs(path);
	}
	return ok;
}

int zvukdl_delete_album(const char *album_id) {
	uint64_t v = album_id ? strtoull(album_id, NULL, 10) : 0;
	int n = 0;
	char last[1000] = "";
	pthread_mutex_lock(&lock);
	index_load();
	for (int i = saved_count - 1; v && i >= 0; i--) {
		if (saved[i].album == v) {
			abs_of(&saved[i], last, sizeof(last));
			if (delete_at(i)) {
				n++;
			}
		}
	}
	index_save();
	pthread_mutex_unlock(&lock);
	if (n > 0 && last[0]) {
		prune_dirs(last);
	}
	return n;
}

// --- claims ------------------------------------------------------------------

static pthread_mutex_t claim_lock = PTHREAD_MUTEX_INITIALIZER;
static char *claims; // the paths, NUL-separated, a second NUL at the end
static size_t claims_len;

void zvukdl_claim(const char *const *paths, int count) {
	size_t len = 1;
	for (int i = 0; i < count; i++) {
		len += strlen(paths[i]) + 1;
	}
	char *block = count > 0 ? malloc(len) : NULL;
	if (block) {
		size_t at = 0;
		for (int i = 0; i < count; i++) {
			size_t n = strlen(paths[i]) + 1;
			memcpy(block + at, paths[i], n);
			at += n;
		}
		block[at] = '\0';
	}
	pthread_mutex_lock(&claim_lock);
	free(claims);
	claims = block;
	claims_len = block ? len : 0;
	pthread_mutex_unlock(&claim_lock);
}

bool zvukdl_claimed(const char *path) {
	if (!path || !claims) {
		return false;
	}
	bool yes = false;
	pthread_mutex_lock(&claim_lock);
	for (const char *p = claims; p && *p && !yes; p += strlen(p) + 1) {
		yes = strcmp(p, path) == 0;
	}
	pthread_mutex_unlock(&claim_lock);
	return yes;
}

// ===========================================================================
// tags on the way to the card (a FILE in front of the real one)
//
// The FLAC and MP3 halves of yandexdl.c's sink: the source's own metadata
// blocks are kept (STREAMINFO first), its tags and padding dropped, and ours
// written with some padding once the last block has gone by; an MP3's ID3v2
// tag is replaced by ours. The audio passes untouched, nothing seeks.
// ===========================================================================

typedef struct {
	char title[ZVUK_TITLE_MAX];
	char artist[ZVUK_NAME_MAX];
	char album[ZVUK_TITLE_MAX];
	char album_artist[ZVUK_NAME_MAX];
	int track, tracks, year;
} tags_t;

enum { S_MAGIC, S_BLOCK_HEAD, S_BLOCK_BODY, S_ID3_HEAD, S_ID3_SKIP, S_PASS };

typedef struct {
	FILE *out;
	bool flac;
	int state;
	const tags_t *tags;
	uint8_t head[10];
	size_t head_len;
	uint8_t *keep;
	size_t keep_len, keep_cap;
	bool keep_this, last, failed;
	uint32_t left;
} sink_t;

typedef struct {
	uint8_t *data;
	size_t len, cap;
	bool failed;
} buf_t;

static void buf_add(buf_t *b, const void *data, size_t len) {
	if (b->failed || len == 0) {
		return;
	}
	if (b->len + len > b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 1024;
		while (cap < b->len + len) {
			cap *= 2;
		}
		uint8_t *grown = realloc(b->data, cap);
		if (!grown) {
			b->failed = true;
			return;
		}
		b->data = grown;
		b->cap = cap;
	}
	memcpy(b->data + b->len, data, len);
	b->len += len;
}

static void le32(buf_t *b, uint32_t v) {
	uint8_t x[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
	buf_add(b, x, 4);
}

static void vorbis_field(buf_t *b, uint32_t *count, const char *name, const char *value) {
	if (!value || !value[0]) {
		return;
	}
	le32(b, (uint32_t)(strlen(name) + 1 + strlen(value)));
	buf_add(b, name, strlen(name));
	buf_add(b, "=", 1);
	buf_add(b, value, strlen(value));
	(*count)++;
}

static void vorbis_number(buf_t *b, uint32_t *count, const char *name, int value) {
	if (value > 0) {
		char text[16];
		snprintf(text, sizeof(text), "%d", value);
		vorbis_field(b, count, name, text);
	}
}

static bool keep_add(sink_t *k, const void *data, size_t len) {
	buf_t b = {k->keep, k->keep_len, k->keep_cap, false};
	buf_add(&b, data, len);
	k->keep = b.data;
	k->keep_len = b.len;
	k->keep_cap = b.cap;
	return !b.failed;
}

static bool flac_header_out(sink_t *k) {
	static const char VENDOR[] = "Sonix Player";
	buf_t c = {0};
	le32(&c, sizeof(VENDOR) - 1);
	buf_add(&c, VENDOR, sizeof(VENDOR) - 1);
	size_t count_at = c.len;
	le32(&c, 0);
	uint32_t fields = 0;
	const tags_t *t = k->tags;
	vorbis_field(&c, &fields, "TITLE", t->title);
	vorbis_field(&c, &fields, "ARTIST", t->artist);
	vorbis_field(&c, &fields, "ALBUM", t->album);
	vorbis_field(&c, &fields, "ALBUMARTIST", t->album_artist);
	vorbis_number(&c, &fields, "TRACKNUMBER", t->track);
	vorbis_number(&c, &fields, "TRACKTOTAL", t->tracks);
	vorbis_number(&c, &fields, "DATE", t->year);
	if (c.failed) {
		free(c.data);
		return false;
	}
	for (int i = 0; i < 4; i++) {
		c.data[count_at + (size_t)i] = (uint8_t)(fields >> (8 * i));
	}
	static const size_t PADDING = 1024;
	uint8_t comment_head[4] = {4, (uint8_t)(c.len >> 16), (uint8_t)(c.len >> 8), (uint8_t)c.len};
	uint8_t padding_head[4] = {0x80 | 1, (uint8_t)(PADDING >> 16), (uint8_t)(PADDING >> 8), (uint8_t)PADDING};
	uint8_t zeros[256] = {0};
	bool ok = fwrite("fLaC", 1, 4, k->out) == 4 && fwrite(k->keep, 1, k->keep_len, k->out) == k->keep_len &&
			  fwrite(comment_head, 1, 4, k->out) == 4 && fwrite(c.data, 1, c.len, k->out) == c.len &&
			  fwrite(padding_head, 1, 4, k->out) == 4;
	for (size_t done = 0; ok && done < PADDING; done += sizeof(zeros)) {
		ok = fwrite(zeros, 1, sizeof(zeros), k->out) == sizeof(zeros);
	}
	free(c.data);
	free(k->keep);
	k->keep = NULL;
	k->keep_len = k->keep_cap = 0;
	return ok;
}

static void id3_frame(buf_t *b, const char *id, const char *text) {
	if (!text || !text[0]) {
		return;
	}
	uint32_t size = (uint32_t)strlen(text) + 1;
	uint8_t head[10] = {(uint8_t)id[0],
						(uint8_t)id[1],
						(uint8_t)id[2],
						(uint8_t)id[3],
						(uint8_t)((size >> 21) & 0x7f),
						(uint8_t)((size >> 14) & 0x7f),
						(uint8_t)((size >> 7) & 0x7f),
						(uint8_t)(size & 0x7f),
						0,
						0};
	buf_add(b, head, sizeof(head));
	buf_add(b, "\x03", 1); // UTF-8
	buf_add(b, text, strlen(text));
}

static bool id3_out(sink_t *k) {
	buf_t f = {0};
	const tags_t *t = k->tags;
	char num[32];
	id3_frame(&f, "TIT2", t->title);
	id3_frame(&f, "TPE1", t->artist);
	id3_frame(&f, "TALB", t->album);
	id3_frame(&f, "TPE2", t->album_artist);
	if (t->track > 0) {
		snprintf(num, sizeof(num), t->tracks > 0 ? "%d/%d" : "%d", t->track, t->tracks);
		id3_frame(&f, "TRCK", num);
	}
	if (t->year > 0) {
		snprintf(num, sizeof(num), "%d", t->year);
		id3_frame(&f, "TDRC", num);
	}
	if (f.failed) {
		free(f.data);
		return false;
	}
	uint32_t size = (uint32_t)f.len;
	uint8_t head[10] = {'I', 'D', '3', 4, 0, 0, (uint8_t)((size >> 21) & 0x7f), (uint8_t)((size >> 14) & 0x7f),
						(uint8_t)((size >> 7) & 0x7f), (uint8_t)(size & 0x7f)};
	bool ok = fwrite(head, 1, sizeof(head), k->out) == sizeof(head) && fwrite(f.data, 1, f.len, k->out) == f.len;
	free(f.data);
	return ok;
}

static ssize_t sink_write(void *cookie, const char *data, size_t size) {
	sink_t *k = cookie;
	const uint8_t *p = (const uint8_t *)data;
	size_t n = size;
	while (n > 0 && !k->failed) {
		if (k->state == S_PASS) {
			k->failed = fwrite(p, 1, n, k->out) != n;
			break;
		}
		if (k->state == S_MAGIC || k->state == S_BLOCK_HEAD || k->state == S_ID3_HEAD) {
			size_t want = k->state == S_ID3_HEAD ? 10 : 4;
			size_t take = want - k->head_len < n ? want - k->head_len : n;
			memcpy(k->head + k->head_len, p, take);
			k->head_len += take;
			p += take;
			n -= take;
			if (k->head_len < want) {
				break;
			}
			k->head_len = 0;
			if (k->state == S_MAGIC) {
				if (memcmp(k->head, "fLaC", 4) != 0) { // not a FLAC after all: as it came
					k->failed = fwrite(k->head, 1, 4, k->out) != 4;
					k->state = S_PASS;
				} else {
					k->state = S_BLOCK_HEAD;
				}
				continue;
			}
			if (k->state == S_BLOCK_HEAD) {
				uint8_t type = k->head[0] & 0x7f;
				k->last = (k->head[0] & 0x80) != 0;
				k->left = (uint32_t)k->head[1] << 16 | (uint32_t)k->head[2] << 8 | k->head[3];
				k->keep_this = type != 1 && type != 4 && type != 127 && (type == 0 || k->left <= KEEP_BLOCK_MAX);
				if (k->keep_this) {
					uint8_t h[4] = {type, k->head[1], k->head[2], k->head[3]};
					k->failed = !keep_add(k, h, 4);
				}
				k->state = S_BLOCK_BODY;
			} else if (memcmp(k->head, "ID3", 3) == 0) {
				k->left = ((uint32_t)(k->head[6] & 0x7f) << 21 | (uint32_t)(k->head[7] & 0x7f) << 14 |
						   (uint32_t)(k->head[8] & 0x7f) << 7 | (k->head[9] & 0x7f)) +
						  ((k->head[5] & 0x10) ? 10 : 0);
				k->failed = !id3_out(k);
				k->state = k->left ? S_ID3_SKIP : S_PASS;
				continue;
			} else {
				k->failed = !id3_out(k) || fwrite(k->head, 1, 10, k->out) != 10;
				k->state = S_PASS;
				continue;
			}
		}
		// S_BLOCK_BODY, S_ID3_SKIP
		size_t take = k->left < n ? k->left : n;
		if (k->state == S_BLOCK_BODY && k->keep_this) {
			k->failed = !keep_add(k, p, take);
		}
		p += take;
		n -= take;
		k->left -= (uint32_t)take;
		if (k->left > 0) {
			break;
		}
		if (k->state == S_ID3_SKIP) {
			k->state = S_PASS;
		} else if (k->last) {
			k->failed = !flac_header_out(k);
			k->state = S_PASS;
		} else {
			k->state = S_BLOCK_HEAD;
		}
	}
	return k->failed ? -1 : (ssize_t)size;
}

static int sink_close(void *cookie) {
	sink_t *k = cookie;
	bool ok = !k->failed && k->state == S_PASS;
	free(k->keep);
	free(k);
	return ok ? 0 : -1;
}

static FILE *sink_open(FILE *out, bool flac, const tags_t *tags) {
	sink_t *k = calloc(1, sizeof(*k));
	if (!k) {
		return NULL;
	}
	k->out = out;
	k->flac = flac;
	k->state = flac ? S_MAGIC : S_ID3_HEAD;
	k->tags = tags;
	cookie_io_functions_t io = {.write = sink_write, .close = sink_close};
	FILE *f = fopencookie(k, "w", io);
	if (!f) {
		free(k);
		return NULL;
	}
	setvbuf(f, NULL, _IOFBF, 64 * 1024);
	return f;
}

// ===========================================================================
// the queue
// ===========================================================================

typedef struct {
	bool album; // only the id and title: expanded on the worker
	zvuk_track_t track;
} entry_t;

static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static entry_t *queue;
static int queue_len;
static bool worker_started, running;
static unsigned generation;
static zvukdl_status_t status;
static void (*listener)(zvukdl_event_t, int, const char *);

void zvukdl_set_listener(void (*cb)(zvukdl_event_t event, int count, const char *text)) { listener = cb; }

static void tell(zvukdl_event_t event, int count, const char *text) {
	if (listener) {
		listener(event, count, text ? text : "");
	}
}

static bool card_usable(void) {
	return root[0] && storage_card_attached() && !usb_storage_active() && storage_sd_writable();
}

static void music_root(char *out, size_t size) {
	int count = 0;
	char **names = library_scan_folders(&count);
	const char *folder = "Music";
	for (int i = 0; i < count; i++) {
		if (names[i] && names[i][0]) {
			folder = names[i];
			break;
		}
	}
	snprintf(out, size, "%s/%s", root, folder);
	library_scan_folders_free(names, count);
}

static bool make_dirs(const char *path) {
	char buf[1024];
	snprintf(buf, sizeof(buf), "%s", path);
	for (char *p = buf + strlen(root) + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
				return false;
			}
			*p = '/';
		}
	}
	return mkdir(buf, 0755) == 0 || errno == EEXIST;
}

static void progress_cb(long written, long total, void *user) {
	unsigned asked = (unsigned)(uintptr_t)user;
	pthread_mutex_lock(&lock);
	status.percent = total > 0 ? (int)((long long)written * 100 / total) : -1;
	if (status.percent > 100) {
		status.percent = 100;
	}
	bool stale = asked != generation;
	pthread_mutex_unlock(&lock);
	if (stale || !card_usable()) {
		zvukcache_fetch_cancel();
	}
}

static bool copy_file(const char *src, FILE *out, unsigned asked) {
	FILE *in = fopen(src, "rb");
	if (!in) {
		return false;
	}
	struct stat st;
	long total = fstat(fileno(in), &st) == 0 ? (long)st.st_size : 0;
	char *buf = malloc(64 * 1024);
	bool ok = buf != NULL;
	long done = 0;
	size_t n;
	while (ok && (n = fread(buf, 1, 64 * 1024, in)) > 0) {
		ok = fwrite(buf, 1, n, out) == n;
		done += (long)n;
		progress_cb(done, total, (void *)(uintptr_t)asked);
		pthread_mutex_lock(&lock);
		ok = ok && asked == generation;
		pthread_mutex_unlock(&lock);
	}
	ok = ok && !ferror(in);
	free(buf);
	fclose(in);
	return ok;
}

static bool exists(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 && st.st_size > 0;
}

static void fetch_cover(const char *folder, const char *url) {
	char path[1100], part[1100];
	snprintf(path, sizeof(path), "%.1024s/cover.jpg", folder);
	if (!url || !url[0] || exists(path)) {
		return;
	}
	char *body = NULL;
	size_t len = 0;
	if (http_get_ex(url, zvuk_stream_headers(), false, &body, &len, 4 * 1024 * 1024, 20, NULL) && len > 0) {
		snprintf(part, sizeof(part), "%.1024s/.cover.jpg.part", folder);
		FILE *f = fopen(part, "wb");
		bool ok = f && fwrite(body, 1, len, f) == len;
		if (f && fclose(f) != 0) {
			ok = false;
		}
		if (!ok || rename(part, path) != 0) {
			remove(part);
		}
	}
	free(body);
}

// The album last asked about: its tracks come one after another.
static zvuk_album_t meta;
static bool meta_ok;

static bool save_track(const zvuk_track_t *t, unsigned asked, char *why, size_t why_size) {
	why[0] = '\0';
	if (!card_usable()) {
		snprintf(why, why_size, "%s", tr("download_no_card"));
		return false;
	}
	if (t->album_id[0] && (!meta_ok || strcmp(meta.id, t->album_id) != 0)) {
		zvuk_track_t *tmp = calloc(1, sizeof(*tmp));
		meta_ok = tmp && zvuk_album_tracks(t->album_id, tmp, 1, &meta) >= 0;
		free(tmp);
	}
	bool have_meta = meta_ok && strcmp(meta.id, t->album_id) == 0;
	tags_t tags;
	memset(&tags, 0, sizeof(tags));
	snprintf(tags.title, sizeof(tags.title), "%s", t->title);
	snprintf(tags.artist, sizeof(tags.artist), "%s", t->artist);
	snprintf(tags.album, sizeof(tags.album), "%s", have_meta ? meta.title : t->album);
	snprintf(tags.album_artist, sizeof(tags.album_artist), "%s",
			 have_meta && meta.artist[0] ? meta.artist : t->artist);
	tags.track = t->track_number;
	tags.tracks = have_meta ? meta.track_count : 0;
	tags.year = have_meta && meta.year ? meta.year : t->year;

	char music[600], artist_dir[160], album_dir[200], album_name[220], base[220], title[200];
	music_root(music, sizeof(music));
	podcastdl_safe_name(tags.album_artist, "Unknown Artist", artist_dir, sizeof(artist_dir));
	if (tags.year > 0) {
		snprintf(album_name, sizeof(album_name), "%.180s (%d)", tags.album, tags.year);
	} else {
		snprintf(album_name, sizeof(album_name), "%.180s", tags.album);
	}
	podcastdl_safe_name(album_name, "Unknown Album", album_dir, sizeof(album_dir));
	podcastdl_safe_name(t->title, t->id, title, sizeof(title));
	if (t->track_number > 0) {
		snprintf(base, sizeof(base), "%02d - %.180s", t->track_number, title);
	} else {
		snprintf(base, sizeof(base), "%s", title);
	}
	char folder[1024];
	snprintf(folder, sizeof(folder), "%s/" ZVUKDL_FOLDER "/%s/%s", music, artist_dir, album_dir);
	static const char *const EXTS[] = {"flac", "mp3"};
	for (size_t i = 0; i < sizeof(EXTS) / sizeof(EXTS[0]); i++) {
		char existing[1300];
		snprintf(existing, sizeof(existing), "%.1024s/%.220s.%s", folder, base, EXTS[i]);
		if (exists(existing)) {
			printf("zvukdl: %s is already there\n", existing);
			index_add(t, tags.album, tags.album_artist, tags.year, existing);
			return true;
		}
	}
	if (!make_dirs(folder)) {
		snprintf(why, why_size, "%s", tr("download_failed"));
		return false;
	}
	// The cached copy when it is what would be fetched: FLAC, or anything when
	// FLAC is not asked for.
	char cached[600] = "";
	bool from_cache = zvukcache_find(t->id, cached, sizeof(cached)) &&
					  (strstr(cached, ".flac") || zvuk_get_quality() != ZVUK_QUALITY_FLAC);
	zvuk_stream_t stream;
	memset(&stream, 0, sizeof(stream));
	if (!from_cache && !zvuk_stream(t->id, &stream)) {
		snprintf(why, why_size, "%s", zvuk_last_error());
		return false;
	}
	bool flac = from_cache ? strstr(cached, ".flac") != NULL : stream.got == ZVUK_QUALITY_FLAC;
	const char *ext = flac ? "flac" : "mp3";
	char path[1300], part[1300];
	snprintf(path, sizeof(path), "%.1024s/%.220s.%s", folder, base, ext);
	snprintf(part, sizeof(part), "%.1024s/.%.220s.%s.part", folder, base, ext);
	FILE *out = fopen(part, "wb");
	if (!out) {
		snprintf(why, why_size, "%s", tr("download_failed"));
		return false;
	}
	FILE *sink = sink_open(out, flac, &tags);
	FILE *to = sink ? sink : out;
	uint64_t samples = 0;
	bool ok;
	if (from_cache) {
		printf("zvukdl: %s from the cache\n", t->id);
		ok = copy_file(cached, to, asked);
	} else {
		ok = zvukcache_fetch(t->id, &stream, to, progress_cb, (void *)(uintptr_t)asked, &samples, why, why_size);
	}
	if (sink && fclose(sink) != 0) {
		ok = false;
	}
	if (fclose(out) != 0) {
		ok = false;
	}
	pthread_mutex_lock(&lock);
	ok = ok && asked == generation;
	pthread_mutex_unlock(&lock);
	if (!ok || rename(part, path) != 0) {
		remove(part);
		if (!why[0]) {
			snprintf(why, why_size, "%s", tr("download_failed"));
		}
		return false;
	}
	if (samples) {
		mp4flac_patch_total_samples(path, samples);
	}
	fetch_cover(folder, have_meta && meta.cover[0] ? meta.cover : t->cover);
	index_add(t, tags.album, tags.album_artist, tags.year, path);
	printf("zvukdl: saved %s\n", path);
	return true;
}

static void library_look(void *user) {
	(void)user;
	if (storage_sd_root() && storage_sd_root()[0]) {
		library_card_returned(storage_sd_root());
	}
}

static void *worker_main(void *arg) {
	(void)arg;
	thread_be_low_priority("zvuk save");
	http_set_thread_stream_headers(zvuk_stream_headers());
	for (;;) {
		pthread_mutex_lock(&lock);
		while (queue_len == 0) {
			pthread_cond_wait(&ready, &lock);
		}
		entry_t e = queue[0];
		memmove(queue, queue + 1, (size_t)(--queue_len) * sizeof(*queue));
		running = true;
		unsigned asked = generation;
		snprintf(status.title, sizeof(status.title), "%.95s", e.track.title);
		status.percent = -1;
		pthread_mutex_unlock(&lock);

		if (e.album) {
			zvuk_track_t *tracks = calloc(300, sizeof(*tracks));
			int n = tracks ? zvuk_album_tracks(e.track.album_id, tracks, 300, NULL) : -1;
			// What is on the card already is not saved again (asked outside
			// the lock, which the index takes too).
			int keep = 0;
			for (int i = 0; i < n; i++) {
				if (!zvukdl_saved(tracks[i].id)) {
					tracks[keep++] = tracks[i];
				}
			}
			pthread_mutex_lock(&lock);
			bool current = asked == generation;
			for (int i = 0; current && i < keep && queue_len < QUEUE_MAX; i++) {
				// Ahead of what was queued after the album: its tracks in order.
				memmove(queue + 1 + i, queue + i, (size_t)(queue_len - i) * sizeof(*queue));
				queue[i] = (entry_t){.album = false, .track = tracks[i]};
				queue_len++;
			}
			status.queued = queue_len;
			running = false;
			pthread_mutex_unlock(&lock);
			if (n < 0) {
				char line[400];
				snprintf(line, sizeof(line), "%s: %s", e.track.title, zvuk_last_error());
				tell(ZVUKDL_FAILED, 0, line);
			}
			free(tracks);
		} else {
			char why[256];
			bool ok = save_track(&e.track, asked, why, sizeof(why));
			pthread_mutex_lock(&lock);
			bool current = asked == generation;
			if (current) {
				status.done += ok;
				status.failed += !ok;
			}
			status.queued = queue_len;
			running = false;
			pthread_mutex_unlock(&lock);
			if (current && ok) {
				tell(ZVUKDL_SAVED, 1, e.track.id);
			} else if (current) {
				char line[480];
				snprintf(line, sizeof(line), "%s: %s", e.track.title, why);
				tell(ZVUKDL_FAILED, 0, line);
			}
		}
		pthread_mutex_lock(&lock);
		bool finished = queue_len == 0 && !running;
		int done_count = status.done;
		if (finished) {
			status.active = false;
			status.title[0] = '\0';
		}
		pthread_mutex_unlock(&lock);
		if (finished) {
			char where[700];
			snprintf(where, sizeof(where), "%s/%s", "Music", ZVUKDL_FOLDER);
			tell(ZVUKDL_FINISHED, done_count, where);
			if (done_count > 0) {
				library_look(NULL);
			}
		}
	}
	return NULL;
}

static bool enqueue(const entry_t *e) {
	pthread_mutex_lock(&lock);
	if (!root[0] || queue_len >= QUEUE_MAX) {
		pthread_mutex_unlock(&lock);
		return false;
	}
	if (!queue) {
		queue = malloc(QUEUE_MAX * sizeof(*queue));
		if (!queue) {
			pthread_mutex_unlock(&lock);
			return false;
		}
	}
	if (!status.active) {
		memset(&status, 0, sizeof(status));
		status.active = true;
	}
	queue[queue_len++] = *e;
	status.queued = queue_len;
	if (!worker_started) {
		pthread_t th;
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		pthread_attr_setstacksize(&attr, 256 * 1024);
		worker_started = pthread_create(&th, &attr, worker_main, NULL) == 0;
		pthread_attr_destroy(&attr);
	}
	pthread_cond_signal(&ready);
	pthread_mutex_unlock(&lock);
	tell(ZVUKDL_QUEUED, 1, e->track.title);
	return true;
}

bool zvukdl_track(const zvuk_track_t *track) {
	if (!track || !track->id[0]) {
		return false;
	}
	entry_t e = {.album = false, .track = *track};
	return enqueue(&e);
}

bool zvukdl_album(const char *album_id, const char *title) {
	if (!album_id || !album_id[0]) {
		return false;
	}
	entry_t e;
	memset(&e, 0, sizeof(e));
	e.album = true;
	snprintf(e.track.album_id, sizeof(e.track.album_id), "%s", album_id);
	snprintf(e.track.title, sizeof(e.track.title), "%s", title ? title : "");
	return enqueue(&e);
}

void zvukdl_status(zvukdl_status_t *out) {
	pthread_mutex_lock(&lock);
	*out = status;
	out->queued = queue_len;
	pthread_mutex_unlock(&lock);
}

bool zvukdl_busy(void) {
	pthread_mutex_lock(&lock);
	bool busy = queue_len > 0 || running;
	pthread_mutex_unlock(&lock);
	return busy;
}

void zvukdl_cancel_all(void) {
	pthread_mutex_lock(&lock);
	bool any = queue_len > 0 || running;
	queue_len = 0;
	generation++;
	status.active = false;
	status.title[0] = '\0';
	pthread_mutex_unlock(&lock);
	zvukcache_fetch_cancel();
	if (any) {
		tell(ZVUKDL_STOPPED, 0, tr("zvuk_save_stopped"));
	}
}

void zvukdl_set_root(const char *sd_root) {
	pthread_mutex_lock(&lock);
	bool changed = strcmp(root, sd_root ? sd_root : "") != 0;
	if (changed) {
		snprintf(root, sizeof(root), "%s", sd_root ? sd_root : "");
		index_forget();
	}
	pthread_mutex_unlock(&lock);
	if (changed) {
		zvukdl_cancel_all();
	}
}

#endif /* SONIX_ZVUK */

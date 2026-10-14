#ifdef SONIX_ZVUK

#include "zvuklist.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// See zvuklist.h.

struct zvuklist {
	pthread_mutex_t lock;
	int refs;
	uint64_t *ids;
	char **packed; // per entry: NULL until its page came (see pack())
	int count, room;
	unsigned *page_use; // per page: when it was last used (0 = not loaded)
	int pages_room;
	unsigned clock;
	bool pinned; // details came whole: never let go
};

// The strings of a track end to end, NUL after each, after a small header of
// numbers: what a loaded entry costs is what it says.
typedef struct {
	int duration;
	int track_number;
	int year;
	bool available;
} head_t;

static char *pack(const zvuk_track_t *t) {
	const char *parts[] = {t->id, t->album_id, t->artist_id, t->title, t->artist, t->album, t->cover};
	size_t len = sizeof(head_t);
	for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
		len += strlen(parts[i]) + 1;
	}
	char *p = malloc(len);
	if (!p) {
		return NULL;
	}
	head_t h = {t->duration, t->track_number, t->year, t->available};
	memcpy(p, &h, sizeof(h));
	char *at = p + sizeof(h);
	for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
		size_t n = strlen(parts[i]) + 1;
		memcpy(at, parts[i], n);
		at += n;
	}
	return p;
}

static void unpack(const char *p, zvuk_track_t *t) {
	memset(t, 0, sizeof(*t));
	head_t h;
	memcpy(&h, p, sizeof(h));
	t->duration = h.duration;
	t->track_number = h.track_number;
	t->year = h.year;
	t->available = h.available;
	struct {
		char *out;
		size_t size;
	} parts[] = {{t->id, sizeof(t->id)},		 {t->album_id, sizeof(t->album_id)}, {t->artist_id, sizeof(t->artist_id)},
				 {t->title, sizeof(t->title)}, {t->artist, sizeof(t->artist)},		 {t->album, sizeof(t->album)},
				 {t->cover, sizeof(t->cover)}};
	const char *at = p + sizeof(h);
	for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
		snprintf(parts[i].out, parts[i].size, "%s", at);
		at += strlen(at) + 1;
	}
}

zvuklist_t *zvuklist_new(void) {
	zvuklist_t *l = calloc(1, sizeof(*l));
	if (l) {
		pthread_mutex_init(&l->lock, NULL);
		l->refs = 1;
	}
	return l;
}

zvuklist_t *zvuklist_ref(zvuklist_t *l) {
	if (l) {
		pthread_mutex_lock(&l->lock);
		l->refs++;
		pthread_mutex_unlock(&l->lock);
	}
	return l;
}

void zvuklist_free(zvuklist_t *l) {
	if (!l) {
		return;
	}
	pthread_mutex_lock(&l->lock);
	bool last = --l->refs == 0;
	pthread_mutex_unlock(&l->lock);
	if (!last) {
		return;
	}
	for (int i = 0; i < l->count; i++) {
		free(l->packed[i]);
	}
	free(l->packed);
	free(l->ids);
	free(l->page_use);
	pthread_mutex_destroy(&l->lock);
	free(l);
}

// Room for one more entry. Call with the lock held.
static bool grow(zvuklist_t *l) {
	if (l->count < l->room) {
		return true;
	}
	int room = l->room ? l->room * 2 : 64;
	uint64_t *ids = realloc(l->ids, (size_t)room * sizeof(*ids));
	if (!ids) {
		return false;
	}
	l->ids = ids;
	char **packed = realloc(l->packed, (size_t)room * sizeof(*packed));
	if (!packed) {
		return false;
	}
	l->packed = packed;
	int pages = (room + ZVUKLIST_PAGE - 1) / ZVUKLIST_PAGE;
	unsigned *use = realloc(l->page_use, (size_t)pages * sizeof(*use));
	if (!use) {
		return false;
	}
	memset(use + l->pages_room, 0, (size_t)(pages - l->pages_room) * sizeof(*use));
	l->page_use = use;
	l->pages_room = pages;
	l->room = room;
	return true;
}

static bool add(zvuklist_t *l, const char *id, const zvuk_track_t *t) {
	uint64_t v = id && id[0] ? strtoull(id, NULL, 10) : 0;
	if (!l || !v) {
		return false;
	}
	char *p = t ? pack(t) : NULL;
	if (t && !p) {
		return false;
	}
	pthread_mutex_lock(&l->lock);
	bool ok = grow(l);
	if (ok) {
		l->ids[l->count] = v;
		l->packed[l->count] = p;
		if (p) {
			l->pinned = true;
			l->page_use[l->count / ZVUKLIST_PAGE] = ++l->clock;
		}
		l->count++;
	} else {
		free(p);
	}
	pthread_mutex_unlock(&l->lock);
	return ok;
}

bool zvuklist_add_id(zvuklist_t *l, const char *id) { return add(l, id, NULL); }

bool zvuklist_add_track(zvuklist_t *l, const zvuk_track_t *t) { return t && add(l, t->id, t); }

zvuklist_t *zvuklist_clone(zvuklist_t *l) {
	zvuklist_t *c = zvuklist_new();
	if (!c || !l) {
		return c;
	}
	pthread_mutex_lock(&l->lock);
	bool ok = true;
	for (int i = 0; ok && i < l->count; i++) {
		ok = grow(c);
		if (ok) {
			c->ids[i] = l->ids[i];
			c->packed[i] = NULL;
			if (l->packed[i]) {
				zvuk_track_t t;
				unpack(l->packed[i], &t);
				c->packed[i] = pack(&t);
				if (c->packed[i]) {
					c->page_use[i / ZVUKLIST_PAGE] = 1;
				}
			}
			c->count++;
		}
	}
	c->pinned = l->pinned;
	pthread_mutex_unlock(&l->lock);
	if (!ok) {
		zvuklist_free(c);
		return NULL;
	}
	return c;
}

int zvuklist_count(zvuklist_t *l) {
	if (!l) {
		return 0;
	}
	pthread_mutex_lock(&l->lock);
	int n = l->count;
	pthread_mutex_unlock(&l->lock);
	return n;
}

bool zvuklist_id(zvuklist_t *l, int index, char *out, size_t size) {
	out[0] = '\0';
	if (!l) {
		return false;
	}
	pthread_mutex_lock(&l->lock);
	bool ok = index >= 0 && index < l->count;
	if (ok) {
		snprintf(out, size, "%llu", (unsigned long long)l->ids[index]);
	}
	pthread_mutex_unlock(&l->lock);
	return ok;
}

int zvuklist_find(zvuklist_t *l, const char *id) {
	uint64_t v = id && id[0] ? strtoull(id, NULL, 10) : 0;
	if (!l || !v) {
		return -1;
	}
	pthread_mutex_lock(&l->lock);
	int found = -1;
	for (int i = 0; i < l->count && found < 0; i++) {
		if (l->ids[i] == v) {
			found = i;
		}
	}
	pthread_mutex_unlock(&l->lock);
	return found;
}

bool zvuklist_track(zvuklist_t *l, int index, zvuk_track_t *out) {
	if (!l) {
		return false;
	}
	pthread_mutex_lock(&l->lock);
	bool ok = index >= 0 && index < l->count && l->packed[index];
	if (ok) {
		l->page_use[index / ZVUKLIST_PAGE] = ++l->clock;
		if (out) {
			unpack(l->packed[index], out);
		}
	}
	pthread_mutex_unlock(&l->lock);
	return ok;
}

bool zvuklist_page_ready(zvuklist_t *l, int page) {
	if (!l) {
		return false;
	}
	pthread_mutex_lock(&l->lock);
	bool ready = page >= 0 && page * ZVUKLIST_PAGE < l->count && l->page_use[page] != 0;
	pthread_mutex_unlock(&l->lock);
	return ready;
}

// Lets go of the least recently used pages past the limit. Lock held.
static void evict(zvuklist_t *l) {
	if (l->pinned) {
		return;
	}
	int pages = (l->count + ZVUKLIST_PAGE - 1) / ZVUKLIST_PAGE;
	for (;;) {
		int loaded = 0, oldest = -1;
		for (int p = 0; p < pages; p++) {
			if (l->page_use[p]) {
				loaded++;
				if (oldest < 0 || l->page_use[p] < l->page_use[oldest]) {
					oldest = p;
				}
			}
		}
		if (loaded <= ZVUKLIST_PAGES_KEPT || oldest < 0) {
			return;
		}
		for (int i = oldest * ZVUKLIST_PAGE; i < l->count && i < (oldest + 1) * ZVUKLIST_PAGE; i++) {
			free(l->packed[i]);
			l->packed[i] = NULL;
		}
		l->page_use[oldest] = 0;
	}
}

int zvuklist_fetch_page(zvuklist_t *l, int page) {
	if (!l || page < 0) {
		return -1;
	}
	char ids[ZVUKLIST_PAGE][ZVUK_ID_MAX];
	const char *ptrs[ZVUKLIST_PAGE];
	int n = 0;
	pthread_mutex_lock(&l->lock);
	for (int i = page * ZVUKLIST_PAGE; i < l->count && n < ZVUKLIST_PAGE; i++, n++) {
		snprintf(ids[n], sizeof(ids[n]), "%llu", (unsigned long long)l->ids[i]);
		ptrs[n] = ids[n];
	}
	bool ready = n > 0 && l->page_use[page] != 0;
	pthread_mutex_unlock(&l->lock);
	if (n == 0 || ready) {
		return n;
	}
	zvuk_track_t *got = calloc(ZVUKLIST_PAGE, sizeof(*got));
	if (!got) {
		return -1;
	}
	int count = zvuk_tracks(ptrs, n, got, ZVUKLIST_PAGE);
	if (count < 0) {
		free(got);
		return -1;
	}
	pthread_mutex_lock(&l->lock);
	int filled = 0;
	for (int k = 0; k < n; k++) {
		int i = page * ZVUKLIST_PAGE + k;
		if (i >= l->count || l->packed[i]) {
			filled += i < l->count;
			continue;
		}
		const zvuk_track_t *match = NULL;
		for (int j = 0; j < count && !match; j++) {
			if (strcmp(got[j].id, ids[k]) == 0) {
				match = &got[j];
			}
		}
		// A track the service no longer has stays in its place, faint, rather
		// than "loading" for ever.
		zvuk_track_t gone;
		if (!match) {
			memset(&gone, 0, sizeof(gone));
			snprintf(gone.id, sizeof(gone.id), "%.*s", ZVUK_ID_MAX - 1, ids[k]);
			snprintf(gone.title, sizeof(gone.title), "\xE2\x80\x94");
			match = &gone;
		}
		l->packed[i] = pack(match);
		filled += l->packed[i] != NULL;
	}
	l->page_use[page] = ++l->clock;
	evict(l);
	pthread_mutex_unlock(&l->lock);
	free(got);
	return filled;
}

size_t zvuklist_memory(zvuklist_t *l) {
	if (!l) {
		return 0;
	}
	pthread_mutex_lock(&l->lock);
	size_t bytes = sizeof(*l) + (size_t)l->room * (sizeof(uint64_t) + sizeof(char *)) +
				   (size_t)l->pages_room * sizeof(unsigned);
	for (int i = 0; i < l->count; i++) {
		if (l->packed[i]) {
			bytes += sizeof(head_t) + strlen(l->packed[i] + sizeof(head_t)) + 140; // about
		}
	}
	pthread_mutex_unlock(&l->lock);
	return bytes;
}

#endif /* SONIX_ZVUK */

#include "yandexlist.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// See yandexlist.h.

// An entry: the track and its album as numbers. A track id that is not a
// number below 2^31 (a user's upload) is kept in `extra` and the entry holds
// its index there with the top bit set. An album that is not a number is
// simply not kept: the album id only travels with a like and a request, where
// leaving it out is allowed.
#define EXTRA_FLAG 0x80000000u

typedef struct {
	uint32_t track;
	uint32_t album;
} ref_t;

// The details of one entry, compact: the four strings in one allocation,
// "title\0artist\0album\0cover\0" (the cover a full https:// URL). NULL text
// = no details yet.
typedef struct {
	char *text;
	uint16_t duration;
	uint8_t track_number;
	uint8_t disc_number;
	bool available;
} detail_t;

typedef struct {
	detail_t d[YANDEXLIST_PAGE];
	unsigned last_use;
} page_t;

struct yandexlist {
	pthread_mutex_t lock;
	int refs_held; // the reference count
	ref_t *refs;
	int count, room;
	char (*extra)[YANDEX_ID_MAX];
	int extra_count, extra_room;
	page_t **pages; // one slot per YANDEXLIST_PAGE entries, NULL = not loaded
	int page_slots;
	int pages_loaded;
	unsigned clock;
	size_t text_bytes;
};

// "12345" -> 12345; false for anything that is not a plain number below 2^31.
static bool parse_id(const char *s, uint32_t *out) {
	if (!s || !s[0] || strlen(s) > 10) {
		return false;
	}
	uint64_t v = 0;
	for (const char *p = s; *p; p++) {
		if (*p < '0' || *p > '9') {
			return false;
		}
		v = v * 10 + (uint64_t)(*p - '0');
	}
	if (v == 0 || v >= EXTRA_FLAG) {
		return false;
	}
	*out = (uint32_t)v;
	return true;
}

// Call with the lock held.
static void ref_track_id(const yandexlist_t *l, const ref_t *r, char *out, size_t size) {
	if (r->track & EXTRA_FLAG) {
		uint32_t at = r->track & ~EXTRA_FLAG;
		snprintf(out, size, "%s", (int)at < l->extra_count ? l->extra[at] : "");
	} else {
		snprintf(out, size, "%u", (unsigned)r->track);
	}
}

yandexlist_t *yandexlist_new(void) {
	yandexlist_t *l = calloc(1, sizeof(*l));
	if (!l) {
		return NULL;
	}
	pthread_mutex_init(&l->lock, NULL);
	l->refs_held = 1;
	return l;
}

yandexlist_t *yandexlist_ref(yandexlist_t *l) {
	if (l) {
		pthread_mutex_lock(&l->lock);
		l->refs_held++;
		pthread_mutex_unlock(&l->lock);
	}
	return l;
}

// The bytes of a packed text: its four strings and their terminators.
static size_t text_size(const char *text) {
	size_t n = 0;
	for (int i = 0; i < 4; i++) {
		n += strlen(text + n) + 1;
	}
	return n;
}

static void page_free(page_t *p) {
	if (!p) {
		return;
	}
	for (int i = 0; i < YANDEXLIST_PAGE; i++) {
		free(p->d[i].text);
	}
	free(p);
}

void yandexlist_free(yandexlist_t *l) {
	if (!l) {
		return;
	}
	pthread_mutex_lock(&l->lock);
	bool last = --l->refs_held == 0;
	pthread_mutex_unlock(&l->lock);
	if (!last) {
		return;
	}
	for (int i = 0; i < l->page_slots; i++) {
		page_free(l->pages[i]);
	}
	free(l->pages);
	free(l->refs);
	free(l->extra);
	pthread_mutex_destroy(&l->lock);
	free(l);
}

// Room for one more entry, and for its page slot. Call with the lock held.
static bool grow_locked(yandexlist_t *l) {
	if (l->count == l->room) {
		int room = l->room ? l->room * 2 : 64;
		ref_t *refs = realloc(l->refs, (size_t)room * sizeof(*refs));
		if (!refs) {
			return false;
		}
		l->refs = refs;
		l->room = room;
	}
	int slots_needed = l->count / YANDEXLIST_PAGE + 1;
	if (slots_needed > l->page_slots) {
		int slots = l->page_slots ? l->page_slots * 2 : 4;
		while (slots < slots_needed) {
			slots *= 2;
		}
		page_t **pages = realloc(l->pages, (size_t)slots * sizeof(*pages));
		if (!pages) {
			return false;
		}
		memset(pages + l->page_slots, 0, (size_t)(slots - l->page_slots) * sizeof(*pages));
		l->pages = pages;
		l->page_slots = slots;
	}
	return true;
}

// Call with the lock held.
static bool add_locked(yandexlist_t *l, const char *track_id, const char *album_id) {
	ref_t r = {0, 0};
	if (!parse_id(track_id, &r.track)) {
		if (!track_id || !track_id[0]) {
			return false;
		}
		if (l->extra_count == l->extra_room) {
			int room = l->extra_room ? l->extra_room * 2 : 8;
			char(*extra)[YANDEX_ID_MAX] = realloc(l->extra, (size_t)room * sizeof(*extra));
			if (!extra) {
				return false;
			}
			l->extra = extra;
			l->extra_room = room;
		}
		snprintf(l->extra[l->extra_count], sizeof(l->extra[0]), "%s", track_id);
		r.track = EXTRA_FLAG | (uint32_t)l->extra_count++;
	}
	if (!parse_id(album_id, &r.album)) {
		r.album = 0;
	}
	if (!grow_locked(l)) {
		return false;
	}
	l->refs[l->count++] = r;
	return true;
}

bool yandexlist_add_id(yandexlist_t *l, const char *track_id, const char *album_id) {
	if (!l) {
		return false;
	}
	pthread_mutex_lock(&l->lock);
	bool ok = add_locked(l, track_id, album_id);
	pthread_mutex_unlock(&l->lock);
	return ok;
}

// The page of entry `index`, made when missing -- with the least recently
// used page let go first when YANDEXLIST_PAGES_KEPT are loaded already. NULL
// when out of memory. Call with the lock held.
static page_t *page_for_locked(yandexlist_t *l, int index) {
	int slot = index / YANDEXLIST_PAGE;
	if (slot >= l->page_slots) {
		return NULL;
	}
	if (l->pages[slot]) {
		return l->pages[slot];
	}
	if (l->pages_loaded >= YANDEXLIST_PAGES_KEPT) {
		int oldest = -1;
		for (int i = 0; i < l->page_slots; i++) {
			if (l->pages[i] && (oldest < 0 || l->pages[i]->last_use < l->pages[oldest]->last_use)) {
				oldest = i;
			}
		}
		if (oldest >= 0) {
			for (int i = 0; i < YANDEXLIST_PAGE; i++) {
				if (l->pages[oldest]->d[i].text) {
					l->text_bytes -= text_size(l->pages[oldest]->d[i].text);
				}
			}
			page_free(l->pages[oldest]);
			l->pages[oldest] = NULL;
			l->pages_loaded--;
		}
	}
	page_t *p = calloc(1, sizeof(*p));
	if (!p) {
		return NULL;
	}
	p->last_use = ++l->clock;
	l->pages[slot] = p;
	l->pages_loaded++;
	return p;
}

// The four strings of a track in one allocation.
static char *pack_text(const yandex_track_t *t) {
	size_t a = strlen(t->title) + 1, b = strlen(t->artist) + 1, c = strlen(t->album) + 1, d = strlen(t->cover) + 1;
	char *text = malloc(a + b + c + d);
	if (!text) {
		return NULL;
	}
	memcpy(text, t->title, a);
	memcpy(text + a, t->artist, b);
	memcpy(text + a + b, t->album, c);
	memcpy(text + a + b + c, t->cover, d);
	return text;
}

// Call with the lock held.
static void set_detail_locked(yandexlist_t *l, int index, const yandex_track_t *t) {
	page_t *p = page_for_locked(l, index);
	if (!p) {
		return;
	}
	detail_t *d = &p->d[index % YANDEXLIST_PAGE];
	char *text = pack_text(t);
	if (!text) {
		return;
	}
	if (d->text) {
		l->text_bytes -= text_size(d->text);
		free(d->text);
	}
	d->text = text;
	l->text_bytes += text_size(text);
	d->duration = (uint16_t)(t->duration > 0 && t->duration < 65535 ? t->duration : 0);
	d->track_number = (uint8_t)(t->track_number > 0 && t->track_number < 256 ? t->track_number : 0);
	d->disc_number = (uint8_t)(t->disc_number > 0 && t->disc_number < 256 ? t->disc_number : 0);
	d->available = t->available;
}

bool yandexlist_add_track(yandexlist_t *l, const yandex_track_t *t) {
	if (!l || !t) {
		return false;
	}
	pthread_mutex_lock(&l->lock);
	bool ok = add_locked(l, t->id, t->album_id);
	if (ok) {
		set_detail_locked(l, l->count - 1, t);
	}
	pthread_mutex_unlock(&l->lock);
	return ok;
}

yandexlist_t *yandexlist_clone(yandexlist_t *l) {
	if (!l) {
		return NULL;
	}
	yandexlist_t *c = yandexlist_new();
	if (!c) {
		return NULL;
	}
	pthread_mutex_lock(&l->lock);
	bool ok = true;
	c->refs = malloc((size_t)(l->count > 0 ? l->count : 1) * sizeof(*c->refs));
	c->pages = calloc((size_t)(l->page_slots > 0 ? l->page_slots : 1), sizeof(*c->pages));
	if (l->extra_count > 0) {
		c->extra = malloc((size_t)l->extra_count * sizeof(*c->extra));
		ok = c->extra != NULL;
	}
	ok = ok && c->refs && c->pages;
	if (ok) {
		memcpy(c->refs, l->refs, (size_t)l->count * sizeof(*c->refs));
		c->room = l->count > 0 ? l->count : 1; // what was allocated above
		c->count = l->count;
		c->page_slots = l->page_slots > 0 ? l->page_slots : 1;
		if (l->extra_count > 0) {
			memcpy(c->extra, l->extra, (size_t)l->extra_count * sizeof(*c->extra));
		}
		c->extra_count = c->extra_room = l->extra_count;
		for (int i = 0; i < l->page_slots && ok; i++) {
			const page_t *src = l->pages[i];
			if (!src) {
				continue;
			}
			page_t *dst = calloc(1, sizeof(*dst));
			ok = dst != NULL;
			for (int k = 0; ok && k < YANDEXLIST_PAGE; k++) {
				dst->d[k] = src->d[k];
				if (src->d[k].text) {
					size_t size = text_size(src->d[k].text);
					dst->d[k].text = malloc(size);
					ok = dst->d[k].text != NULL;
					if (ok) {
						memcpy(dst->d[k].text, src->d[k].text, size);
						c->text_bytes += size;
					}
				}
			}
			if (dst) {
				dst->last_use = src->last_use;
				c->pages[i] = dst;
				c->pages_loaded++;
			}
		}
		c->clock = l->clock;
	}
	pthread_mutex_unlock(&l->lock);
	if (!ok) {
		yandexlist_free(c);
		return NULL;
	}
	return c;
}

int yandexlist_count(yandexlist_t *l) {
	if (!l) {
		return 0;
	}
	pthread_mutex_lock(&l->lock);
	int n = l->count;
	pthread_mutex_unlock(&l->lock);
	return n;
}

bool yandexlist_ids(yandexlist_t *l, int index, char *id, size_t id_size, char *album, size_t album_size) {
	if (!l) {
		return false;
	}
	pthread_mutex_lock(&l->lock);
	bool ok = index >= 0 && index < l->count;
	if (ok) {
		ref_track_id(l, &l->refs[index], id, id_size);
		if (album && album_size > 0) {
			if (l->refs[index].album) {
				snprintf(album, album_size, "%u", (unsigned)l->refs[index].album);
			} else {
				album[0] = '\0';
			}
		}
	}
	pthread_mutex_unlock(&l->lock);
	return ok;
}

int yandexlist_find(yandexlist_t *l, const char *track_id) {
	if (!l || !track_id || !track_id[0]) {
		return -1;
	}
	uint32_t want;
	bool numeric = parse_id(track_id, &want);
	pthread_mutex_lock(&l->lock);
	int found = -1;
	for (int i = 0; i < l->count && found < 0; i++) {
		uint32_t t = l->refs[i].track;
		if (numeric) {
			found = t == want ? i : -1;
		} else if (t & EXTRA_FLAG) {
			uint32_t at = t & ~EXTRA_FLAG;
			found = (int)at < l->extra_count && strcmp(l->extra[at], track_id) == 0 ? i : -1;
		}
	}
	pthread_mutex_unlock(&l->lock);
	return found;
}

// Unpacks entry `index` into a yandex_track_t. Call with the lock held, with
// the details known to be there.
static void unpack_locked(yandexlist_t *l, int index, const detail_t *d, yandex_track_t *out) {
	memset(out, 0, sizeof(*out));
	ref_track_id(l, &l->refs[index], out->id, sizeof(out->id));
	if (l->refs[index].album) {
		snprintf(out->album_id, sizeof(out->album_id), "%u", (unsigned)l->refs[index].album);
	}
	const char *p = d->text;
	snprintf(out->title, sizeof(out->title), "%s", p);
	p += strlen(p) + 1;
	snprintf(out->artist, sizeof(out->artist), "%s", p);
	p += strlen(p) + 1;
	snprintf(out->album, sizeof(out->album), "%s", p);
	p += strlen(p) + 1;
	snprintf(out->cover, sizeof(out->cover), "%s", p);
	out->duration = d->duration;
	out->track_number = d->track_number;
	out->disc_number = d->disc_number;
	out->available = d->available;
}

bool yandexlist_track(yandexlist_t *l, int index, yandex_track_t *out) {
	if (!l) {
		return false;
	}
	pthread_mutex_lock(&l->lock);
	bool ok = false;
	if (index >= 0 && index < l->count) {
		page_t *p = l->pages[index / YANDEXLIST_PAGE];
		const detail_t *d = p ? &p->d[index % YANDEXLIST_PAGE] : NULL;
		ok = d && d->text;
		if (ok) {
			p->last_use = ++l->clock;
			if (out) {
				unpack_locked(l, index, d, out);
			}
		}
	}
	pthread_mutex_unlock(&l->lock);
	return ok;
}

// Whether every entry of the page has details (or was asked for and the
// service had none). Call with the lock held.
static bool page_complete_locked(yandexlist_t *l, int page) {
	if (page < 0 || page >= l->page_slots || page * YANDEXLIST_PAGE >= l->count) {
		return page >= 0; // past the end: nothing to fetch
	}
	const page_t *p = l->pages[page];
	if (!p) {
		return false;
	}
	int end = (page + 1) * YANDEXLIST_PAGE < l->count ? YANDEXLIST_PAGE : l->count - page * YANDEXLIST_PAGE;
	for (int i = 0; i < end; i++) {
		if (!p->d[i].text) {
			return false;
		}
	}
	return true;
}

bool yandexlist_page_ready(yandexlist_t *l, int page) {
	if (!l) {
		return false;
	}
	pthread_mutex_lock(&l->lock);
	bool ready = page_complete_locked(l, page);
	pthread_mutex_unlock(&l->lock);
	return ready;
}

int yandexlist_fetch_page(yandexlist_t *l, int page) {
	if (!l || page < 0) {
		return -1;
	}
	// The ids still without details, "id" or "id:album", as /tracks takes them.
	char refs[YANDEXLIST_PAGE][YANDEX_ID_MAX * 2];
	const char *ptrs[YANDEXLIST_PAGE];
	int index_of[YANDEXLIST_PAGE];
	int want = 0, have = 0;
	pthread_mutex_lock(&l->lock);
	int first = page * YANDEXLIST_PAGE;
	for (int i = first; i < l->count && i < first + YANDEXLIST_PAGE; i++) {
		const page_t *p = page < l->page_slots ? l->pages[page] : NULL;
		if (p && p->d[i - first].text) {
			have++;
			continue;
		}
		char id[YANDEX_ID_MAX];
		ref_track_id(l, &l->refs[i], id, sizeof(id));
		if (l->refs[i].album) {
			snprintf(refs[want], sizeof(refs[0]), "%s:%u", id, (unsigned)l->refs[i].album);
		} else {
			snprintf(refs[want], sizeof(refs[0]), "%s", id);
		}
		ptrs[want] = refs[want];
		index_of[want] = i;
		want++;
	}
	pthread_mutex_unlock(&l->lock);
	if (want == 0) {
		return have;
	}

	yandex_track_t *got = calloc((size_t)want, sizeof(*got));
	if (!got) {
		return -1;
	}
	int n = yandex_tracks(ptrs, want, got, want);
	if (n < 0) {
		free(got);
		return -1;
	}
	pthread_mutex_lock(&l->lock);
	// The answer is matched by id, not by position: a track the service no
	// longer has is left out of it. Those get an empty record marked not
	// available, so the row says so and the page is not asked for again.
	for (int k = 0; k < want; k++) {
		int index = index_of[k];
		if (index >= l->count) {
			continue;
		}
		char id[YANDEX_ID_MAX];
		ref_track_id(l, &l->refs[index], id, sizeof(id));
		const yandex_track_t *match = NULL;
		for (int j = 0; j < n && !match; j++) {
			if (strcmp(got[j].id, id) == 0) {
				match = &got[j];
			}
		}
		yandex_track_t gone;
		if (!match) {
			memset(&gone, 0, sizeof(gone));
			snprintf(gone.id, sizeof(gone.id), "%s", id);
			gone.available = false;
			match = &gone;
		}
		set_detail_locked(l, index, match);
		have++;
	}
	pthread_mutex_unlock(&l->lock);
	free(got);
	return have;
}

size_t yandexlist_memory(yandexlist_t *l) {
	if (!l) {
		return 0;
	}
	pthread_mutex_lock(&l->lock);
	size_t bytes = sizeof(*l) + (size_t)l->room * sizeof(ref_t) + (size_t)l->extra_room * YANDEX_ID_MAX;
	bytes += (size_t)l->page_slots * sizeof(page_t *) + (size_t)l->pages_loaded * sizeof(page_t) + l->text_bytes;
	pthread_mutex_unlock(&l->lock);
	return bytes;
}

#include "rowart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/shell/sx_palette.h"
#include "src/gui/shell/sx_tokens.h"
#include "src/system/library/covertone.h"
#include "src/system/library/library.h"

// A grey sleeve's chroma: below this a hue is not seen, and the plaque stays
// grey instead of being painted a colour the cover does not have.
#define GREY_CHROMA 0.04f

static lv_color_t plaque(float hue, float chroma) {
	float c = chroma < GREY_CHROMA ? chroma : SX_T_COVER_TONE.C;
	return sx_oklch(SX_T_COVER_TONE.L, c, hue);
}

// The plaque of an album as covertone knows it. An album with no picture at
// all is kept with its title's hash for a hue and nothing measured -- chroma
// and lightness both 0, which no real sleeve has, a grey one included -- and
// its plaque is that hue at the token's strength (the handoff: no cover, the
// hue is the title's hash), not the grey of a grey sleeve. The screen's
// colours stay neutral for such a track all the same (sx_palette.c).
static lv_color_t plaque_known(float hue, float chroma, float light) {
	if (chroma <= 0.0f && light <= 0.0f) {
		return plaque(hue, SX_T_COVER_TONE.C);
	}
	return plaque(hue, chroma);
}

void rowart_bind_id(rowart_t *art, int64_t album_id, const char *title) {
	if (art->album_id == album_id && album_id != 0 && (art->tone_known || art->has_picture)) {
		return;
	}
	bool same = art->album_id == album_id;
	art->album_id = album_id;
	if (!same) {
		art->has_picture = false;
		art->asked = false;
		art->settled = false;
		art->revision = 0;
	}
	float h = 0.0f, c = 0.0f, l = 0.0f;
	if (album_id != 0 && covertone_for_album(album_id, &h, &c, &l)) {
		art->tone = plaque_known(h, c, l);
		art->tone_known = true;
	} else {
		art->tone = plaque((float)covertone_hash_hue(title ? title : ""), SX_T_COVER_TONE.C);
		art->tone_known = false;
	}
	if (album_id == 0) {
		art->settled = true;
	}
}

void rowart_bind(rowart_t *art, const char *album_value) {
	char title[256] = "";
	if (album_value && album_value[0]) {
		library_album_title(album_value, title, sizeof(title));
	}
	rowart_bind_id(art, album_value && album_value[0] ? covertone_album_id(album_value) : 0, title);
}

lv_color_t rowart_tone(const rowart_t *art) { return art->tone; }

const lv_image_dsc_t *rowart_picture(const rowart_t *art) { return art->has_picture ? &art->dsc : NULL; }

bool rowart_wanted(const rowart_t *art) {
	if (art->album_id == 0 || art->has_picture) {
		return false;
	}
	return !art->settled || art->revision != covertone_revision();
}

bool rowart_load(rowart_t *art) {
	if (!rowart_wanted(art)) {
		return false;
	}
	art->revision = covertone_revision();
	bool changed = false;
	// The tone may have landed since the row was bound.
	float h = 0.0f, c = 0.0f, l = 0.0f;
	if (!art->tone_known && covertone_for_album(art->album_id, &h, &c, &l)) {
		art->tone = plaque_known(h, c, l);
		art->tone_known = true;
		changed = true;
	}
	if (!art->buf) {
		art->buf = malloc(COVERTONE_THUMB_BYTES(COVERTONE_THUMB_SMALL));
		if (!art->buf) {
			art->settled = true;
			return changed;
		}
	}
	if (covertone_thumb(art->album_id, COVERTONE_THUMB_SMALL, &art->dsc, art->buf,
						COVERTONE_THUMB_BYTES(COVERTONE_THUMB_SMALL))) {
		art->has_picture = true;
		art->settled = true;
		return true;
	}
	// Not made yet, or no picture at all: asked for once, and tried again
	// when covertone has news (the revision moves).
	if (!art->asked) {
		covertone_request(art->album_id);
		art->asked = true;
	}
	art->settled = true;
	return changed;
}

void rowart_free(rowart_t *art) {
	free(art->buf);
	memset(art, 0, sizeof(*art));
}

// ---------------------------------------------------------------------------
// The track -> album cache
// ---------------------------------------------------------------------------

#define TRACK_CACHE 64

static struct {
	uint64_t key;
	unsigned age;
	bool found;
	char album[256];
} track_cache[TRACK_CACHE];
static unsigned track_clock;
static unsigned track_cache_revision;

static uint64_t path_key(const char *path) {
	uint64_t h = 1469598103934665603ULL; // FNV-1a
	for (const unsigned char *s = (const unsigned char *)path; *s; s++) {
		h ^= *s;
		h *= 1099511628211ULL;
	}
	return h ? h : 1;
}

bool rowart_album_of_track(const char *path, char *out, size_t size) {
	if (size > 0) {
		out[0] = '\0';
	}
	if (!path || !path[0]) {
		return false;
	}
	// A rescan can file a track under another album: what was remembered
	// before it is forgotten.
	unsigned rev = library_revision(LIBRARY_LIST_TRACKS);
	if (rev != track_cache_revision) {
		memset(track_cache, 0, sizeof(track_cache));
		track_cache_revision = rev;
	}
	uint64_t key = path_key(path);
	int oldest = 0;
	for (int i = 0; i < TRACK_CACHE; i++) {
		if (track_cache[i].key == key) {
			track_cache[i].age = ++track_clock;
			snprintf(out, size, "%s", track_cache[i].album);
			return track_cache[i].found;
		}
		if (track_cache[i].age < track_cache[oldest].age) {
			oldest = i;
		}
	}
	char album[256] = "";
	bool found = library_track_album_value(path, album, sizeof(album));
	track_cache[oldest].key = key;
	track_cache[oldest].age = ++track_clock;
	track_cache[oldest].found = found;
	snprintf(track_cache[oldest].album, sizeof(track_cache[oldest].album), "%s", found ? album : "");
	snprintf(out, size, "%s", found ? album : "");
	return found;
}

bool rowart_list_busy(lv_obj_t *list) {
	if (!list) {
		return false;
	}
	for (lv_indev_t *indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev)) {
		if (lv_indev_get_scroll_obj(indev) != NULL) {
			return true;
		}
	}
	return lv_obj_is_scrolling(list);
}

#ifndef ROWART_H
#define ROWART_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

// ---------------------------------------------------------------------------
// A list row's cover, the UI 2.0 way (handoff, "Обложки: пайплайн")
//
// A row is drawn at once with a plaque in its album's tone -- known from
// memory (covertone_for_album), or a hash of the album's title until the
// pipeline has looked at it -- and the 56 px thumbnail takes the plaque's place
// once it is read: one 6 KB read off the card (covertone_thumb), done on the UI
// thread for the rows on screen only, a few per pass, and never while the list
// is moving. A thumbnail not made yet is asked for (covertone_request) and
// looked for again when covertone says something landed.
//
// What a row keeps: the album's id, its tone and, from its first picture on, a
// buffer of its own for the pixels (COVERTONE_THUMB_BYTES(56)), reused for
// every album the row shows after. A pool of a dozen rows therefore costs
// 75 KB at most, whatever the size of the library, and nothing for rows that
// never showed a picture.
//
// A track row knows its file, not its album: rowart_album_of_track() asks the
// index once and remembers the last 64 answers, so scrolling up and down a
// list does not ask again.
//
// UI thread only.
// ---------------------------------------------------------------------------

typedef struct {
	int64_t album_id; // 0: no album (the plaque in the neutral cover tone)
	lv_color_t tone;
	bool tone_known;	// from the pipeline, not from the hash
	bool has_picture;	// `dsc` shows this album's thumbnail
	bool asked;			// covertone_request() made for it
	bool settled;		// nothing more to try until covertone says something landed
	uint32_t revision;	// covertone_revision() when it last tried
	lv_image_dsc_t dsc;
	uint8_t *buf;
} rowart_t;

// The row now stands for the album `album_value` (an Albums row's name, see
// library.h; NULL or "" for none). Keeps the picture when it is the same album.
void rowart_bind(rowart_t *art, const char *album_value);

// The same for an id worked out elsewhere, with the title to hash for a tone.
void rowart_bind_id(rowart_t *art, int64_t album_id, const char *title);

// The plaque's colour.
lv_color_t rowart_tone(const rowart_t *art);

// The picture, or NULL while there is none.
const lv_image_dsc_t *rowart_picture(const rowart_t *art);

// One attempt at the picture (a card read). True when the row has a picture
// now that it did not have before -- or a better tone -- and should be drawn
// again.
bool rowart_load(rowart_t *art);

// Whether a load is worth trying now: no picture yet, and not given up since
// covertone last had news.
bool rowart_wanted(const rowart_t *art);

void rowart_free(rowart_t *art);

// The album value a track belongs to, into `out`; false when the index does
// not know the file. Cached.
bool rowart_album_of_track(const char *path, char *out, size_t size);

// Whether `list` is being dragged or still moving: the time to leave the card
// alone.
bool rowart_list_busy(lv_obj_t *list);

#endif /* ROWART_H */

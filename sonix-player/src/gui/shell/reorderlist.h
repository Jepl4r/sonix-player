#ifndef REORDERLIST_H
#define REORDERLIST_H

#include <stdbool.h>

#include "lvgl/lvgl.h"

// Lists of rows arranged by dragging them, on one canvas: the shape of the
// control centre page, the Music page's tiles and the home screen's. Each list
// has a heading, and each row carries a picture, a name and a grip.
//
// A row is taken by its grip and nowhere else, wears the accent border while it
// is carried, and an accent rule shows where it would land. All the lists live
// on one canvas, so a row crossing from one to another is one coordinate and
// not a change of parent. The page scrolls by itself while a row is held near
// its top or bottom edge.
//
// The rows are only a view: the page that owns the items says how many each
// list has, what each one is, and does the moving when a row is dropped.

#define REORDERLIST_MAX_LISTS 3

typedef struct {
	int lists;
	const char *headings[REORDERLIST_MAX_LISTS]; // translation tags
	// Shown as "n/limit" level with the list's heading; 0 for no count.
	int limits[REORDERLIST_MAX_LISTS];
	// Every item of every list together: how many rows are built.
	int max_rows;
	// Tile artwork, at half size in its own colours, rather than an interface
	// glyph in the theme's.
	bool artwork;

	int (*count)(int list);
	const lv_image_dsc_t *(*icon)(int list, int position);
	const char *(*name)(int list, int position); // a translation tag
	// The row at `from` in `from_list` dropped into the gap `to` in `to_list`,
	// already counted with the row lifted out. Never called for a drop back
	// into its own place.
	void (*move)(int from_list, int from, int to_list, int to);
	// After every repaint, for a page with more on it than the lists. May be
	// NULL.
	void (*refreshed)(void);
} reorderlist_def_t;

// Builds the canvas at the top of `page`, the scrolling column of a
// settingsrow_page(), `width` wide. `def` is copied. The lists repaint whenever
// the screen is shown and after every drop.
lv_obj_t *reorderlist_create(lv_obj_t *page, int width, const reorderlist_def_t *def);

void reorderlist_refresh(lv_obj_t *list);

#endif /* REORDERLIST_H */

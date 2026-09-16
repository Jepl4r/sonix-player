#include "ebookpage.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "src/system/ebook/ebook.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/ebook/ebookcovers.h"
#include "src/gui/ebook/ebookmarkspage.h"
#include "src/gui/ebook/ebookreader.h"
#include "src/gui/ebook/ebooksettings.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"

lv_obj_t *ebookpage_screen;

// ---------------------------------------------------------------------------
// The shelf
//
// Every .epub in the Ebook folder at the root of the card, drawn as its cover.
//
// A cover is inside the book, which means opening the book to see it -- the
// ZIP directory, container.xml and the OPF, for a picture. On a shelf of thirty
// books that is thirty of those, and doing it while the page is being built
// would be a second or two of black screen. So the tiles go up at once with the
// file's name on them, and a worker fills the pictures in behind: the same
// arrangement the album lists use, for the same reason.
//
// The worker opens one book at a time and closes it before the next, so the
// shelf costs one book's worth of arena however many books are on it.
// ---------------------------------------------------------------------------

#define SHELF_MAX 300
#define TILE_COLUMNS 2
#define TILE_GAP 14
#define COVER_RATIO_NUM 3 // covers are drawn 2:3, the shape of a paperback
#define COVER_RATIO_DEN 2

typedef struct {
	// Only the file name: the folder is the same for all of them, so keeping a
	// whole path per book would be three hundred copies of the same prefix.
	char file[256];
	char name[256];	 // the file name, until the book says what it is called
	lv_obj_t *tile;
	lv_obj_t *glyph; // the placeholder, recoloured at every theme switch
	lv_obj_t *image;
	lv_obj_t *label;
	cover_image_t cover;
	bool has_cover;
} shelf_entry_t;

static gui_config_t *g_cfg;
static lv_obj_t *grid;
static lv_obj_t *page_container; // the scroller: what "on screen" is measured against
static lv_obj_t *empty_note;

static shelf_entry_t *shelf;
static int shelf_count;
static int cover_w, cover_h;
static char shelf_dir[512];
static uint32_t covers_token;

static void entry_path(const shelf_entry_t *entry, char *out, size_t size) {
	snprintf(out, size, "%s/%s", shelf_dir, entry->file);
}

static void open_book_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index >= 0 && index < shelf_count) {
		char path[800];
		entry_path(&shelf[index], path, sizeof(path));
		ebookreader_open(path);
	}
}

// The covers arrive from ebookcovers.c, which runs them on a detached thread
// and hands each one over on the UI thread. Keeping that out of here is what
// lets the bookmarks page have the same behaviour without a second copy of it.
static bool cover_ready(int index, const char *title, const cover_image_t *cover) {
	if (!shelf || index < 0 || index >= shelf_count) {
		return false;
	}
	shelf_entry_t *entry = &shelf[index];
	if (title && title[0]) {
		snprintf(entry->name, sizeof(entry->name), "%s", title);
		if (entry->label) {
			lv_label_set_text(entry->label, entry->name);
		}
	}
	if (!cover || !entry->image) {
		return false;
	}
	// Kept: freed by shelf_free() with everything else.
	entry->cover = *cover;
	entry->has_cover = true;
	lv_image_set_src(entry->image, &entry->cover.dsc);
	lv_obj_remove_flag(entry->image, LV_OBJ_FLAG_HIDDEN);
	return true;
}

// One screen of margin above and below, so the next screenful is decoded before
// it is scrolled to and the reader never watches a cover appear.
#define COVER_LOOKAHEAD 720

static void want_visible_covers(void) {
	if (!shelf || !shelf_count || !page_container) {
		return;
	}
	// The tiles, as an array of objects, which is what ebookcovers wants: the
	// shelf keeps them inside a struct.
	lv_obj_t **tiles = calloc((size_t)shelf_count, sizeof(*tiles));
	if (!tiles) {
		return;
	}
	for (int i = 0; i < shelf_count; i++) {
		tiles[i] = shelf[i].tile;
	}
	ebookcovers_want_visible(covers_token, page_container, tiles, shelf_count, COVER_LOOKAHEAD);
	free(tiles);
}

static void scrolled_cb(lv_event_t *e) {
	(void)e;
	want_visible_covers();
}

static void worker_begin(void) {
	if (!shelf_count || !shelf) {
		return;
	}
	// One contiguous array of names is what ebookcovers wants, and the shelf
	// keeps its names inside a struct, so they are gathered here. It is one
	// allocation of a quarter of a kilobyte per book, freed before this returns.
	char(*files)[256] = calloc((size_t)shelf_count, sizeof(files[0]));
	if (!files) {
		return;
	}
	for (int i = 0; i < shelf_count; i++) {
		memcpy(files[i], shelf[i].file, sizeof(files[0]));
	}
	covers_token = ebookcovers_start(shelf_dir, (const char(*)[256])files, shelf_count, cover_w, cover_h, cover_ready);
	free(files);
	want_visible_covers();
}

// The recolour of an SVG glyph is a style set when the tile is built, which is
// a snapshot of the theme at that moment. The shelf is rebuilt on arrival, so
// this matters only for a theme changed while it is on screen -- from the
// control centre, which can be pulled down over any page.
static void shelf_theme_refresh(void) {
	for (int i = 0; i < shelf_count; i++) {
		if (shelf[i].glyph) {
			lv_obj_set_style_image_recolor(shelf[i].glyph, theme()->text_secondary, 0);
		}
	}
}

static void shelf_free(void) {
	// Told, not waited for: it stops within one book of here, in its own time.
	// Joining it would hold the page being left on screen and unresponsive.
	ebookcovers_stop(covers_token);
	if (grid) {
		lv_obj_clean(grid);
	}
	for (int i = 0; i < shelf_count; i++) {
		if (shelf[i].has_cover) {
			cover_free(&shelf[i].cover);
		}
	}
	shelf_count = 0;
	free(shelf);
	shelf = NULL;
}

static int compare_entries(const void *a, const void *b) {
	return strcasecmp(((const shelf_entry_t *)a)->name, ((const shelf_entry_t *)b)->name);
}

static void scan_folder(void) {
	snprintf(shelf_dir, sizeof(shelf_dir), "%s/Ebook", g_cfg->sd_root_path ? g_cfg->sd_root_path : "");

	DIR *d = opendir(shelf_dir);
	if (!d) {
		return;
	}
	// Allocated only for as long as the page is up: a shelf that is never
	// visited costs nothing, and one that is left gives the memory straight back.
	if (!shelf) {
		shelf = calloc(SHELF_MAX, sizeof(shelf_entry_t));
	}
	if (!shelf) {
		closedir(d);
		return;
	}
	struct dirent *de;
	while ((de = readdir(d)) != NULL && shelf_count < SHELF_MAX) {
		size_t len = strlen(de->d_name);
		if (len < 6 || len >= sizeof(shelf[0].file) || strcasecmp(de->d_name + len - 5, ".epub") != 0) {
			continue;
		}
		shelf_entry_t *entry = &shelf[shelf_count];
		memset(entry, 0, sizeof(*entry));
		memcpy(entry->file, de->d_name, len + 1);
		// The name without the extension, until the book is opened and says
		// what it is really called.
		snprintf(entry->name, sizeof(entry->name), "%.*s", (int)(len - 5), de->d_name);
		shelf_count++;
	}
	closedir(d);

	qsort(shelf, (size_t)shelf_count, sizeof(shelf_entry_t), compare_entries);
}

static void build_tiles(void) {
	int width = (int)g_cfg->screen_width - 2 * g_cfg->padding;
	cover_w = (width - (TILE_COLUMNS - 1) * TILE_GAP) / TILE_COLUMNS;
	cover_h = cover_w * COVER_RATIO_NUM / COVER_RATIO_DEN;

	for (int i = 0; i < shelf_count; i++) {
		shelf_entry_t *entry = &shelf[i];

		lv_obj_t *tile = lv_obj_create(grid);
		lv_obj_remove_style_all(tile);
		lv_obj_set_size(tile, cover_w, cover_h + 46);
		lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
		// The press has to reach the page under the tile as well as the tile:
		// a clickable object swallows it by default, and the swipe that goes
		// back is watched on the page. open_book_cb() refuses to open a book at
		// the end of a drag, so the two do not fight.
		lv_obj_add_flag(tile, LV_OBJ_FLAG_EVENT_BUBBLE);
		lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_style_pad_row(tile, 6, 0);
		lv_obj_add_event_cb(tile, open_book_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

		// What is there before the cover arrives, and what stays when the book
		// has none: a card in the surface colour with a book glyph on it. A
		// blank hole would read as a shelf that failed to load.
		lv_obj_t *plate = lv_obj_create(tile);
		lv_obj_remove_style_all(plate);
		lv_obj_set_size(plate, cover_w, cover_h);
		lv_obj_add_style(plate, &theme_style_card, 0);
		lv_obj_set_style_radius(plate, 8, 0);
		lv_obj_set_style_border_width(plate, 0, 0);
		lv_obj_set_style_shadow_width(plate, 0, 0);
		lv_obj_remove_flag(plate, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_add_flag(plate, LV_OBJ_FLAG_EVENT_BUBBLE);

		lv_obj_t *glyph = lv_image_create(plate);
		lv_image_set_src(glyph, &icon_ebook_cover);
		lv_obj_center(glyph);
		// Stored white, like every glyph drawn from an SVG, so it has to be
		// recoloured or it is a white shape on a white card. Faint on purpose:
		// it is what is there instead of a cover, not something to look at.
		lv_obj_set_style_image_recolor(glyph, theme()->text_secondary, 0);
		lv_obj_set_style_image_recolor_opa(glyph, LV_OPA_COVER, 0);
		lv_obj_set_style_image_opa(glyph, LV_OPA_50, 0);
		entry->glyph = glyph;

		entry->image = lv_image_create(plate);
		lv_obj_center(entry->image);
		lv_obj_add_flag(entry->image, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(entry->image, LV_OBJ_FLAG_EVENT_BUBBLE);

		entry->label = lv_label_create(tile);
		lv_label_set_long_mode(entry->label, LV_LABEL_LONG_DOT);
		lv_obj_set_width(entry->label, cover_w);
		lv_label_set_text(entry->label, entry->name);
		lv_obj_add_style(entry->label, &theme_style_text, 0);
		lv_obj_set_style_text_font(entry->label, &font_ui_18, 0);
		lv_obj_set_style_text_align(entry->label, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_add_flag(entry->label, LV_OBJ_FLAG_EVENT_BUBBLE);

		entry->tile = tile;
	}
}

// The same shape the Music page's corner buttons have, so the two pages read
// as the same kind of page.
static lv_obj_t *corner_button(gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph) {
	lv_obj_t *button = lv_btn_create(ebookpage_screen);
	lv_obj_set_size(button, 56, 56);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -cfg->padding - slot * (56 + 6), cfg->padding + cfg->top_bar_height);

	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, glyph);
	// A style and not a recolour: a style follows the theme by itself.
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_center(icon);
	return button;
}

static void settings_cb(lv_event_t *e) {
	(void)e;
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	switch_screen(ebooksettings_screen);
}

static void bookmarks_cb(lv_event_t *e) {
	(void)e;
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	ebookmarkspage_open();
}

void ebookpage_open(void) { switch_screen(ebookpage_screen); }

// The shelf is built on arrival and not in ebookpage_open(), because arriving
// is not only what the More page's tile does: closing a book comes back here
// through the history, and a shelf built only on the way in would be an empty
// page every time a reader put a book down.
static void loaded_cb(lv_event_t *e) {
	(void)e;
	shelf_free();
	scan_folder();

	if (shelf_count) {
		lv_obj_add_flag(empty_note, LV_OBJ_FLAG_HIDDEN);
		build_tiles();
		worker_begin();
	} else {
		lv_obj_remove_flag(empty_note, LV_OBJ_FLAG_HIDDEN);
	}
}

// The shelf is not kept while the page is away: the covers are a megabyte or
// two of decoded pixels, and the page is rebuilt in a moment when it comes back.
static void unloaded_cb(lv_event_t *e) {
	(void)e;
	shelf_free();
}

void ebookpage_init(gui_config_t *cfg) {
	g_cfg = cfg;

	ebookpage_screen = lv_obj_create(NULL);
	lv_obj_add_style(ebookpage_screen, &theme_style_screen, 0);

	lv_obj_t *container = settingsrow_page(ebookpage_screen, cfg, "books");
	page_container = container;
	// Both ends of a flick: SCROLL keeps up with a slow drag, SCROLL_END catches
	// where a fast one landed.
	lv_obj_add_event_cb(container, scrolled_cb, LV_EVENT_SCROLL, NULL);
	lv_obj_add_event_cb(container, scrolled_cb, LV_EVENT_SCROLL_END, NULL);
	// One button in the corner, so the heading has to be told to keep clear of
	// it -- the same arrangement the Music page has.
	settingsrow_title_corner_slots(settingsrow_page_title(ebookpage_screen), cfg, 2);

	lv_obj_t *settings_btn = corner_button(cfg, 0, &icon_music_settings);
	lv_obj_add_event_cb(settings_btn, settings_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *marks_btn = lv_btn_create(ebookpage_screen);
	lv_obj_set_size(marks_btn, 56, 56);
	lv_obj_set_style_bg_opa(marks_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(marks_btn, 0, 0);
	lv_obj_set_style_shadow_width(marks_btn, 0, 0);
	lv_obj_set_style_pad_all(marks_btn, 0, 0);
	lv_obj_align(marks_btn, LV_ALIGN_TOP_RIGHT, -cfg->padding - (56 + 6), cfg->padding + cfg->top_bar_height);
	lv_obj_add_event_cb(marks_btn, bookmarks_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *marks_icon = lv_image_create(marks_btn);
	lv_image_set_src(marks_icon, &icon_bookmark);
	// A style and not a recolour: a style follows the theme by itself.
	lv_obj_add_style(marks_icon, &theme_style_icon, 0);
	lv_obj_center(marks_icon);
	lv_obj_set_style_pad_row(container, TILE_GAP, 0);

	grid = lv_obj_create(container);
	lv_obj_remove_style_all(grid);
	lv_obj_set_size(grid, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_column(grid, TILE_GAP, 0);
	lv_obj_set_style_pad_row(grid, TILE_GAP, 0);
	lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
	// Bubbling is one step at a time: the tile passes the press to this, and
	// without this it stops here instead of reaching the container, which is
	// where the swipe that goes back is watched.
	lv_obj_add_flag(grid, LV_OBJ_FLAG_EVENT_BUBBLE);

	empty_note = lv_label_create(container);
	lv_label_set_long_mode(empty_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(empty_note, lv_pct(100));
	lv_obj_add_style(empty_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(empty_note, &font_ui_20, 0);
	lv_obj_set_style_text_align(empty_note, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_text(empty_note, tr("ebook_empty_note"));
	lv_obj_add_flag(empty_note, LV_OBJ_FLAG_HIDDEN);

	lv_obj_add_event_cb(ebookpage_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(ebookpage_screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(ebookpage_screen);
	// And on the container too: it is the surface that scrolls, and a press that
	// lands on it rather than on a tile has to start the same gesture.
	switcher_attach_back_gesture(container);
	player_sheet_attach_drag(container, true);
	theme_register_refresh(shelf_theme_refresh);
}

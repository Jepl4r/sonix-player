#include "homescreen.h"

#include <stdio.h>
#include <string.h>

#include "src/system/core/config.h"

// Kept in the config under [homescreen]: the three lists by the tiles' names
// ("home = music,wireless,dac,more,settings"), and a section's tiles by the
// section's name and their own ("streaming_tidal"), 1 or 0. A tile no list
// names goes to the end of the list it starts in, so one added by a later build
// turns up without anyone placing it.
#define CONFIG_SECTION "homescreen"

// The tiles with a place: everything on the main menu and the More page.
#define MAX_TILES 16

// The two sections with tiles of their own, and room for two more.
#define MAX_SECTIONS 4
// The most tiles a section has: a full two by three.
#define MAX_SECTION_TILES 6

// Never hidden: without it nothing could be put back. In More only while the
// More tile is on the main menu.
#define SETTINGS_TILE "settings"
// The door to the More page: on the main menu or hidden, never inside itself.
#define MORE_TILE "more"

typedef struct {
	grid_entry_t entry;
	homescreen_place_t start; // the list it is in until the user moves it
} tile_t;

static tile_t tiles[MAX_TILES];
static int tile_count;

typedef struct {
	lv_obj_t *grid;
	gui_config_t *cfg;
} page_t;

static page_t home;
static page_t more;

typedef struct {
	lv_obj_t *grid;
	gui_config_t *cfg;
	const char *section;
	grid_entry_t entries[MAX_SECTION_TILES];
	int count;
	int columns;
	int rows;
	bool clear_corner_buttons;
} section_t;

static section_t sections[MAX_SECTIONS];
static int section_count;

static const char *const LIST_KEYS[HOMESCREEN_PLACE_COUNT] = {
	[HOMESCREEN_HOME] = "home",
	[HOMESCREEN_MORE] = "more",
	[HOMESCREEN_HIDDEN] = "hidden",
};

// The three lists, as indexes into tiles[]. Read from the config the first
// time they are wanted after a page registers, since a list can only name the
// tiles already registered.
static int lists[HOMESCREEN_PLACE_COUNT][MAX_TILES];
static int list_n[HOMESCREEN_PLACE_COUNT];
static bool lists_loaded;

static bool is(const char *label, const char *name) { return label && strcmp(label, name) == 0; }

static int tile_index(const char *label, size_t len) {
	for (int i = 0; i < tile_count; i++) {
		if (strlen(tiles[i].entry.label) == len && strncmp(tiles[i].entry.label, label, len) == 0) {
			return i;
		}
	}
	return -1;
}

static int find_tile(const char *label) { return label ? tile_index(label, strlen(label)) : -1; }

// ---------------------------------------------------------------------------
// The lists
// ---------------------------------------------------------------------------

static int position_in(homescreen_place_t place, int tile) {
	for (int i = 0; i < list_n[place]; i++) {
		if (lists[place][i] == tile) {
			return i;
		}
	}
	return -1;
}

static int place_of(int tile) {
	for (int p = 0; p < HOMESCREEN_PLACE_COUNT; p++) {
		if (position_in((homescreen_place_t)p, tile) >= 0) {
			return p;
		}
	}
	return -1;
}

static void list_remove(int tile) {
	for (int p = 0; p < HOMESCREEN_PLACE_COUNT; p++) {
		int at = position_in((homescreen_place_t)p, tile);
		if (at >= 0) {
			memmove(&lists[p][at], &lists[p][at + 1], (size_t)(list_n[p] - at - 1) * sizeof(int));
			list_n[p]--;
		}
	}
}

static void list_insert(homescreen_place_t place, int tile, int at) {
	if (at < 0 || at > list_n[place]) {
		at = list_n[place];
	}
	memmove(&lists[place][at + 1], &lists[place][at], (size_t)(list_n[place] - at) * sizeof(int));
	lists[place][at] = tile;
	list_n[place]++;
}

// One saved list ("music,wireless,..."), skipping names this build does not
// know and any tile already placed.
static void parse_list(homescreen_place_t place, const char *text) {
	const char *at = text ? text : "";
	while (*at) {
		const char *end = strchr(at, ',');
		size_t len = end ? (size_t)(end - at) : strlen(at);
		int tile = tile_index(at, len);
		if (tile >= 0 && place_of(tile) < 0) {
			list_insert(place, tile, -1);
		}
		if (!end) {
			break;
		}
		at = end + 1;
	}
}

// Moves the last tile of `from` that may leave it to the front of `to`.
static bool overflow_down(homescreen_place_t from, homescreen_place_t to) {
	for (int i = list_n[from] - 1; i >= 0; i--) {
		int tile = lists[from][i];
		const char *label = tiles[tile].entry.label;
		if (is(label, SETTINGS_TILE) || is(label, MORE_TILE)) {
			continue;
		}
		list_remove(tile);
		list_insert(to, tile, 0);
		return true;
	}
	return false;
}

static void lists_load(void) {
	if (lists_loaded) {
		return;
	}
	lists_loaded = true;
	memset(list_n, 0, sizeof(list_n));
	for (int p = 0; p < HOMESCREEN_PLACE_COUNT; p++) {
		parse_list((homescreen_place_t)p, config_get(CONFIG_SECTION, LIST_KEYS[p], ""));
	}
	// Whatever no list names goes where it starts, at the end.
	for (int i = 0; i < tile_count; i++) {
		if (place_of(i) < 0) {
			list_insert(tiles[i].start, i, -1);
		}
	}
	// A config that says otherwise, edited by hand: Settings and the More tile
	// go back to the main menu rather than out of reach.
	int settings = find_tile(SETTINGS_TILE);
	int more_tile = find_tile(MORE_TILE);
	if (more_tile >= 0 && place_of(more_tile) == HOMESCREEN_MORE) {
		list_remove(more_tile);
		list_insert(HOMESCREEN_HOME, more_tile, -1);
	}
	if (settings >= 0 && (place_of(settings) == HOMESCREEN_HIDDEN ||
						  (place_of(settings) == HOMESCREEN_MORE && place_of(more_tile) != HOMESCREEN_HOME))) {
		list_remove(settings);
		list_insert(HOMESCREEN_HOME, settings, -1);
	}
	// Never more than a page holds: a later build's new tile, or the fixes
	// above, can take a list past six. The last tile that may move steps down,
	// from the main menu to More and from More to hidden, so neither grid ever
	// has to leave a tile off -- least of all Settings or the More tile.
	while (list_n[HOMESCREEN_HOME] > HOMESCREEN_SLOTS) {
		if (!overflow_down(HOMESCREEN_HOME, list_n[HOMESCREEN_MORE] < HOMESCREEN_SLOTS ? HOMESCREEN_MORE : HOMESCREEN_HIDDEN)) {
			break;
		}
	}
	while (list_n[HOMESCREEN_MORE] > HOMESCREEN_SLOTS) {
		if (!overflow_down(HOMESCREEN_MORE, HOMESCREEN_HIDDEN)) {
			break;
		}
	}
}

static void lists_save(void) {
	for (int p = 0; p < HOMESCREEN_PLACE_COUNT; p++) {
		char text[256] = "";
		size_t used = 0;
		for (int i = 0; i < list_n[p]; i++) {
			int wrote = snprintf(text + used, sizeof(text) - used, "%s%s", i ? "," : "", tiles[lists[p][i]].entry.label);
			if (wrote < 0 || (size_t)wrote >= sizeof(text) - used) {
				break;
			}
			used += (size_t)wrote;
		}
		config_set(CONFIG_SECTION, LIST_KEYS[p], text);
	}
	config_save();
}

// A page's grid from its list, which lists_load() keeps to six.
static void fill(page_t *page, homescreen_place_t place, bool clear_corner_buttons) {
	if (!page->grid) {
		return;
	}
	grid_entry_t shown[HOMESCREEN_SLOTS];
	int n = 0;
	for (int i = 0; i < list_n[place] && n < HOMESCREEN_SLOTS; i++) {
		shown[n++] = tiles[lists[place][i]].entry;
	}
	gridpage_set_entries(page->grid, page->cfg, shown, n, 2, 3, clear_corner_buttons);
}

static void fill_pages(void) {
	lists_load();
	fill(&home, HOMESCREEN_HOME, false);
	fill(&more, HOMESCREEN_MORE, true);
}

static void add_tiles(const grid_entry_t *entries, int count, homescreen_place_t start) {
	for (int i = 0; i < count && tile_count < MAX_TILES; i++) {
		tiles[tile_count].entry = entries[i];
		tiles[tile_count].start = start;
		tile_count++;
	}
	lists_loaded = false;
}

// Either page may be built first; both are filled again once the other's
// tiles are known.
void homescreen_register_home(lv_obj_t *grid, gui_config_t *cfg, const grid_entry_t *entries, int count) {
	home.grid = grid;
	home.cfg = cfg;
	add_tiles(entries, count, HOMESCREEN_HOME);
	fill_pages();
}

void homescreen_register_more(lv_obj_t *grid, gui_config_t *cfg, const grid_entry_t *entries, int count) {
	more.grid = grid;
	more.cfg = cfg;
	add_tiles(entries, count, HOMESCREEN_MORE);
	fill_pages();
}

int homescreen_count(homescreen_place_t place) {
	lists_load();
	return place < HOMESCREEN_PLACE_COUNT ? list_n[place] : 0;
}

const char *homescreen_at(homescreen_place_t place, int position) {
	lists_load();
	if (place >= HOMESCREEN_PLACE_COUNT || position < 0 || position >= list_n[place]) {
		return NULL;
	}
	return tiles[lists[place][position]].entry.label;
}

const lv_image_dsc_t *homescreen_icon(const char *label) {
	int tile = find_tile(label);
	return tile >= 0 ? tiles[tile].entry.icon : NULL;
}

homescreen_place_t homescreen_place(const char *label) {
	lists_load();
	int tile = find_tile(label);
	int place = tile >= 0 ? place_of(tile) : -1;
	return place >= 0 ? (homescreen_place_t)place : HOMESCREEN_HOME;
}

homescreen_move_t homescreen_move(const char *label, homescreen_place_t place, int position) {
	lists_load();
	int tile = find_tile(label);
	if (tile < 0 || place >= HOMESCREEN_PLACE_COUNT) {
		return HOMESCREEN_UNKNOWN;
	}
	homescreen_place_t from = (homescreen_place_t)place_of(tile);
	if (is(label, SETTINGS_TILE) && place == HOMESCREEN_HIDDEN) {
		return HOMESCREEN_SETTINGS_HIDDEN;
	}
	if (is(label, MORE_TILE) && place == HOMESCREEN_MORE) {
		return HOMESCREEN_MORE_IN_MORE;
	}
	if (is(label, SETTINGS_TILE) && place == HOMESCREEN_MORE && homescreen_place(MORE_TILE) != HOMESCREEN_HOME) {
		return HOMESCREEN_SETTINGS_UNREACHABLE;
	}
	if (is(label, MORE_TILE) && place != HOMESCREEN_HOME && homescreen_place(SETTINGS_TILE) == HOMESCREEN_MORE) {
		return HOMESCREEN_SETTINGS_UNREACHABLE;
	}
	if (place != from && place != HOMESCREEN_HIDDEN && list_n[place] >= HOMESCREEN_SLOTS) {
		return place == HOMESCREEN_HOME ? HOMESCREEN_HOME_FULL : HOMESCREEN_MORE_FULL;
	}
	list_remove(tile);
	list_insert(place, tile, position);
	lists_save();
	fill_pages();
	return HOMESCREEN_MOVED;
}

// ---------------------------------------------------------------------------
// A section's own tiles
// ---------------------------------------------------------------------------

static section_t *find_section(const char *section) {
	for (int i = 0; i < section_count; i++) {
		if (is(sections[i].section, section)) {
			return &sections[i];
		}
	}
	return NULL;
}

static void section_key(const char *section, const char *label, char *out, size_t size) {
	snprintf(out, size, "%s_%s", section, label);
}

bool homescreen_section_tile_visible(const char *section, const char *label) {
	char key[64];
	section_key(section, label, key, sizeof(key));
	return config_get_bool(CONFIG_SECTION, key, true);
}

static void fill_section(section_t *s) {
	grid_entry_t shown[MAX_SECTION_TILES];
	int n = 0;
	for (int i = 0; i < s->count; i++) {
		if (homescreen_section_tile_visible(s->section, s->entries[i].label)) {
			shown[n++] = s->entries[i];
		}
	}
	gridpage_set_entries(s->grid, s->cfg, shown, n, s->columns, s->rows, s->clear_corner_buttons);
}

void homescreen_register_section(lv_obj_t *grid, gui_config_t *cfg, const char *section, const grid_entry_t *entries,
								 int count, int columns, int rows, bool clear_corner_buttons) {
	if (!grid || section_count >= MAX_SECTIONS) {
		return;
	}
	section_t *s = &sections[section_count++];
	s->grid = grid;
	s->cfg = cfg;
	s->section = section;
	s->count = count < MAX_SECTION_TILES ? count : MAX_SECTION_TILES;
	memcpy(s->entries, entries, (size_t)s->count * sizeof(*entries));
	s->columns = columns;
	s->rows = rows;
	s->clear_corner_buttons = clear_corner_buttons;
	fill_section(s);
}

int homescreen_section_tile_count(const char *section) {
	section_t *s = find_section(section);
	return s ? s->count : 0;
}

const char *homescreen_section_tile_label(const char *section, int index) {
	section_t *s = find_section(section);
	return s && index >= 0 && index < s->count ? s->entries[index].label : NULL;
}

void homescreen_set_section_tile_visible(const char *section, const char *label, bool visible) {
	char key[64];
	section_key(section, label, key, sizeof(key));
	config_set_bool(CONFIG_SECTION, key, visible);
	config_save();

	section_t *s = find_section(section);
	if (s) {
		fill_section(s);
	}
}

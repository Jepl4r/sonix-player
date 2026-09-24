#include "radiopage.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/system/core/lang.h"
#include "src/system/streaming/radio.h"
#include "src/system/net/wifi.h"

lv_obj_t *radiopage_screen;
lv_obj_t *radiolist_screen;
lv_obj_t *radiosearch_screen;

// The list is windowed like every other long list here: a fixed pool of row
// widgets rides the scroll position. Two hundred stations is well past the
// point where building a widget per entry is felt on this hardware.
#define ROW_HEIGHT 100
#define ROW_GAP 8
#define ROW_PITCH (ROW_HEIGHT + ROW_GAP)
#define ROW_RADIUS 12
#define ROW_POOL 10

#define POLL_MS 300

// What the list page is showing.
typedef enum {
	LIST_TERMS,	   // languages, countries or genres
	LIST_STATIONS, // the stations under one of those, or a search's hits
	LIST_FAVOURITES,
	LIST_RECENT,
	LIST_CUSTOM, // the stations written down in radio.txt
} list_mode_t;

// The three that come from the card rather than from the directory: no request
// is in flight for them, and they are reloaded rather than polled.
#define LIST_IS_STORED(m) ((m) == LIST_FAVOURITES || (m) == LIST_RECENT || (m) == LIST_CUSTOM)

typedef struct {
	lv_obj_t *button;
	lv_obj_t *name;
	lv_obj_t *chevron;
	lv_obj_t *menu_btn; // the ellipsis, on the favourites list only
	lv_obj_t *playmark; // the accent bar on the station that is loaded, radio.txt only
	int index;
} row_t;

// The mark on the loaded station, as the track lists draw it on the track that
// is playing (medialist.c).
#define PLAYMARK_WIDTH 6
#define PLAYMARK_HEIGHT 52
#define PLAYMARK_INSET 4 // from the row's left edge
#define ROW_PAD_HOR 14

// The radio.txt line the rows were last marked for, -1 for none.
static int marked_custom = -1;

static gui_config_t *g_cfg;

// --- the hub ---
static lv_obj_t *hub_container;
static lv_obj_t *hub_wifi_warning;
static lv_obj_t *hub_wifi_row;
static lv_obj_t *hub_browse_rows[3];
static lv_obj_t *hub_favourites_btn;
static lv_obj_t *hub_recent_btn;
static lv_obj_t *hub_search_btn;
static lv_obj_t *hub_custom_btn;

// --- the search page ---
static lv_obj_t *search_field;
static keyboard_t *search_keyboard;
static lv_timer_t *hub_timer;

// --- the list ---
static lv_obj_t *list_view;
static lv_obj_t *list_body;
static lv_obj_t *list_message; // "Loading...", "Nothing here", the error
static lv_obj_t *list_title;
static row_t rows[ROW_POOL];
static lv_timer_t *list_timer;

static list_mode_t list_mode;
static radio_browse_t list_browse;

// The back step inside the page. The categories (languages, countries, genres)
// and their stations live on the same screen: entering a genre fills the list
// with stations without changing screen, so as far as the global history is
// concerned nothing happened, so the chevron would leave for the radio's main
// page instead of returning to the genre list. Same arrangement as the Qobuz,
// Tidal and podcast lists: a guard that consumes the step inside the page, plus
// its peek for the swipe.
static bool stations_from_terms;	 // the listed stations came from a category
static char terms_title[96];		 // the category list's title, to return to it
static bool list_back_has_step(void);
static bool list_back_guard(void);
static unsigned last_job_serial;

static radio_term_t terms[300];
static int term_count;

// Four pages held at once. A country can hold thousands of stations, so the
// list is paged rather than capped: the next hundred are asked for when the
// user scrolls near the bottom. Four hundred rows is not a ceiling anyone
// reaches by dragging a finger, and it is under half a megabyte.
#define RADIO_MAX_HELD (4 * RADIO_PAGE_SIZE)
static radio_station_t stations[RADIO_MAX_HELD];
static int station_count;

// What the current station list is, so the next page can be asked for.
static char page_value[128];	  // the term, or the search text
static bool page_is_search;
static int page_offset;			  // where the next page starts, in the server's terms
static bool page_more;			  // the last page came back full: there may be more
static bool page_loading;		  // a page is in flight
static bool page_append;		  // ...and it is a continuation, not a fresh list

static void list_window_update(void);
static void list_reload_stored(void);
static const char *stored_empty_message(void);

// ---------------------------------------------------------------------------

static bool wifi_is_connected(void) {
#ifdef HOST_BUILD
	// The simulator runs on a machine with a network and no wlan0 to ask
	// about, so the gate would be permanently shut and the page untestable.
	return true;
#else
	wifi_status_t status;
	wifi_get_status(&status);
	return status.state == WIFI_STATE_CONNECTED && status.ip[0] != '\0';
#endif
}

static void hide(lv_obj_t *o) {
	if (o) {
		lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
	}
}
static void show(lv_obj_t *o) {
	if (o) {
		lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
	}
}

// ---------------------------------------------------------------------------
// the list page
// ---------------------------------------------------------------------------

static int list_count(void) {
	switch (list_mode) {
	case LIST_TERMS:
		return term_count;
	case LIST_STATIONS:
	case LIST_FAVOURITES:
	default:
		return station_count;
	}
}

static void set_message(const char *text) {
	if (!list_message) {
		return;
	}
	if (text && text[0]) {
		lv_label_set_text(list_message, tr(text));
		show(list_message);
	} else {
		hide(list_message);
	}
}

static void row_update_playmark(row_t *row) {
	if (list_mode == LIST_CUSTOM && row->index >= 0 && row->index == marked_custom) {
		show(row->playmark);
	} else {
		hide(row->playmark);
	}
}

static void row_bind(row_t *row, int index) {
	row->index = index;
	row_update_playmark(row);

	if (index < 0 || index >= list_count()) {
		hide(row->button);
		return;
	}

	show(row->button);
	lv_obj_set_y(row->button, index * ROW_PITCH);

	if (list_mode == LIST_TERMS) {
		lv_label_set_text(row->name, terms[index].label);
		show(row->chevron);
		hide(row->menu_btn);
		return;
	}

	const radio_station_t *s = &stations[index];
	lv_label_set_text(row->name, s->name);

	// A station that cannot be played is drawn faint: still there, still
	// tappable (it says why), but plainly not one of the ones that will. Since
	// the row carries the name alone, the fading is the only sign -- the reason
	// in full comes from the popup on tap.
	bool unplayable = radio_station_problem(s) != NULL;
	lv_obj_set_style_opa(row->button, unplayable ? LV_OPA_50 : LV_OPA_COVER, 0);

	hide(row->chevron);

	// The ellipsis is only on the favourites: it is the one list where a row
	// can be taken away, and a menu with nothing in it on the others would be
	// a button that does nothing.
	if (list_mode == LIST_FAVOURITES) {
		show(row->menu_btn);
	} else {
		hide(row->menu_btn);
	}
}

// `keep_scroll` is what tells a page appended to the bottom from a list
// replaced outright: jumping back to the top after loading more would undo
// the scroll that asked for it.
static void list_rebuild_keep(bool keep_scroll) {
	int count = list_count();

	lv_obj_set_height(list_body, count > 0 ? count * ROW_PITCH : ROW_PITCH);
	if (!keep_scroll) {
		lv_obj_scroll_to_y(list_view, 0, LV_ANIM_OFF);
	}

	for (int i = 0; i < ROW_POOL; i++) {
		rows[i].index = -2; // force every row to rebind
	}
	list_window_update();
}

static void list_rebuild(void) { list_rebuild_keep(false); }

static void list_window_update(void) {
	int count = list_count();
	if (count <= 0) {
		for (int i = 0; i < ROW_POOL; i++) {
			row_bind(&rows[i], -1);
		}
		return;
	}

	int scroll = lv_obj_get_scroll_y(list_view);
	if (scroll < 0) {
		scroll = 0;
	}

	int first = (scroll / ROW_PITCH) - 1;
	if (first + ROW_POOL > count) {
		first = count - ROW_POOL;
	}
	if (first < 0) {
		first = 0;
	}

	for (int i = 0; i < ROW_POOL; i++) {
		int index = first + i;
		row_bind(&rows[index % ROW_POOL], index < count ? index : -1);
	}
}

// Asks for the next hundred. Called when the scroll comes within a screenful
// of the bottom, and does nothing when there is nothing more to ask for.
static void list_load_more(void) {
	if (page_loading || !page_more || list_mode != LIST_STATIONS) {
		return;
	}
	if (station_count >= RADIO_MAX_HELD) {
		page_more = false;
		return;
	}

	page_loading = true;
	page_append = true;
	last_job_serial = radio_job_serial();

	// page_offset, not station_count: the two drift apart as soon as one
	// station is dropped for being https-only, and asking from the wrong place
	// means rows repeated or skipped.
	if (page_is_search) {
		radio_request_search(page_value, page_offset);
	} else {
		radio_request_stations(list_browse, page_value, page_offset);
	}
}

static void list_scroll_cb(lv_event_t *e) {
	(void)e;

	// A sideways drag that is pulling the player in moves this list too, and
	// acting on that movement would ask the directory for another page every
	// time the user reached for the player. Same guard the other list pages
	// use.
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}

	list_window_update();

	// Within a screenful of the end is close enough: the request takes a
	// moment, and arriving at the last row to find the list simply stops is
	// exactly what this replaces.
	int scroll = lv_obj_get_scroll_y(list_view);
	int viewport = lv_obj_get_height(list_view);
	if (scroll + 2 * viewport >= station_count * ROW_PITCH) {
		list_load_more();
	}
}

static void row_clicked_cb(lv_event_t *e) {
	// The release at the end of a sideways drag lands on whatever row the
	// finger happens to be over, so without this pulling the player in would
	// start that station. Same guard every other list page carries.
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}

	lv_obj_t *button = lv_event_get_current_target(e);

	int index = -1;
	for (int i = 0; i < ROW_POOL; i++) {
		if (rows[i].button == button) {
			index = rows[i].index;
			break;
		}
	}
	if (index < 0 || index >= list_count()) {
		return;
	}

	if (list_mode == LIST_TERMS) {
		const char *value = terms[index].name;
		snprintf(terms_title, sizeof(terms_title), "%s", lv_label_get_text(list_title));
		lv_label_set_text(list_title, terms[index].label);
		stations_from_terms = true;

		list_mode = LIST_STATIONS;
		station_count = 0;
		term_count = 0;
		page_is_search = false;
		page_offset = 0;
		page_more = false;
		page_append = false;
		page_loading = true;
		snprintf(page_value, sizeof(page_value), "%s", value);
		list_rebuild();
		set_message("loading");

		last_job_serial = radio_job_serial();
		radio_request_stations(list_browse, page_value, 0);
		return;
	}

	// A station that cannot be played says why, here, instead of being started
	// and failing silently.
	const char *problem = radio_station_problem(&stations[index]);
	if (problem) {
		gui_notify_popup(problem);
		return;
	}

	// Otherwise start it and go to the player: connecting takes a moment, and
	// the player is where that moment is explained.
	if (radio_play(&stations[index])) {
		player_refresh_now_playing();
		player_sheet_open(true);
	} else {
		gui_notify_popup("radio_cannot_start_the_station");
	}
}

// ---------------------------------------------------------------------------
// Removing a favourite
//
// The uuid rather than the row index: the confirmation runs after the card has
// closed, and by then the list may have been rebuilt underneath it.
// ---------------------------------------------------------------------------

static char menu_uuid[40];
static char menu_name[RADIO_NAME_MAX];

static void remove_confirmed(void *user) {
	(void)user;
	if (!menu_uuid[0]) {
		return;
	}

	radio_fav_remove(menu_uuid);
	menu_uuid[0] = '\0';

	if (list_mode == LIST_FAVOURITES) {
		list_reload_stored();
		list_rebuild();
		set_message(station_count ? NULL : stored_empty_message());
	}

	// The player's own star is about the same station.
	player_refresh_now_playing();
}

static void menu_remove_action(void *user) {
	(void)user;
	if (!menu_uuid[0]) {
		return;
	}
	confirm_show("remove_from_favourites", menu_name, "remove", remove_confirmed, NULL);
}

static void menu_btn_cb(lv_event_t *e) {
	lv_event_stop_bubbling(e);

	lv_obj_t *button = lv_event_get_current_target(e);
	int index = -1;
	for (int i = 0; i < ROW_POOL; i++) {
		if (rows[i].menu_btn == button) {
			index = rows[i].index;
			break;
		}
	}
	if (index < 0 || index >= station_count) {
		return;
	}

	snprintf(menu_uuid, sizeof(menu_uuid), "%s", stations[index].uuid);
	snprintf(menu_name, sizeof(menu_name), "%s", stations[index].name);

	static const popover_item_t items[] = {
		{"remove_from_favourites", menu_remove_action, NULL},
	};
	popover_show(button, items, 1);
}

// The two lists that come out of the database rather than off the network.
static void list_reload_stored(void) {
	term_count = 0;
	station_count = 0;

	int max = (int)(sizeof(stations) / sizeof(stations[0]));
	int n;
	switch (list_mode) {
	case LIST_RECENT:
		n = radio_recent_count();
		break;
	case LIST_CUSTOM:
		n = radio_custom_count();
		break;
	default:
		n = radio_fav_count();
		break;
	}
	if (n > max) {
		n = max;
	}

	for (int i = 0; i < n; i++) {
		bool got;
		switch (list_mode) {
		case LIST_RECENT:
			got = radio_recent_get(i, &stations[station_count]);
			break;
		case LIST_CUSTOM:
			got = radio_custom_get(i, &stations[station_count]);
			break;
		default:
			got = radio_fav_get(i, &stations[station_count]);
			break;
		}
		if (got) {
			station_count++;
		}
	}
	if (list_mode == LIST_CUSTOM) {
		marked_custom = radio_custom_current_index();
	}
}

// What an empty stored list should say.
//
// The two tags live in an array rather than in the return statement so that
// tools/extract_strings.py can find them (its TABLES list). They reach tr()
// through set_message(), which is a sink the extractor discovers on its own --
// but it discovers it by reading the call sites, and what this function returns
// is not something a call site can be read for. A tag only named here would end
// up in no language file and be printed raw.
static const char *const STORED_EMPTY[] = {"radio_no_favourite_stations", "radio_recent_empty", "radio_custom_empty",
										   "radio_custom_no_file"};

static const char *stored_empty_message(void) {
	if (list_mode == LIST_RECENT) {
		return STORED_EMPTY[1];
	}
	if (list_mode == LIST_CUSTOM) {
		// An empty list and a missing file are different problems and want
		// different sentences: one says the lines could not be read, the other
		// says where to put the file.
		return STORED_EMPTY[radio_custom_file_present() ? 2 : 3];
	}
	return STORED_EMPTY[0];
}

// Watches the worker while a request is in flight.
static void list_poll_cb(lv_timer_t *timer) {
	(void)timer;

	if (lv_screen_active() != radiolist_screen) {
		return;
	}

	// radio.txt: the mark follows the station, which changes from the player
	// and the side keys as well as from here.
	if (list_mode == LIST_CUSTOM) {
		int now = radio_custom_current_index();
		if (now != marked_custom) {
			marked_custom = now;
			for (int i = 0; i < ROW_POOL; i++) {
				row_update_playmark(&rows[i]);
			}
		}
	}

	if (LIST_IS_STORED(list_mode)) {
		return;
	}

	unsigned serial = radio_job_serial();
	if (serial == last_job_serial) {
		return;
	}
	last_job_serial = serial;

	bool appending = page_append;
	page_append = false;
	page_loading = false;

	if (radio_job_state() != RADIO_JOB_OK) {
		if (!appending) {
			set_message("radio_directory_unreachable");
		}
		page_more = false;
		return;
	}

	if (list_mode == LIST_TERMS) {
		term_count = radio_get_terms(terms, (int)(sizeof(terms) / sizeof(terms[0])));
		station_count = 0;
		page_more = false;
		list_rebuild();
		set_message(term_count ? NULL : "nothing_to_show");
		return;
	}

	int room = RADIO_MAX_HELD - (appending ? station_count : 0);
	if (room > RADIO_PAGE_SIZE) {
		room = RADIO_PAGE_SIZE;
	}

	int got = 0;
	if (room > 0) {
		got = radio_get_stations(appending ? &stations[station_count] : stations, room);
	}

	// Whether there is more is about what the server sent, not what survived
	// the filter: a full page with one https-only station in it comes back as
	// ninety-nine, and reading that as "the end" would stop the list there.
	int raw = radio_last_raw_count();
	page_offset = (appending ? page_offset : 0) + raw;
	page_more = (raw >= RADIO_PAGE_SIZE) && (station_count + got < RADIO_MAX_HELD);

	station_count = appending ? station_count + got : got;
	term_count = 0;

	list_rebuild_keep(appending);
	set_message(station_count ? NULL : "radio_no_stations_found");
}

static void list_loaded_cb(lv_event_t *e) {
	(void)e;
	// Both stored lists can change from the player while this page is closed:
	// the star adds and removes favourites, and every station played lands in
	// the recents.
	if (LIST_IS_STORED(list_mode)) {
		list_reload_stored();
		list_rebuild();
		set_message(station_count ? NULL : stored_empty_message());
	} else {
		list_window_update();
	}
}

static void build_list_page(gui_config_t *cfg) {
	lv_obj_add_style(radiolist_screen, &theme_style_screen, 0);
	list_title = settingsrow_title(radiolist_screen, cfg, "radio");

	int content_top = settingsrow_content_top(cfg);
	int width = cfg->screen_width - 2 * cfg->padding;

	list_view = lv_obj_create(radiolist_screen);
	lv_obj_set_size(list_view, cfg->screen_width, cfg->screen_height - content_top);
	lv_obj_align(list_view, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(list_view, 0, 0);
	lv_obj_set_style_border_width(list_view, 0, 0);
	lv_obj_set_style_radius(list_view, 0, 0);
	lv_obj_set_style_pad_hor(list_view, cfg->padding, 0);
	lv_obj_set_style_pad_ver(list_view, 0, 0);
	lv_obj_set_scroll_dir(list_view, LV_DIR_VER);
	lv_obj_add_event_cb(list_view, list_scroll_cb, LV_EVENT_SCROLL, NULL);

	// The list is a page surface like any other: dragging left on it pulls the
	// player in. Attached to the list and not only to the screen behind it,
	// because the list covers the screen and would otherwise eat the drag.
	player_sheet_attach_drag(list_view, true);

	list_body = lv_obj_create(list_view);
	lv_obj_set_width(list_body, width);
	lv_obj_set_height(list_body, ROW_PITCH);
	lv_obj_set_pos(list_body, 0, 0);
	lv_obj_set_style_bg_opa(list_body, 0, 0);
	lv_obj_set_style_border_width(list_body, 0, 0);
	lv_obj_set_style_pad_all(list_body, 0, 0);
	lv_obj_remove_flag(list_body, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(list_body, LV_OBJ_FLAG_EVENT_BUBBLE);

	list_message = lv_label_create(list_view);
	lv_label_set_text(list_message, "");
	lv_obj_set_style_text_align(list_message, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(list_message, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(list_message, &font_ui_24, 0);
	// Wrapped and inside the margins: a message long enough to explain where to
	// put a file runs off both edges of the screen otherwise.
	lv_label_set_long_mode(list_message, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(list_message, cfg->screen_width - 4 * cfg->padding);
	lv_obj_align(list_message, LV_ALIGN_TOP_MID, 0, 120);
	hide(list_message);

	for (int i = 0; i < ROW_POOL; i++) {
		row_t *row = &rows[i];

		row->button = lv_btn_create(list_body);
		lv_obj_set_size(row->button, width, ROW_HEIGHT);
		lv_obj_set_x(row->button, 0);
		lv_obj_add_style(row->button, &theme_style_card, 0);
		lv_obj_add_style(row->button, &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_set_style_radius(row->button, ROW_RADIUS, 0);
		lv_obj_set_style_border_width(row->button, 0, 0);
		lv_obj_set_style_shadow_width(row->button, 0, 0);
		lv_obj_set_style_pad_hor(row->button, ROW_PAD_HOR, 0);
		lv_obj_set_style_pad_ver(row->button, 10, 0);
		lv_obj_set_style_pad_column(row->button, 14, 0);
		hide(row->button);
		lv_obj_add_flag(row->button, LV_OBJ_FLAG_EVENT_BUBBLE);
		lv_obj_add_event_cb(row->button, row_clicked_cb, LV_EVENT_CLICKED, NULL);
		lv_obj_set_flex_flow(row->button, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row->button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

		// The name alone. No icon -- an identical note on every row says nothing,
		// and a station is recognised by its name -- and no detail line: codec,
		// bitrate and country are player-page information, not something to
		// scroll past with a thumb. A station that cannot be played stays faint,
		// and the popup says why.
		lv_obj_t *text = lv_obj_create(row->button);
		lv_obj_set_flex_grow(text, 1);
		lv_obj_set_height(text, LV_SIZE_CONTENT);
		lv_obj_set_style_bg_opa(text, 0, 0);
		lv_obj_set_style_border_width(text, 0, 0);
		lv_obj_set_style_pad_all(text, 0, 0);
		lv_obj_remove_flag(text, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_add_flag(text, LV_OBJ_FLAG_EVENT_BUBBLE);
		lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(text, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);

		row->name = lv_label_create(text);
		lv_label_set_long_mode(row->name, LV_LABEL_LONG_DOT);
		lv_obj_set_width(row->name, lv_pct(100));
		// One line, and the height is what says so. LV_LABEL_LONG_DOT wraps
		// first and only puts the dots in when the wrapped text runs past the
		// height it is given -- so with the height left to the content there is
		// never anything to cut, and a long station name takes as many lines as
		// it needs, through the row underneath it.
		lv_obj_set_height(row->name, lv_font_get_line_height(&font_ui_24));
		lv_obj_add_style(row->name, &theme_style_text, 0);
		lv_obj_set_style_text_font(row->name, &font_ui_24, 0);

		row->menu_btn = lv_btn_create(row->button);
		lv_obj_set_size(row->menu_btn, 52, 52);
		lv_obj_set_style_bg_opa(row->menu_btn, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(row->menu_btn, 0, 0);
		lv_obj_set_style_shadow_width(row->menu_btn, 0, 0);
		lv_obj_set_style_pad_all(row->menu_btn, 0, 0);
		lv_obj_add_event_cb(row->menu_btn, menu_btn_cb, LV_EVENT_CLICKED, NULL);
		hide(row->menu_btn);

		lv_obj_t *dots = lv_image_create(row->menu_btn);
		lv_image_set_src(dots, &icon_ellipsis_vertical);
		lv_obj_add_style(dots, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(dots, LV_OPA_COVER, 0);
		lv_obj_center(dots);

		row->chevron = lv_image_create(row->button);
		lv_image_set_src(row->chevron, &icon_chevron_right);
		lv_obj_add_style(row->chevron, &theme_style_icon, 0);
		lv_obj_set_style_image_opa(row->chevron, LV_OPA_60, 0);

		// Outside the flex layout, in the row's own left padding: it marks the
		// row without moving anything on it.
		row->playmark = lv_obj_create(row->button);
		lv_obj_add_flag(row->playmark, LV_OBJ_FLAG_IGNORE_LAYOUT);
		lv_obj_set_size(row->playmark, PLAYMARK_WIDTH, PLAYMARK_HEIGHT);
		lv_obj_align(row->playmark, LV_ALIGN_LEFT_MID, PLAYMARK_INSET - ROW_PAD_HOR, 0);
		lv_obj_add_style(row->playmark, &theme_style_accent_bg, 0);
		lv_obj_set_style_radius(row->playmark, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_border_width(row->playmark, 0, 0);
		lv_obj_set_style_shadow_width(row->playmark, 0, 0);
		lv_obj_set_style_pad_all(row->playmark, 0, 0);
		lv_obj_remove_flag(row->playmark, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_remove_flag(row->playmark, LV_OBJ_FLAG_CLICKABLE);
		hide(row->playmark);

		row->index = -1;
	}

	switcher_attach_back_gesture(list_view);
	switcher_set_back_guard(radiolist_screen, list_back_guard);
	switcher_set_back_guard_peek(radiolist_screen, list_back_has_step);
	lv_obj_add_event_cb(radiolist_screen, list_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);

	list_timer = lv_timer_create(list_poll_cb, POLL_MS, NULL);
}

// ---------------------------------------------------------------------------
// the hub
// ---------------------------------------------------------------------------

// The Music page's corner button, to the pixel: this page has to look like
// that one, not like a cousin of it.
static lv_obj_t *corner_button(gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph) {
	lv_obj_t *button = lv_btn_create(radiopage_screen);
	lv_obj_set_size(button, 56, 56);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -cfg->padding - slot * (56 + 6), cfg->padding + cfg->top_bar_height);

	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, glyph);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_center(icon);

	return button;
}

static bool list_back_has_step(void) { return list_mode == LIST_STATIONS && stations_from_terms; }

static bool list_back_guard(void) {
	if (!list_back_has_step()) {
		return false; // leave the page, as usual
	}
	// Back to the category list that was entered from: the same request
	// open_browse makes, with the title that list had.
	stations_from_terms = false;
	list_mode = LIST_TERMS;
	term_count = 0;
	station_count = 0;
	page_more = false;
	page_loading = false;
	page_append = false;
	lv_label_set_text(list_title, terms_title);
	list_rebuild();
	set_message("loading");
	last_job_serial = radio_job_serial();
	radio_request_terms(list_browse);
	return true;
}

static void open_browse(radio_browse_t kind, const char *title) {
	list_browse = kind;
	stations_from_terms = false;
	list_mode = LIST_TERMS;
	term_count = 0;
	station_count = 0;
	page_more = false;
	page_loading = false;
	page_append = false;

	lv_label_set_text(list_title, tr(title));
	list_rebuild();
	set_message("loading");

	last_job_serial = radio_job_serial();
	radio_request_terms(kind);

	switch_screen(radiolist_screen);
}

static void language_cb(lv_event_t *e) {
	(void)e;
	open_browse(RADIO_BROWSE_LANGUAGES, "language");
}

static void country_cb(lv_event_t *e) {
	(void)e;
	open_browse(RADIO_BROWSE_COUNTRIES, "radio_country");
}

static void genre_cb(lv_event_t *e) {
	(void)e;
	open_browse(RADIO_BROWSE_GENRES, "genre");
}

static void open_stored(list_mode_t mode, const char *title) {
	stations_from_terms = false;
	list_mode = mode;
	page_more = false;
	page_loading = false;
	page_append = false;
	lv_label_set_text(list_title, tr(title));
	list_reload_stored();
	list_rebuild();
	set_message(station_count ? NULL : stored_empty_message());
	switch_screen(radiolist_screen);
}

static void favourites_cb(lv_event_t *e) {
	(void)e;
	open_stored(LIST_FAVOURITES, "favourites");
}

static void recent_cb(lv_event_t *e) {
	(void)e;
	open_stored(LIST_RECENT, "radio_recently_played");
}

static void search_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(search_field, "");
	switch_screen(radiosearch_screen);
}

// The file is read again every time the page is opened, so a card edited under
// the player shows what is on it now rather than what it held at boot.
static void custom_cb(lv_event_t *e) {
	(void)e;
	radio_custom_reload();
	open_stored(LIST_CUSTOM, "radio_custom");
}

static void wifi_row_cb(lv_event_t *e) {
	(void)e;
	switch_screen(wifisettings_screen);
}

// Shows or hides what needs a network, from whether there is one.
static void hub_refresh(void) {
	bool connected = wifi_is_connected();

	for (int i = 0; i < 3; i++) {
		if (connected) {
			show(hub_browse_rows[i]);
		} else {
			hide(hub_browse_rows[i]);
		}
	}

	// With no network the page is the notice and nothing else. The starred and
	// recent lists do come out of the database rather than the directory, but
	// every station in them is a stream: offering them over a page that says
	// the Wi-Fi is off leads to three corner buttons that open lists nothing in
	// which can be played.
	if (connected) {
		show(hub_favourites_btn);
		show(hub_recent_btn);
		show(hub_search_btn);
		show(hub_custom_btn);
		hide(hub_wifi_warning);
		hide(hub_wifi_row);
	} else {
		hide(hub_favourites_btn);
		hide(hub_recent_btn);
		hide(hub_search_btn);
		// The file is on the card and reads without a network, but every line
		// in it is a stream: a list nothing in which can be played is the same
		// empty promise the other three would be.
		hide(hub_custom_btn);
		show(hub_wifi_warning);
		show(hub_wifi_row);
	}

	// The heading gets the width the buttons are not using.
	settingsrow_title_corner_slots(settingsrow_page_title(radiopage_screen), g_cfg, connected ? 4 : 0);
}

static void hub_poll_cb(lv_timer_t *timer) {
	(void)timer;
	if (lv_screen_active() != radiopage_screen) {
		return;
	}
	hub_refresh();
}

static void hub_loaded_cb(lv_event_t *e) {
	(void)e;
	hub_refresh();
}

// ---------------------------------------------------------------------------
// the search page
//
// A field, the keyboard, and that is all: the hits are stations, and there is
// already a page that draws a list of stations. Pressing the magnifier asks
// the directory and hands the answer to it.
// ---------------------------------------------------------------------------

static void run_search(void) {
	const char *text = lv_textarea_get_text(search_field);
	if (!text || !text[0]) {
		return;
	}

	list_mode = LIST_STATIONS;
	term_count = 0;
	station_count = 0;
	page_is_search = true;
	page_offset = 0;
	page_more = false;
	page_append = false;
	page_loading = true;
	snprintf(page_value, sizeof(page_value), "%s", text);

	lv_label_set_text(list_title, text);
	list_rebuild();
	set_message("loading");

	last_job_serial = radio_job_serial();
	radio_request_search(page_value, 0);

	switch_screen(radiolist_screen);
}

static void search_accept_cb(lv_event_t *e) {
	(void)e;
	run_search();
}

static void search_loaded_cb(lv_event_t *e) {
	(void)e;
	// A fresh visit opens with the keyboard up and the caret in the field.
	keyboard_reset(search_keyboard);
	keyboard_set_visible(search_keyboard, true);
	lv_obj_add_state(search_field, LV_STATE_FOCUSED);
}

static void build_search_page(gui_config_t *cfg) {
	lv_obj_add_style(radiosearch_screen, &theme_style_screen, 0);
	settingsrow_title(radiosearch_screen, cfg, "radio_search_radio");

	int top = settingsrow_content_top(cfg);

	search_field = lv_textarea_create(radiosearch_screen);
	lv_textarea_set_one_line(search_field, true);
	lv_textarea_set_placeholder_text(search_field, tr("radio_station_name"));
	lv_obj_set_size(search_field, cfg->screen_width - 2 * cfg->padding, 62);
	lv_obj_set_scrollbar_mode(search_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(search_field, LV_ALIGN_TOP_LEFT, cfg->padding, top);
	lv_obj_add_style(search_field, &theme_style_card, 0);
	lv_obj_set_style_radius(search_field, 12, 0);
	lv_obj_set_style_border_width(search_field, 0, 0);
	lv_obj_set_style_shadow_width(search_field, 0, 0);
	lv_obj_set_style_pad_all(search_field, 14, 0);
	lv_obj_set_style_text_font(search_field, &font_ui_24, 0);
	keyboard_style_caret(search_field);

	// 316 px is the other search page's keyboard height, and the two want to be
	// the same keyboard in the same place.
	search_keyboard = keyboard_create(radiosearch_screen, cfg->screen_width, 316, search_field, &icon_search, NULL,
									  search_accept_cb, NULL);

	lv_obj_add_event_cb(radiosearch_screen, search_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(radiosearch_screen);
}

void radiopage_init(gui_config_t *cfg) {
	g_cfg = cfg;

	radio_init();

	hub_container = settingsrow_page(radiopage_screen, cfg, "radio");
	settingsrow_title_corner_slots(settingsrow_page_title(radiopage_screen), cfg, 3);

	// The drag towards the player. The screen already has it (gui.c attaches it
	// along with the other pages'), but the container sits on top and would take
	// the gesture, so as on the Qobuz pages it is attached to both.
	player_sheet_attach_drag(hub_container, true);

	// Top right, in the corner every page of this player keeps its shortcuts:
	// the starred stations, the ones played most recently to their left, and
	// the search to the left of those. Same buttons, same order and the same
	// spacing as the Music page.
	hub_favourites_btn = corner_button(cfg, 0, &icon_star_corner);
	lv_obj_add_event_cb(hub_favourites_btn, favourites_cb, LV_EVENT_CLICKED, NULL);

	hub_recent_btn = corner_button(cfg, 1, &icon_radio_recent);
	lv_obj_add_event_cb(hub_recent_btn, recent_cb, LV_EVENT_CLICKED, NULL);

	hub_custom_btn = corner_button(cfg, 2, &icon_radio_custom);
	lv_obj_add_event_cb(hub_custom_btn, custom_cb, LV_EVENT_CLICKED, NULL);

	hub_search_btn = corner_button(cfg, 3, &icon_search);
	lv_obj_add_event_cb(hub_search_btn, search_cb, LV_EVENT_CLICKED, NULL);

	hub_browse_rows[0] = settingsrow_add(hub_container, "language", NULL, language_cb, NULL);
	hub_browse_rows[1] = settingsrow_add(hub_container, "radio_country", NULL, country_cb, NULL);
	hub_browse_rows[2] = settingsrow_add(hub_container, "genre", NULL, genre_cb, NULL);

	// The same notice as the Wi-Fi transfer page: a centred message, and below
	// it the "Wi-Fi settings" row that leads to where it can be fixed.
	hub_wifi_warning = lv_label_create(hub_container);
	lv_label_set_text(hub_wifi_warning, tr("enable_wi_fi_first"));
	lv_label_set_long_mode(hub_wifi_warning, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(hub_wifi_warning, lv_pct(100));
	lv_obj_add_style(hub_wifi_warning, &theme_style_text, 0);
	lv_obj_set_style_text_font(hub_wifi_warning, &font_ui_22, 0);
	lv_obj_set_style_text_align(hub_wifi_warning, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_pad_hor(hub_wifi_warning, 4, 0);
	lv_obj_set_style_margin_top(hub_wifi_warning, 14, 0);
	hide(hub_wifi_warning);

	hub_wifi_row = settingsrow_add(hub_container, "wi_fi_settings", NULL, wifi_row_cb, NULL);
	lv_obj_set_style_margin_top(hub_wifi_row, 16, 0);
	hide(hub_wifi_row);

	build_list_page(cfg);
	build_search_page(cfg);

	lv_obj_add_event_cb(radiopage_screen, hub_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);

	// The Wi-Fi can come and go while the page is open; the rows follow it.
	hub_timer = lv_timer_create(hub_poll_cb, 1000, NULL);
	(void)hub_timer;
}

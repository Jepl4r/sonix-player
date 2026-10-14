#include "radiopage.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/azindex.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/sx_card.h"
#include "src/gui/shell/sx_frame.h"
#include "src/gui/shell/sx_row.h"
#include "src/gui/shell/sx_sheet.h"
#include "src/gui/shell/sx_styles.h"
#include "src/gui/shell/sx_tokens.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/streaming/streamerror.h"
#include "src/gui/streaming/svclist.h"
#include "src/gui/streaming/svcui.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/library/library.h"
#include "src/system/net/wifi.h"
#include "src/system/streaming/radio.h"

lv_obj_t *radiopage_screen;
lv_obj_t *radiolist_screen;
lv_obj_t *radiosearch_screen;

// ---------------------------------------------------------------------------
// Internet radio, 2.0
//
// The handoff's radio page is a list of stations, each with its star, a tap
// playing it live in the mini player. Here that list is the user's starred
// stations, on the page itself under a card of the ways into the directory
// -- genres, countries, languages, the recently played, the stations of
// radio.txt -- each of which opens the same kind of list on the list page.
// The search stays in the header.
//
// Every list draws its rows with svclist (sx_row): a plaque in the station's
// own tone with the radio glyph, the name, "genre · codec bitrate" under it,
// the star at the right. Tap plays (the page stays: the mini player shows
// what is on air), hold brings up the station's sheet (listen, star), the
// star adds or removes the favourite with Undo in the toast. A station rows
// have no swipes: a stream is not something to queue (the gesture matrix).
// ---------------------------------------------------------------------------

#define POLL_MS 300
// Rows in the pool: a screenful (eight on the R1) and a half.
#define ROW_POOL 14

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

static gui_config_t *g_cfg;

// The radio.txt line the rows were last marked for, -1 for none.
static int marked_custom = -1;

// The station the other lists were last marked for -- the directory's, the
// starred and the recent ones: its uuid, or its address when it has none.
// Empty when no station is loaded.
static char marked_station[RADIO_URL_MAX];

// --- the hub ---
static svclist_t *hub;
static lv_obj_t *hub_top;	   // the cards above the starred stations, in the hub's scroller
static lv_obj_t *hub_catalogue; // the ways into the directory
static lv_obj_t *hub_offline;  // or, with no network, the notice and the way to the Wi-Fi
static lv_obj_t *hub_empty;	   // "star a station and it is here"
static lv_obj_t *hub_search_btn;
static lv_obj_t *hub_title;
static radio_station_t *favs; // the starred stations, read at each visit
static int fav_count;
static bool hub_online;

// --- the search page ---
static lv_obj_t *search_field;
static keyboard_t *search_keyboard;

// --- the list ---
static svclist_t *list;
static lv_obj_t *list_note;	 // "Nothing here", the stored lists' empty sentences
static lv_obj_t *list_error; // the directory could not be reached
static lv_obj_t *list_spin;
static lv_obj_t *list_title;
static lv_obj_t *sort_btn; // A-Z / Z-A, on the three category lists
static lv_obj_t *sort_icon;
static azindex_t *list_index; // the letters down the right edge, same lists
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

static void list_reload_stored(void);
static const char *stored_empty_message(void);
static void hub_reload(void);

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

static int list_count(void) { return list_mode == LIST_TERMS ? term_count : station_count; }

static void set_message(const char *text) {
	lv_obj_set_hidden(list_spin, true);
	if (text && text[0]) {
		lv_label_set_text(list_note, tr(text));
		lv_obj_set_hidden(list_note, false);
	} else {
		lv_obj_set_hidden(list_note, true);
	}
}

// A request is on its way: the spinner, and nothing else.
static void set_loading(void) {
	lv_obj_set_hidden(list_note, true);
	streamerror_hide(list_error);
	lv_obj_set_hidden(list_spin, false);
}

// ---------------------------------------------------------------------------
// What a station row says
//
// Under the name, the line the prototype gives a station: its genre and its
// stream -- "Jazz · MP3 256", "Eclectic · FLAC" -- and on a search's hits the
// country code too, since a search crosses every country and the same name can
// be a station in three of them. The quality the 1.2.5 rows put in a coloured
// glyph is now in these words.
// ---------------------------------------------------------------------------

static void station_subtitle(const radio_station_t *s, bool with_code, char *out, size_t size) {
	char genre[48] = "";
	// The first of the directory's comma-separated tags, with a capital.
	size_t n = strcspn(s->tags, ",");
	if (n > 0 && n < sizeof(genre)) {
		memcpy(genre, s->tags, n);
		genre[n] = '\0';
		if (genre[0] >= 'a' && genre[0] <= 'z') {
			genre[0] = (char)(genre[0] - 'a' + 'A');
		}
	}
	char stream[32] = "";
	if (s->codec[0] && s->bitrate > 0) {
		snprintf(stream, sizeof(stream), "%s %d", s->codec, s->bitrate);
	} else if (s->codec[0]) {
		snprintf(stream, sizeof(stream), "%s", s->codec);
	}
	// A station of radio.txt knows only its address: the host then, so the
	// row has its second line like every other (and two stations of one
	// broadcaster read as such).
	char host[64] = "";
	if (!genre[0] && !stream[0]) {
		const char *h = strstr(s->url, "://");
		h = h ? h + 3 : s->url;
		size_t len = strcspn(h, ":/?");
		if (len >= sizeof(host)) {
			len = sizeof(host) - 1;
		}
		memcpy(host, h, len);
		host[len] = '\0';
	}
	const char *parts[4] = {genre, stream, host, with_code ? s->country : ""};
	out[0] = '\0';
	size_t used = 0;
	for (int i = 0; i < 4; i++) {
		if (!parts[i][0] || used >= size) {
			continue;
		}
		int wrote = snprintf(out + used, size - used, "%s%s", used ? " \xC2\xB7 " : "", parts[i]);
		if (wrote > 0) {
			used += (size_t)wrote;
		}
	}
}

static const char *station_key(const radio_station_t *s) { return s->uuid[0] ? s->uuid : s->url; }

// Reads which station is loaded, playing or stopped. True when the answer
// differs from the one the rows were marked with.
static bool marked_station_update(void) {
	radio_station_t now;
	char key[sizeof(marked_station)] = "";
	if (radio_current_station(&now)) {
		snprintf(key, sizeof(key), "%s", station_key(&now));
	}
	if (strcmp(key, marked_station) == 0) {
		return false;
	}
	snprintf(marked_station, sizeof(marked_station), "%s", key);
	return true;
}

static void station_bind(lv_obj_t *row, const radio_station_t *s, bool with_code, bool playing) {
	char sub[200];
	station_subtitle(s, with_code, sub, sizeof(sub));
	sx_row_set_title(row, s->name);
	sx_row_set_subtitle(row, sub, NULL, NULL);
	sx_row_set_thumb(row, &icon_radio_custom, svcui_tone(s->name));
	sx_row_set_trail(row, SX_ROW_TRAIL_STAR);
	sx_row_set_starred(row, radio_fav_contains(s->uuid));
	sx_row_set_playing(row, playing);
	// A station that cannot be played is drawn faint: still there, still
	// tappable (it says why), but plainly not one of the ones that will.
	lv_obj_set_style_opa(row, radio_station_problem(s) ? LV_OPA_50 : LV_OPA_COVER, 0);
}

// ---------------------------------------------------------------------------
// The category lists in order
//
// Languages, countries and genres are listed alphabetically, either way round,
// in the music library's own collation: accents folded, a leading article
// skipped when the library skips them. The letters of the A-Z strip come from
// the same collation, so each points at the rows it names.
// ---------------------------------------------------------------------------

// Which of the three lists run Z-A, one bit per radio_browse_t, kept across
// restarts like the track lists' direction.
static bool terms_desc(radio_browse_t kind) {
	return (config_get_int("radio", "sort_desc", 0) & (1L << kind)) != 0;
}

static void terms_set_desc(radio_browse_t kind, bool desc) {
	long mask = config_get_int("radio", "sort_desc", 0);
	mask = desc ? (mask | (1L << kind)) : (mask & ~(1L << kind));
	config_set_int("radio", "sort_desc", mask);
	config_save();
}

// The sort keys of the terms being sorted: qsort takes no context.
static char (*sort_keys)[LIBRARY_SORT_KEY_MAX];

static int term_order(const void *a, const void *b) {
	int x = *(const int *)a;
	int y = *(const int *)b;
	int order = strcmp(sort_keys[x], sort_keys[y]);
	return order ? order : x - y;
}

// Puts terms[] in order and tells the strip where each letter starts.
static void terms_sort(void) {
	bool desc = terms_desc(list_browse);
	int first[LIBRARY_INDEX_BUCKETS];
	for (int i = 0; i < LIBRARY_INDEX_BUCKETS; i++) {
		first[i] = -1;
	}

	sort_keys = malloc((size_t)(term_count > 0 ? term_count : 1) * sizeof(*sort_keys));
	int *order = malloc((size_t)(term_count > 0 ? term_count : 1) * sizeof(*order));
	radio_term_t *sorted = malloc((size_t)(term_count > 0 ? term_count : 1) * sizeof(*sorted));
	if (!sort_keys || !order || !sorted) {
		free(sort_keys);
		free(order);
		free(sorted);
		sort_keys = NULL;
		azindex_set_rows(list_index, NULL, 0, false);
		return;
	}

	for (int i = 0; i < term_count; i++) {
		library_sort_key(terms[i].label, sort_keys[i], sizeof(sort_keys[i]));
		order[i] = i;
	}
	qsort(order, (size_t)term_count, sizeof(*order), term_order);

	for (int i = 0; i < term_count; i++) {
		int from = order[desc ? term_count - 1 - i : i];
		sorted[i] = terms[from];
		int bucket = library_index_slot(library_index_letter_of_key(sort_keys[from]));
		// The first row of a letter is where it starts reading down, whichever
		// way round the list runs.
		if (first[bucket] < 0) {
			first[bucket] = i;
		}
	}
	memcpy(terms, sorted, (size_t)term_count * sizeof(*terms));

	free(sort_keys);
	free(order);
	free(sorted);
	sort_keys = NULL;

	azindex_set_rows(list_index, first, term_count, desc);
}

// The corner button and the strip belong to the category lists only: a list
// of stations is in the directory's order, most played first, and has no
// letters to jump between.
static void sort_button_update(void) {
	if (list_mode == LIST_TERMS) {
		lv_image_set_src(sort_icon, terms_desc(list_browse) ? &icon_sort_za : &icon_sort_az);
		lv_obj_set_hidden(sort_btn, false);
	} else {
		lv_obj_set_hidden(sort_btn, true);
		azindex_set_rows(list_index, NULL, 0, false);
	}
	settingsrow_title_corner_slots(list_title, g_cfg, list_mode == LIST_TERMS ? 1 : 0);
}

// ---------------------------------------------------------------------------
// the list page's rows
// ---------------------------------------------------------------------------

// The first letter of `label` (a whole UTF-8 character), for the plaque.
static void first_letter(const char *label, char *out, size_t size) {
	size_t n = 1;
	unsigned char c = (unsigned char)label[0];
	if (c >= 0xF0) {
		n = 4;
	} else if (c >= 0xE0) {
		n = 3;
	} else if (c >= 0xC0) {
		n = 2;
	}
	if (!c || n >= size || strlen(label) < n) {
		snprintf(out, size, "#");
		return;
	}
	memcpy(out, label, n);
	out[n] = '\0';
	if (n == 1 && out[0] >= 'a' && out[0] <= 'z') {
		out[0] = (char)(out[0] - 'a' + 'A');
	}
}

static bool row_is_marked(int index) {
	if (list_mode == LIST_CUSTOM) {
		return index == marked_custom;
	}
	return list_mode != LIST_TERMS && marked_station[0] && strcmp(station_key(&stations[index]), marked_station) == 0;
}

static void list_bind(lv_obj_t *row, int index) {
	if (index < 0 || index >= list_count()) {
		return;
	}
	if (list_mode == LIST_TERMS) {
		const radio_term_t *t = &terms[index];
		char letter[8], sub[64];
		first_letter(t->label, letter, sizeof(letter));
		lv_snprintf(sub, sizeof(sub), tr("radio_stations_n"), t->stationcount);
		sx_row_set_title(row, t->label);
		sx_row_set_subtitle(row, t->stationcount > 0 ? sub : "", NULL, NULL);
		sx_row_set_letter(row, letter, svcui_tone(t->label), false);
		sx_row_set_trail(row, SX_ROW_TRAIL_CHEVRON);
		sx_row_set_playing(row, false);
		lv_obj_set_style_opa(row, LV_OPA_COVER, 0);
		return;
	}
	station_bind(row, &stations[index], page_is_search && list_mode == LIST_STATIONS, row_is_marked(index));
}

// radio.txt's headings, over the first station of each group.
static bool list_heading(int index, char *out, size_t size) {
	return list_mode == LIST_CUSTOM && radio_custom_heading(index, out, size);
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
	if (list_mode == LIST_TERMS) {
		azindex_flash(list_index);
	}
}

static void list_rebuild_keep(bool keep_scroll) {
	marked_station_update(); // the rows below are marked from it
	svclist_set_count(list, list_count(), keep_scroll);
}

static void list_rebuild(void) { list_rebuild_keep(false); }

// ---------------------------------------------------------------------------
// Playing, the star and the sheet -- the same for the list page and the hub
// ---------------------------------------------------------------------------

// Starts `s` -- one of `all`, which previous and next then walk (radio.txt
// has its own) -- and stays: the mini player shows it connecting, then what
// is on air. A station that cannot be played says why instead.
static void play_station(const radio_station_t *s, const radio_station_t *all, int count, bool custom) {
	const char *problem = radio_station_problem(s);
	if (problem) {
		gui_notify_popup(problem);
		return;
	}
	if (!custom) {
		radio_set_list(all, count);
	}
	if (radio_play(s)) {
		player_refresh_now_playing();
	} else {
		gui_notify_popup("radio_cannot_start_the_station");
	}
	if (marked_station_update()) {
		svclist_rebind(list);
		svclist_rebind(hub);
	}
}

// The starred station toggled from a row or its sheet: at once, with Undo.
// The uuid rather than a row: the toast outlives the rows it was raised on.
static radio_station_t star_station;
static bool star_now_on;

static void star_refresh(void) {
	svclist_rebind(list);
	svclist_rebind(hub);
	// The player's own star is about the same station.
	player_refresh_now_playing();
}

static void star_apply(bool on) {
	if (on) {
		radio_fav_add(&star_station);
	} else {
		radio_fav_remove(star_station.uuid);
	}
	star_now_on = on;
	star_refresh();
}

static void star_undo(void *user) {
	(void)user;
	star_apply(!star_now_on);
}

static void station_star(const radio_station_t *s) {
	star_station = *s;
	star_apply(!radio_fav_contains(s->uuid));
	svcui_star_toast(star_now_on, "svc_fav_added", "svc_fav_removed", s->name, star_undo, NULL);
}

// The sheet: the station, "live" under it, Listen and the star. Copied, as
// the rows may be rebound while it is up.
static radio_station_t sheet_station;
static bool sheet_from_hub;

static void sheet_listen(void *user) {
	(void)user;
	if (sheet_from_hub) {
		play_station(&sheet_station, favs, fav_count, false);
	} else {
		play_station(&sheet_station, stations, station_count, list_mode == LIST_CUSTOM);
	}
}

static void sheet_star(void *user) {
	(void)user;
	station_star(&sheet_station);
}

static void station_sheet(const radio_station_t *s, bool from_hub) {
	sheet_station = *s;
	sheet_from_hub = from_hub;
	static char subtitle[240];
	char sub[200];
	station_subtitle(s, true, sub, sizeof(sub));
	snprintf(subtitle, sizeof(subtitle), "%s%s%s", sub, sub[0] ? " \xC2\xB7 " : "", tr("radio_live"));
	bool fav = radio_fav_contains(s->uuid);
	sx_sheet_item_t items[] = {
		{.label = tr("radio_listen"), .icon = &icon_play_small, .action = sheet_listen},
		{.label = tr(fav ? "remove_from_favourites" : "medialist_add_to_favourites"),
		 .icon = fav ? &icon_star_filled : &icon_star,
		 .action = sheet_star},
	};
	sx_sheet_menu(sheet_station.name, subtitle, items, 2);
}

// ---------------------------------------------------------------------------
// the list page's gestures
// ---------------------------------------------------------------------------

static void enter_term(int index) {
	const char *value = terms[index].name;
	snprintf(terms_title, sizeof(terms_title), "%s", lv_label_get_text(list_title));
	lv_label_set_text(list_title, terms[index].label);
	stations_from_terms = true;

	list_mode = LIST_STATIONS;
	snprintf(page_value, sizeof(page_value), "%s", value);
	station_count = 0;
	term_count = 0;
	page_is_search = false;
	page_offset = 0;
	page_more = false;
	page_append = false;
	page_loading = true;
	sort_button_update();
	list_rebuild();
	set_loading();

	last_job_serial = radio_job_serial();
	radio_request_stations(list_browse, page_value, 0);
}

static void list_tap(lv_obj_t *row, void *user) {
	(void)row;
	int index = (int)(intptr_t)user;
	if (index < 0 || index >= list_count()) {
		return;
	}
	if (list_mode == LIST_TERMS) {
		enter_term(index);
		return;
	}
	play_station(&stations[index], stations, station_count, list_mode == LIST_CUSTOM);
}

static void list_hold(lv_obj_t *row, void *user) {
	(void)row;
	int index = (int)(intptr_t)user;
	if (list_mode != LIST_TERMS && index >= 0 && index < station_count) {
		station_sheet(&stations[index], false);
	}
}

static void list_star(lv_obj_t *row, void *user) {
	(void)row;
	int index = (int)(intptr_t)user;
	if (list_mode != LIST_TERMS && index >= 0 && index < station_count) {
		station_star(&stations[index]);
	}
}

// The two lists that come out of the database rather than off the network,
// and radio.txt.
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
// The tags live in an array rather than in the return statement so that
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

static void retry_list(void);

// A station started from these pages plays in the mini player, the page
// staying where it is -- so when it cannot play, the reason has to come here:
// the player that used to open and say it is not on screen. Said once per
// failure, in the toast.
static char error_said[sizeof(((radio_now_t *)0)->error)];

static void error_watch(void) {
	radio_now_t now;
	radio_get_now(&now);
	if (!now.error[0]) {
		error_said[0] = '\0';
		return;
	}
	if (strcmp(now.error, error_said) != 0) {
		snprintf(error_said, sizeof(error_said), "%s", now.error);
		toast_error(now.error);
	}
}

// Watches the worker while a request is in flight, and the station playing.
static void list_poll_cb(lv_timer_t *timer) {

	lv_obj_t *active = lv_screen_active();
	if (active != radiopage_screen && active != radiolist_screen) {
		// Off the radio pages there is nothing to watch: the timer sleeps
		// until one of them is loaded again (list_loaded_cb, hub_loaded_cb),
		// rather than waking the CPU three times a second for nothing. A
		// request still in flight is picked up then by its serial.
		lv_timer_pause(timer);
		return;
	}
	error_watch();
	if (active == radiopage_screen) {
		// The hub's starred stations carry the mark too.
		if (marked_station_update()) {
			svclist_rebind(hub);
		}
		return;
	}
	if (active != radiolist_screen) {
		return;
	}

	// The mark follows the station, which changes from the player and the
	// side keys as well as from here.
	if (list_mode == LIST_CUSTOM) {
		int now = radio_custom_current_index();
		if (now != marked_custom) {
			marked_custom = now;
			svclist_rebind(list);
		}
	} else if (list_mode != LIST_TERMS && marked_station_update()) {
		svclist_rebind(list);
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
			// Where the list would have been, with Retry: the same
			// request again (streamerror.h).
			lv_obj_set_hidden(list_spin, true);
			streamerror_show(list_error, streamerr_online() ? STREAM_ERR_NO_RESPONSE : STREAM_ERR_NO_WIFI,
							 tr("radio_directory_unreachable"), retry_list, NULL);
		}
		page_more = false;
		return;
	}
	streamerror_hide(list_error);

	if (list_mode == LIST_TERMS) {
		term_count = radio_get_terms(terms, (int)(sizeof(terms) / sizeof(terms[0])));
		station_count = 0;
		page_more = false;
		terms_sort();
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
	lv_timer_resume(list_timer);
	// The stored lists can change from the player while this page is closed:
	// the star adds and removes favourites, and every station played lands in
	// the recents.
	if (LIST_IS_STORED(list_mode)) {
		list_reload_stored();
		list_rebuild();
		set_message(station_count ? NULL : stored_empty_message());
	} else {
		svclist_rebind(list);
	}
}

static void sort_clicked_cb(lv_event_t *e) {
	(void)e;
	if (list_mode != LIST_TERMS) {
		return;
	}
	terms_set_desc(list_browse, !terms_desc(list_browse));
	sort_button_update();
	terms_sort();
	list_rebuild();
}

static void index_jumped(void) { svclist_rebind(list); }

static void build_list_page(gui_config_t *cfg) {
	lv_obj_add_style(radiolist_screen, &theme_style_screen, 0);
	list_title = settingsrow_title(radiolist_screen, cfg, "radio");

	int top = settingsrow_content_top(cfg);
	static const sx_row_handlers_t handlers = {
		.on_tap = list_tap,
		.on_long_press = list_hold,
		.on_star = list_star,
	};
	static const svclist_cbs_t cbs = {
		.bind = list_bind,
		.heading = list_heading,
		.near_end = list_load_more,
	};
	list = svclist_create(radiolist_screen, &handlers, &cbs, ROW_POOL, false);
	lv_obj_t *view = svclist_obj(list);
	lv_obj_set_size(view, cfg->screen_width - 2 * SX_BODY_PAD_L, cfg->screen_height - top);
	lv_obj_set_pos(view, SX_BODY_PAD_L, top);
	lv_obj_add_event_cb(view, list_scroll_cb, LV_EVENT_SCROLL, NULL);

	list_note = lv_label_create(radiolist_screen);
	lv_label_set_text(list_note, "");
	lv_obj_set_style_text_align(list_note, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(list_note, &sx_style_text2, 0);
	lv_obj_set_style_text_font(list_note, &font_ui_22, 0);
	// Wrapped and inside the margins: a message long enough to explain where to
	// put a file runs off both edges of the screen otherwise.
	lv_label_set_long_mode(list_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(list_note, cfg->screen_width - 4 * cfg->padding);
	lv_obj_align(list_note, LV_ALIGN_TOP_MID, 0, top + 100);
	lv_obj_set_hidden(list_note, true);

	list_error = streamerror_create(radiolist_screen);
	lv_obj_set_width(list_error, cfg->screen_width - 2 * SX_BODY_PAD_L);
	lv_obj_align(list_error, LV_ALIGN_TOP_MID, 0, top);
	list_spin = svcui_spinner(radiolist_screen);

	// A-Z / Z-A in the header, and the strip of letters down the right edge:
	// the category lists only (see sort_button_update).
	sort_btn = svcui_header_button(radiolist_screen, cfg, 0, &icon_sort_az, sort_clicked_cb, NULL);
	sort_icon = lv_obj_get_child(sort_btn, 0);
	lv_obj_set_hidden(sort_btn, true);

	list_index = azindex_create(radiolist_screen, cfg, view, sx_row_height(false) + SX_ROW_GAP, index_jumped);

	switcher_set_back_guard(radiolist_screen, list_back_guard);
	switcher_set_back_guard_peek(radiolist_screen, list_back_has_step);
	lv_obj_add_event_cb(radiolist_screen, list_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);

	list_timer = lv_timer_create(list_poll_cb, POLL_MS, NULL);
}

// ---------------------------------------------------------------------------
// opening a list
// ---------------------------------------------------------------------------

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
	sort_button_update();
	list_rebuild();
	set_loading();
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
	sort_button_update();
	list_rebuild();
	set_loading();

	last_job_serial = radio_job_serial();
	radio_request_terms(kind);

	switch_screen(radiolist_screen);
}

static void open_stored(list_mode_t mode, const char *title) {
	stations_from_terms = false;
	list_mode = mode;
	page_more = false;
	page_loading = false;
	page_append = false;
	page_is_search = false;
	streamerror_hide(list_error);
	lv_label_set_text(list_title, tr(title));
	sort_button_update();
	list_reload_stored();
	list_rebuild();
	set_message(station_count ? NULL : stored_empty_message());
	switch_screen(radiolist_screen);
}

// The failed request again, from the same place.
static void retry_list(void) {
	set_loading();
	last_job_serial = radio_job_serial();
	page_loading = true;
	if (list_mode == LIST_TERMS) {
		radio_request_terms(list_browse);
	} else if (page_is_search) {
		radio_request_search(page_value, 0);
	} else {
		radio_request_stations(list_browse, page_value, 0);
	}
}

// ---------------------------------------------------------------------------
// the hub
//
// One scroller: the catalogue card (or, with no network, the notice and the
// way to the Wi-Fi), then "FAVOURITES · N" and the starred stations as rows,
// as many as there are, drawn by the same pool as any list.
// ---------------------------------------------------------------------------

typedef enum { GO_GENRES, GO_COUNTRIES, GO_LANGUAGES, GO_RECENT, GO_CUSTOM } hub_go_t;

static void catalogue_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	switch ((hub_go_t)(intptr_t)lv_event_get_user_data(e)) {
	case GO_GENRES:
		open_browse(RADIO_BROWSE_GENRES, "genre");
		break;
	case GO_COUNTRIES:
		open_browse(RADIO_BROWSE_COUNTRIES, "radio_country");
		break;
	case GO_LANGUAGES:
		open_browse(RADIO_BROWSE_LANGUAGES, "language");
		break;
	case GO_RECENT:
		open_stored(LIST_RECENT, "radio_recently_played");
		break;
	case GO_CUSTOM:
		// The file is read again every time the list is opened, so a card
		// edited under the player shows what is on it now rather than what
		// it held at boot.
		radio_custom_reload();
		open_stored(LIST_CUSTOM, "radio_custom");
		break;
	}
}

static void search_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(search_field, "");
	switch_screen(radiosearch_screen);
}

static void wifi_card_cb(lv_event_t *e) {
	(void)e;
	switch_screen(wifisettings_screen);
}

static void hub_bind(lv_obj_t *row, int index) {
	if (index >= 0 && index < fav_count) {
		station_bind(row, &favs[index], false, marked_station[0] && strcmp(station_key(&favs[index]), marked_station) == 0);
	}
}

static bool hub_heading(int index, char *out, size_t size) {
	if (index != 0) {
		return false;
	}
	lv_snprintf(out, size, tr("radio_favourites_n"), fav_count);
	return true;
}

static void hub_tap(lv_obj_t *row, void *user) {
	(void)row;
	int index = (int)(intptr_t)user;
	if (index >= 0 && index < fav_count) {
		play_station(&favs[index], favs, fav_count, false);
	}
}

static void hub_hold(lv_obj_t *row, void *user) {
	(void)row;
	int index = (int)(intptr_t)user;
	if (index >= 0 && index < fav_count) {
		station_sheet(&favs[index], true);
	}
}

static void hub_star(lv_obj_t *row, void *user) {
	(void)row;
	int index = (int)(intptr_t)user;
	if (index >= 0 && index < fav_count) {
		station_star(&favs[index]);
	}
}

// The starred stations, read again: the player's star, the control centre
// and the list page all change them.
static void favs_read(void) {
	free(favs);
	favs = NULL;
	fav_count = 0;
	int n = hub_online ? radio_fav_count() : 0;
	if (n <= 0) {
		return;
	}
	favs = malloc((size_t)n * sizeof(*favs));
	if (!favs) {
		return;
	}
	for (int i = 0; i < n; i++) {
		if (radio_fav_get(i, &favs[fav_count])) {
			fav_count++;
		}
	}
}

static void hub_reload(void) {
	hub_online = wifi_is_connected();
	favs_read();
	// With no network the page is the notice and nothing else. The starred
	// and recent lists do come out of the database rather than the directory,
	// but every station in them is a stream: offering them over a page that
	// says the Wi-Fi is off leads to lists nothing in which can be played.
	lv_obj_set_hidden(hub_catalogue, !hub_online);
	lv_obj_set_hidden(hub_offline, hub_online);
	lv_obj_set_hidden(hub_search_btn, !hub_online);
	lv_obj_set_hidden(hub_empty, !hub_online || fav_count > 0);
	settingsrow_title_corner_slots(hub_title, g_cfg, hub_online ? 1 : 0);
	lv_obj_update_layout(hub_top);
	svclist_set_top(hub, lv_obj_get_height(hub_top));
	marked_station_update();
	svclist_set_count(hub, fav_count, true);
}

static lv_timer_t *hub_timer;

static void hub_poll_cb(lv_timer_t *timer) {
	// The Wi-Fi can come and go while the page is open; the page follows it.
	// Away from the page the timer sleeps until the page is loaded again.
	if (lv_screen_active() != radiopage_screen) {
		lv_timer_pause(timer);
		return;
	}
	if (wifi_is_connected() != hub_online) {
		hub_reload();
	}
}

static void hub_loaded_cb(lv_event_t *e) {
	(void)e;
	lv_timer_resume(hub_timer);
	lv_timer_resume(list_timer); // the error watch and the playing mark
	hub_reload();
}

static void build_hub(gui_config_t *cfg) {
	lv_obj_add_style(radiopage_screen, &theme_style_screen, 0);
	hub_title = settingsrow_title(radiopage_screen, cfg, "radio");
	settingsrow_title_corner_slots(hub_title, cfg, 1);
	hub_search_btn = svcui_header_button(radiopage_screen, cfg, 0, &icon_search, search_cb, NULL);

	int top = settingsrow_content_top(cfg);
	static const sx_row_handlers_t handlers = {
		.on_tap = hub_tap,
		.on_long_press = hub_hold,
		.on_star = hub_star,
	};
	static const svclist_cbs_t cbs = {.bind = hub_bind, .heading = hub_heading};
	hub = svclist_create(radiopage_screen, &handlers, &cbs, ROW_POOL, false);
	lv_obj_t *view = svclist_obj(hub);
	lv_obj_set_size(view, cfg->screen_width - 2 * SX_BODY_PAD_L, cfg->screen_height - top);
	lv_obj_set_pos(view, SX_BODY_PAD_L, top);

	// What scrolls above the stations.
	hub_top = sx_box(view);
	lv_obj_set_size(hub_top, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(hub_top, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(hub_top, SX_CARD_GAP, 0);

	// The ways into the directory, and the two lists of stations kept on the
	// card: one tap each, as pills on one card -- five rows of their own
	// would push the starred stations off the screen.
	hub_catalogue = sx_card_create(hub_top);
	sx_card_title(hub_catalogue, tr("radio_catalogue"));
	lv_obj_t *pills = sx_card_pills(hub_catalogue);
	static const char *const GO_LABELS[] = {"genre", "radio_country", "language", "radio_recently_played",
											"radio_custom"};
	for (int i = 0; i < 5; i++) {
		sx_pill(pills, tr(GO_LABELS[i]), i, catalogue_cb);
	}

	// The same notice as the Wi-Fi transfer page: a sentence, and the card
	// that leads to where it can be fixed.
	hub_offline = sx_box(hub_top);
	lv_obj_set_size(hub_offline, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(hub_offline, LV_FLEX_FLOW_COLUMN);
	svcui_note(hub_offline, tr("enable_wi_fi_first"));
	lv_obj_t *wifi = sx_card_create(hub_offline);
	sx_card_title(wifi, tr("wi_fi_settings"));
	sx_card_chevron(wifi);
	sx_card_on_click(wifi, wifi_card_cb, NULL);
	lv_obj_set_hidden(hub_offline, true);

	hub_empty = svcui_note(hub_top, tr("radio_favourites_hint"));
	lv_obj_set_style_pad_top(hub_empty, 12, 0);
	lv_obj_set_hidden(hub_empty, true);

	lv_obj_add_event_cb(radiopage_screen, hub_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	hub_timer = lv_timer_create(hub_poll_cb, 1000, NULL);
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
	stations_from_terms = false;
	term_count = 0;
	station_count = 0;
	page_is_search = true;
	page_offset = 0;
	page_more = false;
	page_append = false;
	page_loading = true;
	snprintf(page_value, sizeof(page_value), "%s", text);

	lv_label_set_text(list_title, text);
	sort_button_update();
	list_rebuild();
	set_loading();

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

	// The handoff's search field: 60 px, the card's surface and radius.
	search_field = lv_textarea_create(radiosearch_screen);
	lv_textarea_set_one_line(search_field, true);
	lv_textarea_set_placeholder_text(search_field, tr("radio_station_name"));
	lv_obj_set_size(search_field, cfg->screen_width - 2 * SX_BODY_PAD_L, 60);
	lv_obj_set_scrollbar_mode(search_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(search_field, LV_ALIGN_TOP_LEFT, SX_BODY_PAD_L, top);
	lv_obj_add_style(search_field, &sx_style_surface, 0);
	lv_obj_add_style(search_field, &sx_style_text, 0);
	lv_obj_set_style_radius(search_field, SX_CARD_R, 0);
	lv_obj_set_style_border_width(search_field, 0, 0);
	lv_obj_set_style_shadow_width(search_field, 0, 0);
	lv_obj_set_style_pad_ver(search_field, 14, 0);
	lv_obj_set_style_pad_hor(search_field, SX_CARD_PAD_H, 0);
	lv_obj_set_style_text_font(search_field, &font_ui_24, 0);
	keyboard_style_caret(search_field);

	// 316 px is the other search page's keyboard height, and the two want to be
	// the same keyboard in the same place.
	search_keyboard = keyboard_create(radiosearch_screen, cfg->screen_width, 316, search_field, &icon_search, NULL,
									  search_accept_cb, NULL);

	lv_obj_add_event_cb(radiosearch_screen, search_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(radiosearch_screen);
}

static void build_pages(gui_config_t *cfg) {
	build_hub(cfg);
	build_list_page(cfg);
	build_search_page(cfg);
}

// The radio engine comes up at boot -- a station can be playing, or resumed,
// without this page ever having been opened -- but the three pages are built on
// the first visit to the hub (see gui_page_defer). They are a few hundred
// objects and a keyboard, and the hub's once-a-second Wi-Fi poll and the
// list's timer only start then too. Nothing outside reaches into them: the
// streaming tile holds the hub's screen pointer, gui.c only attaches the back
// gesture to the three screens, and the list and search pages are reached only
// from the hub, which does not exist before it is built.
void radiopage_init(gui_config_t *cfg) {
	g_cfg = cfg;

	radio_init();

	gui_page_defer(radiopage_screen, build_pages);
}

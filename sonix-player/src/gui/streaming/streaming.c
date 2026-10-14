#include "streaming.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/streaming/downloadspage.h"
#include "src/gui/streaming/podcastpage.h"
#include "src/gui/streaming/qobuzpage.h"
#include "src/gui/streaming/radiopage.h"
#include "src/gui/streaming/subsonicpage.h"
#include "src/gui/streaming/svcui.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/sx_card.h"
#include "src/gui/shell/sx_sheet.h"
#include "src/gui/shell/sx_styles.h"
#include "src/gui/shell/sx_toast.h"
#include "src/gui/streaming/tidalpage.h"
#include "src/gui/streaming/yandexpage.h"
#ifdef SONIX_ZVUK
#include "src/gui/streaming/zvukpage.h"
#include "src/system/streaming/zvuk.h"
#endif
#include "src/system/streaming/cardimport.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/streaming/podcast.h"
#include "src/system/streaming/qobuz.h"
#include "src/system/streaming/subsonic.h"
#include "src/system/streaming/tidal.h"
#include "src/system/streaming/yandex.h"

lv_obj_t *streaming_screen;
static lv_obj_t *add_screen;

// ---------------------------------------------------------------------------
// Which services are on the page
//
// Not every service is for everyone: someone with no Tidal account has no use
// for a Tidal tile, and six tiles where two are used make the two harder to
// find. So the page carries only the services the user has added, in the
// order they were added; the "+" in the corner lists the rest, and a long
// press on a tile takes it off again.
//
// The list is kept by the services' names in [streaming] tiles, comma
// separated. All six names together are 41 characters, well inside the
// config's 128-character value.
//
// A service whose application keys this firmware does not have can be neither
// added nor shown: its page could only say it does not work. It stays in the
// saved list all the same, so the day a firmware with the keys is installed it
// is back where the user put it.
// ---------------------------------------------------------------------------

typedef enum {
	SVC_TIDAL,
	SVC_QOBUZ,
	SVC_RADIO,
	SVC_PODCASTS,
	SVC_SUBSONIC,
	SVC_YANDEX,
#ifdef SONIX_ZVUK
	SVC_ZVUK,
#endif
	SVC_COUNT,
} service_t;

static const struct {
	const char *id; // its name in the config, never translated
	grid_entry_t entry;
} SERVICES[SVC_COUNT] = {
	[SVC_TIDAL] = {"tidal", {"tidal", &icon_menu_tidal, &tidal_screen, NULL}},
	[SVC_QOBUZ] = {"qobuz", {"qobuz", &icon_menu_qobuz, &qobuz_screen, NULL}},
	[SVC_RADIO] = {"radio", {"radio", &icon_menu_radio, &radiopage_screen, NULL}},
	[SVC_PODCASTS] = {"podcasts", {"podcasts", &icon_menu_podcast, &podcast_screen, NULL}},
	// The user's own server: Navidrome, Gonic, Airsonic...
	[SVC_SUBSONIC] = {"subsonic", {"subsonic", &icon_menu_subsonic, &subsonic_screen, NULL}},
	[SVC_YANDEX] = {"yandex", {"yandex", &icon_menu_yandex, &yandex_screen, NULL}},
#ifdef SONIX_ZVUK
	// Fork only (Makefile: SONIX_ZVUK).
	[SVC_ZVUK] = {"zvuk", {"zvuk", &icon_menu_zvuk, &zvuk_screen, NULL}},
#endif
};

static service_t added[SVC_COUNT];
static int added_n;

// Whether the firmware carries what the service needs to work at all. Radio
// and Subsonic need no keys of ours: the station directory is open, and a
// Subsonic server is the user's own.
static bool service_usable(service_t s) {
	switch (s) {
	case SVC_TIDAL:
		return tidal_configured();
	case SVC_QOBUZ:
		return qobuz_configured();
	case SVC_PODCASTS:
		return podcast_configured();
	case SVC_YANDEX:
		return yandex_configured();
	default:
		return true;
	}
}

// Signed in already: on the first start with this build such a service goes
// on the page whatever the defaults say, so an update never hides an account
// the user was using.
static bool service_signed_in(service_t s) {
	switch (s) {
	case SVC_TIDAL:
		return tidal_logged_in();
	case SVC_QOBUZ:
		return qobuz_logged_in();
	case SVC_SUBSONIC:
		return subsonic_logged_in();
	case SVC_YANDEX:
		return yandex_logged_in();
#ifdef SONIX_ZVUK
	case SVC_ZVUK:
		return zvuk_logged_in();
#endif
	default:
		return false;
	}
}

static bool is_added(service_t s) {
	for (int i = 0; i < added_n; i++) {
		if (added[i] == s) {
			return true;
		}
	}
	return false;
}

static void tiles_save(void) {
	char text[128] = "";
	size_t used = 0;
	for (int i = 0; i < added_n; i++) {
		int wrote = snprintf(text + used, sizeof(text) - used, "%s%s", i ? "," : "", SERVICES[added[i]].id);
		if (wrote < 0 || (size_t)wrote >= sizeof(text) - used) {
			break;
		}
		used += (size_t)wrote;
	}
	// An empty value is a choice too (every tile taken off) and is kept as
	// such: only a missing key means "never chosen".
	config_set("streaming", "tiles", text);
	config_save();
}

static void tiles_load(void) {
	added_n = 0;
	const char *saved = config_get("streaming", "tiles", NULL);
	if (saved) {
		const char *at = saved;
		while (*at && added_n < SVC_COUNT) {
			const char *end = strchr(at, ',');
			size_t len = end ? (size_t)(end - at) : strlen(at);
			for (int s = 0; s < SVC_COUNT; s++) {
				if (strlen(SERVICES[s].id) == len && strncmp(SERVICES[s].id, at, len) == 0 &&
					!is_added((service_t)s)) {
					added[added_n++] = (service_t)s;
				}
			}
			if (!end) {
				break;
			}
			at = end + 1;
		}
		return;
	}

	// Never chosen: a new player, or the first start after the update that
	// brought this page. The two services that need no account at all (radio,
	// and podcasts when the firmware has the directory's keys), plus every
	// one already signed in, in the order the page always had -- so an update
	// keeps every tile that was in use and leaves out only the ones never
	// taken past their sign-in page. Written down at once, so a later
	// sign-out or a firmware without some keys does not move the tiles about
	// by itself.
	for (int s = 0; s < SVC_COUNT; s++) {
		bool wanted = s == SVC_RADIO || s == SVC_PODCASTS || service_signed_in((service_t)s);
		if (wanted && service_usable((service_t)s)) {
			added[added_n++] = (service_t)s;
		}
	}
	tiles_save();
}

// ---------------------------------------------------------------------------
// The page
// ---------------------------------------------------------------------------

static gui_config_t *page_cfg;
static lv_obj_t *tile_grid;
static lv_obj_t *empty_note;

// The service a tile shows, by its place on the grid: the grid holds only the
// usable ones, so the two lists are not the same length.
static service_t shown[SVC_COUNT];

static void tile_long_pressed_cb(lv_event_t *e);

static void tiles_rebuild(void) {
	grid_entry_t entries[SVC_COUNT] = {0};
	int n = 0;
	for (int i = 0; i < added_n; i++) {
		if (service_usable(added[i])) {
			shown[n] = added[i];
			entries[n++] = SERVICES[added[i]].entry;
		}
	}
	// Two columns and three rows, exactly like Music and Wireless, so a tile
	// here is the same size as a tile there whatever number of them there is.
	gridpage_set_entries(tile_grid, page_cfg, entries, n, 2, 3, true);
	for (int i = 0; i < n; i++) {
		lv_obj_add_event_cb(lv_obj_get_child(tile_grid, i), tile_long_pressed_cb, LV_EVENT_LONG_PRESSED,
							(void *)(intptr_t)i);
	}
	lv_obj_set_hidden(empty_note, n > 0);
}

// Taking a tile off. The service itself is untouched -- a Tidal account stays
// signed in, the podcasts stay subscribed -- so adding it back later finds
// everything as it was. Nothing is lost, so 2.0 asks nothing first: the tile
// goes at once and the toast offers it back (principle 6: a change is a toast
// with Undo; confirmation is for what cannot be undone).
static service_t removing;
static int removed_at = -1; // where it stood, for Undo

static void undo_remove(void *user) {
	(void)user;
	if (removed_at < 0 || is_added(removing) || added_n >= SVC_COUNT) {
		return;
	}
	int at = removed_at > added_n ? added_n : removed_at;
	memmove(&added[at + 1], &added[at], (size_t)(added_n - at) * sizeof(*added));
	added[at] = removing;
	added_n++;
	removed_at = -1;
	tiles_save();
	tiles_rebuild();
}

static void remove_action(void *user) {
	(void)user;
	for (int i = 0; i < added_n; i++) {
		if (added[i] == removing) {
			memmove(&added[i], &added[i + 1], (size_t)(added_n - i - 1) * sizeof(*added));
			added_n--;
			removed_at = i;
			break;
		}
	}
	tiles_save();
	tiles_rebuild();
	char line[200];
	lv_snprintf(line, sizeof(line), tr("streaming_tile_removed"), tr(SERVICES[removing].entry.label));
	sx_toast_undo(line, undo_remove, NULL);
}

static void tile_long_pressed_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active() || coverflow_drag_active()) {
		return; // a swipe that lingered, not a press on the tile
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= SVC_COUNT) {
		return;
	}
	// The lift that ends this press would otherwise arrive as a tap and open
	// the service under the sheet.
	lv_indev_t *indev = lv_indev_active();
	if (indev) {
		lv_indev_wait_release(indev);
	}
	removing = shown[index];
	sx_sheet_item_t items[] = {
		{.label = tr("streaming_remove_tile"), .icon = &icon_x_small, .destructive = true, .action = remove_action},
	};
	sx_sheet_menu(tr(SERVICES[removing].entry.label), tr("streaming_remove_note"), items, 1);
}

// ---------------------------------------------------------------------------
// "Add a service"
//
// Every service not on the page yet, as the handoff's cards: its name, a word
// on what it is, and a plus. One the firmware has no keys for is listed all
// the same, dimmed and saying why, so the user knows it exists and why it
// cannot be had, rather than wondering where it went.
// ---------------------------------------------------------------------------

static lv_obj_t *add_list;

// What each service is, in a few words, under its name. The keys sit in an
// array so the string extractor finds them (see radiopage.c).
static const char *const ABOUT[SVC_COUNT] = {
	[SVC_TIDAL] = "streaming_about_tidal",		 [SVC_QOBUZ] = "streaming_about_qobuz",
	[SVC_RADIO] = "streaming_about_radio",		 [SVC_PODCASTS] = "streaming_about_podcasts",
	[SVC_SUBSONIC] = "streaming_about_subsonic", [SVC_YANDEX] = "streaming_about_yandex",
#ifdef SONIX_ZVUK
	[SVC_ZVUK] = "streaming_about_zvuk",
#endif
};

static void add_row_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	service_t s = (service_t)(intptr_t)lv_event_get_user_data(e);
	if (!service_usable(s)) {
		toast_error("streaming_no_keys");
		return;
	}
	if (!is_added(s) && added_n < SVC_COUNT) {
		added[added_n++] = s;
		tiles_save();
		tiles_rebuild();
	}
	// Straight back to the page with the new tile on it: adding is done one
	// service at a time, and the page is where the user wanted to be.
	switch_screen_return_to(streaming_screen);
}

static void add_row(service_t s) {
	bool usable = service_usable(s);
	lv_obj_t *card = sx_card_create(add_list);
	sx_card_title(card, tr(SERVICES[s].entry.label));
	sx_card_subtitle(card, tr(usable ? ABOUT[s] : "streaming_no_keys"), false);
	sx_card_on_click(card, add_row_cb, (void *)(intptr_t)s);
	if (!usable) {
		// Dimmed, and a tap says why rather than doing nothing.
		sx_card_set_dim(card, true);
		return;
	}
	lv_obj_t *head = lv_obj_get_child(card, 0);
	lv_obj_t *plus = lv_image_create(head);
	lv_image_set_src(plus, &icon_plus);
	lv_obj_add_style(plus, &sx_style_icon_accent, 0);
}

static void add_rows_build(void) {
	lv_obj_clean(add_list);
	int listed = 0;
	// Usable ones first, so what can be added is at the top of the list.
	for (int pass = 0; pass < 2; pass++) {
		for (int s = 0; s < SVC_COUNT; s++) {
			if (!is_added((service_t)s) && service_usable((service_t)s) == (pass == 0)) {
				add_row((service_t)s);
				listed++;
			}
		}
	}
	if (listed == 0) {
		svcui_note(add_list, tr("streaming_all_added"));
	}
	lv_obj_scroll_to_y(add_list, 0, LV_ANIM_OFF);
}

static void open_add_cb(lv_event_t *e) {
	(void)e;
	// Built at each opening rather than kept: six cards cost nothing to make,
	// and the list depends on what was added or taken off since.
	add_rows_build();
	switch_screen(add_screen);
}

// Downloads: every service's cache and what is coming down, in one place (see
// downloadspage.h).
static void open_downloads_cb(lv_event_t *e) {
	(void)e;
	downloadspage_open(page_cfg);
}

// ---------------------------------------------------------------------------
// Accounts brought on the card (cardimport.h): what became of the file, said
// in a toast whatever page is up. When it signed a service in and its page is
// the one on screen, the page is shown again, signed in. Not for the outcomes
// that leave the file in place: the page would look for it again on being
// shown, and round it would go.
// ---------------------------------------------------------------------------

typedef struct {
	char service[16];
	cardimport_result_t result;
	char message[256];
} import_note_t;

static void import_note_async(void *user) {
	import_note_t *n = user;
	const char *text = n->message[0] ? n->message : tr("streaming_import_failed");
	if (n->result == CARDIMPORT_OK) {
		toast_success(text);
		for (int s = 0; s < SVC_COUNT; s++) {
			lv_obj_t *screen = SERVICES[s].entry.target ? *SERVICES[s].entry.target : NULL;
			if (strcmp(SERVICES[s].id, n->service) == 0 && screen && lv_screen_active() == screen) {
				lv_obj_send_event(screen, LV_EVENT_SCREEN_LOADED, NULL);
			}
		}
	} else {
		toast_error(text);
	}
	free(n);
}

static void import_listener(const char *service, cardimport_result_t result, const char *message) {
	import_note_t *n = malloc(sizeof(*n));
	if (!n) {
		return;
	}
	snprintf(n->service, sizeof(n->service), "%s", service);
	n->result = result;
	snprintf(n->message, sizeof(n->message), "%s", message ? message : "");
	if (!gui_post(import_note_async, n)) {
		free(n);
	}
}

void streaming_init(gui_config_t *cfg) {
	qobuzpage_init(cfg);
	tidalpage_init(cfg);
	podcastpage_init(cfg);
	subsonicpage_init(cfg);
	yandexpage_init(cfg);
#ifdef SONIX_ZVUK
	zvukpage_init(cfg);
#endif
	cardimport_add_listener(import_listener);

	page_cfg = cfg;
	tile_grid = gridpage_build(streaming_screen, cfg, NULL, 0, 2, 3, true);
#ifdef SONIX_ZVUK
	// Seven services do not fit three rows of two: the rest scroll.
	gridpage_set_scrolling(tile_grid);
#endif

	// With every tile taken off, the page says where they come back from.
	empty_note = lv_label_create(streaming_screen);
	lv_label_set_text(empty_note, tr("streaming_empty"));
	lv_label_set_long_mode(empty_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(empty_note, cfg->screen_width - 4 * cfg->padding);
	lv_obj_set_style_text_align(empty_note, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(empty_note, &sx_style_text2, 0);
	lv_obj_set_style_text_font(empty_note, &font_ui_24, 0);
	lv_obj_align(empty_note, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_clickable(empty_note, false);

	tiles_load();
	tiles_rebuild();

	lv_obj_t *title = settingsrow_title(streaming_screen, cfg, "streaming");
	settingsrow_title_corner_slots(title, cfg, 2);

	svcui_header_button(streaming_screen, cfg, 0, &icon_plus, open_add_cb, NULL);
	svcui_header_button(streaming_screen, cfg, 1, &icon_download, open_downloads_cb, NULL);

	add_screen = lv_obj_create(NULL);
	add_list = settingsrow_page(add_screen, cfg, "streaming_add_service");
	settingsrow_title_corner_slots(settingsrow_page_title(add_screen), cfg, 0);
	switcher_attach_back_gesture(add_screen);
}

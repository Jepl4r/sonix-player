#include "streaming.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/streaming/downloadspage.h"
#include "src/gui/streaming/podcastpage.h"
#include "src/gui/streaming/qobuzpage.h"
#include "src/gui/streaming/radiopage.h"
#include "src/gui/streaming/subsonicpage.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/streaming/tidalpage.h"
#include "src/gui/streaming/yandexpage.h"
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
// everything as it was.
static service_t removing;

static void remove_confirmed(void *user) {
	(void)user;
	for (int i = 0; i < added_n; i++) {
		if (added[i] == removing) {
			memmove(&added[i], &added[i + 1], (size_t)(added_n - i - 1) * sizeof(*added));
			added_n--;
			break;
		}
	}
	tiles_save();
	tiles_rebuild();
}

static void remove_action(void *user) {
	(void)user;
	char title[160];
	lv_snprintf(title, sizeof(title), tr("streaming_remove_question"), tr(SERVICES[removing].entry.label));
	confirm_show(title, "streaming_remove_note", "remove", remove_confirmed, NULL);
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
	// the service under the menu.
	lv_indev_t *indev = lv_indev_active();
	if (indev) {
		lv_indev_wait_release(indev);
	}
	removing = shown[index];
	static const popover_item_t items[] = {
		{"streaming_remove_tile", remove_action, NULL},
	};
	popover_show(lv_event_get_current_target(e), items, 1);
}

// ---------------------------------------------------------------------------
// The list behind the "+"
//
// Every service not on the page yet, each with its picture and name. One the
// firmware has no keys for is listed all the same, dimmed and saying why, so
// the user knows it exists and why it cannot be had, rather than wondering
// where it went.
// ---------------------------------------------------------------------------

#define ADD_ROW_HEIGHT 88
#define ADD_ICON_BOX 56

static lv_obj_t *add_list;

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

	lv_obj_t *row = lv_btn_create(add_list);
	lv_obj_set_size(row, lv_pct(100), ADD_ROW_HEIGHT);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, 12, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, 16, 0);
	lv_obj_set_style_pad_ver(row, 0, 0);
	lv_obj_set_style_pad_column(row, 14, 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_event_bubble(row, true);
	lv_obj_add_event_cb(row, add_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)s);

	// The tile's own picture at half size, in its own colours, as on the
	// Music page's arrangement list.
	lv_obj_t *box = lv_obj_create(row);
	lv_obj_remove_style_all(box);
	lv_obj_set_size(box, ADD_ICON_BOX, ADD_ICON_BOX);
	lv_obj_set_clickable(box, false);
	lv_obj_t *icon = lv_image_create(box);
	lv_image_set_src(icon, SERVICES[s].entry.icon);
	lv_image_set_scale(icon, LV_SCALE_NONE / 2);
	lv_obj_center(icon);
	if (!usable) {
		lv_obj_set_style_image_opa(icon, LV_OPA_40, 0);
	}

	// Name, and under it why it cannot be added when it cannot.
	lv_obj_t *text = lv_obj_create(row);
	lv_obj_remove_style_all(text);
	lv_obj_set_flex_grow(text, 1);
	lv_obj_set_height(text, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(text, 2, 0);
	lv_obj_set_clickable(text, false);
	lv_obj_set_event_bubble(text, true);

	lv_obj_t *name = lv_label_create(text);
	lv_label_set_text(name, tr(SERVICES[s].entry.label));
	lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
	lv_obj_set_width(name, lv_pct(100));
	lv_obj_add_style(name, usable ? &theme_style_text : &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(name, &font_ui_24, 0);

	if (!usable) {
		lv_obj_t *why = lv_label_create(text);
		lv_label_set_text(why, tr("streaming_no_keys"));
		lv_label_set_long_mode(why, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(why, lv_pct(100));
		lv_obj_add_style(why, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(why, &font_ui_20, 0);
		return;
	}

	lv_obj_t *plus = lv_image_create(row);
	lv_image_set_src(plus, &icon_plus);
	lv_obj_add_style(plus, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor(plus, theme()->accent, 0);
	lv_obj_set_style_image_recolor_opa(plus, LV_OPA_COVER, 0);
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
		lv_obj_t *note = lv_label_create(add_list);
		lv_label_set_text(note, tr("streaming_all_added"));
		lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(note, lv_pct(100));
		lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_add_style(note, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(note, &font_ui_24, 0);
		lv_obj_set_style_pad_top(note, 40, 0);
	}
	lv_obj_scroll_to_y(add_list, 0, LV_ANIM_OFF);
}

static void open_add_cb(lv_event_t *e) {
	(void)e;
	// Built at each opening rather than kept: six rows cost nothing to make,
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

// The corner buttons, the same shape as the Music page's search and settings:
// slot 0 in the corner, slot 1 to its left.
static lv_obj_t *corner_button(gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph) {
	lv_obj_t *button = lv_btn_create(streaming_screen);
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

void streaming_init(gui_config_t *cfg) {
	qobuzpage_init(cfg);
	tidalpage_init(cfg);
	podcastpage_init(cfg);
	subsonicpage_init(cfg);
	yandexpage_init(cfg);

	page_cfg = cfg;
	tile_grid = gridpage_build(streaming_screen, cfg, NULL, 0, 2, 3, true);

	// With every tile taken off, the page says where they come back from.
	empty_note = lv_label_create(streaming_screen);
	lv_label_set_text(empty_note, tr("streaming_empty"));
	lv_label_set_long_mode(empty_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(empty_note, cfg->screen_width - 4 * cfg->padding);
	lv_obj_set_style_text_align(empty_note, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(empty_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(empty_note, &font_ui_24, 0);
	lv_obj_align(empty_note, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_clickable(empty_note, false);

	tiles_load();
	tiles_rebuild();

	lv_obj_t *title = settingsrow_title(streaming_screen, cfg, "streaming");
	settingsrow_title_corner_slots(title, cfg, 2);

	lv_obj_t *add_btn = corner_button(cfg, 0, &icon_plus);
	lv_obj_add_event_cb(add_btn, open_add_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *downloads_btn = corner_button(cfg, 1, &icon_download);
	lv_obj_add_event_cb(downloads_btn, open_downloads_cb, LV_EVENT_CLICKED, NULL);

	add_screen = lv_obj_create(NULL);
	add_list = settingsrow_page(add_screen, cfg, "streaming_add_service");
	settingsrow_title_corner_slots(settingsrow_page_title(add_screen), cfg, 0);
	switcher_attach_back_gesture(add_screen);
}

#include "search.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/library/medialist.h"
#include "src/gui/library/playlistpage.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/uiscale.h"
#include "src/system/core/lang.h"
#include "src/system/library/library.h"
#include "src/system/library/playlists.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"

lv_obj_t *search_screen;

#define SEARCH_MAX_PER_CATEGORY 12
#define SEARCH_DEBOUNCE_MS 350

// Artwork on the result rows: the same size the library lists use.
#define SEARCH_THUMB ui_px(56)
#define SEARCH_ROW_H ui_px(76)
#define SEARCH_THUMB_POLL_MS 200

static lv_obj_t *field;	  // the textarea the keyboard types into
static lv_obj_t *clear_btn; // the x that empties it
static keyboard_t *keyboard;
static lv_obj_t *results; // scrollable column the hits are rebuilt into
static lv_timer_t *debounce_timer;
static void search_keyboard_show(bool visible);
static int results_top;	  // where the scrolling area starts
static int screen_h;
static int keyboard_height;

// ---------------------------------------------------------------------------
// Artwork for the track and album hits. A search shows at most twelve of each,
// which is exactly the coverloader pool reserved for this page, so every row
// that can carry a picture owns one slot for as long as the results stand.
// ---------------------------------------------------------------------------

typedef struct {
	lv_obj_t *icon;	   // the image widget on the row
	const lv_image_dsc_t *glyph; // what the row shows without a picture
	char path[512];	   // what to decode the artwork from
	bool requested;
	bool settled;	   // nothing more to wait for (arrived, or there is none)
	bool has_image;
	cover_image_t image;
} art_row_t;

static art_row_t art_rows[COVERLOADER_SEARCH_COUNT];
static int art_row_count;
static lv_timer_t *thumb_timer;

// Lets go of every picture and every pending decode. Called before the rows
// themselves are deleted, so nothing points at freed pixels.
static void art_reset(void) {
	for (int i = 0; i < art_row_count; i++) {
		coverloader_release(COVERLOADER_SEARCH_BASE + i);
		if (art_rows[i].has_image) {
			// The widget is about to go, but drop the source first anyway,
			// and restore the glyph this row started with -- not a music note
			// on a row that was showing an album.
			if (art_rows[i].icon) {
				lv_image_set_src(art_rows[i].icon, art_rows[i].glyph ? art_rows[i].glyph : &icon_music2);
			}
			cover_free(&art_rows[i].image);
		}
	}
	memset(art_rows, 0, sizeof(art_rows));
	art_row_count = 0;
	if (thumb_timer) {
		lv_timer_pause(thumb_timer);
	}
}

// Gives the decoded pictures back without forgetting the rows.
//
// art_reset() above cannot be used for this: it clears the whole table, so the
// paths go with the pixels and nothing could ever ask for them again. Here the
// row keeps its widget and its path and only loses the picture, which is what
// makes coming back to the page a re-decode rather than an empty list.
//
// Twenty-four rows of 56x56 is about 150 kB, and the next search is the only
// other moment at which the page lets go of it.
static void art_drop_pictures(void) {
	for (int i = 0; i < art_row_count; i++) {
		art_row_t *row = &art_rows[i];
		coverloader_release(COVERLOADER_SEARCH_BASE + i);
		if (row->has_image) {
			if (row->icon) {
				lv_image_set_src(row->icon, row->glyph ? row->glyph : &icon_music2);
				lv_obj_set_style_image_recolor_opa(row->icon, LV_OPA_COVER, 0);
			}
			cover_free(&row->image);
		}
		row->has_image = false;
		row->requested = false;
		row->settled = false; // so the next visit asks again
	}
	if (thumb_timer) {
		lv_timer_pause(thumb_timer);
	}
}

// Same wait as the streaming pages, and for the same reason: tapping a hit goes
// to the player, and coming straight back must not cost a dozen decodes off the
// card.
#define ART_DROP_DELAY_MS 20000

static lv_timer_t *art_drop_timer;

static void art_drop_cb(lv_timer_t *timer) {
	lv_timer_pause(timer);
	art_drop_pictures();

	// And back to the kernel, not merely back to the arena. A thumbnail is
	// 7200 bytes, well under the 96 kB mmap threshold main.c sets, so the
	// thumbnails come out of the heap rather than out of their own mappings
	// and freeing them alone leaves the arena holding the space.
	malloc_trim(0);
}

// Hands each row's artwork request to the loader and collects the results as
// they arrive.
static void thumbs_update(void) {
	bool pending = false;

	for (int i = 0; i < art_row_count; i++) {
		art_row_t *row = &art_rows[i];
		if (row->settled || !row->icon || !row->path[0]) {
			continue;
		}

		int slot = COVERLOADER_SEARCH_BASE + i;
		if (!row->requested) {
			coverloader_request(slot, row->path, SEARCH_THUMB);
			row->requested = true;
		}

		bool finished = false;
		cover_image_t image;
		if (coverloader_take(slot, &image, &finished)) {
			row->image = image;
			row->has_image = true;
			row->settled = true;
			// Album art must not be tinted the way the glyphs are.
			lv_image_set_src(row->icon, &row->image.dsc);
			lv_obj_set_style_image_recolor_opa(row->icon, LV_OPA_TRANSP, 0);
		} else if (finished) {
			row->settled = true; // this file carries no artwork
		} else {
			pending = true;
		}
	}

	if (pending) {
		lv_timer_resume(thumb_timer);
	}
}

static void thumb_timer_cb(lv_timer_t *timer) {
	lv_timer_pause(timer); // thumbs_update turns it back on while work remains
	if (lv_screen_active() == search_screen) {
		thumbs_update();
	}
}

// One category's widgets are only created when it has hits, so the page
// never shows empty headings.
static lv_obj_t *section_tracks;
static lv_obj_t *section_albums;
static lv_obj_t *section_artists;

// ---------------------------------------------------------------------------
// result rows
// ---------------------------------------------------------------------------

static void track_clicked_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	const char *path = lv_event_get_user_data(e);
	if (path && path[0]) {
		player_play_file(path);
		switch_screen(player_screen);
	}
}

static void album_clicked_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	const char *name = lv_event_get_user_data(e);
	if (name && name[0]) {
		medialist_open(name, LIBRARY_LIST_TRACKS, LIBRARY_FILTER_ALBUM, name);
	}
}

static void artist_clicked_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	const char *name = lv_event_get_user_data(e);
	if (name && name[0]) {
		medialist_open(name, LIBRARY_LIST_TRACKS, LIBRARY_FILTER_ARTIST, name);
	}
}

// The strings rows point at must outlive the click; they are freed together
// on the next rebuild via this deletion hook.
static void row_user_free_cb(lv_event_t *e) { free(lv_event_get_user_data(e)); }

// ---------------------------------------------------------------------------
// the menu a track hit opens on a long press
// ---------------------------------------------------------------------------

// The popover holds at most six rows.
#define MENU_PLAYLISTS_MAX 6

static char menu_path[512];
static char menu_name[256];
static lv_obj_t *menu_anchor;
// The playlists that hold the track, gathered when the menu opens.
static char menu_playlists[MENU_PLAYLISTS_MAX][128];
static int menu_playlist_count;

static void menu_fav_action(void *user) {
	(void)user;
	if (menu_path[0]) {
		library_fav_toggle(menu_path, menu_name, "");
	}
}

// Right after the track playing, the same as the library lists' menu. The
// queue on disk is written now so a power-off does not lose the addition.
static void menu_queue_action(void *user) {
	(void)user;
	if (menu_path[0] && playlist_insert_next(menu_path)) {
		device_state_queue_changed();
		toast_success("added_to_the_queue");
	}
}

static void menu_playlist_action(void *user) {
	(void)user;
	if (menu_path[0]) {
		playlistpage_add_track(menu_path);
	}
}

static void menu_remove_from(const char *playlist) {
	if (playlists_remove_track(playlist, menu_path)) {
		toast_success("medialist_track_removed");
	} else {
		gui_notify_popup("medialist_remove_failed");
	}
}

static void menu_remove_one_action(void *user) {
	int i = (int)(intptr_t)user;
	if (i >= 0 && i < menu_playlist_count && menu_path[0]) {
		menu_remove_from(menu_playlists[i]);
	}
}

static void remove_menu_show_async(void *user) {
	(void)user;
	if (menu_playlist_count < 1 || !menu_anchor || !lv_obj_is_valid(menu_anchor)) {
		return;
	}
	popover_item_t items[MENU_PLAYLISTS_MAX];
	for (int i = 0; i < menu_playlist_count; i++) {
		items[i] = (popover_item_t){menu_playlists[i], menu_remove_one_action, (void *)(intptr_t)i, false};
	}
	popover_show(menu_anchor, items, menu_playlist_count);
}

// One playlist holds the track: it leaves that one. Several: a second menu
// names them, and the track leaves the one picked. That menu is built on the
// next pass of the loop, because this runs inside the click of a row the
// popover is about to delete.
static void menu_remove_action(void *user) {
	(void)user;
	if (menu_playlist_count == 1) {
		menu_remove_from(menu_playlists[0]);
	} else if (menu_playlist_count > 1) {
		lv_async_call(remove_menu_show_async, NULL);
	}
}

static bool collect_playlist_cb(const char *name, void *user) {
	(void)user;
	if (playlists_has_track(name, menu_path)) {
		snprintf(menu_playlists[menu_playlist_count], sizeof(menu_playlists[0]), "%s", name);
		menu_playlist_count++;
	}
	return menu_playlist_count < MENU_PLAYLISTS_MAX;
}

// The row's user data is the path followed by the track's name, see make_row.
static void track_long_pressed_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	const char *path = lv_event_get_user_data(e);
	if (!path || !path[0]) {
		return;
	}

	// The lift that ends this press would otherwise arrive as a click and
	// start the track.
	lv_indev_t *indev = lv_indev_active();
	if (indev) {
		lv_indev_wait_release(indev);
	}

	snprintf(menu_path, sizeof(menu_path), "%s", path);
	snprintf(menu_name, sizeof(menu_name), "%s", path + strlen(path) + 1);
	menu_anchor = lv_event_get_current_target(e);

	menu_playlist_count = 0;
	playlists_for_each(collect_playlist_cb, NULL);

	popover_item_t items[4];
	int n = 0;
	items[n++] = (popover_item_t){
		library_fav_contains(menu_path) ? "remove_from_favourites" : "medialist_add_to_favourites", menu_fav_action, NULL,
		false};
	items[n++] = (popover_item_t){"add_to_queue", menu_queue_action, NULL, false};
	items[n++] = (popover_item_t){"add_to_playlist", menu_playlist_action, NULL, false};
	if (menu_playlist_count > 0) {
		items[n++] = (popover_item_t){"medialist_remove_from_playlist", menu_remove_action, NULL, false};
	}
	popover_show(menu_anchor, items, n);
}

static lv_obj_t *make_section(const char *title) {
	lv_obj_t *label = lv_label_create(results);
	lv_label_set_text(label, tr(title));
	lv_obj_add_style(label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_obj_set_style_pad_top(label, ui_px(8), 0);
	return label;
}

// One result row. `art_path` is the file its artwork comes from (a track's own
// file, an album's first track); NULL leaves the glyph in place. `long_cb`, when
// given, answers a long press. Both callbacks get `user_str` as their user data,
// with `name` stored right after its terminator.
static void make_row(const char *name, const lv_image_dsc_t *glyph, lv_event_cb_t cb, lv_event_cb_t long_cb,
					 const char *user_str, bool chevron, const char *art_path) {
	const char *user = user_str ? user_str : "";
	const char *named = name ? name : "";
	size_t user_len = strlen(user);
	char *copy = malloc(user_len + strlen(named) + 2);
	if (!copy) {
		return;
	}
	memcpy(copy, user, user_len + 1);
	strcpy(copy + user_len + 1, named);

	lv_obj_t *row = lv_btn_create(results);
	lv_obj_set_size(row, lv_pct(100), SEARCH_ROW_H);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, ui_px(12), 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, ui_px(16), 0);
	lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, ui_px(12), 0);
	lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, copy);
	if (long_cb) {
		lv_obj_add_event_cb(row, long_cb, LV_EVENT_LONG_PRESSED, copy);
	}
	lv_obj_add_event_cb(row, row_user_free_cb, LV_EVENT_DELETE, copy);

	lv_obj_t *icon = lv_image_create(row);
	lv_image_set_src(icon, glyph);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
	lv_obj_set_size(icon, SEARCH_THUMB, SEARCH_THUMB);
	lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
	lv_obj_set_style_radius(icon, ui_px(8), 0);
	lv_obj_set_style_clip_corner(icon, true, 0);

	// The artwork arrives later, from the loader thread; until then the glyph
	// stands in.
	if (art_path && art_path[0] && art_row_count < COVERLOADER_SEARCH_COUNT) {
		art_row_t *slot = &art_rows[art_row_count++];
		slot->icon = icon;
		slot->glyph = glyph;
		snprintf(slot->path, sizeof(slot->path), "%s", art_path);
		lv_timer_resume(thumb_timer);
	}

	lv_obj_t *label = lv_label_create(row);
	// An album comes named by its value (name and key, see library.h).
	char shown[256];
	library_album_title(name, shown, sizeof(shown));
	lv_label_set_text(label, shown);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_flex_grow(label, 1);
	lv_obj_set_height(label, ui_px(30));
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_24, 0);

	if (chevron) {
		lv_obj_t *chev = lv_image_create(row);
		lv_image_set_src(chev, &icon_chevron_right);
		lv_obj_add_style(chev, &theme_style_icon, 0);
		lv_obj_set_style_image_opa(chev, LV_OPA_60, 0);
	}
}

// ---------------------------------------------------------------------------
// running the query
// ---------------------------------------------------------------------------

static void search_hit_cb(library_search_type_t type, const char *name, const char *path, void *user) {
	(void)user;

	switch (type) {
	case LIBRARY_SEARCH_TRACK:
		if (!section_tracks) {
			section_tracks = make_section("search_tracks_2");
		}
		// A track's artwork comes from the track's own file.
		make_row(name, &icon_music2, track_clicked_cb, track_long_pressed_cb, path, false, path);
		break;
	case LIBRARY_SEARCH_ALBUM:
		if (!section_albums) {
			section_albums = make_section("albums");
		}
		// An album's comes from its first track, which the query brings along.
		// Until it does -- or if the record has no artwork at all -- the disc
		// glyph stands in, the same one the Album list uses.
		make_row(name, &icon_album, album_clicked_cb, NULL, name, true, path);
		break;
	case LIBRARY_SEARCH_ARTIST:
		if (!section_artists) {
			section_artists = make_section("artists");
		}
		// An artist row never carries artwork: the glyph is the final image,
		// not a stand-in.
		make_row(name, &icon_artist, artist_clicked_cb, NULL, name, true, NULL);
		break;
	}
}

static void run_search(void) {
	art_reset(); // pictures go before the widgets that point at them
	lv_obj_clean(results);
	lv_obj_scroll_to_y(results, 0, LV_ANIM_OFF);
	section_tracks = section_albums = section_artists = NULL;

	const char *query = lv_textarea_get_text(field);
	if (!query || !query[0]) {
		return;
	}

	int hits = library_search(query, SEARCH_MAX_PER_CATEGORY, search_hit_cb, NULL);
	if (hits == 0) {
		lv_obj_t *none = lv_label_create(results);
		lv_label_set_text(none, tr("no_results"));
		lv_obj_add_style(none, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(none, &font_ui_24, 0);
		lv_obj_set_style_pad_top(none, ui_px(16), 0);
	}
}

static void debounce_cb(lv_timer_t *timer) {
	lv_timer_pause(timer);
	run_search();
}

// The caret's own look: accent-coloured and a little thicker than LVGL's
// hairline, drawn as the cursor part's left border. Applied to the focused
// state too, because LVGL's built-in theme styles the cursor on
// LV_PART_CURSOR|LV_STATE_FOCUSED, and would otherwise hand the caret back to
// the default on the first tap in the field.
static void caret_apply_style(void) { keyboard_style_caret(field); }

// Shows the clear button only when there is something to clear.
static void refresh_clear_button(void) {
	if (!clear_btn) {
		return;
	}
	const char *text = lv_textarea_get_text(field);
	if (text && text[0]) {
		lv_obj_remove_flag(clear_btn, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(clear_btn, LV_OBJ_FLAG_HIDDEN);
	}
}

static void clear_clicked_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(field, "");
	lv_timer_pause(debounce_timer);
	run_search(); // empties the results with the query
	refresh_clear_button();
	search_keyboard_show(true); // ready for the next query
}

static void field_changed_cb(lv_event_t *e) {
	(void)e;
	refresh_clear_button();
	// Re-arm rather than query per keystroke: the LIKE scan runs once the
	// typing pauses for a beat.
	lv_timer_reset(debounce_timer);
	lv_timer_resume(debounce_timer);
}


// The accent key on the keyboard: run the query now and hand the page to the
// results.
static void kb_search_cb(lv_event_t *e) {
	(void)e;
	lv_timer_pause(debounce_timer);
	run_search();
	search_keyboard_show(false); // the results get the whole page
}

// The results area stops exactly where the keyboard starts, and grows into the
// freed space when the keyboard goes away. Full height with a keyboard's worth
// of bottom padding instead would let the list scroll far past its last row and
// slide the hits under the keys.
static void layout_results(void) {
	bool kb_visible = keyboard_is_visible(keyboard);
	int height = screen_h - results_top - (kb_visible ? keyboard_height : 0);
	if (height < 60) {
		height = 60;
	}
	lv_obj_set_height(results, height);
}

static void search_keyboard_show(bool visible) {
	keyboard_set_visible(keyboard, visible);
	layout_results();
}

static void field_clicked_cb(lv_event_t *e) {
	(void)e;
	search_keyboard_show(true);
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	// A fresh visit starts with the keyboard up and the cursor in the field.
	search_keyboard_show(true);

	if (art_drop_timer) {
		lv_timer_pause(art_drop_timer); // back before the wait ran out
	}
	if (art_row_count > 0 && thumb_timer) {
		lv_timer_resume(thumb_timer); // whatever was dropped is asked for again
	}
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	if (art_drop_timer) {
		lv_timer_reset(art_drop_timer);
		lv_timer_resume(art_drop_timer);
	}
}

// ---------------------------------------------------------------------------

void search_open(void) { switch_screen(search_screen); }

void search_init(gui_config_t *cfg) {
	search_screen = lv_obj_create(NULL);
	lv_obj_add_style(search_screen, &theme_style_screen, 0);
	settingsrow_title(search_screen, cfg, "search_2");

	int top = settingsrow_content_top(cfg);
	int keyboard_h = ui_px(316); // tall enough for honest fingertip-sized keys
	screen_h = cfg->screen_height;
	keyboard_height = keyboard_h;
	results_top = top + ui_px(62) + ui_px(10);

	field = lv_textarea_create(search_screen);
	lv_textarea_set_one_line(field, true);
	lv_textarea_set_placeholder_text(field, tr("search"));
	// Tall enough for the 24 px line plus its padding: at 56 the text does not
	// quite fit and the content bobs on every keystroke.
	lv_obj_set_size(field, cfg->screen_width - 2 * cfg->padding, ui_px(62));
	lv_obj_set_scrollbar_mode(field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(field, LV_ALIGN_TOP_LEFT, cfg->padding, top);
	lv_obj_add_style(field, &theme_style_card, 0);
	lv_obj_set_style_radius(field, ui_px(12), 0);
	lv_obj_set_style_border_width(field, 0, 0);
	lv_obj_set_style_shadow_width(field, 0, 0);
	lv_obj_set_style_pad_all(field, ui_px(14), 0);
	lv_obj_set_style_text_font(field, &font_ui_24, 0);
	lv_obj_add_event_cb(field, field_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(field, field_clicked_cb, LV_EVENT_CLICKED, NULL);

	caret_apply_style();

	// The clear button, over the field's right edge: wipes the query and the
	// results in one tap. Hidden while the field is empty.
	clear_btn = lv_btn_create(search_screen);
	lv_obj_set_size(clear_btn, ui_px(56), ui_px(56));
	lv_obj_align(clear_btn, LV_ALIGN_TOP_RIGHT, -cfg->padding - ui_px(4), top + (ui_px(62) - ui_px(56)) / 2);
	lv_obj_set_style_bg_opa(clear_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(clear_btn, 0, 0);
	lv_obj_set_style_border_width(clear_btn, 0, 0);
	lv_obj_set_style_pad_all(clear_btn, 0, 0);
	lv_obj_add_flag(clear_btn, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_event_cb(clear_btn, clear_clicked_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *clear_icon = lv_image_create(clear_btn);
	lv_image_set_src(clear_icon, &icon_clear);
	lv_obj_add_style(clear_icon, &theme_style_icon, 0);
	lv_obj_set_style_image_opa(clear_icon, LV_OPA_70, 0);
	lv_obj_center(clear_icon);

	// Room for it, so a long query never runs underneath the x.
	lv_obj_set_style_pad_right(field, ui_px(60), 0);

	results = lv_obj_create(search_screen);
	lv_obj_set_width(results, cfg->screen_width);
	lv_obj_align(results, LV_ALIGN_TOP_LEFT, 0, results_top);
	lv_obj_set_style_bg_opa(results, 0, 0);
	lv_obj_set_style_border_width(results, 0, 0);
	lv_obj_set_style_radius(results, 0, 0);
	lv_obj_set_style_pad_hor(results, cfg->padding, 0);
	lv_obj_set_style_pad_bottom(results, ui_px(12), 0);
	lv_obj_set_style_pad_gap(results, ui_px(8), 0);
	lv_obj_set_scroll_dir(results, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(results, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_flex_flow(results, LV_FLEX_FLOW_COLUMN);
	lv_obj_add_flag(results, LV_OBJ_FLAG_EVENT_BUBBLE);

	keyboard = keyboard_create(search_screen, cfg->screen_width, keyboard_h, field, &icon_search, NULL, kb_search_cb,
							   NULL);
	layout_results();

	debounce_timer = lv_timer_create(debounce_cb, SEARCH_DEBOUNCE_MS, NULL);
	lv_timer_pause(debounce_timer);

	thumb_timer = lv_timer_create(thumb_timer_cb, SEARCH_THUMB_POLL_MS, NULL);
	lv_timer_pause(thumb_timer);
	art_drop_timer = lv_timer_create(art_drop_cb, ART_DROP_DELAY_MS, NULL);
	lv_timer_pause(art_drop_timer);

	lv_obj_add_event_cb(search_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(search_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(search_screen);
	player_sheet_attach_drag(search_screen, true);
}

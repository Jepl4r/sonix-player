#include "podcastsaved.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/uiscale.h"
#include "src/system/core/lang.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/podcastdl.h"

#define SAVED_ROW_HEIGHT ui_px(88)
#define SAVED_THUMB ui_px(60)
#define SAVED_MAX_SHOWS 128
#define SAVED_MAX_EPISODES 400

lv_obj_t *podcastsaved_screen;
static lv_obj_t *episodes_screen;

static gui_config_t *g_cfg;
static lv_obj_t *shows_list;
static lv_obj_t *shows_empty;
static lv_obj_t *episodes_list;
static lv_obj_t *episodes_title;

typedef struct {
	char name[256];
	int episodes;
	cover_image_t thumb;
} show_t;

typedef struct {
	char *path;
	time_t when;
} episode_t;

static show_t *shows;
static int show_count;
static episode_t episodes[SAVED_MAX_EPISODES];
static int episode_count;
static char open_show[256];
static cover_image_t episode_thumb;
static int menu_episode = -1;

// ---------------------------------------------------------------------------
// reading the card
// ---------------------------------------------------------------------------

static int count_episodes(const char *folder) {
	DIR *dir = opendir(folder);
	if (!dir) {
		return 0;
	}
	int n = 0;
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (de->d_name[0] != '.' && playlist_is_playable_file(de->d_name)) {
			n++;
		}
	}
	closedir(dir);
	return n;
}

static int show_cmp(const void *a, const void *b) {
	return strcasecmp(((const show_t *)a)->name, ((const show_t *)b)->name);
}

static void shows_free(void) {
	for (int i = 0; i < show_count; i++) {
		cover_free(&shows[i].thumb);
	}
	show_count = 0;
}

// Every folder under Podcast holding at least one episode, alphabetically.
static void shows_read(void) {
	shows_free();
	if (!shows) {
		shows = calloc(SAVED_MAX_SHOWS, sizeof(*shows));
		if (!shows) {
			return;
		}
	}
	const char *root = podcastdl_root();
	DIR *dir = root[0] ? opendir(root) : NULL;
	if (!dir) {
		return;
	}
	struct dirent *de;
	while ((de = readdir(dir)) != NULL && show_count < SAVED_MAX_SHOWS) {
		if (de->d_name[0] == '.' || strlen(de->d_name) >= sizeof(shows[0].name)) {
			continue;
		}
		char path[800];
		snprintf(path, sizeof(path), "%s/%s", root, de->d_name);
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
			continue;
		}
		int n = count_episodes(path);
		if (n == 0) {
			continue;
		}
		show_t *s = &shows[show_count++];
		memset(s, 0, sizeof(*s));
		snprintf(s->name, sizeof(s->name), "%s", de->d_name);
		s->episodes = n;
	}
	closedir(dir);
	qsort(shows, (size_t)show_count, sizeof(*shows), show_cmp);
}

static int episode_cmp(const void *a, const void *b) {
	time_t x = ((const episode_t *)a)->when, y = ((const episode_t *)b)->when;
	return x < y ? 1 : x > y ? -1 : 0;
}

static void episodes_free(void) {
	for (int i = 0; i < episode_count; i++) {
		free(episodes[i].path);
	}
	episode_count = 0;
	cover_free(&episode_thumb);
}

// The episodes of the open podcast, newest first: podcastdl.c dates each file
// to the episode's publication.
static void episodes_read(void) {
	episodes_free();
	char folder[800];
	snprintf(folder, sizeof(folder), "%s/%s", podcastdl_root(), open_show);
	DIR *dir = opendir(folder);
	if (!dir) {
		return;
	}
	struct dirent *de;
	while ((de = readdir(dir)) != NULL && episode_count < SAVED_MAX_EPISODES) {
		if (de->d_name[0] == '.' || !playlist_is_playable_file(de->d_name)) {
			continue;
		}
		char path[1100];
		snprintf(path, sizeof(path), "%s/%s", folder, de->d_name);
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
			continue;
		}
		char *copy = strdup(path);
		if (!copy) {
			break;
		}
		episodes[episode_count].path = copy;
		episodes[episode_count].when = st.st_mtime;
		episode_count++;
	}
	closedir(dir);
	qsort(episodes, (size_t)episode_count, sizeof(*episodes), episode_cmp);
	cover_thumb_load(folder, true, SAVED_THUMB, &episode_thumb);
}

// ---------------------------------------------------------------------------
// the rows
// ---------------------------------------------------------------------------

// "12 March 2026", in the interface language (see podcastpage.c).
static void format_date(time_t when, char *out, size_t size) {
	static const char *const MONTHS[12] = {"podcast_january", "podcast_february", "podcast_march",
										   "podcast_april",	  "podcast_may",	  "podcast_june",
										   "podcast_july",	  "podcast_august",	  "podcast_september",
										   "podcast_october", "podcast_november", "podcast_december"};
	struct tm tm_when;
	if (when <= 0 || !localtime_r(&when, &tm_when)) {
		out[0] = '\0';
		return;
	}
	snprintf(out, size, "%d %s %d", tm_when.tm_mday, tr(MONTHS[tm_when.tm_mon % 12]), tm_when.tm_year + 1900);
}

static void paint_thumb(lv_obj_t *thumb, const cover_image_t *image) {
	if (image && image->pixels) {
		lv_image_set_src(thumb, &image->dsc);
		lv_obj_set_style_radius(thumb, ui_px(6), 0);
		lv_obj_set_style_clip_corner(thumb, true, 0);
		return;
	}
	lv_image_set_src(thumb, &icon_podcast_list);
	lv_obj_add_style(thumb, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(thumb, LV_OPA_COVER, 0);
}

static lv_obj_t *make_row(lv_obj_t *parent, const cover_image_t *image, const char *title, const char *detail,
						  lv_event_cb_t cb, lv_event_cb_t long_cb, int index) {
	lv_obj_t *row = lv_btn_create(parent);
	lv_obj_set_size(row, lv_pct(100), SAVED_ROW_HEIGHT);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, ui_px(12), 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, ui_px(16), 0);
	lv_obj_set_style_pad_ver(row, 0, 0);
	lv_obj_set_style_pad_column(row, ui_px(12), 0);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);
	if (long_cb) {
		lv_obj_add_event_cb(row, long_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)index);
	}

	lv_obj_t *thumb = lv_image_create(row);
	lv_obj_set_size(thumb, SAVED_THUMB, SAVED_THUMB);
	lv_image_set_inner_align(thumb, LV_IMAGE_ALIGN_CENTER);
	lv_obj_add_flag(thumb, LV_OBJ_FLAG_EVENT_BUBBLE);
	paint_thumb(thumb, image);

	lv_obj_t *texts = lv_obj_create(row);
	lv_obj_remove_style_all(texts);
	lv_obj_set_flex_grow(texts, 1);
	lv_obj_set_height(texts, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_row(texts, ui_px(6), 0);
	lv_obj_remove_flag(texts, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(texts, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);

	lv_obj_t *name = lv_label_create(texts);
	lv_label_set_text(name, title);
	lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
	lv_obj_set_width(name, lv_pct(100));
	lv_obj_set_height(name, lv_font_get_line_height(&font_ui_22));
	lv_obj_add_style(name, &theme_style_text, 0);
	lv_obj_set_style_text_font(name, &font_ui_22, 0);

	if (detail && detail[0]) {
		lv_obj_t *sub = lv_label_create(texts);
		lv_label_set_text(sub, detail);
		lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
		lv_obj_set_width(sub, lv_pct(100));
		lv_obj_set_height(sub, lv_font_get_line_height(&font_ui_18));
		lv_obj_add_style(sub, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(sub, &font_ui_18, 0);
	}
	return row;
}

// The file name without its folder and extension: the episode's title.
static void episode_name(const char *path, char *out, size_t size) {
	const char *slash = strrchr(path, '/');
	snprintf(out, size, "%s", slash ? slash + 1 : path);
	char *dot = strrchr(out, '.');
	if (dot && dot != out) {
		*dot = '\0';
	}
}

// ---------------------------------------------------------------------------
// the podcasts
// ---------------------------------------------------------------------------

static void show_clicked_cb(lv_event_t *e) {
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	if (i < 0 || i >= show_count) {
		return;
	}
	snprintf(open_show, sizeof(open_show), "%s", shows[i].name);
	switch_screen(episodes_screen);
}

static void shows_fill(void) {
	lv_obj_clean(shows_list);
	shows_read();
	for (int i = 0; i < show_count; i++) {
		char folder[800];
		snprintf(folder, sizeof(folder), "%s/%s", podcastdl_root(), shows[i].name);
		cover_thumb_load(folder, true, SAVED_THUMB, &shows[i].thumb);
		char detail[64];
		if (shows[i].episodes == 1) {
			snprintf(detail, sizeof(detail), "%s", tr("podcast_one_episode"));
		} else {
			snprintf(detail, sizeof(detail), tr("podcast_episodes_2"), shows[i].episodes);
		}
		make_row(shows_list, &shows[i].thumb, shows[i].name, detail, show_clicked_cb, NULL, i);
	}
	if (show_count == 0) {
		lv_obj_remove_flag(shows_empty, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(shows_empty, LV_OBJ_FLAG_HIDDEN);
	}
}

static void shows_loaded_cb(lv_event_t *e) {
	(void)e;
	shows_fill();
}

// The rows go and the thumbnails with them: the page is read again on return.
static void shows_unloaded_cb(lv_event_t *e) {
	(void)e;
	lv_obj_clean(shows_list);
	shows_free();
}

// ---------------------------------------------------------------------------
// the episodes of one podcast
// ---------------------------------------------------------------------------

static void episodes_fill(void);

// The episodes in the order shown, from the one tapped onwards into the past,
// as the online list plays them.
static void episode_clicked_cb(lv_event_t *e) {
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	if (i < 0 || i >= episode_count) {
		return;
	}
	const char *paths[SAVED_MAX_EPISODES];
	for (int k = 0; k < episode_count; k++) {
		paths[k] = episodes[k].path;
	}
	device_state_play_list_ordered(paths, episode_count, i);
	player_refresh_now_playing();
	switch_screen(player_screen);
}

// A folder left with nothing but its artwork goes too.
static void remove_folder_if_empty(const char *folder) {
	if (count_episodes(folder) > 0) {
		return;
	}
	char cover[900];
	snprintf(cover, sizeof(cover), "%s/cover.jpg", folder);
	remove(cover);
	snprintf(cover, sizeof(cover), "%s/cover.png", folder);
	remove(cover);
	rmdir(folder);
}

static void delete_confirmed(void *user) {
	(void)user;
	if (menu_episode < 0 || menu_episode >= episode_count) {
		return;
	}
	if (remove(episodes[menu_episode].path) != 0) {
		toast_error(tr("card_write_failed"));
		return;
	}
	char tags[1200];
	snprintf(tags, sizeof(tags), "%s.tags", episodes[menu_episode].path);
	remove(tags);
	sync();
	char folder[800];
	snprintf(folder, sizeof(folder), "%s/%s", podcastdl_root(), open_show);
	remove_folder_if_empty(folder);
	menu_episode = -1;
	episodes_fill();
	if (episode_count == 0) {
		switch_screen_return_to(podcastsaved_screen);
	}
}

static void delete_cb(void *user) {
	(void)user;
	if (menu_episode < 0 || menu_episode >= episode_count) {
		return;
	}
	char name[256];
	episode_name(episodes[menu_episode].path, name, sizeof(name));
	confirm_show("podcast_delete_download", name, "delete", delete_confirmed, NULL);
}

static void episode_long_cb(lv_event_t *e) {
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	if (i < 0 || i >= episode_count) {
		return;
	}
	menu_episode = i;
	// The lift that ends this press would otherwise arrive as a click and
	// start the episode.
	lv_indev_t *indev = lv_indev_active();
	if (indev) {
		lv_indev_wait_release(indev);
	}
	static const popover_item_t ITEMS[] = {{"delete", delete_cb, NULL}};
	popover_show(lv_event_get_current_target(e), ITEMS, 1);
}

static void episodes_fill(void) {
	lv_obj_clean(episodes_list);
	episodes_read();
	lv_label_set_text(episodes_title, open_show);
	settingsrow_title_corner_slots(episodes_title, g_cfg, 0);
	for (int i = 0; i < episode_count; i++) {
		char name[256];
		char when[48];
		if (!podcastcache_tag(episodes[i].path, "title", name, sizeof(name))) {
			episode_name(episodes[i].path, name, sizeof(name));
		}
		format_date(episodes[i].when, when, sizeof(when));
		make_row(episodes_list, &episode_thumb, name, when, episode_clicked_cb, episode_long_cb, i);
	}
}

static void episodes_loaded_cb(lv_event_t *e) {
	(void)e;
	episodes_fill();
}

static void episodes_unloaded_cb(lv_event_t *e) {
	(void)e;
	lv_obj_clean(episodes_list);
	episodes_free();
}

// ---------------------------------------------------------------------------
// building
// ---------------------------------------------------------------------------

void podcastsaved_init(gui_config_t *cfg) {
	g_cfg = cfg;

	podcastsaved_screen = lv_obj_create(NULL);
	shows_list = settingsrow_page(podcastsaved_screen, cfg, "podcast_downloaded");
	lv_obj_set_flex_align(shows_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	player_sheet_attach_drag(shows_list, true);
	switcher_attach_back_gesture(podcastsaved_screen);
	lv_obj_add_event_cb(podcastsaved_screen, shows_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(podcastsaved_screen, shows_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

	shows_empty = lv_label_create(podcastsaved_screen);
	lv_label_set_text(shows_empty, tr("podcast_no_downloads"));
	lv_label_set_long_mode(shows_empty, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(shows_empty, cfg->screen_width - 4 * cfg->padding);
	lv_obj_set_style_text_align(shows_empty, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(shows_empty, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(shows_empty, &font_ui_24, 0);
	lv_obj_align(shows_empty, LV_ALIGN_TOP_MID, 0, settingsrow_content_top(cfg) + ui_px(140));
	lv_obj_add_flag(shows_empty, LV_OBJ_FLAG_HIDDEN);

	episodes_screen = lv_obj_create(NULL);
	episodes_list = settingsrow_page(episodes_screen, cfg, "podcasts");
	lv_obj_set_flex_align(episodes_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	episodes_title = settingsrow_page_title(episodes_screen);
	player_sheet_attach_drag(episodes_list, true);
	switcher_attach_back_gesture(episodes_screen);
	lv_obj_add_event_cb(episodes_screen, episodes_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(episodes_screen, episodes_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
}

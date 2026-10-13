#include "downloadspage.h"

#include <dirent.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/decode/growfile.h"
#include "src/system/device/power.h"
#include "src/system/streaming/provider.h"
#include "src/system/streaming/qobuz.h"
#include "src/system/streaming/qobuzcache.h"
#include "src/system/streaming/subsoniccache.h"
#include "src/system/streaming/tidal.h"
#include "src/system/streaming/tidalcache.h"
#include "src/system/streaming/yandex.h"
#include "src/system/streaming/yandexcache.h"
#include "src/system/streaming/yandexdl.h"

// See downloadspage.h.

// ---------------------------------------------------------------------------
// the services
//
// Named here rather than found in the provider registry because what the page
// needs of each -- its ceiling, its own "clear", whether this firmware can run
// it at all -- is not in stream_provider_t, and four entries do not justify
// growing that table for one page. The registry still answers where the cache
// is and whose a growing file is.
// ---------------------------------------------------------------------------

static bool always(void) { return true; }

typedef struct {
	const char *id;		  // the provider's id, and the translation key of its name
	long limit;			  // the cache's ceiling, bytes
	bool (*usable)(void); // the firmware has what the service needs
	void (*clear)(void);  // the service's own clear of its cache
} service_t;

static const service_t SERVICES[] = {
	{"qobuz", QOBUZCACHE_MAX_BYTES, qobuz_configured, qobuzcache_clear},
	{"tidal", TIDALCACHE_MAX_BYTES, tidal_configured, tidalcache_clear},
	{"subsonic", SUBSONICCACHE_MAX_BYTES, always, subsoniccache_clear},
	// Spares the file coming down (see yandexcache_clear). The save queue is
	// not touched: it writes into the music library, not the cache.
	{"yandex", YANDEXCACHE_MAX_BYTES, yandex_configured, yandexcache_clear},
};
#define SERVICE_COUNT ((int)(sizeof(SERVICES) / sizeof(SERVICES[0])))
#define YANDEX_INDEX 3

static const stream_provider_t *provider_of(int index) {
	for (int i = 0; i < stream_provider_count(); i++) {
		const stream_provider_t *p = stream_provider_at(i);
		if (p && p->id && strcmp(p->id, SERVICES[index].id) == 0) {
			return p;
		}
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// measuring the caches, off the interface thread
//
// A cache folder holds a few hundred files at most, but stat()ing them on a
// card that is being written to can take a while, and the interface must not
// wait for it. So a thread measures, at nice ten, and the page is told when
// the numbers are in. The same thread clears a cache when asked: removing
// hundreds of files is the same kind of wait.
// ---------------------------------------------------------------------------

typedef struct {
	long bytes;
	int tracks;
	bool known;
} usage_t;

static pthread_mutex_t usage_lock = PTHREAD_MUTEX_INITIALIZER;
static usage_t usage[SERVICE_COUNT];
static bool scanning;
static bool scan_again;
static int clear_pending = -1; // a service to clear before the next measure

static bool ends_with(const char *s, const char *suffix) {
	size_t n = strlen(s), m = strlen(suffix);
	return n >= m && strcmp(s + n - m, suffix) == 0;
}

// A track of the cache, not one of its companions: the tags and cover beside
// each track, the marker of a download that never finished, a file still
// being written under a temporary name.
static bool is_companion(const char *name) {
	static const char *const SUFFIXES[] = {".tags", ".jpg", ".png", ".part", ".tmp", ".incompleto", ".json"};
	for (size_t i = 0; i < sizeof(SUFFIXES) / sizeof(SUFFIXES[0]); i++) {
		if (ends_with(name, SUFFIXES[i])) {
			return true;
		}
	}
	return false;
}

static usage_t measure(const char *dir) {
	usage_t u = {.known = true};
	DIR *d = dir && dir[0] ? opendir(dir) : NULL;
	if (!d) {
		return u; // no card, or nothing cached yet: zero is the truth
	}
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') {
			continue;
		}
		char path[640];
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= sizeof(path)) {
			continue;
		}
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
			continue;
		}
		u.bytes += (long)st.st_size;
		if (is_companion(e->d_name)) {
			continue;
		}
		// A track whose download never finished keeps a marker beside it and
		// is not one the user has; nor is the one coming down now (Yandex
		// writes no marker), which the card shows on a line of its own.
		char marker[660];
		snprintf(marker, sizeof(marker), "%s.incompleto", path);
		if (access(marker, F_OK) != 0 && !growfile_is_growing(path)) {
			u.tracks++;
		}
	}
	closedir(d);
	return u;
}

static void usage_ready_async(void *user);

static void *scan_main(void *arg) {
	(void)arg;
	thread_be_low_priority("downloads scan");
	for (;;) {
		pthread_mutex_lock(&usage_lock);
		int clear = clear_pending;
		clear_pending = -1;
		pthread_mutex_unlock(&usage_lock);
		if (clear >= 0 && clear < SERVICE_COUNT) {
			printf("downloads: clearing the %s cache\n", SERVICES[clear].id);
			SERVICES[clear].clear();
		}

		usage_t got[SERVICE_COUNT];
		for (int i = 0; i < SERVICE_COUNT; i++) {
			const stream_provider_t *p = provider_of(i);
			got[i] = measure(p && p->cache_dir ? p->cache_dir() : NULL);
		}

		pthread_mutex_lock(&usage_lock);
		memcpy(usage, got, sizeof(usage));
		bool again = scan_again || clear_pending >= 0;
		scan_again = false;
		if (!again) {
			scanning = false;
		}
		pthread_mutex_unlock(&usage_lock);
		gui_post(usage_ready_async, (void *)(intptr_t)clear);
		if (!again) {
			break;
		}
	}
	return NULL;
}

// Measures again (and first clears `clear`, unless it is -1). A request while
// a pass is running is folded into one more pass.
static void scan_start(int clear) {
	pthread_mutex_lock(&usage_lock);
	if (clear >= 0) {
		clear_pending = clear;
	}
	if (scanning) {
		scan_again = true;
		pthread_mutex_unlock(&usage_lock);
		return;
	}
	scanning = true;
	pthread_mutex_unlock(&usage_lock);

	pthread_t th;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, 128 * 1024);
	if (pthread_create(&th, &attr, scan_main, NULL) != 0) {
		pthread_mutex_lock(&usage_lock);
		scanning = false;
		pthread_mutex_unlock(&usage_lock);
	}
	pthread_attr_destroy(&attr);
}

// ---------------------------------------------------------------------------
// the page
// ---------------------------------------------------------------------------

typedef struct {
	lv_obj_t *card;
	lv_obj_t *usage;
	lv_obj_t *now;
	lv_obj_t *save;		// Yandex only: the "Save to library" queue
	lv_obj_t *stop;		// and its stop button
	char now_path[512]; // the growing file `now_title` names
	char now_title[200];
	bool title_from_tags;
} card_t;

static gui_config_t *page_cfg;
static lv_obj_t *page_screen;
static lv_obj_t *page_list;
static lv_timer_t *refresh_timer;
static card_t cards[SERVICE_COUNT];

// What was growing at the last tick: a path that is gone means a download
// ended, and the cache it ended in has changed size.
static char last_growing[GROWFILE_MAX][512];
static int last_growing_count;

static void size_text(long bytes, char *out, size_t size) {
	// Rounded up: a track that is there never reads as "0 MB".
	long mb = (bytes + 1024L * 1024L - 1) / (1024L * 1024L);
	if (mb < 1024) {
		snprintf(out, size, tr("downloads_size_mb"), (int)mb);
	} else {
		long tenths = mb * 10 / 1024;
		snprintf(out, size, tr("downloads_size_gb"), (int)(tenths / 10), (int)(tenths % 10));
	}
}

static void refresh_usage(void) {
	usage_t now[SERVICE_COUNT];
	pthread_mutex_lock(&usage_lock);
	memcpy(now, usage, sizeof(now));
	pthread_mutex_unlock(&usage_lock);
	for (int i = 0; i < SERVICE_COUNT; i++) {
		if (!cards[i].usage) {
			continue;
		}
		if (!now[i].known) {
			lv_label_set_text(cards[i].usage, tr("downloads_measuring"));
			continue;
		}
		char used[32], limit[32];
		size_text(now[i].bytes, used, sizeof(used));
		size_text(SERVICES[i].limit, limit, sizeof(limit));
		lv_label_set_text_fmt(cards[i].usage, tr("downloads_cache_usage"), used, limit, now[i].tracks);
	}
}

static void usage_ready_async(void *user) {
	int cleared = (int)(intptr_t)user;
	if (cleared >= 0) {
		toast_success("downloads_cleared");
	}
	if (page_screen) {
		refresh_usage();
	}
}

// "Artist - Title" from the sidecar beside the track, or the file's name until
// the sidecar is there (it is written a moment after the download starts).
static void track_title(const char *growing, char *out, size_t size, bool *from_tags) {
	char path[512];
	snprintf(path, sizeof(path), "%s", growing);
	// Yandex writes a whole file under "<name>.part" and renames it when
	// complete; the sidecar is beside the final name.
	if (ends_with(path, ".part")) {
		path[strlen(path) - 5] = '\0';
	}
	char title[160] = "", artist[160] = "";
	char tags[600];
	snprintf(tags, sizeof(tags), "%.560s.tags", path);
	FILE *f = fopen(tags, "r");
	if (f) {
		char line[400];
		while (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\r\n")] = '\0';
			if (strncmp(line, "title=", 6) == 0) {
				snprintf(title, sizeof(title), "%.159s", line + 6);
			} else if (strncmp(line, "artist=", 7) == 0) {
				snprintf(artist, sizeof(artist), "%.159s", line + 7);
			}
		}
		fclose(f);
	}
	*from_tags = title[0] != '\0';
	// Bounded to what a line of the card can show anyway.
	if (title[0] && artist[0]) {
		snprintf(out, size, "%.90s \xE2\x80\x94 %.100s", artist, title);
	} else if (title[0]) {
		snprintf(out, size, "%.150s", title);
	} else {
		const char *slash = strrchr(path, '/');
		snprintf(out, size, "%.150s", slash ? slash + 1 : path);
	}
}

static void progress_text(long done, long total, char *out, size_t size) {
	if (total > 0) {
		long pct = done * 100 / total;
		snprintf(out, size, "%ld%%", pct > 100 ? 100 : pct);
	} else {
		// Unknown size (a Tidal DASH stream, a Yandex file): how much is in.
		size_text(done, out, size);
	}
}

static void refresh_live(void) {
	growfile_info_t growing[GROWFILE_MAX];
	int count = growfile_list(growing, GROWFILE_MAX);

	// A download that was there and is not any more has ended: the cache it
	// wrote to is measured again.
	bool ended = false;
	for (int i = 0; i < last_growing_count && !ended; i++) {
		bool still = false;
		for (int k = 0; k < count && !still; k++) {
			still = strcmp(growing[k].path, last_growing[i]) == 0;
		}
		ended = !still;
	}
	for (int i = 0; i < count; i++) {
		snprintf(last_growing[i], sizeof(last_growing[i]), "%.511s", growing[i].path);
	}
	last_growing_count = count;
	if (ended) {
		scan_start(-1);
	}

	for (int s = 0; s < SERVICE_COUNT; s++) {
		card_t *c = &cards[s];
		if (!c->now) {
			continue;
		}
		const stream_provider_t *p = provider_of(s);
		const growfile_info_t *mine = NULL;
		for (int i = 0; p && p->owns_path && i < count && !mine; i++) {
			if (p->owns_path(growing[i].path)) {
				mine = &growing[i];
			}
		}
		if (!mine) {
			c->now_path[0] = '\0';
			lv_obj_set_hidden(c->now, true);
			continue;
		}
		// The sidecar is read when the track changes, and again until it
		// has appeared -- not every second.
		if (strcmp(c->now_path, mine->path) != 0 || !c->title_from_tags) {
			snprintf(c->now_path, sizeof(c->now_path), "%s", mine->path);
			track_title(mine->path, c->now_title, sizeof(c->now_title), &c->title_from_tags);
		}
		char progress[32];
		progress_text(mine->done, mine->total, progress, sizeof(progress));
		char what[260];
		snprintf(what, sizeof(what), "%s \xC2\xB7 %s", c->now_title, progress);
		lv_label_set_text_fmt(c->now, tr("downloads_now"), what);
		lv_obj_set_hidden(c->now, false);
	}

	// Yandex's "Save to library" queue: its own status, read-only.
	card_t *y = &cards[YANDEX_INDEX];
	if (y->save) {
		yandexdl_status_t st;
		yandexdl_status(&st);
		if (st.active) {
			char what[200];
			if (st.title[0] && st.percent >= 0) {
				snprintf(what, sizeof(what), "%s \xC2\xB7 %d%%", st.title, st.percent);
			} else {
				snprintf(what, sizeof(what), "%s", st.title[0] ? st.title : "\xE2\x80\xA6");
			}
			char line[400];
			lv_snprintf(line, sizeof(line), tr("downloads_saving"), what);
			if (st.queued > 0) {
				size_t used = strlen(line);
				lv_snprintf(line + used, sizeof(line) - used, "\n");
				used = strlen(line);
				lv_snprintf(line + used, sizeof(line) - used, tr("downloads_saving_queue"), st.queued);
			}
			lv_label_set_text(y->save, line);
		}
		lv_obj_set_hidden(y->save, !st.active);
		lv_obj_set_hidden(y->stop, !st.active);
	}
}

static void refresh_cb(lv_timer_t *timer) {
	(void)timer;
	// Left open with the screen off, nobody is reading it: the tick stays
	// (a timer is nothing) but the work waits for the screen to come on.
	if (!power_screen_is_on()) {
		return;
	}
	refresh_live();
}

// --- the buttons -----------------------------------------------------------

static int clearing;

static void clear_confirmed(void *user) {
	(void)user;
	scan_start(clearing);
}

static void clear_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	clearing = (int)(intptr_t)lv_event_get_user_data(e);
	usage_t u;
	pthread_mutex_lock(&usage_lock);
	u = usage[clearing];
	pthread_mutex_unlock(&usage_lock);
	char title[160], size[32], note[300];
	lv_snprintf(title, sizeof(title), tr("downloads_clear_question"), tr(SERVICES[clearing].id));
	size_text(u.bytes, size, sizeof(size));
	lv_snprintf(note, sizeof(note), tr("downloads_clear_note"), size);
	confirm_show(title, note, "downloads_clear", clear_confirmed, NULL);
}

static void stop_saving_confirmed(void *user) {
	(void)user;
	yandexdl_cancel_all();
	refresh_live();
}

static void stop_saving_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	confirm_show("downloads_stop_saving_question", "downloads_stop_saving_note", "downloads_stop", stop_saving_confirmed, NULL);
}

// --- building --------------------------------------------------------------

static lv_obj_t *line_label(lv_obj_t *parent, bool dim) {
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_add_style(label, dim ? &theme_style_text_dim : &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_20, 0);
	lv_label_set_text(label, "");
	return label;
}

static lv_obj_t *small_button(lv_obj_t *parent, const char *key, lv_event_cb_t cb, void *user) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, LV_SIZE_CONTENT, 52);
	lv_obj_set_style_radius(btn, 12, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_pad_hor(btn, 20, 0);
	lv_obj_add_style(btn, &theme_style_switch, 0); // the quiet neutral fill
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);
	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, tr(key));
	lv_obj_set_style_text_font(label, &font_ui_20, 0);
	lv_obj_set_style_text_color(label, theme()->text_primary, 0);
	lv_obj_center(label);
	return btn;
}

static void build_card(int s) {
	card_t *c = &cards[s];
	c->card = lv_obj_create(page_list);
	lv_obj_add_style(c->card, &theme_style_card, 0);
	lv_obj_set_size(c->card, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_radius(c->card, 12, 0);
	lv_obj_set_style_border_width(c->card, 0, 0);
	lv_obj_set_style_pad_all(c->card, 16, 0);
	lv_obj_set_style_pad_row(c->card, 8, 0);
	lv_obj_set_flex_flow(c->card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scrollable(c->card, false);
	lv_obj_set_event_bubble(c->card, true);

	lv_obj_t *name = lv_label_create(c->card);
	lv_label_set_text(name, tr(SERVICES[s].id));
	lv_obj_add_style(name, &theme_style_text, 0);
	lv_obj_set_style_text_font(name, &font_ui_24, 0);

	c->usage = line_label(c->card, true);
	lv_label_set_text(c->usage, tr("downloads_measuring"));
	c->now = line_label(c->card, false);
	lv_obj_set_hidden(c->now, true);

	if (s == YANDEX_INDEX) {
		c->save = line_label(c->card, false);
		lv_obj_set_hidden(c->save, true);
	}

	lv_obj_t *buttons = lv_obj_create(c->card);
	lv_obj_remove_style_all(buttons);
	lv_obj_set_size(buttons, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_column(buttons, 10, 0);
	lv_obj_set_style_pad_row(buttons, 10, 0);
	lv_obj_set_style_pad_top(buttons, 4, 0);
	lv_obj_set_event_bubble(buttons, true);
	small_button(buttons, "downloads_clear_cache", clear_cb, (void *)(intptr_t)s);
	if (s == YANDEX_INDEX) {
		c->stop = small_button(buttons, "downloads_stop_saving", stop_saving_cb, NULL);
		lv_obj_set_hidden(c->stop, true);
	}
}

// Which cards to show: the services this firmware can run. Decided at each
// opening, so keys arriving with a firmware update need no restart of the
// page -- they do of the player.
static void rebuild_cards(void) {
	lv_obj_clean(page_list);
	memset(cards, 0, sizeof(cards));
	for (int s = 0; s < SERVICE_COUNT; s++) {
		if (SERVICES[s].usable()) {
			build_card(s);
		}
	}
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	lv_timer_resume(refresh_timer);
}

static void unloaded_cb(lv_event_t *e) {
	(void)e;
	// Nothing runs while the page is not on screen.
	lv_timer_pause(refresh_timer);
}

static void build_page(gui_config_t *cfg) {
	page_screen = lv_obj_create(NULL);
	page_list = settingsrow_page(page_screen, cfg, "downloads");
	settingsrow_title_corner_slots(settingsrow_page_title(page_screen), cfg, 0);
	refresh_timer = lv_timer_create(refresh_cb, 1000, NULL);
	lv_timer_pause(refresh_timer);
	lv_obj_add_event_cb(page_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(page_screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(page_screen);
}

void downloadspage_open(gui_config_t *cfg) {
	if (!page_screen) {
		page_cfg = cfg;
		build_page(cfg);
	}
	rebuild_cards();
	refresh_usage();
	last_growing_count = 0;
	refresh_live();
	scan_start(-1);
	switch_screen(page_screen);
}

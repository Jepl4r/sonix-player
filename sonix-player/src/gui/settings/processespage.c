#include "processespage.h"

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

lv_obj_t *processespage_screen;

// The page container (a scrolling column) and the parts rebuilt on every open:
// the RAM card at the top and the process list below it.
static lv_obj_t *page_container;
static lv_obj_t *ram_label;
static lv_obj_t *proc_list; // emptied and refilled on every SCREEN_LOADED

// A process, reduced to what is worth showing.
typedef struct {
	long rss_kb;
	char name[24];
} proc_row_t;

#define PROC_MAX 200	 // how many are collected
#define PROC_SHOWN 60	 // how many are shown (the largest)

// One line of /proc/meminfo, in kB. -1 when absent.
static long meminfo_kb(const char *key) {
	FILE *f = fopen("/proc/meminfo", "r");
	if (!f) {
		return -1;
	}
	char line[160];
	size_t klen = strlen(key);
	long val = -1;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, key, klen) == 0 && line[klen] == ':') {
			val = atol(line + klen + 1);
			break;
		}
	}
	fclose(f);
	return val;
}

// The process name (comm), newline stripped. False if the PID is already gone.
static bool read_comm(const char *pid, char *out, size_t size) {
	char path[300];
	snprintf(path, sizeof(path), "/proc/%s/comm", pid);
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	if (!fgets(out, (int)size, f)) {
		fclose(f);
		return false;
	}
	fclose(f);
	out[strcspn(out, "\n")] = '\0';
	return out[0] != '\0';
}

// Resident memory in kB, from /proc/PID/statm (second field, in pages). -1 if
// the PID is already gone.
static long read_rss_kb(const char *pid, long page_kb) {
	char path[300];
	snprintf(path, sizeof(path), "/proc/%s/statm", pid);
	FILE *f = fopen(path, "r");
	if (!f) {
		return -1;
	}
	long total_pages = 0, rss_pages = 0;
	int got = fscanf(f, "%ld %ld", &total_pages, &rss_pages);
	fclose(f);
	if (got < 2) {
		return -1;
	}
	return rss_pages * page_kb;
}

static int rss_desc(const void *a, const void *b) {
	long ra = ((const proc_row_t *)a)->rss_kb;
	long rb = ((const proc_row_t *)b)->rss_kb;
	return ra < rb ? 1 : ra > rb ? -1 : 0;
}

// "12,3 MB" above a megabyte, "820 kB" below: this device's RAM is measured in
// megabytes, but a small process must not read as "0 MB".
static void format_kb(long kb, char *out, size_t size) {
	if (kb < 0) {
		snprintf(out, size, "-");
	} else if (kb >= 1024) {
		snprintf(out, size, "%ld,%ld MB", kb / 1024, (kb % 1024) * 10 / 1024);
	} else {
		snprintf(out, size, "%ld kB", kb);
	}
}

// A list row: name on the left, memory on the right, like the other settings
// rows but with no chevron and no touch handling.
static void add_proc_row(const char *name, long rss_kb) {
	lv_obj_t *card = lv_obj_create(proc_list);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 12, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_hor(card, 18, 0);
	lv_obj_set_style_pad_ver(card, 10, 0);
	lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *name_lbl = lv_label_create(card);
	lv_label_set_text(name_lbl, name);
	lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
	lv_obj_set_width(name_lbl, lv_pct(62));
	lv_obj_set_style_text_font(name_lbl, &font_ui_22, 0);
	lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 0, 0);

	char mem[24];
	format_kb(rss_kb, mem, sizeof(mem));
	lv_obj_t *mem_lbl = lv_label_create(card);
	lv_label_set_text(mem_lbl, mem);
	lv_obj_add_style(mem_lbl, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(mem_lbl, &font_ui_20, 0);
	lv_obj_align(mem_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
}

// Re-reads everything from /proc and redraws the RAM card and the list.
static void rebuild(void) {
	// --- RAM ---
	long total = meminfo_kb("MemTotal");
	long avail = meminfo_kb("MemAvailable");
	long freek = meminfo_kb("MemFree");
	if (avail < 0) {
		// Older kernels may not have MemAvailable (the 4.4 on this device does,
		// but not every build): fall back to free+buffers+cached.
		long buffers = meminfo_kb("Buffers");
		long cached = meminfo_kb("Cached");
		avail = (freek < 0 ? 0 : freek) + (buffers < 0 ? 0 : buffers) + (cached < 0 ? 0 : cached);
	}
	if (ram_label) {
		if (total > 0) {
			long used = total - avail;
			if (used < 0) {
				used = 0;
			}
			int pct = (int)(used * 100 / total);
			char used_s[24], total_s[24], avail_s[24];
			format_kb(used, used_s, sizeof(used_s));
			format_kb(total, total_s, sizeof(total_s));
			format_kb(avail, avail_s, sizeof(avail_s));
			// The player's own heap, on a third line. /proc says how much of the
			// process is anonymous but not what it is; this is the part of that
			// the interface and everything else asked malloc for, and watching
			// it while walking the pages is what tells a growth from a level.
			size_t heap_used = 0;
			heap_usage(&heap_used, NULL);
			char heap_s[24];
			format_kb((long)(heap_used / 1024), heap_s, sizeof(heap_s));
			lv_label_set_text_fmt(ram_label, "%s: %s / %s (%d%%)\n%s: %s\n%s: %s", tr("processes_ram_used"), used_s, total_s,
								  pct, tr("processes_ram_free"), avail_s, tr("processes_player_heap"), heap_s);
		} else {
			lv_label_set_text(ram_label, "/proc/meminfo?");
		}
	}

	// --- processes ---
	if (!proc_list) {
		return;
	}
	lv_obj_clean(proc_list); // drop the rows from the previous pass

	long page_kb = sysconf(_SC_PAGESIZE) / 1024;
	if (page_kb < 1) {
		page_kb = 4;
	}

	static proc_row_t rows[PROC_MAX];
	int n = 0;
	DIR *d = opendir("/proc");
	if (d) {
		struct dirent *e;
		while ((e = readdir(d)) != NULL && n < PROC_MAX) {
			// Only all-digit directory names: those are the PIDs.
			const char *p = e->d_name;
			bool numeric = p[0] != '\0';
			for (const char *c = p; *c; c++) {
				if (*c < '0' || *c > '9') {
					numeric = false;
					break;
				}
			}
			if (!numeric) {
				continue;
			}
			char name[24];
			if (!read_comm(p, name, sizeof(name))) {
				continue;
			}
			long rss = read_rss_kb(p, page_kb);
			rows[n].rss_kb = rss;
			snprintf(rows[n].name, sizeof(rows[n].name), "%s", name);
			n++;
		}
		closedir(d);
	}

	qsort(rows, (size_t)n, sizeof(rows[0]), rss_desc);

	int shown = n < PROC_SHOWN ? n : PROC_SHOWN;
	for (int i = 0; i < shown; i++) {
		add_proc_row(rows[i].name, rows[i].rss_kb);
	}
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	rebuild();
}

// Refresh on demand: the page already redraws on every open, but watching a
// process grow should not require leaving and re-entering for each new figure.
static void refresh_clicked_cb(lv_event_t *e) {
	(void)e;
	rebuild();
}

void processespage_init(gui_config_t *cfg) {
	page_container = settingsrow_page(processespage_screen, cfg, "processes");

	// The top-right icon, like the other page corner buttons.
	settingsrow_title_corner_slots(settingsrow_page_title(processespage_screen), cfg, 1);
	{
		lv_obj_t *button = lv_btn_create(processespage_screen);
		lv_obj_set_size(button, 56, 56);
		lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(button, 0, 0);
		lv_obj_set_style_shadow_width(button, 0, 0);
		lv_obj_set_style_pad_all(button, 0, 0);
		lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -cfg->padding, cfg->padding + cfg->top_bar_height);
		lv_obj_add_event_cb(button, refresh_clicked_cb, LV_EVENT_CLICKED, NULL);

		lv_obj_t *icon = lv_image_create(button);
		lv_image_set_src(icon, &icon_refresh);
		lv_obj_add_style(icon, &theme_style_icon, 0);
		lv_obj_center(icon);
	}

	// The RAM card: two lines, used/total and free.
	lv_obj_t *ram_card = lv_obj_create(page_container);
	lv_obj_set_width(ram_card, lv_pct(100));
	lv_obj_set_height(ram_card, LV_SIZE_CONTENT);
	lv_obj_add_style(ram_card, &theme_style_card, 0);
	lv_obj_set_style_radius(ram_card, 12, 0);
	lv_obj_set_style_border_width(ram_card, 0, 0);
	lv_obj_set_style_shadow_width(ram_card, 0, 0);
	lv_obj_set_style_pad_all(ram_card, 18, 0);
	lv_obj_remove_flag(ram_card, LV_OBJ_FLAG_SCROLLABLE);

	ram_label = lv_label_create(ram_card);
	lv_obj_set_width(ram_label, lv_pct(100));
	lv_obj_set_style_text_font(ram_label, &font_ui_22, 0);
	lv_label_set_text(ram_label, "");

	// The list heading, then the list itself: a column refilled on every open.
	lv_obj_t *list_title = lv_label_create(page_container);
	lv_label_set_text(list_title, tr("processes_running_processes"));
	lv_obj_add_style(list_title, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(list_title, &font_ui_18, 0);
	lv_obj_set_style_pad_top(list_title, 6, 0);

	proc_list = lv_obj_create(page_container);
	lv_obj_set_width(proc_list, lv_pct(100));
	lv_obj_set_height(proc_list, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(proc_list, 0, 0);
	lv_obj_set_style_border_width(proc_list, 0, 0);
	lv_obj_set_style_pad_all(proc_list, 0, 0);
	lv_obj_set_style_pad_row(proc_list, 8, 0);
	lv_obj_set_flex_flow(proc_list, LV_FLEX_FLOW_COLUMN);
	lv_obj_remove_flag(proc_list, LV_OBJ_FLAG_SCROLLABLE);

	rebuild();
	lv_obj_add_event_cb(processespage_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}

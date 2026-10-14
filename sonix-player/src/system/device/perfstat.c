#define _GNU_SOURCE 1 // dl_iterate_phdr

#include "perfstat.h"

#include <dirent.h>
#include <link.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lvgl/src/core/lv_global.h"
#include "lvgl/src/display/lv_display_private.h"
#include "lvgl/src/misc/lv_anim_private.h"
#include "lvgl/src/misc/lv_timer_private.h"
#include "src/system/core/config.h"

// See perfstat.h. Everything below runs on the UI thread, so nothing is
// locked; the /proc reads happen once per report, never per pass.

#define PERF_TIMERS_MAX 192	  // snapshot of the timer list, per pass
#define PERF_CALLBACKS_MAX 96 // distinct callbacks counted per report
#define PERF_THREADS_MAX 64	  // threads followed between reports
#define PERF_IGNORED_MAX 4

static bool enabled;
static lv_display_t *display;
static uint32_t report_ms;

// The timer list as it stood before the pass, to tell afterwards which ones ran:
// a timer that ran has a new last_run.
typedef struct {
	lv_timer_t *timer;
	uint32_t last_run;
} timer_snap_t;
static timer_snap_t snap[PERF_TIMERS_MAX];
static int snap_count;

static lv_timer_t *ignored[PERF_IGNORED_MAX];
static int ignored_count;

typedef struct {
	lv_timer_cb_t cb;
	uint32_t runs;
	uint32_t period; // as last seen, to tell two users of one callback apart by eye
} cb_count_t;
static cb_count_t callbacks[PERF_CALLBACKS_MAX];
static int callback_count;

// This report's counters.
static uint32_t passes;		   // lv_timer_handler() calls
static uint32_t busy_passes;   // passes in which a counted timer ran
static uint32_t woke_timeout;  // poll() ran out: a timer was due
static uint32_t woke_post;	   // gui_post() from another thread
static uint32_t woke_touch;	   // the touch panel's descriptor
static uint32_t frames;		   // refreshes that drew something
static uint32_t empty_refr;	   // refreshes that found nothing to draw
static uint64_t frame_cpu_us;  // UI thread CPU over the drawn frames
static uint64_t frame_flush_us; // ... of which in the flush callback (on the emulator: SDL)
static uint64_t frame_layout_us; // ... of which before rendering (layout, invalidation joins)
static uint32_t draw_max;		 // the worst frame without its flush
static uint32_t frame_cpu_max; // the worst one
static uint64_t frame_px;	   // pixels repainted
static uint32_t frame_px_max;
static uint32_t frames_over_16; // frames that cost more CPU than a 60 Hz period
static uint64_t handler_cpu_us; // UI thread CPU inside lv_timer_handler(), frames included
static uint64_t ignored_cpu_us; // ... in passes where only ignored (emulator) timers ran

// The frame being drawn.
static uint64_t refr_t0;
static uint64_t flush_t0;
static uint32_t refr_flush;
static bool refr_rendering;
static uint32_t refr_px;
static uint64_t pass_t0;

// Since the last report.
static uint64_t last_wall_us;
static unsigned long long last_proc_ticks;

typedef struct {
	int tid;
	unsigned long switches;
	bool seen;
} thread_count_t;
static thread_count_t threads[PERF_THREADS_MAX];
static int thread_count;

// Where the executable was loaded: callback addresses are printed relative to
// it, which is what addr2line takes for the binary on disk (0 when it is not
// position-independent, as the device build is).
static uintptr_t load_bias;

static uint64_t clock_us(clockid_t id) {
	struct timespec ts;
	clock_gettime(id, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static uint64_t thread_cpu_us(void) { return clock_us(CLOCK_THREAD_CPUTIME_ID); }

static int find_bias(struct dl_phdr_info *info, size_t size, void *data) {
	(void)size;
	// The first object listed is the executable itself.
	*(uintptr_t *)data = (uintptr_t)info->dlpi_addr;
	return 1;
}

bool perfstat_enabled(void) { return enabled; }

void perfstat_ignore_timer(lv_timer_t *timer) {
	if (timer && ignored_count < PERF_IGNORED_MAX) {
		ignored[ignored_count++] = timer;
	}
}

static bool is_ignored(const lv_timer_t *timer) {
	for (int i = 0; i < ignored_count; i++) {
		if (ignored[i] == timer) {
			return true;
		}
	}
	return false;
}

void perfstat_pass_begin(void) {
	if (!enabled) {
		return;
	}
	snap_count = 0;
	for (lv_timer_t *t = lv_timer_get_next(NULL); t && snap_count < PERF_TIMERS_MAX; t = lv_timer_get_next(t)) {
		snap[snap_count].timer = t;
		snap[snap_count].last_run = t->last_run;
		snap_count++;
	}
	pass_t0 = thread_cpu_us();
}

static void count_callback(lv_timer_t *t) {
	for (int i = 0; i < callback_count; i++) {
		if (callbacks[i].cb == t->timer_cb) {
			callbacks[i].runs++;
			callbacks[i].period = t->period;
			return;
		}
	}
	if (callback_count < PERF_CALLBACKS_MAX) {
		callbacks[callback_count].cb = t->timer_cb;
		callbacks[callback_count].runs = 1;
		callbacks[callback_count].period = t->period;
		callback_count++;
	}
}

void perfstat_pass_end(void) {
	if (!enabled) {
		return;
	}
	uint64_t pass_cpu = thread_cpu_us() - pass_t0;
	handler_cpu_us += pass_cpu;
	passes++;
	bool counted = false;
	// Only timers that existed before the pass and still do: one deleted in
	// the pass is no longer in the list, one created in it is not in the
	// snapshot. Both orders are the list's own, so one walk does.
	int s = 0;
	for (lv_timer_t *t = lv_timer_get_next(NULL); t; t = lv_timer_get_next(t)) {
		int found = -1;
		for (int k = s; k < snap_count; k++) {
			if (snap[k].timer == t) {
				found = k;
				break;
			}
		}
		if (found < 0) {
			continue;
		}
		s = found + 1;
		if (snap[found].last_run != t->last_run && !is_ignored(t)) {
			count_callback(t);
			counted = true;
		}
	}
	if (counted) {
		busy_passes++;
	} else {
		ignored_cpu_us += pass_cpu;
	}
}

void perfstat_woke(bool by_post, bool by_touch) {
	if (!enabled) {
		return;
	}
	if (by_post) {
		woke_post++;
	}
	if (by_touch) {
		woke_touch++;
	}
	if (!by_post && !by_touch) {
		woke_timeout++;
	}
}

static void refr_event_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	if (code == LV_EVENT_REFR_START) {
		refr_t0 = thread_cpu_us();
		refr_rendering = false;
		refr_px = 0;
		refr_flush = 0;
	} else if (code == LV_EVENT_FLUSH_START) {
		flush_t0 = thread_cpu_us();
	} else if (code == LV_EVENT_FLUSH_FINISH) {
		refr_flush += (uint32_t)(thread_cpu_us() - flush_t0);
	} else if (code == LV_EVENT_RENDER_START) {
		refr_rendering = true;
		frame_layout_us += thread_cpu_us() - refr_t0;
		lv_display_t *d = lv_event_get_target(e);
		for (uint32_t i = 0; i < d->inv_p; i++) {
			if (!d->inv_area_joined[i]) {
				refr_px += (uint32_t)lv_area_get_size(&d->inv_areas[i]);
			}
		}
	} else if (code == LV_EVENT_REFR_READY) {
		if (!refr_rendering) {
			empty_refr++;
			return;
		}
		uint32_t cpu = (uint32_t)(thread_cpu_us() - refr_t0);
		frames++;
		frame_cpu_us += cpu;
		frame_flush_us += refr_flush;
		if (cpu > frame_cpu_max) {
			frame_cpu_max = cpu;
		}
		if (cpu - refr_flush > draw_max) {
			draw_max = cpu - refr_flush;
		}
		if (cpu > 16000) {
			frames_over_16++;
		}
		frame_px += refr_px;
		if (refr_px > frame_px_max) {
			frame_px_max = refr_px;
		}
	}
}

static unsigned long long proc_ticks(void) {
	FILE *f = fopen("/proc/self/stat", "r");
	if (!f) {
		return 0;
	}
	char buf[1024];
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	// Fields after the command name, which may hold spaces: utime is the
	// 14th field overall, the 12th after ")".
	char *p = strrchr(buf, ')');
	if (!p) {
		return 0;
	}
	unsigned long utime = 0, stime = 0;
	if (sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &utime, &stime) != 2) {
		return 0;
	}
	return (unsigned long long)utime + stime;
}

// One line of /proc/self/status, in kB.
static long status_kb(const char *key) {
	FILE *f = fopen("/proc/self/status", "r");
	if (!f) {
		return -1;
	}
	char line[128];
	size_t len = strlen(key);
	long kb = -1;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, key, len) == 0 && line[len] == ':') {
			kb = strtol(line + len + 1, NULL, 10);
			break;
		}
	}
	fclose(f);
	return kb;
}

static unsigned long thread_switches(int tid, char *name, size_t name_size) {
	char path[64];
	char line[128];
	unsigned long total = 0;
	snprintf(path, sizeof(path), "/proc/self/task/%d/status", tid);
	FILE *f = fopen(path, "r");
	if (!f) {
		return 0;
	}
	name[0] = '\0';
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "Name:", 5) == 0) {
			char *v = line + 5;
			while (*v == ' ' || *v == '\t') {
				v++;
			}
			v[strcspn(v, "\n")] = '\0';
			snprintf(name, name_size, "%.15s", v);
		} else if (strncmp(line, "voluntary_ctxt_switches:", 24) == 0 ||
				   strncmp(line, "nonvoluntary_ctxt_switches:", 27) == 0) {
			total += strtoul(strchr(line, ':') + 1, NULL, 10);
		}
	}
	fclose(f);
	return total;
}

// Appends the threads that switched since the last report, busiest first, as
// "name/tid:switches".
static void report_threads(char *out, size_t size, double secs) {
	typedef struct {
		int tid;
		unsigned long delta;
		char name[20];
	} row_t;
	row_t rows[PERF_THREADS_MAX];
	int row_count = 0;

	for (int i = 0; i < thread_count; i++) {
		threads[i].seen = false;
	}
	DIR *dir = opendir("/proc/self/task");
	if (!dir) {
		return;
	}
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		int tid = atoi(de->d_name);
		if (tid <= 0) {
			continue;
		}
		char name[20];
		unsigned long now = thread_switches(tid, name, sizeof(name));
		int k;
		for (k = 0; k < thread_count; k++) {
			if (threads[k].tid == tid) {
				break;
			}
		}
		unsigned long delta;
		if (k < thread_count) {
			delta = now - threads[k].switches;
		} else {
			delta = now; // new since the last report: all of its switches are recent
			if (thread_count >= PERF_THREADS_MAX) {
				continue;
			}
			threads[thread_count].tid = tid;
			k = thread_count++;
		}
		threads[k].switches = now;
		threads[k].seen = true;
		if (delta > 0 && row_count < PERF_THREADS_MAX) {
			rows[row_count].tid = tid;
			rows[row_count].delta = delta;
			snprintf(rows[row_count].name, sizeof(rows[row_count].name), "%s", name);
			row_count++;
		}
	}
	closedir(dir);

	// Threads that ended are forgotten, so their slots come back.
	int keep = 0;
	for (int i = 0; i < thread_count; i++) {
		if (threads[i].seen) {
			threads[keep++] = threads[i];
		}
	}
	thread_count = keep;

	// Busiest first; a dozen rows is plenty.
	for (int i = 0; i < row_count; i++) {
		for (int j = i + 1; j < row_count; j++) {
			if (rows[j].delta > rows[i].delta) {
				row_t tmp = rows[i];
				rows[i] = rows[j];
				rows[j] = tmp;
			}
		}
	}
	size_t used = strlen(out);
	for (int i = 0; i < row_count && i < 12 && used < size; i++) {
		used += (size_t)snprintf(out + used, size - used, " %s/%d:%.1f", rows[i].name, rows[i].tid,
								 rows[i].delta / secs);
	}
}

static void report_cb(lv_timer_t *timer) {
	(void)timer;
	uint64_t now = clock_us(CLOCK_MONOTONIC);
	double secs = (now - last_wall_us) / 1e6;
	if (secs <= 0) {
		return;
	}
	unsigned long long ticks = proc_ticks();
	long hz = sysconf(_SC_CLK_TCK);
	double cpu_pct = hz > 0 ? (double)(ticks - last_proc_ticks) * 100.0 / (double)hz / secs : 0;

	// mallinfo(), not mallinfo2(): the device's glibc 2.22 has only the old
	// one, and its int fields are ample for a 64 MB machine.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
	struct mallinfo mi = mallinfo();
#pragma GCC diagnostic pop
	fprintf(stderr,
			"perf: %.1f s: loop %.1f/s (timers %.1f/s; woke: due %.1f post %.1f touch %.1f/s) handler cpu %.2f ms/s (+%.2f emulator-only); "
			"frames %u (%.1f/s), cpu avg %.2f max %.2f ms (without flush avg %.3f max %.3f, layout avg %.3f), >16 ms %u, px avg %u max %u, empty %u; "
			"process cpu %.1f %%, rss %ld kB (peak %ld), heap %d kB in use, %d kB free\n",
			secs, passes / secs, busy_passes / secs, woke_timeout / secs, woke_post / secs, woke_touch / secs,
			(handler_cpu_us - ignored_cpu_us) / 1000.0 / secs, ignored_cpu_us / 1000.0 / secs, frames, frames / secs, frames ? frame_cpu_us / 1000.0 / frames : 0.0,
			frame_cpu_max / 1000.0, frames ? (frame_cpu_us - frame_flush_us) / 1000.0 / frames : 0.0, draw_max / 1000.0,
			frames ? frame_layout_us / 1000.0 / frames : 0.0, frames_over_16, frames ? (unsigned)(frame_px / frames) : 0u, frame_px_max,
			empty_refr, cpu_pct, status_kb("VmRSS"), status_kb("VmHWM"), mi.uordblks / 1024, mi.fordblks / 1024);

	// Timers, busiest first.
	for (int i = 0; i < callback_count; i++) {
		for (int j = i + 1; j < callback_count; j++) {
			if (callbacks[j].runs > callbacks[i].runs) {
				cb_count_t tmp = callbacks[i];
				callbacks[i] = callbacks[j];
				callbacks[j] = tmp;
			}
		}
	}
	char line[1024];
	size_t used = (size_t)snprintf(line, sizeof(line), "perf: timers/s");
	for (int i = 0; i < callback_count && used < sizeof(line); i++) {
		used += (size_t)snprintf(line + used, sizeof(line) - used, " %#lx(%u):%.1f",
								 (unsigned long)((uintptr_t)callbacks[i].cb - load_bias), (unsigned)callbacks[i].period,
								 callbacks[i].runs / secs);
	}
	fprintf(stderr, "%s\n", line);

	// The animations alive right now: one that never ends keeps the
	// animation timer at 60 Hz whether or not anything is seen to move.
	// The exec callbacks, resolved like the timers, with each one's duration.
	// (Whether its widget is visible cannot be asked: an animation's variable
	// need not be a widget at all.)
	uint32_t anims = lv_anim_count_running();
	if (anims) {
		used = (size_t)snprintf(line, sizeof(line), "perf: %u animation(s):", (unsigned)anims);
		int listed = 0;
		lv_anim_t *a;
		LV_LL_READ(&LV_GLOBAL_DEFAULT()->anim_state.anim_ll, a) {
			if (listed++ >= 12 || used >= sizeof(line)) {
				break;
			}
			uintptr_t exec = a->exec_cb ? (uintptr_t)a->exec_cb : (uintptr_t)a->custom_exec_cb;
			used += (size_t)snprintf(line + used, sizeof(line) - used, " %#lx(%d ms)",
									 (unsigned long)(exec - load_bias), (int)a->duration);
		}
		fprintf(stderr, "%s\n", line);
	}

	snprintf(line, sizeof(line), "perf: thread switches/s");
	report_threads(line, sizeof(line), secs);
	fprintf(stderr, "%s\n", line);

	passes = busy_passes = woke_timeout = woke_post = woke_touch = 0;
	frames = empty_refr = frame_cpu_max = frame_px_max = frames_over_16 = 0;
	frame_cpu_us = frame_px = handler_cpu_us = ignored_cpu_us = 0;
	frame_flush_us = frame_layout_us = 0;
	draw_max = 0;
	callback_count = 0;
	last_wall_us = now;
	last_proc_ticks = ticks;
}

void perfstat_init(lv_display_t *disp) {
	const char *env = getenv("SONIX_PERF");
	long seconds = env && env[0] ? strtol(env, NULL, 10) : config_get_int("debug", "perf_stats", 0);
	if (seconds <= 0 || !disp) {
		return;
	}
	report_ms = (uint32_t)(seconds > 3600 ? 3600 : seconds) * 1000u;
	display = disp;
	dl_iterate_phdr(find_bias, &load_bias);

	lv_display_add_event_cb(disp, refr_event_cb, LV_EVENT_REFR_START, NULL);
	lv_display_add_event_cb(disp, refr_event_cb, LV_EVENT_RENDER_START, NULL);
	lv_display_add_event_cb(disp, refr_event_cb, LV_EVENT_REFR_READY, NULL);
	lv_display_add_event_cb(disp, refr_event_cb, LV_EVENT_FLUSH_START, NULL);
	lv_display_add_event_cb(disp, refr_event_cb, LV_EVENT_FLUSH_FINISH, NULL);

	lv_timer_t *t = lv_timer_create(report_cb, report_ms, NULL);
	perfstat_ignore_timer(t); // its own runs are not the player's
	last_wall_us = clock_us(CLOCK_MONOTONIC);
	last_proc_ticks = proc_ticks();
	char unused[1] = {0};
	report_threads(unused, sizeof(unused), 1); // the baseline for every thread
	enabled = true;
	fprintf(stderr, "perf: statistics every %u s; timer callbacks relative to %#lx (addr2line -f -e <unstripped binary>)\n",
			report_ms / 1000u, (unsigned long)load_bias);
}

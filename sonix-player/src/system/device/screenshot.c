#include "screenshot.h"

#include "lvgl/lvgl.h"

#include "src/gui/shell/gui.h"
#include "src/system/core/config.h"
#include "src/system/image/png_write.h"
#include "src/system/device/power.h"
#include "src/system/device/system.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define SCREENSHOT_DIR "Screenshots"

// ---------------------------------------------------------------------------
// the setting
// ---------------------------------------------------------------------------

bool screenshot_enabled(void) { return config_get_bool("system", "screenshots", false); }

void screenshot_set_enabled(bool enabled) {
	config_set_bool("system", "screenshots", enabled);
	config_save();
}

const char *screenshot_folder_name(void) { return SCREENSHOT_DIR; }

// ---------------------------------------------------------------------------
// the capture
//
// Three steps on three threads, each where it is for a reason:
//
//   key thread     only makes the request -- it cannot stop to write a
//                  megabyte, or it would miss the next key
//   GUI thread     copies the framebuffer page. It is the only thread that can
//                  do so without risking half of one page and half of the
//                  other, because it is the one that swaps them
//   writer thread  compresses and writes. That takes tenths of a second: doing
//                  it on the GUI thread would freeze the interface at the very
//                  moment being photographed
// ---------------------------------------------------------------------------

typedef struct {
	// The pixels are borrowed: display_capture_frame() always returns the same
	// block, reused on every shot (see power.h). Only the pointer is kept here,
	// and it is never freed.
	const uint16_t *pixels;
	int width;
	int height;
} job_t;

// One at a time. Without this, holding the keys down would queue a capture per
// repeat and fill memory with frames.
static pthread_mutex_t busy_lock = PTHREAD_MUTEX_INITIALIZER;
static bool busy;

static void finish(void) {
	pthread_mutex_lock(&busy_lock);
	busy = false;
	pthread_mutex_unlock(&busy_lock);
}

// "Screenshots/screenshot-20260822-195631.png", with a counter appended when
// that second is already taken: two shots in the same second are unlikely but
// possible, and silently overwriting a file would be worse.
static bool build_path(char *out, size_t out_size, const char *dir) {
	time_t now = time(NULL);
	struct tm tm_now;
	if (!localtime_r(&now, &tm_now)) {
		return false;
	}

	char stamp[32];
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm_now);

	for (int extra = 0; extra < 100; extra++) {
		if (extra == 0) {
			snprintf(out, out_size, "%s/screenshot-%s.png", dir, stamp);
		} else {
			snprintf(out, out_size, "%s/screenshot-%s-%d.png", dir, stamp, extra);
		}
		struct stat st;
		if (stat(out, &st) != 0) {
			return true;
		}
	}
	return false;
}

static void *write_thread(void *arg) {
	job_t *job = arg;

	const char *root = storage_sd_root();
	if (!root || !*root) {
		printf("screenshot: no card to write to\n");
		gui_notify_popup("screenshot_failed_no_card");
		goto done;
	}

	char dir[1024];
	snprintf(dir, sizeof(dir), "%s/%s", root, SCREENSHOT_DIR);
	if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
		printf("screenshot: cannot create %s: %s\n", dir, strerror(errno));
		gui_notify_popup("screenshot_failed");
		goto done;
	}

	char path[1200];
	if (!build_path(path, sizeof(path), dir)) {
		gui_notify_popup("screenshot_failed");
		goto done;
	}

	if (!png_write_rgb565(path, job->pixels, job->width, job->height)) {
		gui_notify_popup("screenshot_failed");
		goto done;
	}

	// No notice on success: the flash already said it, a third of a second ago.
	// A popup now would land after the fact, on top of whatever was being
	// photographed. The path stays in the log, which is where it gets looked up.
	printf("screenshot: %s\n", path);

done:
	free(job);
	finish();
	return NULL;
}

// ---------------------------------------------------------------------------
// the flash
//
// What iOS and Android do: the screen goes white and back, and that is all the
// confirmation needed -- what was photographed was just being looked at, so
// there is nothing to read.
//
// It lives on the system layer, above everything else (popups and keyboard
// included), and is not clickable: a touch during the fade has to reach the
// page underneath rather than die here.
//
// Timing: straight to full white, then fade out. A flash that ramps up is not
// a flash.
// ---------------------------------------------------------------------------

#define FLASH_FADE_MS 240

static void flash_opa_cb(void *var, int32_t value) {
	lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

static void flash_done_cb(lv_anim_t *a) { lv_obj_delete((lv_obj_t *)a->var); }

static void flash_show(void) {
	lv_obj_t *flash = lv_obj_create(lv_layer_sys());
	lv_obj_set_size(flash, lv_pct(100), lv_pct(100));
	lv_obj_set_pos(flash, 0, 0);
	lv_obj_set_style_bg_color(flash, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(flash, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(flash, 0, 0);
	lv_obj_set_style_radius(flash, 0, 0);
	lv_obj_set_style_pad_all(flash, 0, 0);
	lv_obj_remove_flag(flash, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_remove_flag(flash, LV_OBJ_FLAG_CLICKABLE);

	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, flash);
	lv_anim_set_exec_cb(&a, flash_opa_cb);
	lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
	lv_anim_set_duration(&a, FLASH_FADE_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_completed_cb(&a, flash_done_cb);
	lv_anim_start(&a);
}

// On the GUI thread: copies the frame and hands it on.
static void capture_async(void *unused) {
	(void)unused;

	int w = 0, h = 0;
	const uint16_t *pixels = display_capture_frame(&w, &h);
	if (!pixels || w <= 0 || h <= 0) {
		printf("screenshot: this display will not let itself be copied\n");
		gui_notify_popup("screenshot_unavailable");
		finish();
		return;
	}

	// After the copy, never before: the flash is an ordinary object and would
	// end up inside the image.
	flash_show();

	job_t *job = malloc(sizeof(*job));
	if (!job) {
		printf("screenshot: no memory for the request\n");
		finish();
		return;
	}
	job->pixels = pixels;
	job->width = w;
	job->height = h;

	pthread_t thread;
	if (pthread_create(&thread, NULL, write_thread, job) != 0) {
		printf("screenshot: cannot start the writer thread\n");
		free(job);
		finish();
		return;
	}
	pthread_detach(thread);
}

void screenshot_request(void) {
	pthread_mutex_lock(&busy_lock);
	if (busy) {
		pthread_mutex_unlock(&busy_lock);
		// Logged, not swallowed: a shot that does not happen and leaves no trace
		// is indistinguishable from a key that did not work.
		printf("screenshot: one is already in progress, skipping this one\n");
		return;
	}
	busy = true;
	pthread_mutex_unlock(&busy_lock);

	// gui_post is the bridge from any thread to the GUI thread: the function
	// runs at the top of the next lv_timer_handler() pass.
	gui_post(capture_async, NULL);
}

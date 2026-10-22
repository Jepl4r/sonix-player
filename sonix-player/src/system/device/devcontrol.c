#include "devcontrol.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/device/power.h"
#include "src/system/device/screenshot.h"

#ifndef CONTROL_FIFO
#define CONTROL_FIFO "/tmp/sonix-control"
#endif
#ifndef STATUS_FILE
#define STATUS_FILE "/tmp/sonix-control.status"
#endif
static const char *control_path = CONTROL_FIFO;
static const char *status_path = STATUS_FILE;
#define RX_CAPACITY 512
#define DEFAULT_TAP_MS 120
#define DEFAULT_SWIPE_MS 350
#define MAX_GESTURE_MS 10000
#define WAKE_SETTLE_MS 250

typedef struct {
	bool active;
	int from_x;
	int from_y;
	int to_x;
	int to_y;
	int last_x;
	int last_y;
	uint32_t starts_at;
	uint32_t duration_ms;
} virtual_gesture_t;

static int control_fd = -1;
static int panel_width;
static int panel_height;
static lv_display_t *control_display;
static lv_indev_t *virtual_pointer;
static virtual_gesture_t gesture;
static char rx_buffer[RX_CAPACITY];
static size_t rx_used;

static int clamp_coord(int value, int limit) {
	if (value < 0) {
		return 0;
	}
	if (value >= limit) {
		return limit - 1;
	}
	return value;
}

static uint32_t clamp_duration(int value, int fallback) {
	if (value <= 0) {
		value = fallback;
	}
	if (value > MAX_GESTURE_MS) {
		value = MAX_GESTURE_MS;
	}
	return (uint32_t)value;
}

static void write_status(const char *text) {
	char temporary[1024];
	if (snprintf(temporary, sizeof(temporary), "%s.XXXXXX", status_path) >= (int)sizeof(temporary))
		return;
	int fd = mkstemp(temporary);
	if (fd < 0)
		return;
	FILE *f = fdopen(fd, "w");
	if (!f) {
		close(fd);
		unlink(temporary);
		return;
	}
	fprintf(f, "%s\n", text);
	fclose(f);
	if (rename(temporary, status_path) != 0) {
		unlink(temporary);
	}
}

static void pointer_read(lv_indev_t *indev, lv_indev_data_t *data) {
	(void)indev;

	int x = gesture.last_x;
	int y = gesture.last_y;
	data->state = LV_INDEV_STATE_RELEASED;

	if (gesture.active) {
		uint32_t now = lv_tick_get();
		int32_t since_start = (int32_t)(now - gesture.starts_at);
		if (since_start >= 0 && (uint32_t)since_start < gesture.duration_ms) {
			uint32_t elapsed = (uint32_t)since_start;
			x = gesture.from_x + (int)(((int64_t)(gesture.to_x - gesture.from_x) * elapsed) / gesture.duration_ms);
			y = gesture.from_y + (int)(((int64_t)(gesture.to_y - gesture.from_y) * elapsed) / gesture.duration_ms);
			data->state = LV_INDEV_STATE_PRESSED;
		} else if (since_start >= 0) {
			x = gesture.to_x;
			y = gesture.to_y;
			gesture.active = false;
		}
	}

	gesture.last_x = x;
	gesture.last_y = y;

	// Commands use what the framebuffer screenshot shows. LVGL itself stays in
	// its original coordinate system when the final framebuffer is rotated.
	if (display_get_rotated()) {
		x = panel_width - 1 - x;
		y = panel_height - 1 - y;
	}
	data->point.x = x;
	data->point.y = y;
}

static bool start_gesture(int x1, int y1, int x2, int y2, uint32_t duration_ms) {
	if (gesture.active) {
		write_status("error busy");
		return false;
	}

	x1 = clamp_coord(x1, panel_width);
	y1 = clamp_coord(y1, panel_height);
	x2 = clamp_coord(x2, panel_width);
	y2 = clamp_coord(y2, panel_height);

	uint32_t delay = 0;
	if (!power_screen_is_on()) {
		power_screen_on();
		delay = WAKE_SETTLE_MS;
	}
	power_notify_activity();
	if (control_display) {
		lv_display_trigger_activity(control_display);
	}

	gesture.from_x = x1;
	gesture.from_y = y1;
	gesture.to_x = x2;
	gesture.to_y = y2;
	gesture.last_x = x1;
	gesture.last_y = y1;
	gesture.starts_at = lv_tick_get() + delay;
	gesture.duration_ms = duration_ms;
	gesture.active = true;
	return true;
}

static void apply_command(char *line) {
	while (*line == ' ' || *line == '\t') {
		line++;
	}
	char *end = line + strlen(line);
	while (end > line && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) {
		*--end = '\0';
	}

	char name[16], status[160];
	int used = 0, args[5], count = 0;
	if (sscanf(line, "%15s%n", name, &used) != 1)
		return;
	char *cursor = line + used;
	while (*cursor) {
		while (*cursor == ' ' || *cursor == '\t')
			cursor++;
		if (!*cursor)
			break;
		errno = 0;
		char *next;
		long value = strtol(cursor, &next, 10);
		if (count == 5 || next == cursor || errno || value < INT_MIN || value > INT_MAX || (*next && *next != ' ' && *next != '\t')) {
			write_status("error invalid arguments");
			return;
		}
		args[count++] = (int)value;
		cursor = next;
	}
	if (!strcmp(name, "tap") && (count == 2 || count == 3)) {
		uint32_t ms = clamp_duration(count == 3 ? args[2] : DEFAULT_TAP_MS, DEFAULT_TAP_MS);
		if (start_gesture(args[0], args[1], args[0], args[1], ms)) {
			snprintf(status, sizeof(status), "ok tap %d %d %u", clamp_coord(args[0], panel_width), clamp_coord(args[1], panel_height), ms);
			write_status(status);
		}
		return;
	}
	if (!strcmp(name, "swipe") && (count == 4 || count == 5)) {
		uint32_t ms = clamp_duration(count == 5 ? args[4] : DEFAULT_SWIPE_MS, DEFAULT_SWIPE_MS);
		if (start_gesture(args[0], args[1], args[2], args[3], ms))
			write_status("ok swipe");
		return;
	}

	if (strcmp(line, "wake") == 0) {
		power_screen_on();
		power_notify_activity();
		if (control_display) {
			lv_display_trigger_activity(control_display);
		}
		write_status("ok wake");
		return;
	}

	if (strcmp(line, "screenshot") == 0 || strcmp(line, "shot") == 0) {
		screenshot_request();
		write_status("ok screenshot");
		return;
	}

	if (strcmp(line, "status") == 0 || strcmp(line, "ping") == 0) {
		snprintf(status, sizeof(status), "ok ready %dx%d screen=%s gesture=%s", panel_width, panel_height, power_screen_is_on() ? "on" : "off", gesture.active ? "active" : "idle");
		write_status(status);
		return;
	}

	write_status("error usage: tap x y [ms] | swipe x1 y1 x2 y2 [ms] | wake | screenshot | status");
}

void devcontrol_init(lv_display_t *display, int width, int height) {
#ifdef HOST_BUILD
	const char *fifo_override = getenv("SONIX_CONTROL_FIFO");
	const char *status_override = getenv("SONIX_CONTROL_STATUS");
	if (fifo_override && *fifo_override)
		control_path = fifo_override;
	if (status_override && *status_override)
		status_path = status_override;
#endif
	control_display = display;
	panel_width = width > 0 ? width : 480;
	panel_height = height > 0 ? height : 800;
	gesture.last_x = panel_width / 2;
	gesture.last_y = panel_height / 2;

	virtual_pointer = lv_indev_create();
	if (!virtual_pointer) {
		fprintf(stderr, "devcontrol: cannot create virtual pointer\n");
		return;
	}
	lv_indev_set_type(virtual_pointer, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(virtual_pointer, pointer_read);
	lv_indev_set_display(virtual_pointer, display);

	unlink(control_path);
	if (mkfifo(control_path, 0600) != 0 && errno != EEXIST) {
		perror("devcontrol: mkfifo");
		return;
	}
	chmod(control_path, 0600);
	control_fd = open(control_path, O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
	if (control_fd < 0) {
		perror("devcontrol: open");
		return;
	}

	char status[96];
	snprintf(status, sizeof(status), "ok ready %dx%d screen=%s gesture=idle", panel_width, panel_height, power_screen_is_on() ? "on" : "off");
	write_status(status);
	printf("devcontrol: %s ready for ADB UI automation\n", control_path);
}

int devcontrol_fd(void) { return control_fd; }

void devcontrol_service(void) {
	static bool discard_line;
	if (control_fd < 0)
		return;
	char input[RX_CAPACITY];
	// Bound each GUI pass, even if a client keeps the FIFO continuously full.
	for (int batch = 0; batch < 8; batch++) {
		ssize_t got = read(control_fd, input, sizeof(input));
		if (got <= 0)
			break;
		for (ssize_t i = 0; i < got; i++) {
			if (input[i] == '\n') {
				if (!discard_line && rx_used) {
					rx_buffer[rx_used] = '\0';
					apply_command(rx_buffer);
				}
				rx_used = 0;
				discard_line = false;
			} else if (!discard_line) {
				if (input[i] == '\0' || rx_used == sizeof(rx_buffer) - 1) {
					discard_line = true;
					rx_used = 0;
					write_status("error invalid or oversized command");
				} else
					rx_buffer[rx_used++] = input[i];
			}
		}
	}
}

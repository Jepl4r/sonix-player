#include "../sonix-player/src/system/device/devcontrol.c"
#include <assert.h>

static uint32_t tick;
static int wakes, shots;
uint32_t lv_tick_get(void) { return tick; }
bool power_screen_is_on(void) { return true; }
void power_screen_on(void) { wakes++; }
void power_notify_activity(void) {}
void lv_display_trigger_activity(lv_display_t *disp) { (void)disp; }
bool display_get_rotated(void) { return false; }
void screenshot_request(void) { shots++; }

static void command(const char *s) {
	char line[256];
	snprintf(line, sizeof(line), "%s", s);
	apply_command(line);
}
int main(void) {
	status_path = "control.status";
	assert(mkfifo("control.status.new", 0600) == 0);
	alarm(2);
	command("status");
	alarm(0);
	unlink("control.status.new");
	panel_width = 480;
	panel_height = 800;
	command("tap 10 20 garbage");
	assert(!gesture.active);
	command("tap 10+20");
	assert(!gesture.active);
	command("tap 999999999999999999999999999 20");
	assert(!gesture.active);
	command("tap 10 20 100");
	assert(gesture.active);
	lv_indev_data_t data = {0};
	pointer_read(NULL, &data);
	assert(data.state == LV_INDEV_STATE_PRESSED && data.point.x == 10 && data.point.y == 20);
	tick = 100;
	pointer_read(NULL, &data);
	assert(!gesture.active && data.state == LV_INDEV_STATE_RELEASED);
	command("swipe -10 -20 600 900 200");
	tick = 200;
	pointer_read(NULL, &data);
	assert(data.point.x == 239 && data.point.y == 399);
	tick = 300;
	pointer_read(NULL, &data);
	assert(data.point.x == 479 && data.point.y == 799 && !gesture.active);
	command("wake");
	command("shot");
	assert(wakes == 1 && shots == 1);
	int fds[2];
	assert(pipe(fds) == 0);
	control_fd = fds[0];
	fcntl(control_fd, F_SETFL, O_NONBLOCK);
	char bad[600];
	memset(bad, 'x', sizeof(bad));
	assert(write(fds[1], bad, sizeof(bad)) == sizeof(bad));
	assert(write(fds[1], "wake\nwake\n", 10) == 10);
	devcontrol_service();
	assert(wakes == 2);
	assert(write(fds[1], "wa", 2) == 2);
	devcontrol_service();
	assert(wakes == 2);
	assert(write(fds[1], "ke\n", 3) == 3);
	devcontrol_service();
	assert(wakes == 3);
	close(fds[0]);
	close(fds[1]);
	puts("PASS devcontrol-commands-gestures-and-framing");
	return 0;
}

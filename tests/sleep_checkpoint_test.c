#define main part_step_main
#include "playback_part_step_test.c"
#undef main
bool playlist_locate(const char *file) { return true; }

int main(void) {
	navigation_test = true;
	book_loaded = true;
	floor_armed = true;
	assert(!device_state_load_list_ordered_paused(NULL, 0, 0));
	assert(floor_armed && !explicit_book_load);
	test_status = AUDIO_STATUS_PLAYING;
	strcpy(current_metadata_file, "a.mp3");
	strcpy(selected, "a.mp3");
	current_position = 98;
	test_now_ms = 39000;
	device_state_sleep_checkpoint(60000);
	// A conservative pre-fade sample stays fixed at 2x, even with a late callback.
	test_now_ms = 41000;
	current_position = 102;
	device_state_sleep_checkpoint(60000);
	test_now_ms = 65000;
	current_position = 150;
	device_state_sleep_checkpoint(60000);
	assert(device_state_sleep_restore(60000));
	assert(sought == 98 && test_status == AUDIO_STATUS_PAUSED && jump_marks == 0);
	// An automatic file transition during the fade retains the preceding part.
	test_status = AUDIO_STATUS_PLAYING;
	current_position = 590;
	test_now_ms = 79000;
	device_state_sleep_checkpoint(100000);
	test_now_ms = 81000;
	current_position = 594;
	device_state_sleep_checkpoint(100000);
	strcpy(current_metadata_file, "b.mp3");
	strcpy(selected, "b.mp3");
	current_position = 15;
	floor_armed = true;
	unsigned serial = device_state_seek_serial();
	test_now_ms = 105000;
	device_state_sleep_checkpoint(100000);
	assert(device_state_sleep_restore(100000));
	assert(strcmp(selected, "a.mp3") == 0 && sought == 590 && jump_marks == 0);
	assert(floor_armed && device_state_seek_serial() == serial);
	// Extending/restarting, pausing, and explicit navigation supersede old samples.
	test_status = AUDIO_STATUS_PLAYING;
	test_now_ms = 110000;
	current_position = 200;
	device_state_sleep_checkpoint(120000);
	current_position = 220;
	device_state_sleep_checkpoint(180000);
	assert(!device_state_sleep_restore(120000));
	device_state_sleep_checkpoint(0);
	assert(!device_state_sleep_restore(180000));
	device_state_sleep_checkpoint(180000);
	test_status = AUDIO_STATUS_PAUSED;
	device_state_sleep_checkpoint(180000);
	assert(!device_state_sleep_restore(180000));
	test_status = AUDIO_STATUS_PLAYING;
	test_now_ms = 170000;
	current_position = 300;
	device_state_sleep_checkpoint(180000);
	device_state_seek(400);
	current_position = 400;
	assert(device_state_sleep_restore(180000) && sought == 400);
	puts("PASS sleep-audible-checkpoint");
}

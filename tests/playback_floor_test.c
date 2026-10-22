#define main audiobook_units_main
#include "audiobook_units_test.c"
#undef main
#define now_ms device_now_ms
#include "../sonix-player/src/system/playback/device_state.c"
#undef now_ms
static double outgoing = 70;
void audio_get_progress(double *current, double *total) { *current = outgoing; *total = 600; }
int main(void) {
	audiobook_track_changed("/other.m4b");
	audiobook_set_rewind_floor("/other.m4b", 100);
	position_writes = 0;
	supersede_book_restore();
	change_book_track("/music.mp3");
	assert(saved_seconds == 100 && strcmp(saved_file, "/other.m4b") == 0);
	assert(position_writes == 1 && !explicit_book_load);
	audiobook_track_changed("/book/2.mp3");
	audiobook_set_rewind_floor("/book/2.mp3", 10);
	audiobook_track_changed("/book/1.mp3");
	outgoing = 580;
	position_writes = 0;
	supersede_book_restore();
	change_book_track("/other.m4b");
	assert(saved_seconds == 10 && strcmp(saved_file, "/book/2.mp3") == 0);
	assert(position_writes == 1 && !explicit_book_load);
	puts("PASS playback-outgoing-floor");
}

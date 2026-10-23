// Integration bench: the real ZIP/parser/config/controller with a deterministic
// audio backend. Can also be cross-compiled and run on the player via ADB.
#undef NDEBUG // the bench's assertions must run in optimized cross builds too
#include "src/system/audio/audio.h"
#include "src/system/core/config.h"
#include "src/system/ebook/ebookaudio.h"
#include "src/system/ebook/epub_internal.h"
#include "src/system/image/miniz/tinfl_impl.inc"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char current_file[512];
static double current_seconds, total_seconds = 300;
static audio_status_t status;
static bool completed;
static bool external;
static unsigned plays;
static double playback_speed = 1;
void device_state_begin_ebook_audio(void) { audio_stop(); }
int audio_play_at(const char *path, double at) {
	snprintf(current_file, sizeof(current_file), "%s", path);
	current_seconds = at;
	status = AUDIO_STATUS_PLAYING;
	completed = false;
	plays++;
	return 0;
}
int audio_play_paused(const char *path, double at) {
	audio_play_at(path, at);
	status = AUDIO_STATUS_PAUSED;
	return 0;
}
void audio_pause(void) { status = AUDIO_STATUS_PAUSED; }
void audio_resume(void) { status = AUDIO_STATUS_PLAYING; }
void audio_stop(void) {
	status = AUDIO_STATUS_STOPPED;
	completed = false;
}
audio_status_t audio_get_status(void) { return status; }
void audio_get_current_file(char *out, size_t size) { snprintf(out, size, "%s", current_file); }
void audio_get_progress(double *at, double *total) {
	if (at)
		*at = current_seconds;
	if (total)
		*total = total_seconds;
}
void audio_seek(double at) { current_seconds = at; }
void audio_set_speed(double speed) {
	assert(speed >= 0.5 && speed <= 3);
	playback_speed = speed;
}
void audio_set_sync_clock(bool enabled) { (void)enabled; }
double audio_get_audible_position(void) { return current_seconds; }
bool audio_take_completion(void) {
	bool result = completed;
	completed = false;
	return result;
}
bool audio_external_wanted(void) { return external; }

static void filename(char *out, size_t size, const char *dir, const char *name) { assert(snprintf(out, size, "%s/%s", dir, name) < (int)size); }
static ebook_t *open_book(const char *dir, const char *name) {
	char path[512];
	filename(path, sizeof(path), dir, name);
	ebook_t *b = ebook_open(path);
	assert(b);
	assert(ebook_open_chapter(b, 0));
	return b;
}
static void wait_for_audio(void) {
	for (unsigned i = 0; i < 10000 && ebookaudio_loading(); i++) {
		usleep(1000);
		ebookaudio_tick(i);
	}
	assert(!ebookaudio_loading());
	assert(!ebookaudio_error());
}
static bool cancel_always(void *user) {
	(void)user;
	return true;
}
static bool cancel_later(void *user) { return ++*(unsigned *)user > 4; }
static void compare_files(const char *first, const char *second) {
	FILE *a = fopen(first, "rb"), *b = fopen(second, "rb");
	assert(a && b);
	unsigned char x[4096], y[4096];
	for (;;) {
		size_t nx = fread(x, 1, sizeof(x), a), ny = fread(y, 1, sizeof(y), b);
		assert(nx == ny && memcmp(x, y, nx) == 0);
		if (!nx)
			break;
	}
	fclose(a);
	fclose(b);
}
int main(int argc, char **argv) {
	assert(argc == 2);
	setbuf(stdout, NULL);
	setbuf(stderr, NULL);
	fprintf(stderr, "bench: parsing\n");
	const char *dir = argv[1];
	char path[512], out[512], expected[512];
	filename(path, sizeof(path), dir, "ebook.ini");
	FILE *config = fopen(path, "wb");
	assert(config);
	fclose(config);
	assert(config_store_init(config_ebook_store(), path));

	ebook_t *b = open_book(dir, "plain.epub");
	assert(ebook_audio_count(b) == 0 && !ebook_audio_unavailable(b));
	ebook_close(b);
	b = open_book(dir, "inline.epub");
	assert(ebook_audio_count(b) == 2);
	assert(strcmp(ebook_audio_clip(b, 0)->path, "EPUB/audio/a & b.wav") == 0);
	assert(strcmp(ebook_audio_clip(b, 1)->extension, "wav") == 0); // MIME-only resource, no extension
	assert(ebook_audio_clip(b, 0)->begin == 0 && ebook_audio_clip(b, 0)->end < 0);
	assert(ebook_block_count(b) == 2); // fallback text and <video> not displayed
	ebook_close(b);

	b = open_book(dir, "overlay.epub");
	assert(ebook_audio_count(b) == 3); // adjacent sentences become one seekable passage
	assert(ebook_audio_clip(b, 0)->begin == 1 && ebook_audio_clip(b, 0)->end == 5);
	assert(ebook_audio_clip(b, 1)->begin == 65.5 && ebook_audio_clip(b, 1)->end == 120);
	assert(ebook_audio_clip(b, 2)->begin == 3600 && ebook_audio_clip(b, 2)->end < 0);
	ebook_close(b);
	b = open_book(dir, "invalid.epub");
	assert(ebook_audio_count(b) == 0 && ebook_audio_unavailable(b));
	ebook_close(b);
	b = open_book(dir, "many.epub");
	assert(ebook_audio_count(b) == 8192 && ebook_audio_unavailable(b));
	ebook_close(b);

	// Normalized whitespace, nested styled runs, escaped fragments and reversed
	// audio/text child order must all resolve to the actual displayed text.
	b = open_book(dir, "sync.epub");
	assert(ebook_audio_count(b) == 1 && ebook_audio_cue_count(b) == 3);
	const ebook_audio_cue_t *first = ebook_audio_cue(b, 0), *second = ebook_audio_cue(b, 1), *third = ebook_audio_cue(b, 2);
	assert(first->begin == 1 && first->end == 3 && first->clip == 0);
	assert(first->text_end - first->text_begin == strlen("Hello bright world."));
	assert(memcmp(ebook_chapter_text(b) + first->text_begin, "Hello bright world.", strlen("Hello bright world.")) == 0);
	assert(second->text_begin != UINT32_MAX && second->text_end > second->text_begin);
	assert(ebook_audio_cue_for_text(b, second->text_begin) == 1);
	uint32_t block_index = 0, block_offset = 0;
	assert(ebook_text_position(b, third->text_begin, &block_index, &block_offset));
	assert(ebook_text_offset(b, block_index, block_offset) == third->text_begin);
	ebookaudio_attach(b, dir);
	assert(ebookaudio_seek_text(second->text_begin));
	assert(!ebookaudio_active());
	ebookaudio_toggle();
	wait_for_audio();
	assert(current_seconds == 3 && ebookaudio_cue_index() == 1);
	unsigned before_step = plays;
	ebookaudio_step(1);
	assert(current_seconds == 7 && ebookaudio_cue_index() == 2 && plays == before_step);
	ebookaudio_step(-1);
	assert(current_seconds == 3 && ebookaudio_cue_index() == 1 && plays == before_step);
	ebookaudio_set_speed(17);
	assert(ebookaudio_speed() == 17 && playback_speed == 1.7);
	ebookaudio_set_speed(100);
	assert(ebookaudio_speed() == 30);
	ebookaudio_set_speed(0);
	assert(ebookaudio_speed() == 5);
	ebookaudio_set_speed(17);
	uint32_t a, z, reading;
	assert(ebookaudio_highlight(&a, &z, &reading) && a == second->text_begin && z == second->text_end);
	uint32_t midpoint = third->text_begin + (third->text_end - third->text_begin) / 2;
	assert(ebookaudio_seek_text(midpoint));
	assert(current_seconds > 11.8 && current_seconds < 12.1);
	assert(ebookaudio_highlight(&a, &z, &reading) && reading == midpoint);
	ebookaudio_toggle();
	assert(status == AUDIO_STATUS_PAUSED);
	ebookaudio_step(-1);
	assert(status == AUDIO_STATUS_PAUSED && current_seconds == 3);
	ebookaudio_detach();
	assert(playback_speed == 1);
	ebookaudio_attach(b, dir);
	assert(ebookaudio_speed() == 17 && !ebookaudio_active());
	ebookaudio_set_speed(10);
	ebookaudio_detach();
	ebook_close(b);

	// Dense chapters retain only compact cues; no ID index or SMIL source.
	b = open_book(dir, "dense.epub");
	assert(ebook_audio_count(b) == 1 && ebook_audio_cue_count(b) == 4000);
	assert(b->audio_targets == NULL);
	for (uint32_t i = 0; i < 4000; i++) {
		const ebook_audio_cue_t *cue = ebook_audio_cue(b, i);
		assert(cue->text_begin != UINT32_MAX && cue->text_end > cue->text_begin);
		assert(ebook_audio_cue_at(b, 0, i + 0.5) == i);
	}
	printf("dense sync: 4000 cues, %zu KiB retained arenas\n", (ebook_book_bytes(b) + ebook_chapter_bytes(b)) / 1024);
	assert(ebook_book_bytes(b) + ebook_chapter_bytes(b) < 1536u * 1024u);
	ebook_close(b);

	// Both ZIP methods, multiple input/output rings, CRC failures, cancellation,
	fprintf(stderr, "bench: extraction\n");
	// existing destination protection and no partial output on failure.
	for (unsigned method = 0; method < 2; method++) {
		filename(path, sizeof(path), dir, method ? "deflated.epub" : "stored.epub");
		filename(out, sizeof(out), dir, "extracted.wav");
		unlink(out);
		assert(ebook_extract_audio(path, "EPUB/audio/long.wav", out, NULL, NULL));
		filename(expected, sizeof(expected), dir, "long.wav");
		compare_files(out, expected);
		assert(!ebook_extract_audio(path, "EPUB/audio/long.wav", out, NULL, NULL));
		compare_files(out, expected);
		unlink(out);
		assert(!ebook_extract_audio(path, "EPUB/audio/long.wav", out, cancel_always, NULL));
		assert(access(out, F_OK) != 0);
		unsigned calls = 0;
		assert(!ebook_extract_audio(path, "EPUB/audio/long.wav", out, cancel_later, &calls));
		assert(access(out, F_OK) != 0);
	}
	filename(path, sizeof(path), dir, "corrupt.epub");
	assert(!ebook_extract_audio(path, "EPUB/audio/long.wav", out, NULL, NULL));
	assert(access(out, F_OK) != 0);
	filename(path, sizeof(path), dir, "truncated.epub");
	assert(!ebook_extract_audio(path, "EPUB/audio/long.wav", out, NULL, NULL));
	assert(access(out, F_OK) != 0);

	// Controller: never autoplay, async preparation, pause/resume, relative
	fprintf(stderr, "bench: playback controller\n");
	// seeking, SMIL continuity, discontinuity, saved resume, natural completion.
	b = open_book(dir, "overlay.epub");
	ebookaudio_attach(b, dir);
	assert(!ebookaudio_active());
	ebookaudio_toggle();
	assert(ebookaudio_loading());
	ebookaudio_toggle(); // pause during extraction
	wait_for_audio();
	assert(!ebookaudio_playing() && status == AUDIO_STATUS_PAUSED);
	ebookaudio_toggle();
	assert(ebookaudio_playing() && current_seconds == 1);
	unsigned started_plays = plays;
	current_seconds = 3.1;
	ebookaudio_tick(20000);
	assert(ebookaudio_index() == 0 && plays == started_plays); // same file, no seek/reopen
	ebookaudio_seek(3.5);
	assert(current_seconds == 4.5);
	ebookaudio_toggle();
	assert(status == AUDIO_STATUS_PAUSED);
	ebookaudio_detach();
	ebookaudio_attach(b, dir);
	assert(ebookaudio_index() == 0 && !ebookaudio_active());
	double at = 0;
	ebookaudio_progress(&at, NULL);
	assert(fabs(at - 3.5) < 0.001);
	ebookaudio_toggle();
	wait_for_audio();
	assert(current_seconds == 4.5);
	ebookaudio_seek_source(4);
	ebookaudio_tick(21000);
	assert(current_seconds == 4); // physical key time is source-relative
	ebookaudio_seek(1e6);
	assert(current_seconds < 5);
	current_seconds = 5.1;
	ebookaudio_tick(22000);
	assert(ebookaudio_index() == 1 && current_seconds == 65.5 && plays > started_plays);
	ebookaudio_step(1);
	assert(ebookaudio_index() == 2 && current_seconds == 3600);
	completed = true;
	status = AUDIO_STATUS_STOPPED;
	ebookaudio_tick(23000);
	assert(!ebookaudio_playing() && !completed && !ebookaudio_error());
	assert(ebookaudio_take_chapter_completion());
	assert(!ebookaudio_take_chapter_completion());
	ebookaudio_leave_chapter();
	ebook_close_chapter(b);
	assert(ebook_open_chapter(b, 0));
	ebookaudio_attach(b, dir);
	ebookaudio_toggle();
	assert(!ebookaudio_loading()); // one cached disk resource survives chapter handoff
	ebookaudio_detach();
	ebook_close(b);

	// Closing/changing chapters cancels a live worker before freeing arenas.
	fprintf(stderr, "bench: cancellation\n");
	b = open_book(dir, "deflated.epub");
	ebookaudio_attach(b, dir);
	ebookaudio_toggle();
	assert(ebookaudio_loading());
	ebookaudio_detach();
	ebook_close(b);
	assert(!ebookaudio_active());

	// Decoder failure and somebody else taking the output must not resume a
	// stale music queue or keep control of the replacement track.
	b = open_book(dir, "inline.epub");
	ebookaudio_attach(b, dir);
	ebookaudio_toggle();
	wait_for_audio();
	status = AUDIO_STATUS_STOPPED;
	ebookaudio_tick(24000);
	assert(ebookaudio_error());
	ebookaudio_toggle();
	assert(ebookaudio_playing());
	snprintf(current_file, sizeof(current_file), "/music/replacement.mp3");
	ebookaudio_tick(25000);
	assert(!ebookaudio_active());
	ebookaudio_toggle();
	wait_for_audio();
	external = true;
	ebookaudio_tick(26000);
	assert(!ebookaudio_active());
	external = false;
	ebookaudio_detach();
	ebook_close(b);
	puts("PASS: EPUB audio parsing, bounded extraction, corruption/cancellation, playback and resume");
	return 0;
}

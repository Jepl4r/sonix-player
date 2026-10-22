#include <pthread.h>
#include <errno.h>
static void pause_hook(void);
#define AUDIO_PAUSE_HOOK pause_hook
#define main part_step_main
#include "playback_part_step_test.c"
#undef main
bool playlist_locate(const char *file) { return true; }

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool restoring, release_restore, seek_started, seek_done;

static void pause_hook(void) {
	// External audio/load callbacks must not inherit the checkpoint state lock.
	assert(pthread_mutex_trylock(&book_checkpoint_lock) == 0);
	pthread_mutex_unlock(&book_checkpoint_lock);
	pthread_mutex_lock(&gate);
	restoring = true;
	pthread_cond_broadcast(&changed);
	while (!release_restore)
		pthread_cond_wait(&changed, &gate);
	pthread_mutex_unlock(&gate);
}
static void *restore_thread(void *unused) {
	assert(device_state_sleep_restore(60000));
	return NULL;
}
static void *seek_thread(void *unused) {
	pthread_mutex_lock(&gate);
	seek_started = true;
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&gate);
	device_state_seek(400);
	pthread_mutex_lock(&gate);
	seek_done = true;
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&gate);
	return NULL;
}
int main(void) {
	navigation_test = true;
	test_status = AUDIO_STATUS_PLAYING;
	strcpy(current_metadata_file, "a.mp3");
	strcpy(selected, "a.mp3");
	current_position = 98;
	test_now_ms = 39000;
	device_state_sleep_checkpoint(60000);
	pthread_t restore, seek;
	assert(pthread_create(&restore, NULL, restore_thread, NULL) == 0);
	pthread_mutex_lock(&gate);
	while (!restoring)
		pthread_cond_wait(&changed, &gate);
	assert(pthread_create(&seek, NULL, seek_thread, NULL) == 0);
	while (!seek_started)
		pthread_cond_wait(&changed, &gate);
	// The old implementation lets the explicit seek finish inside a stale restore.
	struct timespec timeout;
	clock_gettime(CLOCK_REALTIME, &timeout);
	timeout.tv_sec++;
	while (!seek_done) {
		int rc = pthread_cond_timedwait(&changed, &gate, &timeout);
		if (rc == ETIMEDOUT)
			break;
		assert(rc == 0);
	}
	release_restore = true;
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&gate);
	pthread_join(restore, NULL);
	pthread_join(seek, NULL);
	assert(sought == 400);
	assert(seek_done && !sleep_checkpoint_file[0]);
	puts("PASS sleep-checkpoint-concurrent-seek");
}

#include "src/system/ebook/ebookaudio.h"
#include "src/system/audio/audio.h"
#include "src/system/core/config.h"
#include "src/system/ebook/bookmarks.h"
#include "src/system/playback/device_state.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// A single extracted resource on the card, never /tmp (RAM on the player).
// The worker owns a separate archive handle and copies of every name, so a
// page turn cannot invalidate memory it is inflating from.
typedef struct {
	pthread_t thread;
	atomic_bool cancelled;
	atomic_bool done;
	bool ok;
	char book[512], member[1024], destination[512];
} extract_job_t;

static ebook_t *book;
static extract_job_t *job;
static char card[512], cache_dir[512], cached_file[512], cached_member[1024];
static uint32_t clip_index, last_save_ms;
static double position;
static atomic_bool owns_output;
static bool started, paused, listened;
static pthread_mutex_t seek_lock = PTHREAD_MUTEX_INITIALIZER;
static double pending_seek = -1;
static const char *error_tag;
static int speed_tenths = 10;
static bool chapter_complete, retain_cache;
static char retained_book[512];

static const ebook_audio_clip_t *clip(void) { return ebook_audio_clip(book, clip_index); }

static void save_position(void) {
	if (!book || !clip() || !listened || !isfinite(position))
		return;
	char key[32], hash[17], value[96];
	bookmark_key(ebook_path(book), hash, sizeof(hash));
	snprintf(key, sizeof(key), "audio_%s", hash);
	snprintf(value, sizeof(value), "%u:%u:%.3f", ebook_chapter_index(book), clip_index, position);
	config_store_set(config_ebook_store(), "ebook_audio", key, value);
	config_store_save(config_ebook_store());
}

static bool cancelled(void *user) { return atomic_load(&((extract_job_t *)user)->cancelled); }
static void *extract_worker(void *user) {
	extract_job_t *work = user;
	work->ok = ebook_extract_audio(work->book, work->member, work->destination, cancelled, work);
	atomic_store(&work->done, true);
	return NULL;
}

static void cancel_job(void) {
	if (!job)
		return;
	atomic_store(&job->cancelled, true);
	pthread_join(job->thread, NULL); // bounded by one 32 KiB read/write
	unlink(job->destination);
	free(job);
	job = NULL;
}

static bool current_is_ours(void) {
	char current[512];
	audio_get_current_file(current, sizeof(current));
	return cached_file[0] && strcmp(current, cached_file) == 0;
}

static void sample_position(void) {
	if (!started || !owns_output || !current_is_ours())
		return;
	double now = 0;
	now = audio_get_audible_position();
	const ebook_audio_clip_t *c = clip();
	if (c && isfinite(now) && now >= c->begin)
		position = now;
}

void ebookaudio_stop(void) {
	chapter_complete = false;
	sample_position();
	save_position();
	cancel_job();
	if (owns_output && (current_is_ours() || !started)) {
		audio_stop();
		audio_set_speed(1.0);
		audio_set_sync_clock(false);
	}
	owns_output = started = paused = false;
	pthread_mutex_lock(&seek_lock);
	pending_seek = -1;
	pthread_mutex_unlock(&seek_lock);
}

void ebookaudio_detach(void) {
	retain_cache = false;
	ebookaudio_stop();
	if (cached_file[0])
		unlink(cached_file);
	if (cache_dir[0])
		rmdir(cache_dir);
	cache_dir[0] = cached_file[0] = cached_member[0] = '\0';
	book = NULL;
	error_tag = NULL;
}

void ebookaudio_leave_chapter(void) {
	snprintf(retained_book, sizeof(retained_book), "%s", book ? ebook_path(book) : "");
	ebookaudio_stop();
	book = NULL;
	error_tag = NULL;
	retain_cache = true;
}

bool ebookaudio_take_chapter_completion(void) {
	bool result = chapter_complete;
	chapter_complete = false;
	return result;
}

void ebookaudio_attach(ebook_t *new_book, const char *sd_root) {
	if (!retain_cache || !new_book || strcmp(retained_book, ebook_path(new_book)))
		ebookaudio_detach();
	retain_cache = false;
	book = new_book;
	snprintf(card, sizeof(card), "%s", sd_root ? sd_root : "");
	clip_index = 0;
	position = 0;
	listened = false;
	last_save_ms = 0;
	speed_tenths = config_store_get_int(config_ebook_store(), "ebook_audio", "speed", 10);
	if (speed_tenths < 5 || speed_tenths > 30)
		speed_tenths = 10;
	if (!clip())
		return;
	position = clip()->begin;
	char key[32], hash[17];
	bookmark_key(ebook_path(book), hash, sizeof(hash));
	snprintf(key, sizeof(key), "audio_%s", hash);
	const char *value = config_store_get(config_ebook_store(), "ebook_audio", key, "");
	unsigned spine = 0, saved_clip = 0;
	double saved_position = 0;
	if (sscanf(value, "%u:%u:%lf", &spine, &saved_clip, &saved_position) == 3 && spine == ebook_chapter_index(book) && saved_clip < ebook_audio_count(book) && isfinite(saved_position)) {
		clip_index = saved_clip;
		const ebook_audio_clip_t *c = clip();
		position = saved_position >= c->begin && (c->end < 0 || saved_position < c->end) ? saved_position : c->begin;
	}
}

static void fail(const char *tag) {
	error_tag = tag;
	if (owns_output && current_is_ours())
		audio_stop();
	started = false;
	paused = false;
}

static void start_cached(void) {
	const ebook_audio_clip_t *c = clip();
	if (!c)
		return;
	audio_set_speed(speed_tenths / 10.0);
	audio_set_sync_clock(true);
	// play_at sets the seek and the play under the same audio mutex; it cannot
	// briefly play the beginning of a shared narration resource before seeking.
	if (paused)
		audio_play_paused(cached_file, position);
	else
		audio_play_at(cached_file, position);
	started = true;
}

static void request_play(void) {
	chapter_complete = false;
	const ebook_audio_clip_t *c = clip();
	if (!c)
		return;
	error_tag = NULL;
	if (!owns_output) {
		device_state_begin_ebook_audio();
		owns_output = true;
	}
	listened = true;
	paused = false;
	started = false;
	cancel_job();
	audio_stop();
	if (cached_file[0] && strcmp(cached_member, c->path) == 0 && access(cached_file, R_OK) == 0) {
		start_cached();
		return;
	}
	if (cached_file[0])
		unlink(cached_file);
	cached_file[0] = cached_member[0] = '\0';
	if (!cache_dir[0]) {
		if (!card[0] || snprintf(cache_dir, sizeof(cache_dir), "%s/.ebook-audio-XXXXXX", card) >= (int)sizeof(cache_dir) || !mkdtemp(cache_dir)) {
			cache_dir[0] = '\0';
			fail("ebookaudio_cache_failed");
			return;
		}
	}
	job = calloc(1, sizeof(*job));
	if (!job) {
		fail("ebookaudio_extract_failed");
		return;
	}
	atomic_init(&job->cancelled, false);
	atomic_init(&job->done, false);
	snprintf(job->book, sizeof(job->book), "%s", ebook_path(book));
	snprintf(job->member, sizeof(job->member), "%s", c->path);
	if (snprintf(job->destination, sizeof(job->destination), "%s/narration.%s", cache_dir, c->extension) >= (int)sizeof(job->destination)) {
		free(job);
		job = NULL;
		fail("ebookaudio_cache_failed");
		return;
	}
	if (pthread_create(&job->thread, NULL, extract_worker, job) != 0) {
		free(job);
		job = NULL;
		fail("ebookaudio_extract_failed");
	}
}

bool ebookaudio_active(void) { return owns_output; }
bool ebookaudio_owns_path(const char *path) { return path && strstr(path, "/.ebook-audio-") != NULL; }
bool ebookaudio_loading(void) { return job != NULL; }
bool ebookaudio_playing(void) { return owns_output && !paused && (job || (started && audio_get_status() == AUDIO_STATUS_PLAYING)); }
const char *ebookaudio_error(void) { return error_tag; }
uint32_t ebookaudio_index(void) { return clip_index; }

void ebookaudio_progress(double *at, double *duration) {
	const ebook_audio_clip_t *c = clip();
	double total = 0;
	if (owns_output && started)
		audio_get_progress(NULL, &total);
	if (at)
		*at = c ? fmax(0, position - c->begin) : 0;
	if (duration)
		*duration = c ? fmax(0, (c->end >= 0 ? c->end : total) - c->begin) : 0;
}

void ebookaudio_toggle(void) {
	if (!clip())
		return;
	if (owns_output && job) {
		paused = !paused;
		return;
	}
	if (owns_output && started && current_is_ours()) {
		if (audio_get_status() == AUDIO_STATUS_PLAYING) {
			audio_pause();
			paused = true;
			sample_position();
			save_position();
			return;
		}
		if (audio_get_status() == AUDIO_STATUS_PAUSED) {
			paused = false;
			audio_resume();
			return;
		}
	}
	request_play();
}

static void select_position(uint32_t index, double seconds) {
	chapter_complete = false;
	const ebook_audio_clip_t *next = ebook_audio_clip(book, index);
	if (!next)
		return;
	clip_index = index;
	position = seconds;
	error_tag = NULL;
	if (owns_output && started && current_is_ours() && strcmp(cached_member, next->path) == 0) {
		audio_seek(position);
	} else if (owns_output && job && strcmp(job->member, next->path) == 0) {
		// The same resource is already being prepared. Only its target changes.
	} else if (owns_output) {
		bool keep_paused = paused;
		request_play();
		paused = keep_paused;
		if (paused && started)
			audio_pause();
	}
	save_position();
}

uint32_t ebookaudio_cue_index(void) { return ebook_audio_cue_at(book, clip_index, position); }

bool ebookaudio_highlight(uint32_t *begin, uint32_t *end, uint32_t *reading) {
	if (!owns_output || !started || error_tag)
		return false;
	const ebook_audio_cue_t *cue = ebook_audio_cue(book, ebookaudio_cue_index());
	if (!cue || cue->text_begin == UINT32_MAX || cue->text_end <= cue->text_begin || position < cue->begin || (cue->end >= 0 && position >= cue->end))
		return false;
	*begin = cue->text_begin;
	*end = cue->text_end;
	// A long authored sentence can span two device pages. Without word-level
	// timestamps, interpolate only the page-follow point within that sentence.
	double fraction = cue->end > cue->begin ? (position - cue->begin) / (cue->end - cue->begin) : 0;
	*reading = *begin + (uint32_t)(fraction * (*end - *begin));
	return true;
}

bool ebookaudio_seek_text(uint32_t text) {
	uint32_t index = ebook_audio_cue_for_text(book, text);
	const ebook_audio_cue_t *cue = ebook_audio_cue(book, index);
	if (!cue)
		return false;
	// A page may start halfway through a long sentence. Its proportion of the
	// authored range provides the closest available time without guessing
	// timings for other sentences or retaining audio samples in memory.
	double seconds = cue->begin;
	if (text > cue->text_begin && text < cue->text_end && cue->end > cue->begin)
		seconds += (cue->end - cue->begin) * (text - cue->text_begin) / (cue->text_end - cue->text_begin);
	select_position(cue->clip, seconds);
	return true;
}

int ebookaudio_speed(void) { return speed_tenths; }
void ebookaudio_set_speed(int tenths) {
	if (tenths < 5)
		tenths = 5;
	if (tenths > 30)
		tenths = 30;
	if (tenths == speed_tenths)
		return;
	speed_tenths = tenths;
	if (owns_output)
		audio_set_speed(tenths / 10.0);
	config_store_set_int(config_ebook_store(), "ebook_audio", "speed", tenths);
	config_store_save(config_ebook_store());
}

void ebookaudio_step(int direction) {
	if (!clip())
		return;
	sample_position();
	save_position();
	uint32_t cue = ebookaudio_cue_index();
	if (cue != UINT32_MAX) {
		int64_t next = (int64_t)cue + (direction < 0 ? -1 : 1);
		const ebook_audio_cue_t *target = next >= 0 ? ebook_audio_cue(book, (uint32_t)next) : NULL;
		if (target) {
			select_position(target->clip, target->begin);
			if (!owns_output)
				request_play();
		}
		return;
	}
	int64_t next = (int64_t)clip_index + (direction < 0 ? -1 : 1);
	if (next < 0 || next >= ebook_audio_count(book))
		return;
	clip_index = (uint32_t)next;
	position = clip()->begin;
	request_play();
}

void ebookaudio_seek(double seconds) {
	if (!clip() || !isfinite(seconds))
		return;
	double duration = 0;
	ebookaudio_progress(NULL, &duration);
	if (seconds < 0)
		seconds = 0;
	if (duration > 0 && seconds >= duration)
		seconds = fmax(0, duration - 0.05);
	position = clip()->begin + seconds;
	if (owns_output && started && current_is_ours())
		audio_seek(position);
	save_position();
}

void ebookaudio_seek_source(double seconds) {
	if (!isfinite(seconds))
		return;
	pthread_mutex_lock(&seek_lock);
	pending_seek = fmax(0, seconds);
	pthread_mutex_unlock(&seek_lock);
}

void ebookaudio_tick(uint32_t now_ms) {
	if (!book || !owns_output)
		return;
	if (audio_external_wanted()) {
		ebookaudio_stop();
		return;
	}
	pthread_mutex_lock(&seek_lock);
	double seek = pending_seek;
	pending_seek = -1;
	pthread_mutex_unlock(&seek_lock);
	if (seek >= 0 && clip())
		ebookaudio_seek(seek - clip()->begin);
	if (job && atomic_load(&job->done)) {
		pthread_join(job->thread, NULL);
		bool ok = job->ok;
		if (ok) {
			snprintf(cached_file, sizeof(cached_file), "%s", job->destination);
			snprintf(cached_member, sizeof(cached_member), "%s", job->member);
		}
		free(job);
		job = NULL;
		if (!ok) {
			fail("ebookaudio_extract_failed");
			return;
		}
		start_cached();
	}
	if (!started)
		return;
	if (!current_is_ours()) {
		ebookaudio_stop();
		return;
	}
	sample_position();
	if ((uint32_t)(now_ms - last_save_ms) >= 10000) {
		save_position();
		last_save_ms = now_ms;
	}
	const ebook_audio_clip_t *c = clip();
	bool completed = audio_take_completion();
	if (!completed && audio_get_status() == AUDIO_STATUS_STOPPED) {
		fail("ebookaudio_play_failed");
		return;
	}
	if (!completed && (paused || audio_get_status() != AUDIO_STATUS_PLAYING || c->end < 0 || position < c->end))
		return;
	if (!completed && c->end < 0)
		return;
	uint32_t next = clip_index + 1;
	if (next < ebook_audio_count(book)) {
		const ebook_audio_clip_t *following = ebook_audio_clip(book, next);
		// Sentence-sized SMIL clips often share one continuous audio file.
		// Keep that decoder open across adjacent clips, rather than seeking at
		// every sentence and repeatedly dropping the output's PCM queue.
		bool continuous = !completed && strcmp(c->path, following->path) == 0 && c->end >= 0 && fabs(c->end - following->begin) < 0.02;
		clip_index = next;
		position = following->begin;
		if (!continuous)
			request_play();
	} else {
		audio_stop();
		started = paused = false;
		position = c->begin; // replay the last clip after finishing
		save_position();
		chapter_complete = true;
	}
}

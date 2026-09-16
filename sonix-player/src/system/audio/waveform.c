#include "waveform.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/system/decode/decode.h"
#include "src/system/decode/sndfile.h"
#include "src/system/core/utils.h"

// How many frames are read at a time. Small enough not to hold half a megabyte
// while the playback thread wants memory, large enough that the per-call cost
// of the decoder disappears against the decoding itself.
#define CHUNK_FRAMES 4096

// The scale the columns are worked out on, and the highest value that ever
// comes out of it. The ceiling is headroom: a column drawn at the full height
// of the box sits flat against its edge, and a row of those is the exact
// picture of audio that has been clipped -- which is what the shape of a track
// must not be mistaken for.
#define WAVE_SCALE 4096
#define WAVE_CEILING 235

// The file, and one entry in it.
//
// Fixed-size records and a linear scan: fifty-six bytes a track, so a card
// holding six thousand of them makes this file about 330 kB and a lookup a
// single read of it, once per track change. An index would be a second
// structure to keep consistent for no useful gain.
//
// The key is a hash and not the path itself, which is what keeps the record
// fixed-size. It covers the path AND the file's size and modification time, so
// that replacing a file with different audio under the same name does not go
// on showing the old shape. Sixty-four bits of FNV-1a over a few thousand rows
// makes a collision vanishingly unlikely.
#define WAVE_MAGIC 0x57584E53u // "SNXW"
#define WAVE_VERSION 3		   // 3: the key covers size and mtime as well as the path
#define WAVE_MAX_RECORDS 8000

typedef struct {
	uint32_t magic;
	uint32_t version;
	uint32_t count;
	uint32_t bars; // so a build with a different column count does not read old rows as new ones
} wave_header_t;

typedef struct {
	uint64_t key;
	uint8_t bars[WAVEFORM_BARS];
} wave_record_t;

static char cache_path[512];

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static pthread_t worker;
static bool worker_running;
static bool screen_on = true;

// What the interface last asked for, what the worker is doing, and the answer.
static char wanted[1024];	 // the track on screen
static char computing[1024]; // what the worker picked up
static char have_path[1024]; // what `have_bars` belongs to
static uint8_t have_bars[WAVEFORM_BARS];
static bool have_answer;

static long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint64_t fnv_bytes(uint64_t h, const void *data, size_t len) {
	const unsigned char *p = data;
	for (size_t i = 0; i < len; i++) {
		h ^= p[i];
		h *= 1099511628211ull;
	}
	return h;
}

// The identity of a track's shape: what it is called, how big it is, and when
// it was last written. A file that was swapped for another one keeps neither
// its size nor its time in practice, so the shape is recomputed rather than
// taken from the row the old file left behind.
static uint64_t path_key(const char *path) {
	uint64_t h = fnv_bytes(1469598103934665603ull, path, strlen(path));

	struct stat st;
	if (stat(path, &st) == 0) {
		uint64_t size = (uint64_t)st.st_size;
		uint64_t when = (uint64_t)st.st_mtime;
		h = fnv_bytes(h, &size, sizeof(size));
		h = fnv_bytes(h, &when, sizeof(when));
	}
	return h;
}

// ---------------------------------------------------------------------------
// the file
// ---------------------------------------------------------------------------

// The last few shapes, kept in memory so that going back to a track does not
// read the file again.
//
// A lookup is a linear scan of waveform.dat, and on a full card that is some
// four hundred kilobytes off the same microSD the music is streaming from,
// once per track change. Eight entries is four hundred and fifty bytes and
// covers what a listener actually does: skipping back a track, flicking
// between two records, a queue that repeats. Playing an album straight through
// is all new keys and misses every time -- this does not pretend otherwise.
#define WAVE_HOT_CACHE 8

typedef struct {
	uint64_t key;
	uint8_t bars[WAVEFORM_BARS];
	bool valid;
} wave_hot_t;

static wave_hot_t hot[WAVE_HOT_CACHE];
static int hot_next; // where the next one goes: round and round, oldest first

static bool hot_lookup(uint64_t key, uint8_t *out) {
	for (int i = 0; i < WAVE_HOT_CACHE; i++) {
		if (hot[i].valid && hot[i].key == key) {
			memcpy(out, hot[i].bars, WAVEFORM_BARS);
			return true;
		}
	}
	return false;
}

static void hot_store(uint64_t key, const uint8_t *bars) {
	for (int i = 0; i < WAVE_HOT_CACHE; i++) {
		if (hot[i].valid && hot[i].key == key) {
			return; // already here, and the contents cannot have changed
		}
	}
	hot[hot_next].key = key;
	memcpy(hot[hot_next].bars, bars, WAVEFORM_BARS);
	hot[hot_next].valid = true;
	hot_next = (hot_next + 1) % WAVE_HOT_CACHE;
}

// Only the worker thread touches the two above, from one place each, so they
// need no lock of their own.

static bool cache_lookup(uint64_t key, uint8_t *out) {
	if (!cache_path[0]) {
		return false;
	}
	FILE *f = fopen(cache_path, "rb");
	if (!f) {
		return false;
	}

	// header.count and not end-of-file: only the first `count` records are
	// live, and a file left short or half-written by an interrupted store must
	// not be read past that point.
	wave_header_t header;
	bool found = false;
	if (fread(&header, sizeof(header), 1, f) == 1 && header.magic == WAVE_MAGIC && header.version == WAVE_VERSION &&
		header.bars == WAVEFORM_BARS && header.count <= WAVE_MAX_RECORDS) {
		wave_record_t record;
		for (uint32_t i = 0; i < header.count; i++) {
			if (fread(&record, sizeof(record), 1, f) != 1) {
				break;
			}
			if (record.key == key) {
				memcpy(out, record.bars, WAVEFORM_BARS);
				found = true;
				break;
			}
		}
	}

	fclose(f);
	return found;
}

// Appends one newly worked-out shape.
//
// Appends, and does not look for an existing row first: the only caller is the
// worker, and it only gets here after cache_lookup() has just failed for this
// same key, so the row is known not to be there. A key that is somehow written
// twice costs one wasted record and nothing else -- the lookup takes the first
// it meets and both hold the same shape.
//
// A file that has filled up is started again from scratch rather than
// compacted: the cost of losing it is that some tracks are worked out once
// more, in the background, at idle priority -- which is not a cost worth
// writing a compactor for. It is truncated when that happens, so the rows past
// the count really are gone rather than merely unreachable.
static void cache_store(uint64_t key, const uint8_t *bars) {
	if (!cache_path[0]) {
		return;
	}

	FILE *f = fopen(cache_path, "r+b");
	wave_header_t header = {WAVE_MAGIC, WAVE_VERSION, 0, WAVEFORM_BARS};
	if (f) {
		if (fread(&header, sizeof(header), 1, f) != 1 || header.magic != WAVE_MAGIC ||
			header.version != WAVE_VERSION || header.bars != WAVEFORM_BARS || header.count >= WAVE_MAX_RECORDS) {
			fclose(f); // unreadable, from another build, or full: begin again
			f = NULL;
		}
	}
	if (!f) {
		f = fopen(cache_path, "w+b"); // which truncates
		if (!f) {
			return;
		}
		header.magic = WAVE_MAGIC;
		header.version = WAVE_VERSION;
		header.count = 0;
		header.bars = WAVEFORM_BARS;
	}

	wave_record_t record;
	record.key = key;
	memcpy(record.bars, bars, WAVEFORM_BARS);

	long slot = (long)header.count;
	if (fseek(f, (long)sizeof(header) + slot * (long)sizeof(record), SEEK_SET) == 0 &&
		fwrite(&record, sizeof(record), 1, f) == 1) {
		header.count++;
		if (fseek(f, 0, SEEK_SET) == 0) {
			fwrite(&header, sizeof(header), 1, f);
		}
	}
	fclose(f);
}

// ---------------------------------------------------------------------------
// the work
// ---------------------------------------------------------------------------

// How a job ended, which is three things and not two. "This file will never
// have a shape" has to be told apart from "the job was abandoned": the first is
// an answer worth writing down so the question is not asked again, the second
// must leave no trace, or the track stays marked as having nothing to draw.
typedef enum {
	WAVE_BUILD_OK,			// there is a shape
	WAVE_BUILD_UNAVAILABLE, // and there never will be: no decoder, DSD, unreadable
	WAVE_BUILD_CANCELLED,	// nothing is wrong; the question simply changed
} wave_build_t;

// True when the track the worker is on is still the one the page wants. Checked
// often: a listener skipping through an album leaves a decode behind on every
// press, and carrying on with it is the whole reason a background thread turns
// into a problem.
//
// The screen is deliberately not part of the answer: a shape is for the file,
// not for the moment, and it is kept, so a dark screen is the best time to
// compute one -- the interface is not drawing and an idle-class thread finally
// gets the core.
static bool still_wanted(const char *path) {
	pthread_mutex_lock(&lock);
	bool same = strcmp(wanted, path) == 0;
	pthread_mutex_unlock(&lock);
	return same;
}

static wave_build_t build(const char *path, uint8_t *bars) {
	decode_format_t format = decode_detect_format(path);

	// A plain WAV has no decoder of its own: audio.c plays one straight from
	// the file and decode_detect_format() says UNKNOWN for it. Reading it here
	// still needs a decoder, and libsndfile is one -- the same substitution the
	// cue path makes, for the same reason.
	if (format == DECODE_FORMAT_UNKNOWN && sndfile_available()) {
		format = DECODE_FORMAT_SNDFILE;
	}
	if (format == DECODE_FORMAT_UNKNOWN) {
		return WAVE_BUILD_UNAVAILABLE;
	}
	decoder_t *dec = decoder_open(path, format);
	if (!dec) {
		return WAVE_BUILD_UNAVAILABLE;
	}

	// A DSD stream carries a bit pattern and not a waveform, so its magnitude
	// means nothing. Nothing is drawn for one.
	if (decoder_passthrough(dec)) {
		decoder_close(dec);
		return WAVE_BUILD_UNAVAILABLE;
	}

	uint64_t total = decoder_total_pcm_frames(dec);
	int channels = decoder_channels(dec);
	if (total == 0 || channels <= 0) {
		decoder_close(dec);
		return WAVE_BUILD_UNAVAILABLE;
	}

	short *buffer = malloc((size_t)CHUNK_FRAMES * (size_t)channels * sizeof(short));
	if (!buffer) {
		decoder_close(dec);
		return WAVE_BUILD_UNAVAILABLE;
	}

	// One accumulator per column, filled as the file goes past.
	//
	// The mean power of the slice, and not its loudest sample: any record
	// mastered in the last thirty years reaches within a hair of full scale
	// somewhere inside every one of a few dozen slices, so a peak column chart
	// is a rectangle. Mean power is what actually varies across a track.
	//
	// Sixty-four bits for the sum, because a slice of a long track holds more
	// than four billion of these; thirty-two for the count, because a slice
	// that held four billion samples would be a track eleven hours long.
	uint64_t power[WAVEFORM_BARS];
	uint32_t counted[WAVEFORM_BARS];
	memset(power, 0, sizeof(power));
	memset(counted, 0, sizeof(counted));

	// Where the current column ends, rather than which column each frame is in.
	//
	// The obvious way to write this loop is `at * WAVEFORM_BARS / total` per
	// frame, and that is a 64-bit division for every sample of the track: some
	// ten million of them on a four-minute album track, on a core that has no
	// 64-bit divide and calls a library routine for each one. There are
	// forty-eight columns, so there are forty-seven moments in the whole track
	// when the answer changes -- so the boundary is worked out at those moments
	// and the rest of the time the loop only compares.
	uint64_t done = 0;
	int column = 0;
	uint64_t next_edge = (total + WAVEFORM_BARS - 1) / WAVEFORM_BARS;

	wave_build_t result = WAVE_BUILD_OK;
	while (done < total) {
		if (!still_wanted(path)) {
			result = WAVE_BUILD_CANCELLED;
			break;
		}

		uint64_t got = decoder_read_pcm_frames_s16(dec, CHUNK_FRAMES, buffer);
		if (got == 0) {
			break;
		}

		// Cut the chunk into runs of frames that all belong to one column, and
		// accumulate each run in one pass. A run reaches from the current
		// position to the next column boundary or the end of the chunk,
		// whichever comes first, so the boundary is tested once per run rather
		// than once per frame.
		//
		// Positions inside a chunk are 32-bit on purpose. The decoder never
		// returns more than CHUNK_FRAMES, so nothing here needs sixty-four bits
		// on a core that has thirty-two; only the track-wide numbers do.
		uint32_t frames = (uint32_t)got;
		uint32_t pos = 0;
		while (pos < frames) {
			uint64_t absolute = done + pos;
			while (column + 1 < WAVEFORM_BARS && absolute >= next_edge) {
				column++;
				next_edge = ((uint64_t)(column + 1) * total + WAVEFORM_BARS - 1) / WAVEFORM_BARS;
			}

			uint32_t take = frames - pos;
			if (column + 1 < WAVEFORM_BARS) {
				uint64_t until_edge = next_edge - absolute; // at least one: the loop above saw to it
				if ((uint64_t)take > until_edge) {
					take = (uint32_t)until_edge;
				}
			}

			// One accumulator in a register for the whole run, added to the
			// column once at the end. The square is worked out in thirty-two
			// bits -- a signed sixteen-bit sample squares into a little over
			// thirty -- and only the sum is sixty-four, because a run can hold
			// a few hundred thousand of them.
			uint64_t sum = 0;
			if (channels == 2) {
				// Stereo has its own path because nearly every record is
				// stereo, and the generic version spends its time on a loop
				// over two things and a multiply to find where they are.
				const short *q = buffer + (size_t)pos * 2u;
				const short *end = q + (size_t)take * 2u;
				while (q < end) {
					int32_t l = q[0], r = q[1];
					sum += (uint64_t)(uint32_t)(l * l) + (uint32_t)(r * r);
					q += 2;
				}
				counted[column] += take * 2u;
			} else if (channels == 1) {
				const short *q = buffer + pos;
				const short *end = q + take;
				while (q < end) {
					int32_t v = *q++;
					sum += (uint32_t)(v * v);
				}
				counted[column] += take;
			} else {
				const short *q = buffer + (size_t)pos * (size_t)channels;
				for (uint32_t f = 0; f < take; f++) {
					for (int c = 0; c < channels; c++) {
						int32_t v = *q++;
						sum += (uint32_t)(v * v);
					}
				}
				counted[column] += take * (uint32_t)channels;
			}
			power[column] += sum;

			pos += take;
		}
		done += got;
	}

	free(buffer);
	decoder_close(dec);
	if (result != WAVE_BUILD_OK) {
		return result;
	}

	// Power to amplitude, then scaled against the loudest column rather than
	// against full scale: a quiet recording drawn against 0 dBFS is a line, and
	// what a waveform is for is the shape of the track and not its level.
	uint32_t level[WAVEFORM_BARS];
	uint32_t loudest = 1;
	for (int i = 0; i < WAVEFORM_BARS; i++) {
		level[i] = counted[i] ? utils_isqrt64(power[i] / counted[i]) : 0;
		if (level[i] > loudest) {
			loudest = level[i];
		}
	}

	for (int i = 0; i < WAVEFORM_BARS; i++) {
		uint32_t norm = level[i] * WAVE_SCALE / loudest; // 0..WAVE_SCALE

		// Halfway between the level and its square root, which is a curve of
		// about 0.7. Mean power alone leaves a quiet verse at a tenth
		// the height of the chorus and unreadable; the square root alone lifts
		// it so far that the track has no dynamics left to look at.
		uint32_t curved = (norm + utils_isqrt64((uint64_t)norm * WAVE_SCALE)) / 2;
		if (curved > WAVE_SCALE) {
			curved = WAVE_SCALE;
		}
		bars[i] = (uint8_t)(curved * WAVE_CEILING / WAVE_SCALE);
	}
	return WAVE_BUILD_OK;
}

static void *worker_main(void *arg) {
	(void)arg;
	thread_be_background("waveform");

	for (;;) {
		char path[sizeof(wanted)];

		pthread_mutex_lock(&lock);
		while (!wanted[0] || strcmp(wanted, have_path) == 0) {
			pthread_cond_wait(&wake, &lock);
		}
		snprintf(path, sizeof(path), "%s", wanted);
		snprintf(computing, sizeof(computing), "%s", path);
		pthread_mutex_unlock(&lock);

		// The file's identity, worked out once for the whole job: each call
		// hashes the path and asks the filesystem for the size and the date.
		uint64_t key = path_key(path);

		uint8_t bars[WAVEFORM_BARS];
		wave_build_t outcome;
		if (hot_lookup(key, bars)) {
			outcome = WAVE_BUILD_OK;
		} else if (cache_lookup(key, bars)) {
			outcome = WAVE_BUILD_OK;
			hot_store(key, bars);
			fprintf(stderr, "waveform: '%s' was already worked out\n", path);
		} else {
			long began = now_ms();
			outcome = build(path, bars);
			if (outcome == WAVE_BUILD_OK) {
				cache_store(key, bars);
				hot_store(key, bars);
				fprintf(stderr, "waveform: '%s' worked out in %ld ms\n", path, now_ms() - began);
			} else if (outcome == WAVE_BUILD_UNAVAILABLE) {
				fprintf(stderr, "waveform: nothing to draw for '%s'\n", path);
			}
		}

		pthread_mutex_lock(&lock);
		computing[0] = '\0';
		if (outcome != WAVE_BUILD_CANCELLED && strcmp(wanted, path) == 0) {
			// Either a shape, or the settled answer that this file has none.
			// Both are written down, because both mean the question is done
			// with and asking it again would spin.
			snprintf(have_path, sizeof(have_path), "%s", path);
			if (outcome == WAVE_BUILD_OK) {
				memcpy(have_bars, bars, WAVEFORM_BARS);
				have_answer = true;
			} else {
				memset(have_bars, 0, WAVEFORM_BARS);
				have_answer = false;
			}
		}
		// A cancelled job writes nothing at all -- not even the path. The wait
		// at the top of this loop only sleeps while the wanted track already
		// has an answer, so leaving have_path unset is what lets the work start
		// again if that track comes back.
		pthread_mutex_unlock(&lock);
	}

	return NULL;
}

// ---------------------------------------------------------------------------
// the face of it
// ---------------------------------------------------------------------------

void waveform_set_cache_dir(const char *sd_root) {
	cache_path[0] = '\0';
	if (!sd_root || !sd_root[0]) {
		return;
	}
	char dir[480];
	snprintf(dir, sizeof(dir), "%s/.local", sd_root);
	mkdir(dir, 0777); // already there is the usual answer
	snprintf(cache_path, sizeof(cache_path), "%s/waveform.dat", dir);

	FILE *f = fopen(cache_path, "ab");
	if (!f) {
		fprintf(stderr, "waveform: '%s' cannot be written (%s); shapes will be computed every time\n", cache_path,
				strerror(errno));
		cache_path[0] = '\0';
		return;
	}
	fclose(f);
}

void waveform_start(void) {
	pthread_mutex_lock(&lock);
	bool start = !worker_running;
	worker_running = true;
	pthread_mutex_unlock(&lock);

	if (start && pthread_create(&worker, NULL, worker_main, NULL) != 0) {
		pthread_mutex_lock(&lock);
		worker_running = false;
		pthread_mutex_unlock(&lock);
		fprintf(stderr, "waveform: no thread; the alternative layout will draw no shapes\n");
		return;
	}
	if (start) {
		pthread_detach(worker);
	}
}

bool waveform_get(const char *path, uint8_t *out) {
	if (!path || !path[0] || !out) {
		return false;
	}

	pthread_mutex_lock(&lock);
	bool changed = strcmp(wanted, path) != 0;
	if (changed) {
		snprintf(wanted, sizeof(wanted), "%s", path);
		pthread_cond_signal(&wake);
	}
	bool ready = have_answer && strcmp(have_path, path) == 0;
	if (ready) {
		memcpy(out, have_bars, WAVEFORM_BARS);
	}
	pthread_mutex_unlock(&lock);

	return ready;
}

bool waveform_pending(const char *path) {
	if (!path || !path[0]) {
		return false;
	}
	pthread_mutex_lock(&lock);
	bool busy = strcmp(computing, path) == 0 || (strcmp(wanted, path) == 0 && strcmp(have_path, path) != 0);
	pthread_mutex_unlock(&lock);
	return busy;
}

// See the note in waveform.h: this does not hold the worker back. It records
// the screen state and signals on the way back up; waking a thread that is
// already awake costs nothing.
void waveform_set_screen_on(bool on) {
	pthread_mutex_lock(&lock);
	bool woken = on && !screen_on;
	screen_on = on;
	if (woken) {
		pthread_cond_signal(&wake);
	}
	pthread_mutex_unlock(&lock);
}

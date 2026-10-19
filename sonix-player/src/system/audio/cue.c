#include "cue.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "src/system/audio/audio.h"
#include "src/system/streaming/radio.h"

// How far the music goes down under a cue, and how quickly. Down fast enough
// that the first note lands on a quiet song, back up slowly enough that the
// return is not a jolt.
#define DUCK_GAIN 0.18f // -15 dB
#define DUCK_IN_S 0.06
#define DUCK_OUT_S 0.40

// Each note swells in and dies away on a raised cosine rather than a straight
// line: no corner at either end, so it is heard as a soft tone rather than a
// switched-on beeper.
#define NOTE_ATTACK_S 0.025
#define NOTE_RELEASE_S 0.080

// A cue asked for while the decode loop is between two writes is taken up by
// the next one. One that nobody takes up within this long belongs to a moment
// that has passed: a track ending just then must not have it sound at the start
// of the next one, seconds late.
#define PENDING_STALE_MS 2000

// The decode loop calls in once per buffer, a few times a second at most
// between calls; a call within this long means a stream a cue can go into.
#define MIXING_FRESH_MS 1000

// Played alone: CD rate, stereo, 16 bits, ten milliseconds a write.
#define ALONE_RATE 44100
#define ALONE_CHUNK 441
#define ALONE_BUFFER_MS 100
#define ALONE_OPEN_TRIES 3

typedef struct {
	double start_s; // from the end of the dip
	double length_s;
	double hz;
	float level; // of full scale
} note_t;

// Around C5: low enough to be easy to live with set after set -- an octave or
// two higher, a beep turns shrill in earbuds -- and still above most of a mix,
// with the music dipped under it. Three quarters of full scale: a pure tone
// this low is heard as quieter than its level suggests, and anything much
// softer is lost under a loud record. Over a song dipped to 0.18 the sum still
// stays inside full scale.
#define TONE_HZ 523.25 // C5
#define PIP_HZ 392.0   // G4, a fourth under
static const note_t BEEP[] = {{0.0, 0.60, TONE_HZ, 0.75f}};
static const note_t TRIPLE[] = {
	{0.00, 0.18, TONE_HZ, 0.75f},
	{0.30, 0.18, TONE_HZ, 0.75f},
	{0.60, 0.18, TONE_HZ, 0.75f},
};
// The pips under the last tone, so the end still stands out.
static const note_t COUNTDOWN[] = {
	{0.0, 0.15, PIP_HZ, 0.70f},
	{1.0, 0.15, PIP_HZ, 0.70f},
	{2.0, 0.15, PIP_HZ, 0.70f},
	{3.0, 0.60, TONE_HZ, 0.75f},
};
static const note_t ACK[] = {{0.0, 0.10, 440.0, 0.50f}};

static const struct {
	const note_t *notes;
	int count;
	int lead_ms; // when the note that marks the moment starts, from the first
} SOUNDS[CUE_COUNT] = {
	[CUE_BEEP] = {BEEP, 1, 0},
	[CUE_TRIPLE] = {TRIPLE, 3, 0},
	[CUE_COUNTDOWN] = {COUNTDOWN, 4, 3000},
	[CUE_ACK] = {ACK, 1, 0},
};

// One cue in progress. The music loop has its own, and so does the thread
// that plays one alone: only one of the two is ever writing.
typedef struct {
	bool active;
	cue_sound_t sound;
	double t;	  // seconds since the cue started, dip included
	double end_s; // when the music is all the way back up
	int note;	  // the note the oscillator is on, or -1
	double phase; // radians
} voice_t;

// Set by cue_play() on any thread, taken by whichever writer runs next: the
// sound plus one, so that 0 means nothing waiting.
static volatile int pending;
static volatile long long pending_ms;
// When the decode loop last called in, for cue_play() to tell a stream it can
// mix into from one it cannot (a DoP stream is playing, and never calls).
static volatile long long mixed_ms;

static voice_t music_voice; // the decode thread's own

static pthread_mutex_t alone_lock = PTHREAD_MUTEX_INITIALIZER;
static bool alone_busy;

static long long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool valid(cue_sound_t sound) { return sound >= 0 && sound < CUE_COUNT; }

int cue_lead_ms(cue_sound_t sound) { return valid(sound) ? SOUNDS[sound].lead_ms : 0; }

static void voice_start(voice_t *v, cue_sound_t sound) {
	const note_t *last = &SOUNDS[sound].notes[SOUNDS[sound].count - 1];
	v->active = true;
	v->sound = sound;
	v->t = 0.0;
	v->end_s = DUCK_IN_S + last->start_s + last->length_s + DUCK_OUT_S;
	v->note = -1;
	v->phase = 0.0;
}

// Takes up a cue waiting for a writer, if there is one and it is still news.
static void take_pending(voice_t *v) {
	if (!pending) {
		return;
	}
	int got = __atomic_exchange_n(&pending, 0, __ATOMIC_ACQ_REL);
	if (got > 0 && now_ms() - pending_ms < PENDING_STALE_MS) {
		voice_start(v, (cue_sound_t)(got - 1));
	}
}

// The gain the music gets at this instant: down over the dip, down while the
// notes sound, back up over the end.
static float duck_gain(const voice_t *v) {
	if (v->t < DUCK_IN_S) {
		return 1.0f - (1.0f - DUCK_GAIN) * (float)(v->t / DUCK_IN_S);
	}
	double up_from = v->end_s - DUCK_OUT_S;
	if (v->t < up_from) {
		return DUCK_GAIN;
	}
	return DUCK_GAIN + (1.0f - DUCK_GAIN) * (float)((v->t - up_from) / DUCK_OUT_S);
}

// The cue's own sample at this instant, -1..1 of full scale.
static float tone(voice_t *v, int rate) {
	double at = v->t - DUCK_IN_S;
	if (at < 0) {
		return 0.0f;
	}
	const note_t *notes = SOUNDS[v->sound].notes;
	int count = SOUNDS[v->sound].count;
	int i = 0;
	while (i < count && at >= notes[i].start_s + notes[i].length_s) {
		i++;
	}
	if (i == count || at < notes[i].start_s) {
		return 0.0f; // between two notes, or past the last
	}
	const note_t *n = &notes[i];
	if (i != v->note) {
		v->note = i;
		v->phase = 0.0;
	}

	double into = at - n->start_s;
	double left = n->length_s - into;
	double env = 1.0;
	if (into < NOTE_ATTACK_S) {
		env = 0.5 - 0.5 * cos(M_PI * into / NOTE_ATTACK_S);
	} else if (left < NOTE_RELEASE_S) {
		env = 0.5 - 0.5 * cos(M_PI * left / NOTE_RELEASE_S);
	}

	// A plain sine: any overtone is what makes a beep sound sharp.
	double s = sin(v->phase);
	v->phase += 2.0 * M_PI * n->hz / rate;
	if (v->phase > 2.0 * M_PI) {
		v->phase -= 2.0 * M_PI;
	}
	return (float)(n->level * env * s);
}

// One frame on: the music's gain and the cue's sample for it, and false once
// the cue is over.
static bool voice_step(voice_t *v, int rate, float *gain, float *sample) {
	if (v->t >= v->end_s) {
		v->active = false;
		*gain = 1.0f;
		*sample = 0.0f;
		return false;
	}
	*gain = duck_gain(v);
	*sample = tone(v, rate);
	v->t += 1.0 / rate;
	return true;
}

static void mix_s16(voice_t *v, short *samples, int frames, int channels, int rate) {
	for (int f = 0; f < frames && v->active; f++) {
		float gain, sample;
		if (!voice_step(v, rate, &gain, &sample)) {
			break;
		}
		short *frame = samples + (size_t)f * channels;
		for (int c = 0; c < channels; c++) {
			float out = frame[c] * gain + sample * 32767.0f;
			frame[c] = (short)(out > 32767.0f ? 32767 : out < -32768.0f ? -32768 : lrintf(out));
		}
	}
}

static void mix_s32(voice_t *v, int32_t *samples, int frames, int channels, int rate) {
	for (int f = 0; f < frames && v->active; f++) {
		float gain, sample;
		if (!voice_step(v, rate, &gain, &sample)) {
			break;
		}
		int32_t *frame = samples + (size_t)f * channels;
		for (int c = 0; c < channels; c++) {
			// In double: a float has 24 bits of mantissa and these are 32.
			double out = (double)frame[c] * gain + (double)sample * 2147483647.0;
			frame[c] = (int32_t)(out > 2147483647.0 ? 2147483647 : out < -2147483648.0 ? INT32_MIN : llrint(out));
		}
	}
}

void cue_mix_s16(short *samples, int frames, int channels, int rate) {
	mixed_ms = now_ms();
	take_pending(&music_voice);
	if (music_voice.active && rate > 0) {
		mix_s16(&music_voice, samples, frames, channels, rate);
	}
}

void cue_mix_s32(int32_t *samples, int frames, int channels, int rate) {
	mixed_ms = now_ms();
	take_pending(&music_voice);
	if (music_voice.active && rate > 0) {
		mix_s32(&music_voice, samples, frames, channels, rate);
	}
}

// ---------------------------------------------------------------------------
// Alone, with nothing playing
// ---------------------------------------------------------------------------

static void *alone_main(void *unused) {
	(void)unused;
	bool open = false;
	for (int tries = 0; tries < ALONE_OPEN_TRIES && !open; tries++) {
		open = audio_external_begin_yielding(ALONE_RATE, 2, 16, ALONE_BUFFER_MS);
	}
	if (!open) {
		fprintf(stderr, "cue: no output to sound on\n");
		pending = 0;
	} else {
		voice_t v = {0};
		short chunk[ALONE_CHUNK * 2];
		take_pending(&v);
		// Silence written past the end too, a buffer's worth, so the last note
		// is out of the DAC before the output is closed under it.
		int tail = ALONE_BUFFER_MS * 2 / 10;
		while (tail > 0 && !audio_external_wanted()) {
			memset(chunk, 0, sizeof(chunk));
			take_pending(&v); // a second cue asked for while this one sounds
			if (v.active) {
				mix_s16(&v, chunk, ALONE_CHUNK, 2, ALONE_RATE);
			} else {
				tail--;
			}
			if (audio_external_write(chunk, ALONE_CHUNK) <= 0) {
				break; // the output went away under it
			}
		}
		audio_external_end();
	}

	pthread_mutex_lock(&alone_lock);
	alone_busy = false;
	pthread_mutex_unlock(&alone_lock);
	return NULL;
}

bool cue_play(cue_sound_t sound) {
	if (!valid(sound)) {
		return false;
	}

	if (audio_get_status() == AUDIO_STATUS_PLAYING) {
		if (now_ms() - mixed_ms > MIXING_FRESH_MS) {
			return false; // playing, but nothing this side can be mixed into
		}
		pending_ms = now_ms();
		__atomic_store_n(&pending, (int)sound + 1, __ATOMIC_RELEASE);
		return true;
	}

	// A station or a receiver has the output and is using it: a cue does not
	// take it away.
	if (radio_is_playing() || radio_is_connecting()) {
		return false;
	}

	pending_ms = now_ms();
	__atomic_store_n(&pending, (int)sound + 1, __ATOMIC_RELEASE);

	pthread_mutex_lock(&alone_lock);
	bool start = !alone_busy;
	alone_busy = true;
	pthread_mutex_unlock(&alone_lock);
	if (!start) {
		return true; // the thread already running takes it up
	}

	pthread_t thread;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	bool ok = pthread_create(&thread, &attr, alone_main, NULL) == 0;
	pthread_attr_destroy(&attr);
	if (!ok) {
		pthread_mutex_lock(&alone_lock);
		alone_busy = false;
		pthread_mutex_unlock(&alone_lock);
		pending = 0;
	}
	return ok;
}

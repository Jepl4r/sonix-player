#ifndef REPLAYGAIN_H
#define REPLAYGAIN_H

#include <stdbool.h>
#include <stdint.h>

#include "src/system/library/metadata.h"

// ReplayGain: playing a whole card at one loudness.
//
// Whoever tagged the file worked out its loudness once and wrote the
// correction into the tags; the player applies it. Nothing is analysed here --
// an analysis pass over a card of music is minutes of work this device should
// not be doing.
//
// Four states, the same three the stock player offers and an extra option
// (`settings_replaygain_vg_off` / `_track` / `_album` in its binary): off, per
// track, per album, track when shuffled. Track and album are not degrees of 
// strictness: correcting each track of a record separately flattens a movement 
// the producer meant to be quiet, so an album is corrected by one figure and a 
// shuffle by each track's own.
// 'Track when shuffled' uses track gain if shuffle mode is on, album gain 
// otherwise.

typedef enum {
	REPLAYGAIN_OFF = 0,
	REPLAYGAIN_TRACK = 1,
	REPLAYGAIN_ALBUM = 2,
	REPLAYGAIN_TRACK_WHEN_SHUFFLED = 3,
} replaygain_mode_t;

replaygain_mode_t replaygain_mode(void);
void replaygain_set_mode(replaygain_mode_t mode);

// Two figures on top of the mode, both in tenths of a decibel and both 0 --
// the behaviour from before they existed -- until changed.
//
// The preamp, [audio] replaygain_preamp: added to every tag's correction, for
// a library tagged against a quieter or louder reference than the player
// should play at. -6.0 to +6.0 dB in half-decibel steps. The peak guard still
// has the last word, so a positive preamp never pushes a tagged track into
// clipping.
//
// The fallback, [audio] replaygain_fallback: what a file with no ReplayGain
// tag at all is played at while ReplayGain is on, so an untagged, mastered-
// loud track does not jump out between corrected ones. -12 to 0 dB in whole
// decibels; 0 leaves such files untouched.
#define REPLAYGAIN_PREAMP_MIN (-60)
#define REPLAYGAIN_PREAMP_MAX 60
#define REPLAYGAIN_PREAMP_STEP 5
#define REPLAYGAIN_FALLBACK_MIN (-120)
#define REPLAYGAIN_FALLBACK_MAX 0
#define REPLAYGAIN_FALLBACK_STEP 10
int replaygain_preamp(void);
void replaygain_set_preamp(int tenths);
int replaygain_fallback(void);
void replaygain_set_fallback(int tenths);

// Called at each track change with what the tags said. Works out the gain once
// and hands it to the audio thread; a file with no ReplayGain tags gets the
// fallback, which is no change at all unless one was set.
void replaygain_load(const song_metadata_t *meta);

// What is being applied right now, in dB, and whether anything is. For the
// interface to be able to say so.
bool replaygain_active(void);
double replaygain_current_db(void);

// In place on interleaved PCM, audio thread only. A no-op when off or when the
// track carries no usable tag.
void replaygain_process(short *frames, int frame_count, int channels);
void replaygain_process_s32(int32_t *frames, int frame_count, int channels);

#endif /* REPLAYGAIN_H */

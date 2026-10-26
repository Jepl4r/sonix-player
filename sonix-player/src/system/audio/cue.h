#ifndef CUE_H
#define CUE_H

#include <stdbool.h>
#include <stdint.h>

// Short signals laid over the music: the workout timer's beeps.
//
// While a track plays, a cue is mixed into the samples on their way to the
// output (audio.c calls the two mixers below from both of its decode loops,
// after the fade and before the software volume). The music dips by 15 dB a
// moment before the first note, stays down while the cue sounds and comes back
// up over a fraction of a second -- the way a phone's satnav talks over a song
// -- so the cue is heard without the volume having to be anywhere special.
//
// With nothing playing, the cue sounds on its own: the output is opened for it
// the way the metronome opens it, and handed back the moment anything else
// wants it. Over a radio station, a receiver or a DoP stream there is nothing
// it can be mixed into and nothing it may take the output from, and it stays
// silent; whoever asked says so on screen.
//
// Synthesised here at whatever rate and width the stream has, so there are no
// sound files to ship and nothing to resample.

typedef enum {
	CUE_BEEP = 0,  // one long beep
	CUE_TRIPLE,	   // three short ones
	CUE_COUNTDOWN, // a pip on each of the last three seconds, then a long high one
	CUE_ACK,	   // a single short blip: "the button was heard"
	CUE_COUNT,
} cue_sound_t;

// How long before the moment it marks a cue starts, in milliseconds: the
// countdown's pips come before the end, not after it. 0 for the others.
int cue_lead_ms(cue_sound_t sound);

// Any thread. Mixed over the track if one is playing, played alone if nothing
// is, and false when neither is possible.
bool cue_play(cue_sound_t sound);

// From audio.c's decode loops only, once per buffer, on interleaved samples
// that are about to be written: S16, or S32 at full scale. `frames` is frames,
// not samples. With no cue sounding it costs a clock read and one load.
void cue_mix_s16(short *samples, int frames, int channels, int rate);
void cue_mix_s32(int32_t *samples, int frames, int channels, int rate);

#endif /* CUE_H */

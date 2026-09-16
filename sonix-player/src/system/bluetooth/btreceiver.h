#ifndef BTRECEIVER_H
#define BTRECEIVER_H

#include <stdbool.h>

// Bluetooth receiver mode: the player stops being a source and becomes a pair
// of headphones.
//
// A phone or a computer connects, encodes the music its own way and sends it
// over A2DP; this device decodes it and puts it out of its own DAC. Which is
// the point of the whole thing: the amplifier and the output stage here are
// better than the ones in a laptop, and this is the only way to use them for
// something the player is not itself playing.
//
// Two halves, and only one of them lives in this file.
//
// The profile is the other half and belongs to the bring-up: bluealsa is
// started with `-p a2dp-source -p a2dp-sink` and bluez publishes both endpoints
// in the local SDP record. That is what lets a MacBook connect at all -- with
// only the source endpoint it finds no profile in common and Connect() fails --
// and it is true whether or not this mode is switched on. Nothing here can make
// that happen later: an endpoint is registered when the daemon starts.
//
// This file is the audio. bluealsa publishes a capture PCM for the link,
//
//     bluealsa:DEV=<mac>,PROFILE=a2dp   opened for capture
//
// and the loop below reads decoded frames out of it and hands them to
// audio_external_write(), the same door the USB DAC mode and the AirPlay
// receiver come through. One PCM takes one writer, so local playback is stopped
// first: in this mode the player is not playing anything of its own.

typedef struct {
	bool active;	// the mode is on and the capture thread is running
	bool streaming; // ...and frames are actually arriving
	char device[64];
	char codec[24];
	unsigned sample_rate;
	unsigned channels;
	unsigned bits;
	char error[128];
} btreceiver_state_t;

// True when something is connected as a source, i.e. when there is anything to
// receive. Cheap: it reads the cache bluealsa's own signals keep.
bool btreceiver_available(void);

// Turns the mode on. Stops local playback, then puts a thread on the capture
// PCM. Returns false when there is nothing connected to receive from.
bool btreceiver_start(void);

// Turns it off and closes the output. Safe when it was never started.
void btreceiver_stop(void);

bool btreceiver_is_active(void);

// True while frames are actually arriving -- the mode on AND something being
// sent. A sender that pauses simply stops sending, so this goes false a moment
// after the phone is paused and true again when it starts.
//
// The same answer btreceiver_get_state() gives in its `streaming` field, as a
// question of its own for a caller that wants nothing else: the shutdown timer
// asks it on every tick, and copying a whole state struct for one bool is
// silly.
bool btreceiver_is_streaming(void);

void btreceiver_get_state(btreceiver_state_t *out);

// Bumped on every change, so a page can redraw only when there is something
// new -- a codec renegotiated under it included.
unsigned btreceiver_serial(void);

// A transport key, sent to the device on the other end over AVRCP instead of
// acted on here. This is the half that makes the side buttons mean what they
// mean on a pair of headphones: the music is not on this device, so the press
// has to reach the machine the music is on.
typedef enum {
	BTRECEIVER_PLAY_PAUSE = 0,
	BTRECEIVER_NEXT,
	BTRECEIVER_PREV,
} btreceiver_key_t;

// Returns false when the mode is off, in which case the caller keeps the key.
bool btreceiver_key(btreceiver_key_t key);

#endif /* BTRECEIVER_H */

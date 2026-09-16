#ifndef AIRPLAY_H
#define AIRPLAY_H

#include <stdbool.h>

// AirPlay, done the way the stock firmware does it -- which is not the way
// anyone would guess.
//
// /usr/bin/shairport is an old abrasive/shairport build with **no ALSA backend
// at all**. Its `-o ot` output is a HiBy-private backend that opens a unix
// socket and writes decoded PCM into it; the socket is served by the *player*.
// The proof is in the binary: it lists libasound.so.2 as NEEDED but
// `readelf --dyn-syms | grep snd_` finds nothing, and its strings carry
// /var/run/airplay_socket_server next to audio_ot.c.
//
// So the shape is:
//
//   player --"SHAIRPORT:TURN_ON:<name>"--> sys_server
//                                              |
//                            system("/usr/bin/shairport_on.sh <name>")
//                                              |
//                          shairport -a <name> -o ot -M /tmp -b 160 &
//                                              |
//     PCM  --> /var/run/airplay_socket_server --> player --> the DAC
//     meta --> /var/run/airplay_meta_server   --> player --> the screen
//
// The receiver is not tied to its page. Once it is switched on it stays on
// until it is switched off, exactly as it does in the stock firmware, so a
// phone can keep playing while the player is used for something else.
//
// Two consequences worth stating plainly. The sockets have to exist and be
// listening *before* shairport is told to start, because it connects to them.
// And the player owns the DAC throughout: shairport never opens one, so local
// playback has to stop, not because of a device conflict but because the same
// output would be carrying two things.
//
// The audio is 44100 Hz, 16-bit, stereo -- fixed, that is what the ot backend
// sends -- and goes out through audio_external_*() in audio.c.
//
// The name is read from /usr/resource/bt_name ("HiBy R3PROII") with spaces
// turned into '%', which shairport turns back into spaces. That is not
// decoration: sys_server builds its command line with sprintf and passes it to
// system(), so a name with a real space in it would arrive as two arguments.

#define AIRPLAY_TEXT_MAX 256

typedef struct {
	bool connected; // a sender has claimed the stream
	bool playing;	// ...and audio is arriving
	char title[AIRPLAY_TEXT_MAX];
	char artist[AIRPLAY_TEXT_MAX];
	char album[AIRPLAY_TEXT_MAX];
	char genre[AIRPLAY_TEXT_MAX];
} airplay_state_t;

// True when the firmware carries shairport and the daemon that starts it.
bool airplay_available(void);

// Starts listening on both sockets and asks sys_server to bring shairport up.
// Returns immediately; the sockets and the request happen on a worker, because
// sys_server runs its scripts through a blocking system().
//
// Local playback is paused for you, when and only when a sender actually
// starts: switching the receiver on is not the same as using it, and there is
// no reason to stop the music for a phone that never connects.
void airplay_set_enabled(bool on);
bool airplay_get_enabled(void);

// True once the sockets are up and shairport has been asked for. There is a
// moment between the switch and this: sys_server queues the request and only
// looks at its queue once a second, and starting the servers comes first.
bool airplay_running(void);

// What the receiver is doing right now. Safe to call from the interface thread;
// it copies under a lock.
void airplay_get_state(airplay_state_t *out);

// Bumped whenever anything in the state changes, so a page can redraw only when
// there is something new rather than every tick.
unsigned airplay_serial(void);

// Hands the DAC back, so local playback -- a track on the card, Tidal, Qobuz --
// can open it.
//
// It has to be asked for, because the receiver cannot tell on its own that a
// sender has finished: shairport connects to the audio socket once, when it
// starts, and holds it for as long as it lives. So "the stream is live" stays
// true for the whole time AirPlay is switched on, and without this the PCM
// opened for it is held -- silent, but held -- and a local track cannot open
// the device.
//
// Refused while audio is actually arriving: a sender in the middle of a song
// owns the output, which is the same rule that pauses a local track when a
// sender starts, read the other way round. True means the device is free, or
// about to be -- the hand-back happens on the AirPlay thread within a poll, and
// the PCM open that follows retries for two seconds anyway.
bool airplay_release_output(void);

// Milliseconds since the last audio arrived, or -1 if none ever has. The stock
// player uses this to close its AirPlay screen after thirty seconds without
// data, telling the user the screen will close; a sender that pauses stops
// sending entirely, so a long silence really does mean the session is over
// rather than quiet.
int airplay_silent_ms(void);

// The name senders see in their AirPlay list.
const char *airplay_name(void);

#endif /* AIRPLAY_H */

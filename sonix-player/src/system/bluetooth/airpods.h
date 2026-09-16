#ifndef AIRPODS_H
#define AIRPODS_H

#include <stdbool.h>
#include <stdint.h>

// What a pair of AirPods says about itself, over Apple's own accessory
// protocol.
//
// Apple headphones speak AAP (also written AACP) on a plain L2CAP channel,
// PSM 0x1001, over the same classic Bluetooth link that carries the music.
// Say hello, ask for notifications, and they start reporting -- among them the
// battery of each earbud and of the case, as real percentages,
// which is the one thing AVRCP never carries.
//
// This is the part of that protocol the player needs, and no more: connect,
// handshake, ask for notifications, read the battery packet, read the metadata
// packet for the model number. Everything else that arrives is stepped over.
//
// The protocol was worked out by the people behind MagicPods, LibrePods and
// the AAP protocol notes; what is implemented here follows those, and the
// packet shapes are written out where they are built and parsed.
//
// Threading: one worker owns the socket. Everything a caller sees is a copy
// taken under a lock.

typedef enum {
	AIRPODS_MODEL_UNKNOWN = 0,
	AIRPODS_MODEL_GEN1,
	AIRPODS_MODEL_GEN2,
	AIRPODS_MODEL_GEN3,
	AIRPODS_MODEL_GEN4,
	AIRPODS_MODEL_PRO,
	AIRPODS_MODEL_MAX,
} airpods_model_t;

typedef enum {
	AIRPODS_CHARGE_UNKNOWN = 0,	 // reported, but the state was not said
	AIRPODS_CHARGE_CHARGING,	 // on the charger
	AIRPODS_CHARGE_NOT_CHARGING, // running off its own battery
	AIRPODS_CHARGE_ABSENT,		 // in the case, or not there at all
} airpods_charge_t;

// What the headphones are doing about the world outside. Off and the three
// modes Apple's own control offers; unknown until they have said.
typedef enum {
	AIRPODS_NOISE_UNKNOWN = 0,
	AIRPODS_NOISE_OFF,
	AIRPODS_NOISE_CANCELLATION,
	AIRPODS_NOISE_TRANSPARENCY,
	AIRPODS_NOISE_ADAPTIVE,
} airpods_noise_t;

// What holding a stem moves between.
//
// Apple's "Press and Hold AirPods" screen is not a choice of one action but a
// choice of a SET: the modes the long press steps through, and it insists on at
// least two of them. The headphones carry it as a bitmask in one setting byte.
//
// The bitmask has a fourth bit, 0x01, for the off mode. It is not used: these
// headphones do not offer off among the modes the long press reaches, and
// setting the bit would also mean sending the separate "off is allowed"
// setting first. The bit is left alone rather than named.
//
// Only the models with three modes to choose between have any of this: the Pro
// from the second generation on, and the fourth generation with cancellation.
// The first Pro and the Max have cancellation and transparency and nothing
// else, so their cycle is both of them and cannot be anything else; on the
// plain AirPods the long press is Siri and there is nothing to choose at all.
#define AIRPODS_CYCLE_CANCELLATION 0x02
#define AIRPODS_CYCLE_TRANSPARENCY 0x04
#define AIRPODS_CYCLE_ADAPTIVE 0x08

// The three together: what the page shows, and the mask a write is trimmed to
// so that what is on screen is what the headphones are given.
#define AIRPODS_CYCLE_ALL (AIRPODS_CYCLE_CANCELLATION | AIRPODS_CYCLE_TRANSPARENCY | AIRPODS_CYCLE_ADAPTIVE)

typedef struct {
	bool known; // false when this part was not in the last report
	int percent;
	airpods_charge_t charge;
} airpods_level_t;

typedef struct {
	bool connected;			// the protocol session is up
	bool have_battery;		// at least one battery report has arrived
	airpods_model_t model;	// which family, for the pictures
	char model_number[16];	// "A2084" and its like, "" when it never arrived
	char name[24];			// "AirPods Pro 2": a product name, never translated
	bool has_noise_control; // cancellation and transparency
	bool has_adaptive;		// and the third mode, which not every model has
	bool has_press_hold;	// the stem's long press can be configured
	airpods_noise_t noise;	// which one is on now
	uint8_t noise_cycle;	// the AIRPODS_CYCLE_* the long press steps through
	bool cycle_known;		// ... and whether that came from the headphones
	airpods_level_t left;
	airpods_level_t right;
	airpods_level_t single; // the Max, which is one headset with one battery
	airpods_level_t charging_case;
	uint32_t serial; // bumps on every change, so a poll can skip a redraw
} airpods_state_t;

// Starts the worker. It sits idle until there is a device to talk to.
void airpods_init(void);

// Stops the worker and closes the session.
void airpods_stop(void);

// The connected audio device, or NULL when there is none. `name` is what
// Bluetooth calls it, used only to guess the family when the headphones never
// send their model number. Cheap to call on every round of a poll.
void airpods_set_device(const char *mac, const char *name);

// A copy of the state, and whether the session is up. The state is cleared
// when a session ends, so a caller that only draws while it returns true never
// sees the numbers of a pair that has gone.
bool airpods_get(airpods_state_t *out);

// Asks the headphones to switch to a mode. Returns at once: the command is
// queued and goes out on the session's own thread, and what comes back is the
// headphones saying what they did, which is what the state above then holds.
// Nothing happens with no session, or on a model that has no noise control.
void airpods_set_noise(airpods_noise_t mode);

// Sets which modes the long press steps through, as AIRPODS_CYCLE_* bits.
// Trimmed to AIRPODS_CYCLE_ALL, then refused with fewer than two of them --
// which is what the headphones themselves require -- and on a model whose stem
// press cannot be configured. Like the mode above it is queued and taken as
// done: this setting is one of the many the headphones do not echo back.
void airpods_set_noise_cycle(uint8_t modes);

// Hands the parsers one packet, as if it had arrived on the socket. False when
// the packet was the other end saying it is going away, which is the session
// ending rather than a reading.
//
// The seam for driving the parsers without a Bluetooth adapter: on a desktop
// the packets the headphones would have sent have to come from somewhere else.
bool airpods_feed_packet(const uint8_t *data, unsigned length);

#endif /* AIRPODS_H */

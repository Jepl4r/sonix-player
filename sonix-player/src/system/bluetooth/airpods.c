#include "airpods.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/utils.h"

// ---------------------------------------------------------------------------
// The socket
//
// Written out here rather than included from bluetooth/l2cap.h: libbluetooth's
// headers are not in the cross toolchain's sysroot, and this is the whole of
// what would be taken from them -- one address family, one protocol number and
// one structure that has not changed since the kernel learned to speak L2CAP.
// ---------------------------------------------------------------------------

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define BTPROTO_L2CAP 0

// AAP's service PSM. The one number the whole protocol hangs from.
#define AAP_PSM 0x1001

struct sockaddr_l2 {
	sa_family_t l2_family;
	uint16_t l2_psm; // little endian on the wire, whatever the machine is
	uint8_t l2_bdaddr[6];
	uint16_t l2_cid;
	uint8_t l2_bdaddr_type; // 0 = BR/EDR, which is what AirPods use for this
};

// "AA:BB:CC:DD:EE:FF" into the six bytes the kernel wants, least significant
// first -- the reverse of how it is written.
static bool parse_mac(const char *text, uint8_t out[6]) {
	int values[6];
	if (!text ||
		sscanf(text, "%x:%x:%x:%x:%x:%x", &values[0], &values[1], &values[2], &values[3], &values[4], &values[5]) != 6) {
		return false;
	}
	for (int i = 0; i < 6; i++) {
		if (values[i] < 0 || values[i] > 255) {
			return false;
		}
		out[5 - i] = (uint8_t)values[i];
	}
	return true;
}

static uint16_t to_le16(uint16_t v) {
	uint8_t bytes[2] = {(uint8_t)(v & 0xff), (uint8_t)(v >> 8)};
	uint16_t out;
	memcpy(&out, bytes, 2);
	return out;
}

// ---------------------------------------------------------------------------
// The packets
//
// Every ordinary AAP message begins with four bytes -- 04 00 04 00 -- and then
// an opcode and a zero. The handshake is the exception: it is a different
// message type and carries its own header.
// ---------------------------------------------------------------------------

static const uint8_t AAP_HANDSHAKE[] = {0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x02, 0x00,
										0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// "Send me everything." The four 0xFF are a filter mask whose bits nobody has
// decoded; all of them on is what an iPhone ends up sending and what every
// implementation of this protocol uses.
static const uint8_t AAP_REQUEST_NOTIFICATIONS[] = {0x04, 0x00, 0x04, 0x00, 0x0F, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};

#define AAP_OPCODE_BATTERY 0x04
#define AAP_OPCODE_SETTING 0x09
#define AAP_OPCODE_METADATA 0x1D

// Two of the many settings that ride on opcode 0x09, told apart by the byte
// after it. Every one of them is the same eleven bytes; only these are read or
// written here.
#define AAP_SETTING_NOISE 0x0D
#define AAP_SETTING_NOISE_CYCLE 0x1A // the modes a long press steps through

// Which part of the headphones a battery entry is about.
#define AAP_BATTERY_SINGLE 0x01
#define AAP_BATTERY_RIGHT 0x02
#define AAP_BATTERY_LEFT 0x04
#define AAP_BATTERY_CASE 0x08

#define RETRY_MS 4000	   // between attempts to open the session
#define CONNECT_ATTEMPTS 3 // before giving up on this device: it is not Apple
#define NOTIFY_RETRY_MS 2000
#define POLL_MS 400
#define READ_BUF 1024
#define MAC_LEN 32
#define NAME_LEN 128

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static pthread_t worker;
static bool worker_running;
static bool stopping;

static char want_mac[MAC_LEN]; // the device to talk to, "" for none
static char want_name[NAME_LEN];
static char have_mac[MAC_LEN];
static int failures;					   // consecutive connect failures for have_mac
static airpods_noise_t pending_noise;	   // a mode asked for, waiting for the session
static int pending_cycle = -1;			   // a long-press set asked for, -1 for none
static airpods_state_t g_state; // what the last packets said

static uint32_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000));
}

static void sleep_ms(int ms) {
	struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
	nanosleep(&ts, NULL);
}

// ---------------------------------------------------------------------------
// Working out which AirPods these are
//
// From the model number the headphones send in their metadata packet -- the
// same one printed on the earbud. Apple's own list is the source; what matters
// here is only the family, because that is what picks the picture.
// ---------------------------------------------------------------------------

// A model number, the family that picks its pictures, the name to put on the
// row, and what it can do about the noise around it. The families are what
// Apple's own list gives; the noise control is what each product actually
// offers -- the first Pro have cancellation and transparency but not the
// adaptive mode, which arrived with the second, and the plain fourth
// generation has none of it while the same generation with cancellation has
// all three.
typedef struct {
	const char *number;
	airpods_model_t model;
	const char *name;
	bool noise_control;
	bool adaptive;
	// Whether the long press has something to choose between. It needs three
	// modes: with only cancellation and transparency the cycle is both of them
	// and cannot be anything else, which is why the first Pro and the Max are
	// false here even though their stem press is not Siri.
	bool press_hold;
} model_entry_t;

static const model_entry_t MODEL_NUMBERS[] = {
	{"A1523", AIRPODS_MODEL_GEN1, "AirPods", false, false, false},
	{"A1722", AIRPODS_MODEL_GEN1, "AirPods", false, false, false},

	{"A2031", AIRPODS_MODEL_GEN2, "AirPods 2", false, false, false},
	{"A2032", AIRPODS_MODEL_GEN2, "AirPods 2", false, false, false},

	{"A2564", AIRPODS_MODEL_GEN3, "AirPods 3", false, false, false},
	{"A2565", AIRPODS_MODEL_GEN3, "AirPods 3", false, false, false},

	{"A3050", AIRPODS_MODEL_GEN4, "AirPods 4", false, false, false},
	{"A3053", AIRPODS_MODEL_GEN4, "AirPods 4", false, false, false},
	{"A3054", AIRPODS_MODEL_GEN4, "AirPods 4", false, false, false},
	{"A3058", AIRPODS_MODEL_GEN4, "AirPods 4", false, false, false},

	{"A3055", AIRPODS_MODEL_GEN4, "AirPods 4 ANC", true, true, true},
	{"A3056", AIRPODS_MODEL_GEN4, "AirPods 4 ANC", true, true, true},
	{"A3057", AIRPODS_MODEL_GEN4, "AirPods 4 ANC", true, true, true},
	{"A3059", AIRPODS_MODEL_GEN4, "AirPods 4 ANC", true, true, true},

	{"A2083", AIRPODS_MODEL_PRO, "AirPods Pro", true, false, false},
	{"A2084", AIRPODS_MODEL_PRO, "AirPods Pro", true, false, false},
	{"A2190", AIRPODS_MODEL_PRO, "AirPods Pro", true, false, false},

	{"A2698", AIRPODS_MODEL_PRO, "AirPods Pro 2", true, true, true},
	{"A2699", AIRPODS_MODEL_PRO, "AirPods Pro 2", true, true, true},
	{"A2700", AIRPODS_MODEL_PRO, "AirPods Pro 2", true, true, true},
	{"A2931", AIRPODS_MODEL_PRO, "AirPods Pro 2", true, true, true},
	{"A2968", AIRPODS_MODEL_PRO, "AirPods Pro 2", true, true, true},
	{"A3047", AIRPODS_MODEL_PRO, "AirPods Pro 2", true, true, true},
	{"A3048", AIRPODS_MODEL_PRO, "AirPods Pro 2", true, true, true},
	{"A3049", AIRPODS_MODEL_PRO, "AirPods Pro 2", true, true, true},

	{"A3063", AIRPODS_MODEL_PRO, "AirPods Pro 3", true, true, true},
	{"A3064", AIRPODS_MODEL_PRO, "AirPods Pro 3", true, true, true},
	{"A3065", AIRPODS_MODEL_PRO, "AirPods Pro 3", true, true, true},
	{"A3122", AIRPODS_MODEL_PRO, "AirPods Pro 3", true, true, true},

	// One headset, cancellation and transparency, no adaptive mode -- and no
	// stem either: the button on the cup moves between the two, and two is not
	// a choice.
	{"A2096", AIRPODS_MODEL_MAX, "AirPods Max", true, false, false},
	{"A3184", AIRPODS_MODEL_MAX, "AirPods Max", true, false, false},
};

static const model_entry_t *entry_from_number(const char *number) {
	for (size_t i = 0; i < sizeof(MODEL_NUMBERS) / sizeof(MODEL_NUMBERS[0]); i++) {
		if (strcmp(MODEL_NUMBERS[i].number, number) == 0) {
			return &MODEL_NUMBERS[i];
		}
	}
	return NULL;
}


// The fallback, for headphones that never send the metadata packet: what
// Bluetooth calls them. Apple's default name carries the family ("Mattia's
// AirPods Pro"), and a renamed pair simply lands on the generic picture, which
// is the right answer when nothing is known.
static airpods_model_t model_from_name(const char *name) {
	if (!name || !name[0]) {
		return AIRPODS_MODEL_UNKNOWN;
	}
	char lower[NAME_LEN];
	size_t i = 0;
	for (; name[i] && i < sizeof(lower) - 1; i++) {
		lower[i] = (char)tolower((unsigned char)name[i]);
	}
	lower[i] = '\0';

	if (!strstr(lower, "airpods")) {
		return AIRPODS_MODEL_UNKNOWN;
	}
	if (strstr(lower, "max")) {
		return AIRPODS_MODEL_MAX;
	}
	if (strstr(lower, "pro")) {
		return AIRPODS_MODEL_PRO;
	}
	return AIRPODS_MODEL_UNKNOWN; // "AirPods" alone does not say which
}

// The name to show before -- or without -- a model number. Never the Bluetooth
// name: that is whatever the owner called them, and the row wants the product.
static const char *default_name(airpods_model_t model) {
	switch (model) {
	case AIRPODS_MODEL_PRO:
		return "AirPods Pro";
	case AIRPODS_MODEL_MAX:
		return "AirPods Max";
	default:
		return "AirPods";
	}
}

// ---------------------------------------------------------------------------
// Reading what arrives
// ---------------------------------------------------------------------------

static airpods_charge_t charge_from_byte(uint8_t status) {
	switch (status) {
	case 0x01:
	case 0x05: // optimised charging, which is still charging
		return AIRPODS_CHARGE_CHARGING;
	case 0x02:
		return AIRPODS_CHARGE_NOT_CHARGING;
	case 0x04:
		return AIRPODS_CHARGE_ABSENT;
	default:
		return AIRPODS_CHARGE_UNKNOWN;
	}
}

// The battery packet:
//
//     04 00 04 00 04 00 N   then N entries of five bytes
//     entry: component | 01 | level 0..100 | status | 01
//
// The count is what the entries are read from, not the packet's length: a pair
// reports three, the Max reports one, and a bud out of its case reports two.
//
// Only the parts the packet names are updated, and the rest keep what they
// last said. A pair taken out of its case stops reporting the case entirely --
// the earbuds cannot talk to it once it is shut -- so a packet that simply does
// not mention the case is not the case saying it has no charge.
static void parse_battery(const uint8_t *data, size_t len) {
	if (len < 7) {
		return;
	}
	int count = data[6];
	if (count < 1 || (size_t)(7 + count * 5) > len) {
		return;
	}

	pthread_mutex_lock(&lock);
	airpods_level_t left = g_state.left;
	airpods_level_t right = g_state.right;
	airpods_level_t single = g_state.single;
	airpods_level_t box = g_state.charging_case;
	pthread_mutex_unlock(&lock);

	for (int i = 0; i < count; i++) {
		const uint8_t *entry = data + 7 + i * 5;
		int level = entry[2];
		airpods_charge_t charge = charge_from_byte(entry[3]);

		// Counterfeit AirPods answer 127 once the case is shut. A level that
		// is not a percentage is not a reading.
		if (level > 100) {
			continue;
		}

		airpods_level_t value = {.known = true, .percent = level, .charge = charge};
		switch (entry[0]) {
		case AAP_BATTERY_LEFT:
			left = value;
			break;
		case AAP_BATTERY_RIGHT:
			right = value;
			break;
		case AAP_BATTERY_SINGLE:
			single = value;
			break;
		case AAP_BATTERY_CASE:
			box = value;
			break;
		default:
			break;
		}
	}

	pthread_mutex_lock(&lock);
	bool changed = memcmp(&g_state.left, &left, sizeof(left)) != 0 ||
				   memcmp(&g_state.right, &right, sizeof(right)) != 0 ||
				   memcmp(&g_state.single, &single, sizeof(single)) != 0 ||
				   memcmp(&g_state.charging_case, &box, sizeof(box)) != 0 || !g_state.have_battery;
	g_state.left = left;
	g_state.right = right;
	g_state.single = single;
	g_state.charging_case = box;
	g_state.have_battery = true;
	if (changed) {
		g_state.serial++;
	}
	pthread_mutex_unlock(&lock);

	if (changed) {
		printf("airpods: battery L=%d%% R=%d%% case=%d%%\n", left.known ? left.percent : -1,
			   right.known ? right.percent : -1, box.known ? box.percent : -1);
	}
}

static airpods_noise_t noise_from_byte(uint8_t value) {
	switch (value) {
	case 0x01:
		return AIRPODS_NOISE_OFF;
	case 0x02:
		return AIRPODS_NOISE_CANCELLATION;
	case 0x03:
		return AIRPODS_NOISE_TRANSPARENCY;
	case 0x04:
		return AIRPODS_NOISE_ADAPTIVE;
	default:
		return AIRPODS_NOISE_UNKNOWN;
	}
}

static uint8_t byte_from_noise(airpods_noise_t mode) {
	switch (mode) {
	case AIRPODS_NOISE_OFF:
		return 0x01;
	case AIRPODS_NOISE_CANCELLATION:
		return 0x02;
	case AIRPODS_NOISE_TRANSPARENCY:
		return 0x03;
	case AIRPODS_NOISE_ADAPTIVE:
		return 0x04;
	default:
		return 0x00;
	}
}

// The set of modes a long press steps through, as the headphones report it.
// Trimmed to the three the page shows, so that what is on screen is what a
// later write puts back.
static void parse_noise_cycle(const uint8_t *data, size_t len) {
	if (len < 8) {
		return;
	}
	uint8_t mask = data[7] & AIRPODS_CYCLE_ALL;

	pthread_mutex_lock(&lock);
	bool changed = !g_state.cycle_known || g_state.noise_cycle != mask;
	g_state.noise_cycle = mask;
	g_state.cycle_known = true;
	if (changed) {
		g_state.serial++;
	}
	pthread_mutex_unlock(&lock);

	if (changed) {
		printf("airpods: long press, modes 0x%02x\n", mask);
	}
}

// A setting the headphones are reporting:
//
//     04 00 04 00 09 00 <which> <value> 00 00 00
//
// Dozens of settings come through here -- press speed, volume swipe, call
// handling. Two are read: the noise control, and the set of modes a long press
// steps through. The byte after the opcode says which one this is, and anything
// else is left alone.
static void parse_setting(const uint8_t *data, size_t len) {
	if (len < 8) {
		return;
	}
	if (data[6] == AAP_SETTING_NOISE_CYCLE) {
		parse_noise_cycle(data, len);
		return;
	}
	if (data[6] != AAP_SETTING_NOISE) {
		// One line the first time each identifier turns up, and never again.
		// The headphones dump the settings they support after the handshake and
		// no request exists for them, so this log is the only way to find out
		// what a given pair actually reports.
		static uint8_t seen[32];
		uint8_t id = data[6];
		if (!(seen[id >> 3] & (uint8_t)(1u << (id & 7)))) {
			seen[id >> 3] |= (uint8_t)(1u << (id & 7));
			printf("airpods: setting 0x%02x = 0x%02x (not used)\n", id, data[7]);
		}
		return;
	}
	airpods_noise_t mode = noise_from_byte(data[7]);
	if (mode == AIRPODS_NOISE_UNKNOWN) {
		// Said rather than dropped: a mode these headphones report with a byte
		// this does not know is the one thing that would leave the tab bar
		// showing nothing selected, and there would be no way to tell.
		printf("airpods: noise control, unknown value 0x%02x\n", data[7]);
		return;
	}

	pthread_mutex_lock(&lock);
	bool changed = g_state.noise != mode;
	g_state.noise = mode;
	if (changed) {
		g_state.serial++;
	}
	pthread_mutex_unlock(&lock);

	if (changed) {
		printf("airpods: noise control -> %d\n", (int)mode);
	}
}

// The metadata packet is a run of NUL-terminated strings: a name, the model
// number, the manufacturer, serial numbers, firmware versions. Rather than
// count the bytes in front of them -- implementations disagree on how many --
// every string in the packet is looked at and the one shaped like a model
// number is taken. "A2084" cannot be mistaken for a firmware version or a
// serial, and if the layout ever shifts this still finds it.
static bool looks_like_model_number(const char *s) {
	if (s[0] != 'A') {
		return false;
	}
	int digits = 0;
	for (const char *p = s + 1; *p; p++) {
		if (!isdigit((unsigned char)*p)) {
			return false;
		}
		digits++;
	}
	return digits == 4;
}

static void parse_metadata(const uint8_t *data, size_t len) {
	char found[16] = "";

	size_t at = 6;
	while (at < len) {
		size_t start = at;
		while (at < len && data[at] >= 0x20 && data[at] < 0x7f) {
			at++;
		}
		size_t length = at - start;
		if (length >= 4 && length < sizeof(found)) {
			char text[16];
			memcpy(text, data + start, length);
			text[length] = '\0';
			if (looks_like_model_number(text)) {
				snprintf(found, sizeof(found), "%s", text);
				break;
			}
		}
		at++; // step over whatever ended the run
	}

	if (!found[0]) {
		return;
	}

	const model_entry_t *entry = entry_from_number(found);

	pthread_mutex_lock(&lock);
	bool changed = strcmp(g_state.model_number, found) != 0;
	snprintf(g_state.model_number, sizeof(g_state.model_number), "%s", found);
	if (entry) {
		g_state.model = entry->model;
		snprintf(g_state.name, sizeof(g_state.name), "%s", entry->name);
		g_state.has_noise_control = entry->noise_control;
		g_state.has_adaptive = entry->adaptive;
		g_state.has_press_hold = entry->press_hold;

		// Until the headphones say otherwise, every mode the model has is in
		// the cycle -- which is how a pair leaves the factory. An empty set
		// cannot be built up one mode at a time, because a set of one is
		// below the minimum the headphones accept.
		if (!g_state.cycle_known) {
			g_state.noise_cycle = AIRPODS_CYCLE_CANCELLATION | AIRPODS_CYCLE_TRANSPARENCY |
								  (entry->adaptive ? AIRPODS_CYCLE_ADAPTIVE : 0);
		}
	}
	if (changed) {
		g_state.serial++;
	}
	pthread_mutex_unlock(&lock);

	if (changed) {
		printf("airpods: model %s\n", found);
	}
}

// True while the session is worth keeping open.
static bool handle_packet(const uint8_t *data, size_t len) {
	if (len < 4) {
		return true;
	}

	// 02 00 04 00 is the other end saying it is going away.
	if (data[0] == 0x02 && data[1] == 0x00 && data[2] == 0x04 && data[3] == 0x00) {
		return false;
	}
	if (data[0] != 0x04 || data[1] != 0x00 || data[2] != 0x04 || data[3] != 0x00) {
		return true; // the connect response, and anything else with its own shape
	}
	if (len < 6) {
		return true;
	}

	switch (data[4]) {
	case AAP_OPCODE_BATTERY:
		parse_battery(data, len);
		break;
	case AAP_OPCODE_SETTING:
		parse_setting(data, len);
		break;
	case AAP_OPCODE_METADATA:
		parse_metadata(data, len);
		break;
	default:
		// Ear detection, noise control, stem presses, head tracking and the
		// rest. They arrive whether or not anyone asked, and stepping over
		// them is the whole of what this needs to do with them.
		break;
	}
	return true;
}

// ---------------------------------------------------------------------------
// The session
// ---------------------------------------------------------------------------

static int aap_connect(const char *mac) {
	struct sockaddr_l2 addr;
	memset(&addr, 0, sizeof(addr));
	addr.l2_family = AF_BLUETOOTH;
	addr.l2_psm = to_le16(AAP_PSM);
	addr.l2_bdaddr_type = 0; // BR/EDR
	if (!parse_mac(mac, addr.l2_bdaddr)) {
		return -1;
	}

	// SEQPACKET, not STREAM: AAP has no length in front of a message, so the
	// only thing that says where one ends is the L2CAP packet boundary. On a
	// stream socket two notifications arriving together would be read as one
	// unparseable blob.
	int fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
	if (fd < 0) {
		return -1;
	}
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static bool send_all(int fd, const uint8_t *data, size_t len) {
	while (len) {
		ssize_t n = write(fd, data, len);
		if (n <= 0) {
			if (n < 0 && errno == EINTR) {
				continue;
			}
			return false;
		}
		data += n;
		len -= (size_t)n;
	}
	return true;
}

static bool should_stop(const char *mac) {
	pthread_mutex_lock(&lock);
	bool stop = stopping || strcmp(want_mac, mac) != 0;
	pthread_mutex_unlock(&lock);
	return stop;
}

// Runs the session until it ends. The socket is read through poll() rather
// than blocked on, so a disconnection or a shutdown is noticed within a tick
// instead of when the next notification happens to arrive.
static void run_session(int fd, const char *mac) {
	uint8_t buf[READ_BUF];

	pthread_mutex_lock(&lock);
	g_state.connected = true;
	g_state.serial++;
	pthread_mutex_unlock(&lock);
	printf("airpods: session open with %s\n", mac);

	uint32_t notified_at = now_ms();
	bool notified_again = false;

	while (!should_stop(mac)) {
		struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
		int ready = poll(&pfd, 1, POLL_MS);
		if (ready < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (ready > 0) {
			if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
				break;
			}
			ssize_t n = read(fd, buf, sizeof(buf));
			if (n == 0) {
				break; // the other end closed
			}
			if (n < 0) {
				if (errno == EINTR || errno == EAGAIN) {
					continue;
				}
				break;
			}
			if (!handle_packet(buf, (size_t)n)) {
				break;
			}
		}

		// A mode asked for on the interface thread goes out here, on the
		// thread that owns the socket.
		pthread_mutex_lock(&lock);
		airpods_noise_t asked = pending_noise;
		pending_noise = AIRPODS_NOISE_UNKNOWN;
		pthread_mutex_unlock(&lock);
		if (asked != AIRPODS_NOISE_UNKNOWN) {
			uint8_t command[] = {0x04, 0x00, 0x04, 0x00, 0x09, 0x00, AAP_SETTING_NOISE, byte_from_noise(asked),
								 0x00, 0x00, 0x00};
			if (!send_all(fd, command, sizeof(command))) {
				break;
			}
		}

		// And the set of modes a long press steps through, the same way.
		pthread_mutex_lock(&lock);
		int cycle = pending_cycle;
		pending_cycle = -1;
		pthread_mutex_unlock(&lock);
		if (cycle >= 0) {
			uint8_t command[] = {0x04, 0x00, 0x04, 0x00, 0x09, 0x00, AAP_SETTING_NOISE_CYCLE, (uint8_t)cycle,
								 0x00, 0x00, 0x00};
			if (!send_all(fd, command, sizeof(command))) {
				break;
			}
		}

		// Some firmware wants asking twice. Costs one packet, and without it
		// the battery sometimes never starts arriving at all.
		if (!notified_again && !g_state.have_battery && (uint32_t)(now_ms() - notified_at) >= NOTIFY_RETRY_MS) {
			notified_again = true;
			send_all(fd, AAP_REQUEST_NOTIFICATIONS, sizeof(AAP_REQUEST_NOTIFICATIONS));
		}
	}

	close(fd);
	printf("airpods: session closed\n");
}

static void clear_state(void) {
	pthread_mutex_lock(&lock);
	uint32_t serial = g_state.serial + 1;
	memset(&g_state, 0, sizeof(g_state));
	g_state.serial = serial;
	// Anything asked for goes with the session it was asked of: a command left
	// queued would go out to whichever pair connects next.
	pending_noise = AIRPODS_NOISE_UNKNOWN;
	pending_cycle = -1;
	pthread_mutex_unlock(&lock);
}

void airpods_set_noise(airpods_noise_t mode) {
	pthread_mutex_lock(&lock);
	if (g_state.connected && g_state.has_noise_control && mode != AIRPODS_NOISE_UNKNOWN) {
		pending_noise = mode;
		// Taken as done straight away, so the tab lights under the finger
		// instead of waiting for a notification that may never name this mode.
		// Whatever the headphones report next wins over it: this is what was
		// asked for, and the packet is what happened.
		if (g_state.noise != mode) {
			g_state.noise = mode;
			g_state.serial++;
		}
	}
	pthread_mutex_unlock(&lock);
}

// How many mode bits are set. The headphones want at least two: a cycle of one
// is not a cycle, and they answer such a mask by keeping what they had.
static int cycle_bits(uint8_t mask) {
	int n = 0;
	for (uint8_t bit = AIRPODS_CYCLE_CANCELLATION; bit <= AIRPODS_CYCLE_ADAPTIVE; bit = (uint8_t)(bit << 1)) {
		if (mask & bit) {
			n++;
		}
	}
	return n;
}

void airpods_set_noise_cycle(uint8_t modes) {
	modes &= AIRPODS_CYCLE_ALL;
	if (cycle_bits(modes) < 2) {
		return;
	}
	pthread_mutex_lock(&lock);
	if (g_state.connected && g_state.has_press_hold) {
		pending_cycle = modes;
		// Taken as done: this is one of the settings the headphones never echo,
		// so waiting for confirmation would leave every switch flipping back
		// under the finger.
		if (!g_state.cycle_known || g_state.noise_cycle != modes) {
			g_state.noise_cycle = modes;
			g_state.cycle_known = true;
			g_state.serial++;
		}
	}
	pthread_mutex_unlock(&lock);
}

bool airpods_feed_packet(const uint8_t *data, unsigned length) { return handle_packet(data, length); }

// ---------------------------------------------------------------------------
// A session with nothing on the other end
//
// A desktop has no Bluetooth adapter, so the page and the row cannot be looked
// at at all unless the packets come from somewhere other than a socket.
// SONIX_AIRPODS_FAKE holds a model number, three levels and optionally a noise
// mode -- "A2084,85,80,62,2" -- and the worker builds exactly the packets those
// describe and hands them to the same parsers. The variable is never set on the
// device.
// ---------------------------------------------------------------------------

static bool fake_session(void) {
	const char *spec = getenv("SONIX_AIRPODS_FAKE");
	if (!spec || !spec[0]) {
		return false;
	}

	char number[16] = "";
	int left = -1, right = -1, box = -1, noise = -1;
	sscanf(spec, "%15[^,],%d,%d,%d,%d", number, &left, &right, &box, &noise);

	pthread_mutex_lock(&lock);
	memset(&g_state, 0, sizeof(g_state));
	g_state.connected = true;
	g_state.serial++;
	pthread_mutex_unlock(&lock);

	if (number[0]) {
		uint8_t metadata[64] = {0x04, 0x00, 0x04, 0x00, 0x1D, 0x00, 0x01, 0xB2, 0x00, 0x08, 0x00};
		size_t at = 11;
		const char *name = "Sonix";
		memcpy(metadata + at, name, strlen(name) + 1);
		at += strlen(name) + 1;
		memcpy(metadata + at, number, strlen(number) + 1);
		at += strlen(number) + 1;
		airpods_feed_packet(metadata, (unsigned)at);
	}

	int levels[3] = {left, right, box};
	uint8_t component[3] = {AAP_BATTERY_LEFT, AAP_BATTERY_RIGHT, AAP_BATTERY_CASE};
	const model_entry_t *fake_entry = entry_from_number(number);
	if (fake_entry && fake_entry->model == AIRPODS_MODEL_MAX) {
		// One headset, one battery, no case: the shape the Max report in.
		levels[1] = levels[2] = -1;
		component[0] = AAP_BATTERY_SINGLE;
	}
	uint8_t battery[32] = {0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00};
	size_t at = 7;
	for (int i = 0; i < 3; i++) {
		if (levels[i] < 0) {
			continue;
		}
		battery[6]++;
		battery[at++] = component[i];
		battery[at++] = 0x01;
		battery[at++] = (uint8_t)levels[i];
		battery[at++] = 0x02;
		battery[at++] = 0x01;
	}
	airpods_feed_packet(battery, (unsigned)at);

	if (noise > 0) {
		uint8_t setting[] = {0x04, 0x00, 0x04, 0x00, 0x09, 0x00, AAP_SETTING_NOISE, (uint8_t)noise, 0x00, 0x00, 0x00};
		airpods_feed_packet(setting, sizeof(setting));
	}


	for (;;) {
		pthread_mutex_lock(&lock);
		bool done = stopping;
		pthread_mutex_unlock(&lock);
		if (done) {
			break;
		}
		sleep_ms(200);
	}
	return true;
}

static void *airpods_worker(void *unused) {
	(void)unused;
	thread_be_background("airpods");

	for (;;) {
		char mac[MAC_LEN];
		char name[NAME_LEN];

		pthread_mutex_lock(&lock);
		if (stopping) {
			pthread_mutex_unlock(&lock);
			break;
		}
		snprintf(mac, sizeof(mac), "%s", want_mac);
		snprintf(name, sizeof(name), "%s", want_name);
		if (strcmp(mac, have_mac) != 0) {
			snprintf(have_mac, sizeof(have_mac), "%s", mac);
			failures = 0;
		}
		int tried = failures;
		pthread_mutex_unlock(&lock);

		if (fake_session()) {
			break;
		}

		if (mac[0] && tried < CONNECT_ATTEMPTS) {
			int fd = aap_connect(mac);
			if (fd >= 0) {
				pthread_mutex_lock(&lock);
				failures = 0;
				memset(&g_state, 0, sizeof(g_state));
				g_state.model = model_from_name(name);
				snprintf(g_state.name, sizeof(g_state.name), "%s", default_name(g_state.model));
				// Until the metadata says otherwise: a Pro or a Max has the
				// two modes, and the adaptive one is not assumed.
				g_state.has_noise_control =
					g_state.model == AIRPODS_MODEL_PRO || g_state.model == AIRPODS_MODEL_MAX;
				g_state.serial++;
				pthread_mutex_unlock(&lock);

				// The handshake, then the request for notifications. A moment
				// between them: the headphones answer the first before they
				// will listen to the second, and every implementation of this
				// protocol leaves the same gap.
				bool ok = send_all(fd, AAP_HANDSHAKE, sizeof(AAP_HANDSHAKE));
				if (ok) {
					sleep_ms(300);
					ok = send_all(fd, AAP_REQUEST_NOTIFICATIONS, sizeof(AAP_REQUEST_NOTIFICATIONS));
				}
				if (ok) {
					run_session(fd, mac);
				} else {
					close(fd);
				}
				clear_state();
			} else {
				pthread_mutex_lock(&lock);
				failures++;
				int count = failures;
				pthread_mutex_unlock(&lock);
				if (count == CONNECT_ATTEMPTS) {
					// Not Apple, or not willing. Said once and then left alone
					// until something else connects: retrying for ever would
					// be a socket every four seconds for the whole time a pair
					// of ordinary headphones is in use.
					printf("airpods: %s does not answer on AAP, not an Apple device\n", mac);
				}
			}
		}

		pthread_mutex_lock(&lock);
		bool work = stopping || strcmp(want_mac, have_mac) != 0;
		if (!work) {
			struct timespec deadline;
			deadline_in_ms(&deadline, RETRY_MS);
			pthread_cond_timedwait(&wake, &lock, &deadline);
		}
		pthread_mutex_unlock(&lock);
	}

	return NULL;
}

// ---------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------

void airpods_init(void) {
	if (worker_running) {
		return;
	}
	if (pthread_create(&worker, NULL, airpods_worker, NULL) != 0) {
		fprintf(stderr, "airpods: could not start the thread\n");
		return;
	}
	worker_running = true;
}

void airpods_stop(void) {
	if (!worker_running) {
		return;
	}
	pthread_mutex_lock(&lock);
	stopping = true;
	want_mac[0] = '\0';
	pthread_cond_signal(&wake);
	pthread_mutex_unlock(&lock);
	pthread_join(worker, NULL);
	worker_running = false;
	stopping = false;
}

void airpods_set_device(const char *mac, const char *name) {
	pthread_mutex_lock(&lock);
	const char *wanted = mac ? mac : "";
	if (strcmp(wanted, want_mac) != 0) {
		snprintf(want_mac, sizeof(want_mac), "%s", wanted);
		snprintf(want_name, sizeof(want_name), "%s", name ? name : "");
		pthread_cond_signal(&wake);
	}
	pthread_mutex_unlock(&lock);
}

bool airpods_get(airpods_state_t *out) {
	pthread_mutex_lock(&lock);
	*out = g_state;
	bool connected = g_state.connected;
	pthread_mutex_unlock(&lock);
	return connected;
}


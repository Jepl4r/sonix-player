#ifndef WIFI_H
#define WIFI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The wifi radio, driven exactly the way the stock player drives it: every
// change of state is a request to sys_server (WIFI:ON, WIFI:SCAN,
// WIFI:SSID_CONNECT, ...), which runs wifi_on.sh and wpa_cli and leaves the
// answers in /data/wifi_result.txt, /data/wifi_status.txt and
// /data/wifi_network.txt, which are then read back. Where the daemon is not
// there (host build, or a firmware without it) the same work is done by hand
// with the scripts and wpa_cli, so nothing here is a dead end on a PC.
//
// Everything in this file is safe to call from the interface thread: the
// blocking parts -- the scripts sleep whole seconds, wpa_cli forks -- run on
// this module's own worker thread, and callers only read a snapshot of what it
// has found. The UI polls; there are no callbacks.

#define WIFI_SSID_MAX 64
#define WIFI_MAX_NETWORKS 40

typedef enum {
	WIFI_STATE_OFF = 0,	   // radio down
	WIFI_STATE_ON,		   // up, associated with nothing
	WIFI_STATE_CONNECTING, // wpa_supplicant is working on it
	WIFI_STATE_CONNECTED,  // associated, and with an address
} wifi_state_t;

// One entry of the network list the page draws. Scan results and saved
// networks are merged into a single list, so a network that is both shows up
// once.
typedef struct {
	char ssid[WIFI_SSID_MAX];
	int signal;	 // dBm as reported by the scan, 0 when unknown
	int bars;	 // 0..3, what the status bar and the row draw
	bool secured; // needs a passphrase
	bool saved;	 // wpa_supplicant already has it
	bool current; // the one currently joined
	int id;		 // wpa_supplicant network id, -1 when not saved
} wifi_network_t;

typedef struct {
	wifi_state_t state;
	char ssid[WIFI_SSID_MAX]; // empty unless connected/connecting
	char ip[40];
	char mac[24];
	int signal; // dBm of the live link, 0 when unknown
	int bars;	// 0..3
} wifi_status_t;

// What the last operation the user asked for is doing. The pages show a
// spinner-ish label from this and a toast when it lands.
typedef enum {
	WIFI_OP_IDLE = 0,
	WIFI_OP_BUSY,
	WIFI_OP_OK,
	WIFI_OP_FAILED,
} wifi_op_t;

// Which of the three things the last result belongs to. The page says
// something different for each -- a network joined names it, one forgotten or
// left does not -- and the result on its own cannot tell them apart.
typedef enum {
	WIFI_OPKIND_CONNECT = 0,
	WIFI_OPKIND_DISCONNECT,
	WIFI_OPKIND_FORGET,
} wifi_opkind_t;

// Why the last attempt to join a network ended the way it did. The page needs
// the distinction: a refused passphrase is worth asking for again, a router
// that never answered is not.
typedef enum {
	WIFI_FAIL_NONE = 0,
	WIFI_FAIL_OTHER,	 // no association, and nothing to say about why
	WIFI_FAIL_WRONG_KEY, // the passphrase was refused or is malformed
	WIFI_FAIL_REFUSED,	 // the command never reached wpa_supplicant
} wifi_fail_t;

// Starts the worker thread and, if the switch was left on, brings the radio
// back up. Cheap and non-blocking: the actual bring-up happens on the worker.
void wifi_init(void);

// False when there is no wlan0 at all (host build, or a device without the
// module loaded). The page says so instead of pretending.
bool wifi_available(void);

// The switch, as the user left it. Persisted under [wireless] wifi.
bool wifi_get_enabled(void);
void wifi_set_enabled(bool on);

// The same switch without recording it as the user's choice: for the idle park,
// which takes the radio down to save current and brings it back at the next
// wake.
void wifi_set_enabled_transient(bool on);

// A snapshot of where the radio is. Never blocks.
void wifi_get_status(wifi_status_t *out);

// The address wlan0 actually has, asked of the kernel rather than of the
// supplicant: an ioctl, no fork, and no flapping. True when there is one.
// This is the right question for "can a server still serve" -- see wifi.c.
bool wifi_interface_address(char *out, size_t size);

// Slows the status poll while something is moving bulk data over the radio.
// Each poll is a fork of the whole player; on one core they come out of the
// transfer's throughput. Costs a few seconds of lag on the signal bars.
void wifi_set_status_poll_slow(bool slow);

// Kicks a scan. Results land in the list a few seconds later; the page watches
// wifi_networks_serial() to know when to redraw.
void wifi_scan_start(void);
bool wifi_scan_running(void);

// Bumped every time the network list is replaced.
uint32_t wifi_networks_serial(void);

// Copies at most `max` entries into `out`, strongest first, and returns how
// many were written.
int wifi_get_networks(wifi_network_t *out, int max);

// Joins a network. `psk` is ignored (and may be NULL) for an open one. A
// network wpa_supplicant already knows is re-selected rather than added again.
void wifi_connect(const char *ssid, const char *psk);

// Leaves the current network without forgetting it.
void wifi_disconnect(void);

// Removes it from wpa_supplicant's list for good.
void wifi_forget(const char *ssid);

// The state of the last connect/forget the user asked for, and the SSID it was
// about. Reading WIFI_OP_OK or WIFI_OP_FAILED clears it back to idle, so the
// page shows each outcome exactly once.
wifi_op_t wifi_take_op_result(char *ssid_out, int ssid_size);

// Why the last connect failed. Unlike the result above this is not consumed by
// reading it: it is set when an attempt ends and stays until the next one
// starts, so the page can read it after taking the result.
wifi_fail_t wifi_last_failure(void);

// What the last result was about. Same rule: set when the request is made, and
// still readable after the result has been taken.
wifi_opkind_t wifi_last_op_kind(void);

// True while the radio is being switched on or off, so the page can grey its
// own switch out for the second or two the script takes.
bool wifi_busy(void);

#endif /* WIFI_H */

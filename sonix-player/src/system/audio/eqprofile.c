#include "eqprofile.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/system/audio/eq.h"
#include "src/system/core/config.h"

// The output whose state is the live one in eq.c, "" until the first routing
// tick with the option on. Mirrored in [eq] po_live so a restart on another
// output knows whose state it found (see eqprofile_output_changed).
static char live[24];
static bool live_known;

// A config key is at most 31 characters: "po_bt" + 12 hex digits is 17.
#define KEY_MAX 32

static void make_key(char *out, size_t size, const char *prefix, const char *output) {
	snprintf(out, size, "%s%s", prefix, output);
}

bool eqprofile_enabled(void) { return config_get_int("eq", "per_output", 0) != 0; }

void eqprofile_set_enabled(bool on) {
	config_set_int("eq", "per_output", on ? 1 : 0);
	// Either way, nobody owns the live state yet. Turned on, the first routing
	// tick hands it to whatever output is in use -- the state on screen is what
	// the user has just been listening with. Turned off, an owner left on file
	// would be stale by the time the option came back on.
	config_set("eq", "po_live", "");
	config_save();
	live[0] = '\0';
	live_known = on;
}

// "101:0,2,3,..." -- graphic EQ, PEQ, MSEB on/off, then the ten bands.
static void save_state(const char *output) {
	char key[KEY_MAX];
	char value[128];
	size_t used = (size_t)snprintf(value, sizeof(value), "%d%d%d:", eq_get_enabled() ? 1 : 0,
								   peq_get_enabled() ? 1 : 0, mseb_get_enabled() ? 1 : 0);
	for (int i = 0; i < EQ_BANDS && used < sizeof(value); i++) {
		used += (size_t)snprintf(value + used, sizeof(value) - used, i ? ",%d" : "%d", eq_get_band(i));
	}
	make_key(key, sizeof(key), "po_", output);
	config_set("eq", key, value);
	make_key(key, sizeof(key), "pn_", output);
	config_set("eq", key, peq_current_preset());
}

// Puts a saved state back. Only what differs is set: every setter writes the
// config, and an output change that touches nothing should not write the card
// ten times. False when there is nothing on file for this output.
static bool load_state(const char *output) {
	char key[KEY_MAX];
	make_key(key, sizeof(key), "po_", output);
	const char *value = config_get("eq", key, "");
	if (strlen(value) < 4 || value[3] != ':') {
		return false;
	}
	bool eq_on = value[0] == '1';
	bool peq_on = value[1] == '1';
	bool mseb_on = value[2] == '1';

	const char *p = value + 4;
	for (int i = 0; i < EQ_BANDS && *p; i++) {
		char *end = NULL;
		long gain = strtol(p, &end, 10);
		if (end == p) {
			break;
		}
		if (gain != eq_get_band(i)) {
			eq_set_band(i, (int)gain);
		}
		p = *end == ',' ? end + 1 : end;
	}
	if (eq_on != eq_get_enabled()) {
		eq_set_enabled(eq_on);
	}

	// The preset first, then the switch: switched on over the old bands for
	// the length of one card read would be heard.
	make_key(key, sizeof(key), "pn_", output);
	char name[128];
	snprintf(name, sizeof(name), "%s", config_get("eq", key, ""));
	if (name[0] && strcmp(name, peq_current_preset()) != 0) {
		if (!peq_preset_load(name, NULL)) {
			// Deleted or renamed on the card since. The bands stay as they are,
			// and so does the switch below, which is still what the user had.
			fprintf(stderr, "eqprofile: PEQ preset '%s' for %s is gone\n", name, output);
		}
	}
	if (peq_on != peq_get_enabled()) {
		peq_set_enabled(peq_on);
	}
	if (mseb_on != mseb_get_enabled()) {
		mseb_set_enabled(mseb_on);
	}
	return true;
}

static bool is_one_pair(const char *output) {
	return strncmp(output, EQPROFILE_BLUETOOTH, 2) == 0 && output[2] != '\0';
}

void eqprofile_output_changed(const char *output, const char *mac) {
	if (!output || !eqprofile_enabled()) {
		live_known = false;
		return;
	}

	char now[24];
	snprintf(now, sizeof(now), "%s", output);
	if (strcmp(output, EQPROFILE_BLUETOOTH) == 0 && mac && mac[0]) {
		size_t used = 2;
		for (const char *c = mac; *c && used + 1 < sizeof(now); c++) {
			if (isxdigit((unsigned char)*c)) {
				now[used++] = (char)toupper((unsigned char)*c);
			}
		}
		now[used] = '\0';
	}

	if (!live_known) {
		// First look since start-up: whose state is live is on file.
		snprintf(live, sizeof(live), "%s", config_get("eq", "po_live", ""));
		live_known = true;
	}
	if (strcmp(now, live) == 0) {
		return;
	}

	if (!live[0]) {
		// The option has just been switched on: the state on screen belongs to
		// the output in use, and nothing is loaded over it. A state left on
		// file from an earlier time the option was on would undo what the user
		// set up since.
		fprintf(stderr, "eqprofile: the current EQ is now %s's\n", now);
		snprintf(live, sizeof(live), "%s", now);
		config_set("eq", "po_live", live);
		config_save();
		return;
	}

	// The outgoing output keeps what it was left at, edits included. A pair of
	// headphones also leaves its state as the one for Bluetooth in general,
	// which is what the next new pair starts from.
	save_state(live);
	if (is_one_pair(live)) {
		save_state(EQPROFILE_BLUETOOTH);
	}

	bool loaded = load_state(now);
	if (!loaded && is_one_pair(now)) {
		loaded = load_state(EQPROFILE_BLUETOOTH);
	}
	fprintf(stderr, "eqprofile: %s -> %s%s\n", live, now,
			loaded ? "" : " (nothing on file: the current EQ stays)");

	snprintf(live, sizeof(live), "%s", now);
	config_set("eq", "po_live", live);
	config_save();
}

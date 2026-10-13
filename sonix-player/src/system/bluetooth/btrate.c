#include "btrate.h"

#include <strings.h>

bool btrate_supported(unsigned rate, const unsigned *rates, int count) {
	for (int i = 0; rates && i < count; i++) {
		if (rates[i] == rate) {
			return true;
		}
	}
	return false;
}

unsigned btrate_highest_upto(unsigned limit, const unsigned *rates, int count) {
	unsigned best = 0;
	for (int i = 0; rates && i < count; i++) {
		if (rates[i] <= limit && rates[i] > best) {
			best = rates[i];
		}
	}
	return best;
}

unsigned btrate_follow(unsigned track_rate, bool dsd, const unsigned *rates, int count) {
	if (track_rate == 0) {
		return 0;
	}
	// 11025 divides every rate of the CD family (11.025, 22.05, 44.1, 88.2,
	// 176.4, 352.8 kHz, and DSD carried as PCM at those) and none of the other
	// one; whatever is left -- 8, 16, 32, 48, 96, 192 kHz -- is closer to 48.
	unsigned base = (track_rate % 11025u == 0) ? 44100u : 48000u;
	// The double rate for anything above the base: 88.2 for 88.2 and 176.4,
	// 96 for 96 and 192 -- the decimator halves the higher ones on the way.
	// DSD always, whatever rate its PCM comes at: it is a hi-res source.
	bool high = dsd || track_rate > base;
	if (high && btrate_supported(base * 2, rates, count)) {
		return base * 2;
	}
	if (btrate_supported(base, rates, count)) {
		return base;
	}
	return 0;
}

unsigned btrate_wanted(bt_rate_mode_t mode, const char *codec, unsigned track_rate, bool dsd, const unsigned *rates,
					   int count) {
	switch (mode) {
	case BT_RATE_FOLLOW:
		return btrate_follow(track_rate, dsd, rates, count);
	case BT_RATE_96K:
		// LDAC only: no other codec this player sends goes above 48 kHz, and
		// for those the choice means what "Auto" means.
		if (codec && strcasecmp(codec, "LDAC") == 0) {
			return btrate_highest_upto(96000, rates, count);
		}
		return 0;
	case BT_RATE_AUTO:
	case BT_RATE_44K: // a start-up option of the daemon, not a request
	default:
		return 0;
	}
}

size_t btrate_ldac_config(unsigned rate, uint8_t out[BTRATE_LDAC_CONFIG_SIZE]) {
	static const struct {
		unsigned rate;
		uint8_t bit;
	} LDAC_RATES[] = {
		{44100, 0x20}, {48000, 0x10}, {88200, 0x08}, {96000, 0x04}, {176400, 0x02}, {192000, 0x01},
	};
	uint8_t bit = 0;
	for (size_t i = 0; i < sizeof(LDAC_RATES) / sizeof(LDAC_RATES[0]); i++) {
		if (LDAC_RATES[i].rate == rate) {
			bit = LDAC_RATES[i].bit;
		}
	}
	if (!bit) {
		return 0;
	}
	// Written byte by byte, not as a struct: the layout is the one on the air,
	// whatever this compiler would do with bit fields.
	out[0] = 0x2D; // vendor id 0x0000012D, little-endian
	out[1] = 0x01;
	out[2] = 0x00;
	out[3] = 0x00;
	out[4] = 0xAA; // codec id 0x00AA, little-endian
	out[5] = 0x00;
	out[6] = bit;  // sampling_freq:6, low bits first
	out[7] = 0x01; // channel_mode:3 = stereo
	return BTRATE_LDAC_CONFIG_SIZE;
}

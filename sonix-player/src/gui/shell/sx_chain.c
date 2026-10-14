#include "sx_chain.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "src/gui/nowplaying/btline.h"
#include "src/system/audio/audio.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/playback/device_state.h"
#include "src/system/streaming/radio.h"

// "44.1" or "96": the rate in kHz the way the Bluetooth line writes it.
static void rate_khz(char *out, size_t size, int hz) {
	if (hz % 1000 == 0) {
		snprintf(out, size, "%d", hz / 1000);
	} else {
		snprintf(out, size, "%.1f", hz / 1000.0);
	}
}

// The codec the link was asked for runs, or none was asked for. The names
// compare without case: the stack says "LDAC", a remembered choice may not.
static bool codec_as_chosen(void) {
	char chosen[BT_CODEC_MAX];
	if (!bluetooth_codec_chosen(chosen, sizeof(chosen)) || !chosen[0]) {
		return true;
	}
	char selected[BT_CODEC_MAX] = "";
	bluetooth_get_codecs(NULL, 0, selected, sizeof(selected));
	if (!selected[0]) {
		return true; // not known yet: no warning on a guess
	}
	return strcasecmp(chosen, selected) == 0;
}

bool sx_chain_short(char *out, size_t size, bool *warn) {
	if (warn) {
		*warn = false;
	}
	if (!out || size == 0) {
		return false;
	}
	out[0] = '\0';

	// Headphones: the link says it all.
	if (btline_compose(out, size)) {
		if (warn) {
			*warn = !codec_as_chosen();
		}
		return true;
	}

	// A station (Phase 2): what comes down the wire -- the file the player had
	// loaded before it is not what plays.
	if (radio_is_active()) {
		radio_now_t now;
		radio_get_now(&now);
		if (now.codec[0] && now.bitrate > 0) {
			snprintf(out, size, "%s \xC2\xB7 %d kbps", now.codec, now.bitrate);
		} else {
			snprintf(out, size, "%s", now.codec);
		}
		return out[0] != '\0';
	}

	// The jack (or USB): what the file is and the rate it plays at.
	const char *file = NULL;
	const song_metadata_t *m = device_state_loaded_metadata(&file);
	(void)m;
	char codec[16] = "";
	audio_get_stream_codec(codec, sizeof(codec));
	if (!codec[0] && file && file[0]) {
		const char *dot = strrchr(file, '.');
		if (dot && !strchr(dot, '/')) {
			snprintf(codec, sizeof(codec), "%s", dot + 1);
			for (char *c = codec; *c; c++) {
				*c = (char)toupper((unsigned char)*c);
			}
		}
	}

	int dsd = audio_get_dsd_multiple();
	int rate = 0;
	audio_get_stream_info(&rate, NULL);

	char rate_text[16] = "";
	if (dsd > 0) {
		snprintf(rate_text, sizeof(rate_text), "DSD%d", dsd);
	} else if (rate > 0) {
		char khz[12];
		rate_khz(khz, sizeof(khz), rate);
		snprintf(rate_text, sizeof(rate_text), "%s kHz", khz);
	}

	if (codec[0] && rate_text[0]) {
		snprintf(out, size, "%s \xC2\xB7 %s", codec, rate_text);
	} else if (codec[0]) {
		snprintf(out, size, "%s", codec);
	} else {
		snprintf(out, size, "%s", rate_text);
	}
	return out[0] != '\0';
}

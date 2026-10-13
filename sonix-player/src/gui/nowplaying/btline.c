#include "btline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/theme.h"
#include "src/system/audio/audio.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/core/config.h"

static lv_obj_t *line;
static int enabled = -1; // -1 until read from the config
// What the line says now, so a poll that finds nothing new writes nothing:
// a label set to its own text is still redrawn.
static char shown[96];

bool btline_enabled(void) {
	if (enabled < 0) {
		enabled = config_get_int("bluetooth", "nowplaying_details", 1) != 0;
	}
	return enabled != 0;
}

void btline_set_enabled(bool on) {
	enabled = on ? 1 : 0;
	config_set_int("bluetooth", "nowplaying_details", enabled);
	config_save();
	btline_refresh();
}

void btline_create(lv_obj_t *parent) {
	line = lv_label_create(parent);
	lv_label_set_text(line, "");
	lv_label_set_long_mode(line, LV_LABEL_LONG_DOT);
	lv_obj_set_width(line, lv_pct(100));
	lv_obj_add_style(line, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(line, &font_ui_18, 0);
	// Words to read, not a control: a press on it belongs to whatever the
	// player does with a press on the track's name, and a drag to the sheet.
	lv_obj_set_clickable(line, false);
	lv_obj_set_hidden(line, true);
	shown[0] = '\0';
}

// "44.1", "48", "88.2", "96": the rates everyone knows, written the way they
// are known, as bluetooth_codec_summary() writes the link's.
static void rate_text(char *out, size_t size, unsigned hz) {
	if (hz % 1000 == 0) {
		snprintf(out, size, "%u", hz / 1000);
	} else {
		snprintf(out, size, "%.1f", hz / 1000.0);
	}
}

// The link as the Bluetooth page words it, and the rate it runs at. On the
// host there is no Bluetooth: SONIX_BTLINE_FAKE="LDAC 990 · 48 kHz|48000"
// stands in for a pair of headphones, so the line can be looked at in the
// emulator.
static bool link_summary(char *out, int size, unsigned *rate) {
#ifdef HOST_BUILD
	const char *fake = getenv("SONIX_BTLINE_FAKE");
	if (fake && fake[0]) {
		const char *bar = strchr(fake, '|');
		snprintf(out, (size_t)size, "%.*s", bar ? (int)(bar - fake) : (int)strlen(fake), fake);
		*rate = bar ? (unsigned)strtoul(bar + 1, NULL, 10) : 0;
		return true;
	}
#endif
	if (!bluetooth_audio_active() || !bluetooth_codec_summary(NULL, out, size)) {
		return false;
	}
	*rate = bluetooth_sink_rate();
	return true;
}

void btline_refresh(void) {
	if (!line) {
		return;
	}
	char text[sizeof(shown)] = "";
	unsigned link = 0;
	if (btline_enabled() && link_summary(text, sizeof(text), &link)) {
		// Where the track does not go out at its own rate: from what to what,
		// whoever does it -- the player's own converter (decimation, the
		// polyphase 44.1 -> 48 kHz one) or, past that, alsa's. With the link's
		// rate not said yet, the rate the output was opened at stands in.
		int track = 0;
		audio_get_stream_info(&track, NULL);
		if (link == 0) {
			link = (unsigned)audio_get_device_rate();
		}
		if (track > 0 && link > 0 && (unsigned)track != link) {
			char from[16], to[16];
			int dsd = audio_get_dsd_multiple();
			if (dsd > 0) {
				snprintf(from, sizeof(from), "DSD%d", dsd);
			} else {
				rate_text(from, sizeof(from), (unsigned)track);
			}
			rate_text(to, sizeof(to), link);
			// The summary ends in " · <rate> kHz"; the conversion takes that
			// place rather than repeating the rate beside it.
			char *last = strstr(text, " \xC2\xB7 ");
			for (char *next = last; next; next = strstr(next + 1, " \xC2\xB7 ")) {
				last = next;
			}
			if (last && strstr(last, "kHz")) {
				*last = '\0';
			}
			size_t used = strlen(text);
			snprintf(text + used, sizeof(text) - used, " \xC2\xB7 %s \xE2\x86\x92 %s kHz", from, to);
		}
	}

	// Last in its column, under the artist. The player's arrangements take the
	// title and the artist out of the column and put them back, and a child put
	// back goes to the end -- past this line, which would then sit on top.
	if (text[0]) {
		lv_obj_t *column = lv_obj_get_parent(line);
		if (lv_obj_get_index(line) != (int32_t)lv_obj_get_child_count(column) - 1) {
			lv_obj_move_foreground(line);
		}
	}

	if (strcmp(text, shown) == 0) {
		return;
	}
	snprintf(shown, sizeof(shown), "%s", text);
	lv_label_set_text(line, text);
	lv_obj_set_hidden(line, text[0] == '\0');
}

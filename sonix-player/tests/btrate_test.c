// Desktop check of the Bluetooth link-rate policy (src/system/bluetooth/btrate.c).
//
//   cc -std=gnu11 -Wall -Wextra -I. tests/btrate_test.c src/system/bluetooth/btrate.c -o /tmp/btrate_test
//   /tmp/btrate_test
//
// With BLUEZ_ALSA_SHARED pointing at a bluez-alsa v4.3.1 checkout's src/shared
// directory, the LDAC blob is also compared with the struct bluealsa itself
// reads it into:
//
//   cc ... -DBLUEZ_ALSA_SHARED -I<bluez-alsa>/src tests/btrate_test.c ...
//
// Without it, the struct below -- copied from that header -- stands in.

#include <stdio.h>
#include <string.h>

#include "src/system/bluetooth/btrate.h"

#ifdef BLUEZ_ALSA_SHARED
#include "shared/a2dp-codecs.h"
#else
// bluez-alsa v4.3.1, src/shared/a2dp-codecs.h
#include <endian.h>
#define LDAC_SAMPLING_FREQ_44100 (1 << 5)
#define LDAC_SAMPLING_FREQ_48000 (1 << 4)
#define LDAC_SAMPLING_FREQ_88200 (1 << 3)
#define LDAC_SAMPLING_FREQ_96000 (1 << 2)
#define LDAC_SAMPLING_FREQ_176400 (1 << 1)
#define LDAC_SAMPLING_FREQ_192000 (1 << 0)
#define LDAC_CHANNEL_MODE_STEREO (1 << 0)
#define LDAC_VENDOR_ID 0x0000012D
#define LDAC_CODEC_ID 0x00AA
typedef struct a2dp_vendor_info {
	uint32_t vendor_id;
	uint16_t codec_id;
} __attribute__((packed)) a2dp_vendor_info_t;
typedef struct a2dp_ldac {
	a2dp_vendor_info_t info;
#if __BYTE_ORDER == __BIG_ENDIAN
	uint8_t rfa1 : 2;
	uint8_t sampling_freq : 6;
	uint8_t rfa2 : 5;
	uint8_t channel_mode : 3;
#else
	uint8_t sampling_freq : 6;
	uint8_t rfa1 : 2;
	uint8_t channel_mode : 3;
	uint8_t rfa2 : 5;
#endif
} __attribute__((packed)) a2dp_ldac_t;
#endif

static int failures;

static void expect(unsigned got, unsigned want, const char *what) {
	if (got != want) {
		printf("FAIL %s: got %u, want %u\n", what, got, want);
		failures++;
	} else {
		printf("ok   %s = %u\n", what, got);
	}
}

static void ldac_blob(unsigned rate, unsigned bit) {
	uint8_t blob[BTRATE_LDAC_CONFIG_SIZE];
	char what[64];
	snprintf(what, sizeof(what), "LDAC blob size at %u", rate);
	expect((unsigned)btrate_ldac_config(rate, blob), sizeof(a2dp_ldac_t), what);

	a2dp_ldac_t ref;
	memset(&ref, 0, sizeof(ref));
	ref.info.vendor_id = htole32(LDAC_VENDOR_ID);
	ref.info.codec_id = htole16(LDAC_CODEC_ID);
	ref.sampling_freq = bit;
	ref.channel_mode = LDAC_CHANNEL_MODE_STEREO;
	snprintf(what, sizeof(what), "LDAC blob bytes at %u match a2dp_ldac_t", rate);
	expect(memcmp(blob, &ref, sizeof(ref)) == 0, 1, what);

	// And read back the way bluealsa does after its memcpy.
	a2dp_ldac_t back;
	memcpy(&back, blob, sizeof(back));
	snprintf(what, sizeof(what), "LDAC blob at %u reads back as the frequency bit", rate);
	expect(back.sampling_freq, bit, what);
	expect(le32toh(back.info.vendor_id), LDAC_VENDOR_ID, "  vendor id");
	expect(le16toh(back.info.codec_id), LDAC_CODEC_ID, "  codec id");
}

int main(void) {
	// The Soundcore Space One Pro over LDAC: 44.1, 48, 88.2, 96.
	const unsigned space_one[] = {44100, 48000, 88200, 96000};
	const int n1 = 4;
	// A pair limited to the two base rates (AAC, SBC, most LDAC earbuds).
	const unsigned base_only[] = {44100, 48000};
	// Something odd: 48 kHz family only.
	const unsigned only_48[] = {48000, 96000};

	expect(btrate_follow(44100, false, space_one, n1), 44100, "follow 44.1");
	expect(btrate_follow(88200, false, space_one, n1), 88200, "follow 88.2");
	expect(btrate_follow(176400, false, space_one, n1), 88200, "follow 176.4");
	expect(btrate_follow(352800, false, space_one, n1), 88200, "follow 352.8");
	expect(btrate_follow(48000, false, space_one, n1), 48000, "follow 48");
	expect(btrate_follow(96000, false, space_one, n1), 96000, "follow 96");
	expect(btrate_follow(192000, false, space_one, n1), 96000, "follow 192");
	expect(btrate_follow(22050, false, space_one, n1), 44100, "follow 22.05");
	expect(btrate_follow(32000, false, space_one, n1), 48000, "follow 32");
	expect(btrate_follow(176400, true, space_one, n1), 88200, "follow DSD64 as PCM");
	expect(btrate_follow(192000, true, space_one, n1), 96000, "follow DSD 48 family as PCM");
	expect(btrate_follow(44100, true, space_one, n1), 88200, "follow DSD at 44.1 still hi-res");
	expect(btrate_follow(0, false, space_one, n1), 0, "follow, no track");

	expect(btrate_follow(176400, false, base_only, 2), 44100, "follow 176.4 without 88.2");
	expect(btrate_follow(96000, false, base_only, 2), 48000, "follow 96 without 96");
	expect(btrate_follow(44100, false, only_48, 2), 0, "follow 44.1 on a 48-only pair: leave it");
	expect(btrate_follow(44100, false, NULL, 0), 0, "follow with no rates known");

	expect(btrate_wanted(BT_RATE_96K, "LDAC", 44100, false, space_one, n1), 96000, "96 kHz on LDAC");
	expect(btrate_wanted(BT_RATE_96K, "ldac", 0, false, base_only, 2), 48000, "96 kHz on LDAC that tops out at 48");
	expect(btrate_wanted(BT_RATE_96K, "AAC", 44100, false, base_only, 2), 0, "96 kHz on AAC = auto");
	expect(btrate_wanted(BT_RATE_AUTO, "LDAC", 44100, false, space_one, n1), 0, "auto");
	expect(btrate_wanted(BT_RATE_44K, "LDAC", 48000, false, space_one, n1), 0, "44.1 (daemon option)");
	expect(btrate_wanted(BT_RATE_FOLLOW, "AAC", 44100, false, base_only, 2), 44100, "follow on AAC");
	expect(btrate_wanted(BT_RATE_FOLLOW, "LDAC", 192000, false, space_one, n1), 96000, "follow on LDAC, 192");

	ldac_blob(44100, LDAC_SAMPLING_FREQ_44100);
	ldac_blob(48000, LDAC_SAMPLING_FREQ_48000);
	ldac_blob(88200, LDAC_SAMPLING_FREQ_88200);
	ldac_blob(96000, LDAC_SAMPLING_FREQ_96000);
	ldac_blob(176400, LDAC_SAMPLING_FREQ_176400);
	ldac_blob(192000, LDAC_SAMPLING_FREQ_192000);
	uint8_t blob[BTRATE_LDAC_CONFIG_SIZE];
	expect((unsigned)btrate_ldac_config(32000, blob), 0, "no LDAC blob at 32 kHz");

	printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
	return failures ? 1 : 0;
}

// Desktop check of the Bluetooth log's btmon filter (src/system/bluetooth/btlog.c):
// which packets reach the file and which are counted as "data packets not
// shown".
//
//   cc -std=gnu11 -Wall -Wextra -I. tests/btlog_filter_test.c -lpthread -o /tmp/btlog_filter_test
//   /tmp/btlog_filter_test [bluetooth.log]
//
// With a bluetooth.log written by the player, every line of it is fed back
// through the filter as btmon output (the time stamps taken off) and the lines
// that would have been written again are counted: on a log from a build
// without the fix for media read as signalling, almost nothing should survive.

#include "src/system/bluetooth/btlog.c"

// What btlog.c needs from the rest of the player, none of which the filter
// touches.
bool config_get_bool(const char *section, const char *key, bool fallback) {
	(void)section;
	(void)key;
	return fallback;
}
void config_set_bool(const char *section, const char *key, bool value) {
	(void)section;
	(void)key;
	(void)value;
}
bool config_save(void) { return true; }
const char *tr(const char *key) { return key; }
void logging_set_line_tap(logging_line_tap_t tap) { (void)tap; }
void bluetooth_daemon_debug_on(void) {}
void thread_be_low_priority(const char *name) { (void)name; }

static int failures;

// What the filter let through so far, as text, and the ring emptied.
static void take_output(char *out, size_t size) {
	size_t n = ring_len < size - 1 ? ring_len : size - 1;
	for (size_t i = 0; i < n; i++) {
		out[i] = ring[(ring_head + i) % RING_SIZE];
	}
	out[n] = '\0';
	ring_head = 0;
	ring_len = 0;
}

static void feed_text(const char *text) { monitor_feed(text, strlen(text)); }

static void expect(bool ok, const char *what) {
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		failures++;
	}
}

static const char *SIGNALLING = "< ACL Data TX: Handle 12 flags 0x00 dlen 10                #50 [hci0] 12.000001\n"
								"      Channel: 65 len 6 [PSM 25 mode Basic (0x00)] {chan 0}\n"
								"      AVDTP: Start (0x07) Command (0x00) type 0x00 label 3 nosp 0\n"
								"        ACP SEID: 1\n";

static const char *MEDIA_AS_SIGNALLING =
	"< ACL Data TX: Handle 12 flags 0x02 dlen 677                #49948 [hci0] 224.706782\n"
	"      Channel: 41024 len 673 [PSM 25 mode Basic (0x00)] {chan 0}\n"
	"      AVDTP: Reserved (0x20) Command (0x00) type 0x00 label 8 nosp 0\n"
	"        10 76 6c 71 5e f9 00 00 00 00 02 aa 15 18 a0 a3  .vlq^...........\n"
	"        ef b8 26 83 2d 6e c5 33 34 aa ef 5d ef 7a ca c3  ..&.-n.34..].z..\n";

static const char *MEDIA_AS_BYTES = "< ACL Data TX: Handle 12 flags 0x02 dlen 677                #49949 [hci0] 224.712000\n"
									"      Channel: 41024 len 673 [PSM 0 mode Basic (0x00)] {chan 0}\n"
									"        80 60 6c 71 5e f9 00 00 00 00 02 aa 15 18 a0 a3  .`lq^...........\n";

static const char *LONG_DECODED = "> ACL Data RX: Handle 12 flags 0x02 dlen 120                #51 [hci0] 12.100000\n"
								  "      Channel: 64 len 116 [PSM 3 mode Basic (0x00)] {chan 1}\n"
								  "      RFCOMM: Unnumbered Info with Header Check (UIH) (0xef)\n";

static const char *EVENT = "> HCI Event: Command Complete (0x0e) plen 4                 #52 [hci0] 12.200000\n"
						   "      Write Scan Enable (0x03|0x001a) ncmd 1\n";

static void synthetic(void) {
	char out[8192];
	accepting = true;
	monitor_reset();

	feed_text(SIGNALLING);
	feed_text(EVENT);
	take_output(out, sizeof(out));
	expect(strstr(out, "AVDTP: Start") != NULL && strstr(out, "ACP SEID: 1") != NULL, "short signalling is kept");

	feed_text(MEDIA_AS_SIGNALLING);
	feed_text(MEDIA_AS_SIGNALLING);
	feed_text(EVENT);
	take_output(out, sizeof(out));
	expect(strstr(out, "Reserved") == NULL && strstr(out, "10 76 6c") == NULL, "media read as signalling is left out");
	expect(strstr(out, "(2 data packets not shown)") != NULL, "...and counted");
	expect(strstr(out, "Write Scan Enable") != NULL, "the next event still goes through");

	feed_text(MEDIA_AS_BYTES);
	feed_text(EVENT);
	take_output(out, sizeof(out));
	expect(strstr(out, "80 60 6c") == NULL && strstr(out, "(1 data packets not shown)") != NULL,
		   "media shown only as bytes is left out, as before");

	feed_text(LONG_DECODED);
	feed_text(EVENT);
	take_output(out, sizeof(out));
	expect(strstr(out, "RFCOMM: Unnumbered") != NULL && strstr(out, "Channel: 64 len 116") != NULL,
		   "a long packet btmon really decodes is kept whole");
}

// A bluetooth.log, its lines fed back as btmon output.
static void replay(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) {
		printf("FAIL cannot open %s\n", path);
		failures++;
		return;
	}
	accepting = true;
	monitor_reset();
	ring_head = ring_len = 0;

	char line[1024];
	unsigned long in_lines = 0, in_bytes = 0, out_bytes = 0, out_lines = 0;
	char out[RING_SIZE + 1];
	while (fgets(line, sizeof(line), f)) {
		// "09:59:01.779 " in front of what btmon printed.
		const char *text = line;
		if (strlen(line) > 13 && line[2] == ':' && line[5] == ':' && line[8] == '.' && line[12] == ' ') {
			text = line + 13;
		}
		in_lines++;
		in_bytes += strlen(text);
		feed_text(text);
		if (ring_len > RING_SIZE / 2) {
			take_output(out, sizeof(out));
			out_bytes += strlen(out);
			out_lines += count_lines(out, strlen(out));
		}
	}
	fclose(f);
	feed_text("> HCI Event: Command Complete (0x0e) plen 4\n");
	take_output(out, sizeof(out));
	out_bytes += strlen(out);
	out_lines += count_lines(out, strlen(out));
	printf("     %s: %lu lines (%lu bytes) in, %lu lines (%lu bytes) out\n", path, in_lines, in_bytes, out_lines,
		   out_bytes);
	printf("     what is left of it:\n%s", out);
	expect(out_bytes * 20 < in_bytes, "the replayed log shrinks to under a twentieth");
}

int main(int argc, char **argv) {
	synthetic();
	if (argc > 1) {
		replay(argv[1]);
	}
	printf("%s\n", failures ? "FAILED" : "all passed");
	return failures ? 1 : 0;
}

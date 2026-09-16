#ifndef SYSINFO_H
#define SYSINFO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// What the information page has to show: the two version numbers, and how much
// space is left on the card.

// ---------------------------------------------------------------------------
// versions
// ---------------------------------------------------------------------------
//
// Not compiled into the binary but read from /usr/resource/sonix/components/system-info.json, a
// text file next to the language directory:
//
//     {
//         "OS_version": "1.0",
//         "Build_version": "182"
//     }
//
// This way updating the build number does not mean rebuilding, and whoever
// assembles a firmware image can write it from the build script. A missing or
// unreadable file is not an error: both entries show a dash.

// Rereads the file. Called once at startup; the two functions below do not
// touch the disk.
void sysinfo_load(void);

// "" when the file is missing or does not carry the key.
const char *sysinfo_os_version(void);
const char *sysinfo_build_version(void);

// The device serial number: "R3PII" plus the first eight hex digits (upper
// case) of the SoC efuse chip id (/proc/jz/efuse/efuse_chip_id, line
// "CHIP_ID: <32 hex>"). It is exactly the number printed on the box, verified
// on a real device (chip id 90a70a42... -> box R3PII90A70A42). "" when the node
// is absent (the host build, or a firmware without the efuse module). Read once
// and kept: efuses do not change.
const char *sysinfo_serial_number(void);

// ---------------------------------------------------------------------------
// microSD card
// ---------------------------------------------------------------------------

typedef struct {
	bool present;	 // false when no card is mounted
	uint64_t total;	 // bytes
	uint64_t used;	 // bytes
	uint64_t free;	 // bytes available to this process, not counting reserved
	int used_percent; // 0..100, rounded
} sysinfo_storage_t;

// false when there is no card or it cannot be queried.
bool sysinfo_sd_usage(sysinfo_storage_t *out);

// "12.3 GB", "512 MB", "48 KB". Decimal point rather than comma: it is the same
// string in all five languages and does not pass through tr().
void sysinfo_format_size(uint64_t bytes, char *out, size_t out_size);

#endif /* SYSINFO_H */

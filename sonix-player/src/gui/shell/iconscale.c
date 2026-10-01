#include "iconscale.h"

#include "icons.h"
#include "src/system/core/respath.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// The file, as tools/svg_to_lvgl.py writes it, little-endian:
//
//   "SXIC", version, icon count, ICONS_LIST_HASH, num, den   (20 bytes)
//   per icon, in the order of icons_all[]: w, h, offset, size   (12 bytes)
//   the pixels, ARGB8888 like icons.c's, each icon at its offset
#define SET_MAGIC "SXIC"
#define SET_VERSION 1
#define HEAD_SIZE 20
#define ENTRY_SIZE 12

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

bool icons_load_scaled(int num, int den) {
	char path[256];
	snprintf(path, sizeof(path), SONIX_RESOURCE_DIR "/icons-%d-%d.bin", num, den);

	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "icons: no %s; the icons stay at the 480-wide design's size\n", path);
		return false;
	}
	struct stat st;
	if (fstat(fd, &st) != 0 || st.st_size < HEAD_SIZE) {
		fprintf(stderr, "icons: %s is too short\n", path);
		close(fd);
		return false;
	}
	size_t size = (size_t)st.st_size;

	// Mapped, not read: the pages are the kernel's to load as the icons are
	// drawn and to drop again, as they would be were the set in the binary.
	// Never unmapped, since the icons point into it from here on.
	const uint8_t *set = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (set == MAP_FAILED) {
		fprintf(stderr, "icons: cannot map %s\n", path);
		return false;
	}

	bool ok = memcmp(set, SET_MAGIC, 4) == 0 && get32(set + 4) == SET_VERSION && get32(set + 8) == ICONS_COUNT &&
			  get32(set + 12) == ICONS_LIST_HASH && get16(set + 16) == num && get16(set + 18) == den &&
			  size >= HEAD_SIZE + (size_t)ICONS_COUNT * ENTRY_SIZE;
	// Every icon inside the file and the size its width and height say,
	// before any of them is touched: a set is taken whole or not at all.
	for (size_t i = 0; ok && i < ICONS_COUNT; i++) {
		const uint8_t *e = set + HEAD_SIZE + i * ENTRY_SIZE;
		uint32_t w = get16(e), h = get16(e + 2), off = get32(e + 4), len = get32(e + 8);
		ok = w > 0 && h > 0 && len == w * h * 4 && off % 4 == 0 && off <= size && len <= size - off;
	}
	if (!ok) {
		fprintf(stderr, "icons: %s is not a set for this build; the icons stay at full size\n", path);
		munmap((void *)set, size);
		return false;
	}

	for (size_t i = 0; i < ICONS_COUNT; i++) {
		const uint8_t *e = set + HEAD_SIZE + i * ENTRY_SIZE;
		lv_image_dsc_t *d = icons_all[i];
		d->header.w = get16(e);
		d->header.h = get16(e + 2);
		d->header.stride = (uint32_t)get16(e) * 4;
		d->data_size = get32(e + 8);
		d->data = set + get32(e + 4);
	}
	printf("icons: drawn at %d/%d, from %s\n", num, den, path);
	return true;
}

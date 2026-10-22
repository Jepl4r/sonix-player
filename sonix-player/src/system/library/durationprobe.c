#include "durationprobe.h"
#include "src/system/decode/decode.h"
#include "src/system/decode/mp4.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define MP3_PREFIX_LIMIT (256 * 1024)

static uint32_t big32(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static double mp3_seconds(const char *path) {
	struct stat st;
	if (stat(path, &st) != 0 || st.st_size <= 0)
		return 0;
	FILE *file = fopen(path, "rb");
	if (!file)
		return 0;
	unsigned char *prefix = malloc(MP3_PREFIX_LIMIT);
	if (!prefix) {
		fclose(file);
		return 0;
	}
	size_t count = fread(prefix, 1, MP3_PREFIX_LIMIT, file);
	size_t start = 0;
	if (count >= 10 && !memcmp(prefix, "ID3", 3)) {
		if ((prefix[6] | prefix[7] | prefix[8] | prefix[9]) & 0x80)
			goto unknown;
		start = 10 + ((size_t)prefix[6] << 21 | (size_t)prefix[7] << 14 | (size_t)prefix[8] << 7 | prefix[9]);
		if (prefix[5] & 0x10)
			start += 10; // ID3v2 footer
	}
	for (size_t i = start; i + 4 <= count; i++) {
		uint32_t h = big32(prefix + i);
		int version = (h >> 19) & 3, layer = (h >> 17) & 3;
		int bit = (h >> 12) & 15, rate = (h >> 10) & 3;
		if ((h & 0xffe00000) != 0xffe00000 || version == 1 || layer != 1 || bit == 0 || bit == 15 || rate == 3)
			continue;
		static const int rates[] = {44100, 48000, 32000};
		static const int mpeg1[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
		static const int mpeg2[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
		int hz = rates[rate] / (version == 3 ? 1 : version == 2 ? 2 : 4);
		int bitrate = (version == 3 ? mpeg1[bit] : mpeg2[bit]) * 1000;
		int samples = version == 3 ? 1152 : 576;
		int frame_size = (version == 3 ? 144 : 72) * bitrate / hz + ((h >> 9) & 1);
		if (i + frame_size > count)
			continue;
		// A second compatible frame keeps tag bytes from being mistaken for audio.
		if (i + frame_size + 4 <= count) {
			uint32_t next = big32(prefix + i + frame_size);
			if ((next & 0xfffe0c00) != (h & 0xfffe0c00))
				continue;
		}
		bool mono = ((h >> 6) & 3) == 3;
		int side = version == 3 ? (mono ? 17 : 32) : (mono ? 9 : 17);
		size_t xing = i + 4 + ((h & 0x10000) ? 0 : 2) + side;
		double seconds = 0;
		if (xing + 12 <= count && (!memcmp(prefix + xing, "Xing", 4) || !memcmp(prefix + xing, "Info", 4)) && (big32(prefix + xing + 4) & 1)) {
			seconds = (double)big32(prefix + xing + 8) * samples / hz;
		}
		if (seconds <= 0) {
			off_t audio_size = st.st_size - (off_t)i;
			unsigned char tail[128];
			if (st.st_size >= 128 && fseeko(file, st.st_size - 128, SEEK_SET) == 0 && fread(tail, 1, 128, file) == 128 && !memcmp(tail, "TAG", 3))
				audio_size -= 128;
			seconds = audio_size > 0 ? (double)audio_size * 8 / bitrate : 0;
		}
		free(prefix);
		fclose(file);
		return seconds;
	}
unknown:
	free(prefix);
	fclose(file);
	return 0;
}

double durationprobe_seconds(const char *path) {
	if (!path || !path[0])
		return 0;
	const char *ext = strrchr(path, '.');
	if (!ext)
		return 0;
	if (!strcasecmp(ext, ".mp3"))
		return mp3_seconds(path);
	double seconds = 0;
	if (!strcasecmp(ext, ".mp4") || !strcasecmp(ext, ".m4a") || !strcasecmp(ext, ".m4b")) {
		mp4_file_t *mp4 = mp4_open(path);
		if (mp4) {
			seconds = mp4_duration_seconds(mp4);
			mp4_close(mp4);
		}
	} else if (!strcasecmp(ext, ".flac") || !strcasecmp(ext, ".opus") || !strcasecmp(ext, ".ogg")) {
		decoder_t *dec = decoder_open_info(path, decode_detect_format(path));
		if (dec) {
			int rate = decoder_sample_rate(dec);
			if (rate > 0)
				seconds = (double)decoder_total_pcm_frames(dec) / rate;
			decoder_close(dec);
		}
	}
	return isfinite(seconds) && seconds > 0 ? seconds : 0;
}

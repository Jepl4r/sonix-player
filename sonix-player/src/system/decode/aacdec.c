#include "aacdec.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The slice of libfdk-aac's interface used here, restated so nothing has to be
// installed to build. The rules this declaration set obeys:
//
//  * only the handle and the error code cross the boundary as opaque values;
//  * CStreamInfo is read through a prefix struct holding the three fields that
//    have been first in that structure since the library was released, so a
//    device carrying a different point release cannot shift them;
//  * every symbol is looked up by name and a missing one turns the whole
//    thing off rather than crashing.

typedef void *HANDLE_AACDECODER;
typedef int AAC_DECODER_ERROR;

#define AAC_DEC_OK 0x0000
#define AAC_DEC_NOT_ENOUGH_BITS 0x1002
#define TT_MP4_RAW 0
// Raw AAC with its ADTS headers in front, the way it arrives inside an HLS
// stream: the configuration travels in the stream itself, so there is no
// AudioSpecificConfig to pass and no container to read.
#define TT_MP4_ADTS 2

#define AAC_PCM_LIMITER_ENABLE 0x0004
#define AAC_PCM_MIN_OUTPUT_CHANNELS 0x0011
#define AAC_PCM_MAX_OUTPUT_CHANNELS 0x0012
#define AAC_CONCEAL_METHOD 0x0100
#define AAC_TPDEC_CLEAR_BUFFER 0x0603

// The head of CStreamInfo. Everything past these three is decoder internals, so
// the tail of the real structure is not described.
typedef struct {
	int sample_rate;
	int frame_size;
	int channels;
} stream_info_head_t;

struct fdk_api {
	HANDLE_AACDECODER (*open)(int transport, unsigned int layers);
	void (*close)(HANDLE_AACDECODER);
	AAC_DECODER_ERROR (*config_raw)(HANDLE_AACDECODER, unsigned char *conf[], const unsigned int length[]);
	AAC_DECODER_ERROR (*fill)(HANDLE_AACDECODER, unsigned char *buf[], const unsigned int size[], unsigned int *valid);
	AAC_DECODER_ERROR (*decode)(HANDLE_AACDECODER, short *pcm, const int pcm_size, const unsigned int flags);
	AAC_DECODER_ERROR (*set_param)(HANDLE_AACDECODER, const int param, const int value);
	stream_info_head_t *(*stream_info)(HANDLE_AACDECODER);
};

static struct fdk_api api;
static void *lib_handle;
static bool load_tried;
static bool load_ok;
static pthread_mutex_t load_lock = PTHREAD_MUTEX_INITIALIZER;
static char last_error[160];

struct aacdec {
	HANDLE_AACDECODER handle;
	int channels;
	int sample_rate;
	int frame_size;
};

static bool bind_symbol(void **slot, const char *name) {
	*slot = dlsym(lib_handle, name);
	if (!*slot) {
		snprintf(last_error, sizeof(last_error), "libfdk-aac does not export %s", name);
		return false;
	}
	return true;
}

bool aacdec_available(void) {
	pthread_mutex_lock(&load_lock);
	if (load_tried) {
		pthread_mutex_unlock(&load_lock);
		return load_ok;
	}
	load_tried = true;

	// The device carries .so.2; the other two are for a build machine.
	static const char *const NAMES[] = {"libfdk-aac.so.2", "libfdk-aac.so.1", "libfdk-aac.so"};
	for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]) && !lib_handle; i++) {
		lib_handle = dlopen(NAMES[i], RTLD_NOW | RTLD_LOCAL);
	}
	if (!lib_handle) {
		snprintf(last_error, sizeof(last_error), "libfdk-aac not found on the device");
		pthread_mutex_unlock(&load_lock);
		return false;
	}

	load_ok = bind_symbol((void **)&api.open, "aacDecoder_Open") && bind_symbol((void **)&api.close, "aacDecoder_Close") &&
			  bind_symbol((void **)&api.config_raw, "aacDecoder_ConfigRaw") &&
			  bind_symbol((void **)&api.fill, "aacDecoder_Fill") &&
			  bind_symbol((void **)&api.decode, "aacDecoder_DecodeFrame") &&
			  bind_symbol((void **)&api.set_param, "aacDecoder_SetParam") &&
			  bind_symbol((void **)&api.stream_info, "aacDecoder_GetStreamInfo");

	if (!load_ok) {
		dlclose(lib_handle);
		lib_handle = NULL;
	} else {
		printf("aac: libfdk-aac loaded\n");
	}
	pthread_mutex_unlock(&load_lock);
	return load_ok;
}

const char *aacdec_last_error(void) { return last_error; }

aacdec_t *aacdec_open(const unsigned char *asc, int asc_len) {
	if (!aacdec_available() || !asc || asc_len <= 0) {
		return NULL;
	}

	aacdec_t *d = calloc(1, sizeof(*d));
	if (!d) {
		return NULL;
	}

	// Raw access units: the MP4 hands over whole frames with no sync word, and
	// the configuration comes from the container rather than the stream.
	d->handle = api.open(TT_MP4_RAW, 1);
	if (!d->handle) {
		snprintf(last_error, sizeof(last_error), "the AAC decoder did not open");
		free(d);
		return NULL;
	}

	unsigned char config[64];
	if (asc_len > (int)sizeof(config)) {
		asc_len = (int)sizeof(config);
	}
	memcpy(config, asc, (size_t)asc_len);
	unsigned char *conf[1] = {config};
	unsigned int conf_len[1] = {(unsigned int)asc_len};

	if (api.config_raw(d->handle, conf, conf_len) != AAC_DEC_OK) {
		snprintf(last_error, sizeof(last_error), "AAC configuration refused by the decoder");
		api.close(d->handle);
		free(d);
		return NULL;
	}

	// A book is spoken word through headphones: the limiter is meant for
	// broadcast material and only costs headroom here, while concealment keeps
	// a damaged frame from becoming a click.
	api.set_param(d->handle, AAC_PCM_LIMITER_ENABLE, 0);
	api.set_param(d->handle, AAC_CONCEAL_METHOD, 1);
	// Leave the channel count alone: a mono book must stay mono, so that
	// audio.c opens a mono stream instead of doubling every sample.
	api.set_param(d->handle, AAC_PCM_MIN_OUTPUT_CHANNELS, -1);
	api.set_param(d->handle, AAC_PCM_MAX_OUTPUT_CHANNELS, -1);

	return d;
}

aacdec_t *aacdec_open_adts(void) {
	if (!aacdec_available()) {
		return NULL;
	}

	aacdec_t *d = calloc(1, sizeof(*d));
	if (!d) {
		return NULL;
	}

	// No aacDecoder_ConfigRaw, by design: in ADTS the sample rate, channels and
	// profile are written in every header, so the decoder configures itself from
	// the first frame. Passing a configuration here would mean guessing one, and
	// on a radio station it can change between programmes.
	d->handle = api.open(TT_MP4_ADTS, 1);
	if (!d->handle) {
		snprintf(last_error, sizeof(last_error), "the AAC decoder did not open");
		free(d);
		return NULL;
	}

	// Limiter on, unlike the audiobook case: radio sends material that is
	// already compressed and pushed hard, and the peaks it produces are exactly
	// what that limiter exists for.
	api.set_param(d->handle, AAC_PCM_LIMITER_ENABLE, 1);
	api.set_param(d->handle, AAC_CONCEAL_METHOD, 1);
	api.set_param(d->handle, AAC_PCM_MIN_OUTPUT_CHANNELS, -1);
	api.set_param(d->handle, AAC_PCM_MAX_OUTPUT_CHANNELS, -1);
	return d;
}

int aacdec_fill(aacdec_t *d, const unsigned char *data, int len) {
	if (!d || !d->handle || !data || len <= 0) {
		return 0;
	}
	unsigned char *buf[1] = {(unsigned char *)data};
	unsigned int size[1] = {(unsigned int)len};
	unsigned int valid = (unsigned int)len;
	if (api.fill(d->handle, buf, size, &valid) != AAC_DEC_OK) {
		return -1;
	}
	// `valid` comes back holding what was not taken: the difference is what the
	// decoder kept, and that is what the caller must consume.
	return len - (int)valid;
}

int aacdec_pull(aacdec_t *d, short *out, int out_samples) {
	if (!d || !d->handle || !out || out_samples <= 0) {
		return -1;
	}
	AAC_DECODER_ERROR err = api.decode(d->handle, out, out_samples, 0);
	if (err == AAC_DEC_NOT_ENOUGH_BITS) {
		return 0; // it needs more input
	}
	if (err != AAC_DEC_OK) {
		return 0; // a damaged frame is not a reason to stop
	}

	stream_info_head_t *info = api.stream_info(d->handle);
	if (!info || info->channels <= 0 || info->frame_size <= 0) {
		return 0;
	}
	d->channels = info->channels;
	d->sample_rate = info->sample_rate;
	d->frame_size = info->frame_size;

	int frames = info->frame_size;
	if (frames * info->channels > out_samples) {
		frames = out_samples / info->channels;
	}
	return frames;
}

void aacdec_close(aacdec_t *d) {
	if (!d) {
		return;
	}
	if (d->handle) {
		api.close(d->handle);
	}
	free(d);
}

int aacdec_decode(aacdec_t *d, const unsigned char *au, int au_len, short *out, int out_samples) {
	if (!d || !d->handle || !out || out_samples <= 0) {
		return -1;
	}

	if (au && au_len > 0) {
		unsigned char *buf[1] = {(unsigned char *)au};
		unsigned int size[1] = {(unsigned int)au_len};
		unsigned int valid = (unsigned int)au_len;
		if (api.fill(d->handle, buf, size, &valid) != AAC_DEC_OK) {
			return -1;
		}
	}

	AAC_DECODER_ERROR err = api.decode(d->handle, out, out_samples, 0);
	if (err == AAC_DEC_NOT_ENOUGH_BITS) {
		return 0; // normal: the decoder is still filling up
	}
	if (err != AAC_DEC_OK) {
		// A concealed or dropped frame is not a reason to stop a ten-hour
		// book; only a missing stream is.
		return 0;
	}

	stream_info_head_t *info = api.stream_info(d->handle);
	if (!info || info->channels <= 0 || info->frame_size <= 0) {
		return 0;
	}
	d->channels = info->channels;
	d->sample_rate = info->sample_rate;
	d->frame_size = info->frame_size;

	int frames = info->frame_size;
	if (frames * info->channels > out_samples) {
		frames = out_samples / info->channels;
	}
	return frames;
}

void aacdec_flush(aacdec_t *d) {
	if (d && d->handle) {
		api.set_param(d->handle, AAC_TPDEC_CLEAR_BUFFER, 1);
	}
}

int aacdec_channels(const aacdec_t *d) { return d ? d->channels : 0; }
int aacdec_sample_rate(const aacdec_t *d) { return d ? d->sample_rate : 0; }
int aacdec_frame_size(const aacdec_t *d) { return d ? d->frame_size : 0; }

// Host test for src/system/audio/resample.c (44.1 -> 48 kHz polyphase).
//
//   gcc -O2 -I. -o /tmp/resample_test tools/resample_test.c src/system/audio/resample.c -lm && /tmp/resample_test
//
// For a set of tones, converts four seconds of a full-scale-minus-6-dB sine,
// fits the expected tone at the output rate by least squares and reports:
//   gain   the level of the tone that came out (passband flatness)
//   resid  everything else, relative to the tone: images, aliases, rounding
//   <20k   the part of that residue below 20 kHz, i.e. what can be heard (an
//          image of an 18 kHz tone folds to 21.9 kHz by design: see resample.h)
// and the CPU time per second of stereo audio. Fails (exit 1) if the passband
// up to 20 kHz is off by more than 0.25 dB or the audible residue of any tone up to
// 20 kHz is above -80 dB.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/system/audio/resample.h"

// resample.c reads its setting through config.h; the test has no config.
const char *config_get(const char *section, const char *key, const char *fallback) {
	(void)section;
	(void)key;
	return fallback;
}

#define IN_RATE 44100
#define OUT_RATE 48000
#define SECONDS 4

static double now_s(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(void) {
	static const double freqs[] = {100, 1000, 5000, 10000, 15000, 18000, 19000, 20000, 21000, 22000};
	int in_frames = IN_RATE * SECONDS;
	int32_t *in = malloc(sizeof(int32_t) * 2 * in_frames);
	int32_t *out = malloc(sizeof(int32_t) * 2 * resample_out_max(in_frames));
	int failures = 0;
	double total_cpu = 0;

	printf("%8s %10s %10s %10s\n", "Hz", "gain dB", "resid dB", "<20k dB");
	for (size_t f = 0; f < sizeof(freqs) / sizeof(freqs[0]); f++) {
		double hz = freqs[f];
		for (int i = 0; i < in_frames; i++) {
			double v = 0.5 * sin(2 * M_PI * hz * i / IN_RATE);
			int32_t s = (int32_t)lrint(v * 8388607.0) * 256;
			in[2 * i] = s;
			in[2 * i + 1] = -s;
		}
		resampler_t r;
		if (resample_init(&r, 2) != 0) {
			return 2;
		}
		// In blocks of an odd size, so the state carried between calls is
		// exercised the way a playback loop would.
		double t0 = now_s();
		int made = 0;
		for (int i = 0; i < in_frames; i += 1001) {
			int n = in_frames - i < 1001 ? in_frames - i : 1001;
			made += resample_s32(&r, in + 2 * i, n, out + 2 * made);
		}
		total_cpu += now_s() - t0;

		// Least squares for a*sin + b*cos at hz, over the middle of the output
		// (the edges carry the filter's start-up), left channel; the right is
		// checked to be its negative.
		int from = OUT_RATE / 4, to = made - OUT_RATE / 4;
		double ss = 0, sc = 0, cc = 0, ys = 0, yc = 0;
		for (int n = from; n < to; n++) {
			double y = out[2 * n] / 256.0 / 8388607.0;
			double s = sin(2 * M_PI * hz * n / OUT_RATE), c = cos(2 * M_PI * hz * n / OUT_RATE);
			ss += s * s;
			sc += s * c;
			cc += c * c;
			ys += y * s;
			yc += y * c;
			if (out[2 * n] != -out[2 * n + 1] && abs(out[2 * n] + out[2 * n + 1]) > 512) {
				printf("channels differ at %d\n", n);
				return 3;
			}
		}
		double det = ss * cc - sc * sc;
		double a = (ys * cc - yc * sc) / det, b = (yc * ss - ys * sc) / det;
		double amp = sqrt(a * a + b * b);
		double resid = 0, sig = 0, audible = 0;
		static double e[OUT_RATE * SECONDS + 16];
		for (int n = from; n < to; n++) {
			double y = out[2 * n] / 256.0 / 8388607.0;
			double fit = a * sin(2 * M_PI * hz * n / OUT_RATE) + b * cos(2 * M_PI * hz * n / OUT_RATE);
			e[n - from] = y - fit;
			resid += (y - fit) * (y - fit);
			sig += fit * fit;
		}
		// What of the residue lies below 20 kHz -- what can be heard: the error
		// through a steep 19.5 kHz lowpass (1023-tap Kaiser windowed sinc).
		enum { LP = 1023 };
		static double lp[LP];
		double wc = 2.0 * 19500.0 / OUT_RATE;
		for (int k = 0; k < LP; k++) {
			double x = k - (LP - 1) / 2.0, rr = x / ((LP - 1) / 2.0);
			double sinc = x == 0 ? 1.0 : sin(M_PI * wc * x) / (M_PI * wc * x);
			double w = 0, t = 1, arg = 10.0 * sqrt(1 - rr * rr), w0 = 0, t0 = 1;
			for (int j = 1; j < 30; j++) {
				t *= (arg / (2.0 * j)) * (arg / (2.0 * j));
				w += t;
				t0 *= (10.0 / (2.0 * j)) * (10.0 / (2.0 * j));
				w0 += t0;
			}
			lp[k] = wc * sinc * (1 + w) / (1 + w0);
		}
		int len = to - from;
		for (int n = LP; n < len; n++) {
			double v = 0;
			for (int k = 0; k < LP; k++) {
				v += lp[k] * e[n - k];
			}
			audible += v * v;
		}
		audible *= (double)len / (len - LP);
		double gain_db = 20 * log10(amp / 0.5);
		double resid_db = 10 * log10(resid / sig);
		double audible_db = 10 * log10(audible / sig + 1e-30);
		printf("%8.0f %10.3f %10.1f %10.1f\n", hz, gain_db, resid_db, audible_db);
		if (hz <= 20000 && (fabs(gain_db) > 0.25 || audible_db > -80)) {
			failures++;
		}
		if (made < in_frames * (double)OUT_RATE / IN_RATE - 2 || made > resample_out_max(in_frames)) {
			printf("frame count wrong: %d\n", made);
			failures++;
		}
	}
	int tones = (int)(sizeof(freqs) / sizeof(freqs[0]));
	printf("CPU: %.2f ms per second of stereo audio on this host\n", 1000.0 * total_cpu / (tones * SECONDS));
	resample_free();
	free(in);
	free(out);
	printf("%s\n", failures ? "FAIL" : "PASS");
	return failures ? 1 : 0;
}

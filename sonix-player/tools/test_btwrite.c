// Il percorso di scrittura VERO del player contro un bluealsa vero.
//
// tools/extract_write_path.py tira fuori da src/system/audio.c le tre funzioni
// che scrivono -- pcm_is_bluetooth, pcm_write_bluetooth, pcm_write_recover --
// e le mette in write_path.inc. Qui sotto vengono compilate quelle, non una
// copia: se audio.c cambia e questo banco non compila piu', ha ragione il banco.
//
// Che cosa si misura. Il player scrive un periodo alla volta e non piu' in
// fretta di quanto il decoder produca: `--pacer` fa esattamente questo, cioe'
// un periodo di audio ogni periodo di tempo reale, che e' il caso peggiore
// realistico (decoder che non ha margine). Poi si guarda il delay, cioe' quanto
// audio c'e' in coda: un flusso sano lo tiene alto e stabile, uno che scivola
// lo vede scendere fino all'underrun, e allora si sente a scatti.
//
// Uso: test_btwrite <nome-pcm> [--seek-at S] [--pausa-at S] [--secondi N] [--pacer]

#include <alsa/asoundlib.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static long log_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

// I finti che il percorso di scrittura chiama e che qui non devono mai servire.
static bool abort_requested;
static bool playback_reader_should_abort(void) { return abort_requested; }
static int reopen_count;
static void auto_set_output(void) {}
static snd_pcm_t *open_pcm_device(int channels, int rate, int bits, snd_pcm_uframes_t *period) {
	(void)channels;
	(void)rate;
	(void)bits;
	(void)period;
	reopen_count++;
	return NULL; // una riapertura qui e' gia' un guasto: il banco lo dice
}

#include "write_path.inc"

// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IOLBF, 0);

	const char *name = argc > 1 ? argv[1] : "bluealsa";
	int seek_at = -1, pause_at = -1, seconds = 12;
	bool pacer = false;
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--seek-at") == 0 && i + 1 < argc) {
			seek_at = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--pausa-at") == 0 && i + 1 < argc) {
			pause_at = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--secondi") == 0 && i + 1 < argc) {
			seconds = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--pacer") == 0) {
			pacer = true;
		}
	}

	snd_pcm_t *pcm = NULL;
	int err = snd_pcm_open(&pcm, name, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
	if (err < 0) {
		printf("apertura di '%s' fallita: %s\n", name, snd_strerror(err));
		return 1;
	}

	const unsigned want_rate = 44100;
	const unsigned channels = 2;

	snd_pcm_hw_params_t *hw;
	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(pcm, hw);
	snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
	snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
	snd_pcm_hw_params_set_channels(pcm, hw, channels);
	unsigned r = want_rate;
	int dir = 0;
	snd_pcm_hw_params_set_rate_near(pcm, hw, &r, &dir);
	unsigned periods = 8;
	snd_pcm_uframes_t want_period = 4096;
	snd_pcm_hw_params_set_periods_near(pcm, hw, &periods, &dir);
	snd_pcm_hw_params_set_period_size_near(pcm, hw, &want_period, &dir);
	if ((err = snd_pcm_hw_params(pcm, hw)) < 0) {
		printf("hw_params: %s\n", snd_strerror(err));
		return 1;
	}

	snd_pcm_uframes_t period = 0, buffer = 0;
	snd_pcm_hw_params_get_period_size(hw, &period, &dir);
	snd_pcm_hw_params_get_buffer_size(hw, &buffer);

	snd_pcm_sw_params_t *sw;
	snd_pcm_sw_params_alloca(&sw);
	snd_pcm_sw_params_current(pcm, sw);
	snd_pcm_sw_params_set_avail_min(pcm, sw, period);
	snd_pcm_sw_params_set_start_threshold(pcm, sw, buffer);
	snd_pcm_sw_params(pcm, sw);

	printf("pcm '%s': %u Hz, periodo %lu, buffer %lu (%.0f ms), scrittore %s\n", name, r, (unsigned long)period,
		   (unsigned long)buffer, 1000.0 * buffer / r, pacer ? "al passo del tempo reale" : "libero");

	const int frame_bytes = (int)channels * 2;
	short *block = calloc(period * channels, sizeof(short));
	for (snd_pcm_uframes_t i = 0; i < period; i++) {
		short v = (short)(8000.0 * sin(2.0 * 3.14159265 * 440.0 * (double)i / r));
		block[i * 2] = v;
		block[i * 2 + 1] = v;
	}

	const long period_ms = (long)(1000.0 * period / r);
	long began = log_ms();
	long next_period_due = began;
	long second_mark = began;
	long frames_second = 0;
	long min_delay = -1;
	int underruns = 0;
	bool did_seek = false, did_pause = false;

	while ((log_ms() - began) / 1000 < seconds) {
		long now = log_ms();

		if (seek_at >= 0 && !did_seek && now - began >= seek_at * 1000L) {
			did_seek = true;
			printf("  --- seek: drop + prepare di fila, come audio.c ---\n");
			snd_pcm_drop(pcm);
			snd_pcm_prepare(pcm);
			next_period_due = log_ms();
		}
		if (pause_at >= 0 && !did_pause && now - began >= pause_at * 1000L) {
			did_pause = true;
			printf("  --- pausa: drop, un secondo fermo, prepare ---\n");
			snd_pcm_drop(pcm);
			struct timespec one = {.tv_sec = 1, .tv_nsec = 0};
			nanosleep(&one, NULL);
			snd_pcm_prepare(pcm);
			next_period_due = log_ms();
			second_mark = log_ms();
		}

		// Il decoder non produce piu' in fretta del tempo reale.
		if (pacer) {
			long wait = next_period_due - log_ms();
			if (wait > 0) {
				struct timespec ts = {.tv_sec = wait / 1000, .tv_nsec = (wait % 1000) * 1000000L};
				nanosleep(&ts, NULL);
			}
			next_period_due += period_ms;
		}

		snd_pcm_sframes_t written = pcm_write_recover(&pcm, block, period, (int)channels, (int)r, 16, &period);
		if (!pcm || written < 0) {
			printf("  scrittura fallita: %s (riaperture %d)\n", snd_strerror((int)written), reopen_count);
			break;
		}
		frames_second += written;

		long t = log_ms();
		if (t - second_mark >= 1000) {
			snd_pcm_sframes_t delay = 0;
			snd_pcm_delay(pcm, &delay);
			if (min_delay < 0 || delay < min_delay) {
				min_delay = delay;
			}
			printf("  t=%2lds  frame=%6ld (%3.0f%%)  coda=%5ld frame (%4.0f ms)  stato=%s\n", (t - began) / 1000,
				   frames_second, 100.0 * frames_second / r, (long)delay, 1000.0 * delay / r,
				   snd_pcm_state_name(snd_pcm_state(pcm)));
			second_mark = t;
			frames_second = 0;
		}
	}

	printf("  coda minima vista: %ld frame (%.0f ms)   underrun: %d   riaperture: %d\n", min_delay,
		   1000.0 * min_delay / r, underruns, reopen_count);
	if (pcm) {
		snd_pcm_close(pcm);
	}
	free(block);
	return 0;
}

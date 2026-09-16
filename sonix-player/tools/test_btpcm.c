// Che cosa succede a un PCM bluealsa quando il player fa una seek.
//
// La seek in audio.c e' snd_pcm_drop() seguita subito da snd_pcm_prepare(), le
// stesse due chiamate della pausa e della ripresa -- solo senza il tempo in
// mezzo. Sul dispositivo, dopo una seek l'audio Bluetooth va a scatti finche'
// non si mette in pausa e si riprende.
//
// Qui la sequenza viene rifatta identica contro un server bluealsa vero
// (test/mock/bluealsa-mock del suo albero, che pubblica un A2DP finto senza
// nessun hardware Bluetooth) e attraverso il plugin vero. I parametri di
// apertura sono quelli di open_pcm_device(): non bloccante, avail_min di un
// periodo, soglia di avvio pari all'intero buffer.
//
// Stampa, secondo per secondo: frame accettati, EAGAIN, EPIPE e il delay. Un
// flusso sano accetta circa `rate` frame al secondo con delay stabile; uno che
// va a scatti si vede come EPIPE che tornano o come delay che crolla a zero.
//
// --stallo-seek MS simula quello che il dispositivo fa e questo banco no: dopo
// una seek il decoder deve riposizionarsi sulla scheda e ridecodificare un
// buffer intero, e su un MIPS lento sono secondi in cui non arriva un frame.
//
// Uso: test_btpcm <nome-pcm> [--seek-at SECONDI] [--secondi N] [--stallo-seek MS]

#include <alsa/asoundlib.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(int argc, char **argv) {
	const char *name = argc > 1 ? argv[1] : "bluealsa";
	int seek_at = -1;
	int seconds = 8;
	int stall_ms = 0;
	unsigned want_rate = 44100;
	bool want_s32 = false;
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--seek-at") == 0 && i + 1 < argc) {
			seek_at = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--secondi") == 0 && i + 1 < argc) {
			seconds = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--stallo-seek") == 0 && i + 1 < argc) {
			stall_ms = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
			want_rate = (unsigned)atoi(argv[++i]);
		} else if (strcmp(argv[i], "--s32") == 0) {
			want_s32 = true;
		}
	}

	setvbuf(stdout, NULL, _IOLBF, 0);

	snd_pcm_t *pcm = NULL;
	int err = snd_pcm_open(&pcm, name, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
	if (err < 0) {
		printf("apertura di '%s' fallita: %s\n", name, snd_strerror(err));
		return 1;
	}

	const unsigned rate = want_rate;
	const unsigned channels = 2;

	snd_pcm_hw_params_t *hw;
	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(pcm, hw);
	snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
	// Con un formato o un rate diversi da quelli del sink, fra il player e il
	// plugin si mette il convertitore di `plug`: e' la catena vera del
	// dispositivo, che apre il PCM al rate del file.
	snd_pcm_hw_params_set_format(pcm, hw, want_s32 ? SND_PCM_FORMAT_S32_LE : SND_PCM_FORMAT_S16_LE);
	snd_pcm_hw_params_set_channels(pcm, hw, channels);
	unsigned r = rate;
	int dir = 0;
	snd_pcm_hw_params_set_rate_near(pcm, hw, &r, &dir);
	// La stessa forma di buffer di open_pcm_device(): otto periodi da 4096
	// frame, cioe' circa 750 ms a 44,1 kHz.
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
	printf("pcm '%s': %u Hz, periodo %lu frame, buffer %lu frame (%.0f ms)\n", name, r, (unsigned long)period,
		   (unsigned long)buffer, 1000.0 * buffer / r);

	// Gli stessi sw params del player.
	snd_pcm_sw_params_t *sw;
	snd_pcm_sw_params_alloca(&sw);
	snd_pcm_sw_params_current(pcm, sw);
	snd_pcm_sw_params_set_avail_min(pcm, sw, period);
	snd_pcm_sw_params_set_start_threshold(pcm, sw, buffer);
	snd_pcm_sw_params(pcm, sw);

	size_t sample_bytes = want_s32 ? 4 : 2;
	void *block = calloc(period * channels, sample_bytes);
	if (!block) {
		return 1;
	}
	// Un tono, non silenzio: un encoder puo' trattare il silenzio diversamente.
	for (snd_pcm_uframes_t i = 0; i < period; i++) {
		double s = sin(2.0 * 3.14159265 * 440.0 * (double)i / r);
		if (want_s32) {
			int32_t v = (int32_t)(s * 500000000.0);
			((int32_t *)block)[i * 2] = v;
			((int32_t *)block)[i * 2 + 1] = v;
		} else {
			short v = (short)(s * 8000.0);
			((short *)block)[i * 2] = v;
			((short *)block)[i * 2 + 1] = v;
		}
	}

	long began = now_ms();
	long second_mark = began;
	long frames_this_second = 0, eagain = 0, epipe = 0, other = 0;
	bool sought = false;
	int worst_gap_ms = 0;
	long last_write_ms = began;

	while ((now_ms() - began) / 1000 < seconds) {
		if (seek_at >= 0 && !sought && (now_ms() - began) >= seek_at * 1000L) {
			sought = true;
			printf("  --- seek: snd_pcm_drop + snd_pcm_prepare, esattamente come audio.c ---\n");
			snd_pcm_drop(pcm);
			snd_pcm_prepare(pcm);
			if (stall_ms > 0) {
				printf("  --- il decoder ci mette %d ms a ridare frame ---\n", stall_ms);
				struct timespec ts = {.tv_sec = stall_ms / 1000, .tv_nsec = (long)(stall_ms % 1000) * 1000000L};
				nanosleep(&ts, NULL);
				second_mark = now_ms();
				last_write_ms = now_ms();
			}
		}

		snd_pcm_sframes_t n = snd_pcm_writei(pcm, block, period);
		long t = now_ms();
		if (n >= 0) {
			frames_this_second += n;
			int gap = (int)(t - last_write_ms);
			// Un buco fra due scritture accettate piu' lungo del buffer non e'
			// attesa: e' il flusso che si e' fermato.
			if (gap > worst_gap_ms) {
				worst_gap_ms = gap;
			}
			last_write_ms = t;
		} else if (n == -EAGAIN) {
			eagain++;
			snd_pcm_wait(pcm, 50);
		} else if (n == -EPIPE) {
			epipe++;
			snd_pcm_prepare(pcm);
		} else {
			other++;
			printf("  errore di scrittura: %s\n", snd_strerror((int)n));
			break;
		}

		if (t - second_mark >= 1000) {
			snd_pcm_sframes_t delay = 0;
			snd_pcm_delay(pcm, &delay);
			printf("  t=%lds  frame=%6ld (%3.0f%% del tempo reale)  EAGAIN=%-5ld EPIPE=%-3ld delay=%5ld  stato=%s\n",
				   (t - began) / 1000, frames_this_second, 100.0 * frames_this_second / r, eagain, epipe, (long)delay,
				   snd_pcm_state_name(snd_pcm_state(pcm)));
			second_mark = t;
			frames_this_second = 0;
			eagain = 0;
			epipe = 0;
		}
	}

	printf("  buco piu' lungo fra due scritture accettate: %d ms\n", worst_gap_ms);
	snd_pcm_close(pcm);
	free(block);
	return other ? 1 : 0;
}

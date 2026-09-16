// Il cambio traccia sulle cuffie Bluetooth.
//
// Segnalazione: la prima traccia si sente, dalla seconda in poi silenzio, con
// le cuffie ancora collegate.
//
// Su Bluetooth il gapless e' spento apposta (round 118), quindi il confine fra
// due tracce e' esattamente questo:
//
//     fine traccia   snd_pcm_start() se PREPARED
//                    pcm_drain_bounded()        <- estratta da audio.c
//                    snd_pcm_close()
//     traccia dopo   snd_pcm_open() + hw/sw params come open_pcm_device()
//                    pcm_write_bluetooth()      <- estratta da audio.c
//
// Qui la sequenza viene rifatta identica, tre tracce di fila, contro un server
// bluealsa vero (il bluealsa-mock del suo albero, che pubblica un A2DP finto
// senza nessun hardware) e attraverso il plugin ALSA vero.
//
// Quello che si guarda non e' "l'apertura e' riuscita" -- riesce anche quando
// il trasporto e' morto, ed e' proprio questo il guasto descritto in audio.c:
// le scritture vengono accettate dal plugin e finiscono nel nulla. Si guarda
// se il flusso **si consuma**: un PCM vivo tiene la coda intorno al buffer e
// accetta circa `rate` frame al secondo, uno morto riempie la coda una volta e
// poi non prende piu' niente.
//
// --pausa MS mette fra una traccia e l'altra il tempo che sul dispositivo ci
// vuole per trovare il file dopo, aprirlo e decodificarne il primo buffer dalla
// microSD. In quella finestra il lettore non e' piu' un client di bluealsa, e
// bluealsa e' libero di rilasciare il trasporto A2DP.
//
// --rumore idle|nice10 accende accanto al lettore un thread che consuma core,
// con la stessa politica che il worker delle copertine usa. E' la domanda del
// round 232 messa in numeri: il lettore ha UN core, e li' un thread di fondo non
// e' gratis per un flusso A2DP, che non ha nessun clock dietro a proteggerlo.
// Va fatto girare con tutto pinnato sullo stesso core (lo fa lo script), o non
// c'e' nessuna contesa da misurare.
//
// Uso: tools/run_bttrack_bench.sh <cartella-build-di-bluez-alsa>

#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static long log_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

// Nel lettore dice se e' arrivato un comando (stop, next, una seek). Qui non
// arriva mai niente: il banco misura il confine fra due tracce che finiscono da
// sole.
static bool playback_reader_should_abort(void) { return false; }

#include "btpath_extracted.h"

// Il thread che sta accanto: fa il lavoro che fa il worker delle copertine --
// gira e consuma -- con la politica che gli si da'. Le due politiche sono
// esattamente quelle di utils.c: thread_be_background() (SCHED_IDLE) e
// thread_be_low_priority() (nice 10).
static volatile int noise_stop;
static volatile unsigned long noise_turns;

static void *noise_thread(void *arg) {
	const char *policy = arg;
	if (strcmp(policy, "idle") == 0) {
		struct sched_param param;
		memset(&param, 0, sizeof(param));
		pthread_setschedparam(pthread_self(), SCHED_IDLE, &param);
	} else {
		setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
	}
	volatile unsigned long x = 0;
	while (!noise_stop) {
		for (int i = 0; i < 100000; i++) {
			x += i;
		}
		noise_turns++;
	}
	return NULL;
}

static int checks;
static int failures;

static void ok(bool cond, const char *fmt, ...) {
	va_list ap;
	checks++;
	if (!cond) {
		failures++;
	}
	printf(cond ? "  ok   " : "  FAIL ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

#define RATE 44100
#define CHANNELS 2
#define TRACK_SECONDS 4

typedef struct {
	long open_ms;		   // quanto ci ha messo ad aprire
	long frames_written;   // frame accettati dal PCM
	long queue_min;		   // la coda piu' corta vista mentre scriveva
	long queue_last;	   // e quella alla fine
	int stalls;			   // secondi in cui non e' entrato niente
	bool opened;
	bool ever_running;
} track_t;

// La soglia di avvio: quanto audio deve essere stato scritto prima che il
// flusso cominci a uscire. open_pcm_device() usa l'intero buffer.
static bool start_on_period;

// Gli stessi parametri di open_pcm_device(): non bloccante, avail_min di un
// periodo, soglia di avvio pari all'intero buffer (o a un periodo con
// --soglia periodo).
static snd_pcm_t *open_like_player(const char *name, snd_pcm_uframes_t *period_out,
								   snd_pcm_uframes_t *buffer_out) {
	snd_pcm_t *pcm = NULL;
	int err = -EBUSY;
	for (int attempt = 0; attempt < 40; attempt++) {
		err = snd_pcm_open(&pcm, name, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
		if (err >= 0) {
			break;
		}
		bool transient = err == -EBUSY || err == -EAGAIN || err == -ENODEV || err == -EIO || err == -ETIMEDOUT ||
						 err == -ECONNREFUSED;
		if (!transient) {
			break;
		}
		usleep(50 * 1000);
	}
	if (err < 0) {
		fprintf(stderr, "        (apertura fallita: %s)\n", snd_strerror(err));
		return NULL;
	}

	snd_pcm_hw_params_t *hw;
	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(pcm, hw);
	snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
	snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
	snd_pcm_hw_params_set_channels(pcm, hw, CHANNELS);
	unsigned rate = RATE;
	int dir = 0;
	snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, &dir);
	unsigned periods = 8;
	snd_pcm_uframes_t period = 4096;
	snd_pcm_hw_params_set_periods_near(pcm, hw, &periods, &dir);
	snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, &dir);
	if (snd_pcm_hw_params(pcm, hw) < 0) {
		snd_pcm_close(pcm);
		return NULL;
	}

	snd_pcm_uframes_t buffer = 0;
	snd_pcm_hw_params_get_buffer_size(hw, &buffer);
	snd_pcm_hw_params_get_period_size(hw, &period, &dir);

	snd_pcm_sw_params_t *sw;
	snd_pcm_sw_params_alloca(&sw);
	if (snd_pcm_sw_params_current(pcm, sw) >= 0) {
		snd_pcm_sw_params_set_avail_min(pcm, sw, period);
		snd_pcm_uframes_t threshold = start_on_period ? period : buffer;
		if (threshold > 0) {
			snd_pcm_sw_params_set_start_threshold(pcm, sw, threshold);
		}
		snd_pcm_sw_params(pcm, sw);
	}

	*period_out = period;
	*buffer_out = buffer;
	return pcm;
}

// Una traccia: apre, scrive al passo del tempo reale per TRACK_SECONDS, e
// chiude come fa il lettore alla fine di una traccia arrivata in fondo.
static track_t play_track(const char *name, int number) {
	track_t t = {0};
	snd_pcm_uframes_t period = 0, buffer = 0;

	long t0 = log_ms();
	snd_pcm_t *pcm = open_like_player(name, &period, &buffer);
	t.open_ms = log_ms() - t0;
	if (!pcm) {
		return t;
	}
	t.opened = true;
	t.queue_min = -1;

	short *block = calloc(period * CHANNELS, sizeof(short));
	// Un tono, perche' un blocco di zeri e' comprimibile in un modo che il
	// silenzio vero non e': quello che si misura e' il flusso, non il segnale,
	// ma tanto vale che sia un flusso onesto.
	for (snd_pcm_uframes_t i = 0; i < period; i++) {
		short v = (short)(8000 * ((i / 50) % 2 ? 1 : -1));
		block[i * CHANNELS] = v;
		block[i * CHANNELS + 1] = v;
	}

	long began = log_ms();
	long running_at = -1;
	long deadline = began + TRACK_SECONDS * 1000;
	long written_in_second = 0;
	long next_second = log_ms() + 1000;

	while (log_ms() < deadline) {
		snd_pcm_sframes_t n = pcm_write_bluetooth(pcm, block, period, CHANNELS * 2);
		if (n > 0) {
			t.frames_written += n;
			written_in_second += n;
		} else if (n < 0 && n != -EAGAIN) {
			snd_pcm_prepare(pcm); // come fa pcm_write_recover su -EPIPE
		}

		snd_pcm_sframes_t left = 0;
		if (snd_pcm_delay(pcm, &left) >= 0) {
			t.queue_last = left;
			if (t.queue_min < 0 || left < t.queue_min) {
				t.queue_min = left;
			}
		}
		if (snd_pcm_state(pcm) == SND_PCM_STATE_RUNNING) {
			if (!t.ever_running) {
				running_at = log_ms() - began;
			}
			t.ever_running = true;
		}

		if (log_ms() >= next_second) {
			if (written_in_second == 0) {
				t.stalls++;
			}
			written_in_second = 0;
			next_second += 1000;
		}
	}

	printf("  traccia %d: apertura %ld ms, primo suono dopo %ld ms, %ld frame (%ld%% del tempo reale), %s\n", number,
		   t.open_ms, running_at, t.frames_written, t.frames_written * 100 / (RATE * TRACK_SECONDS),
		   t.ever_running ? "RUNNING" : "NON e' mai partito");

	// La fine della traccia, esattamente come play_decoded_file().
	if (snd_pcm_state(pcm) == SND_PCM_STATE_PREPARED) {
		snd_pcm_start(pcm);
	}
	long d0 = log_ms();
	pcm_drain_bounded(pcm);
	long drain_ms = log_ms() - d0;
	snd_pcm_close(pcm);
	printf("            chiusura: drain %ld ms\n", drain_ms);

	free(block);
	return t;
}

int main(int argc, char **argv) {
	const char *name = argc > 1 ? argv[1] : "bluealsa";
	int gap_ms = 0;
	const char *noise = NULL;
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--pausa") == 0 && i + 1 < argc) {
			gap_ms = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--rumore") == 0 && i + 1 < argc) {
			noise = argv[++i];
		} else if (strcmp(argv[i], "--soglia") == 0 && i + 1 < argc) {
			start_on_period = strcmp(argv[++i], "periodo") == 0;
		}
	}
	setvbuf(stdout, NULL, _IOLBF, 0);

	printf("tre tracce di fila su '%s', %d ms fra una e l'altra%s%s\n\n", name, gap_ms,
		   noise ? ", con un thread di fondo a " : "", noise ? noise : "");

	pthread_t noise_id;
	if (noise) {
		pthread_create(&noise_id, NULL, noise_thread, (void *)noise);
	}

	track_t t[3];
	for (int i = 0; i < 3; i++) {
		if (i > 0 && gap_ms > 0) {
			usleep((useconds_t)gap_ms * 1000);
		}
		t[i] = play_track(name, i + 1);
	}

	printf("\nquello che conta\n");
	long want = (long)RATE * TRACK_SECONDS * 80 / 100; // otto decimi del tempo reale

	for (int i = 0; i < 3; i++) {
		ok(t[i].opened, "la traccia %d apre il PCM", i + 1);
	}
	for (int i = 0; i < 3; i++) {
		ok(t[i].ever_running, "la traccia %d fa partire lo stream", i + 1);
	}
	for (int i = 0; i < 3; i++) {
		// Il controllo vero: un PCM morto accetta un buffer e poi niente piu'.
		// Quattro secondi di traccia devono valere quattro secondi di flusso.
		ok(t[i].frames_written >= want, "la traccia %d scorre davvero (%ld frame, ne servono %ld)", i + 1,
		   t[i].frames_written, want);
	}
	for (int i = 0; i < 3; i++) {
		ok(t[i].stalls == 0, "e nella traccia %d non c'e' un secondo in cui non entra niente", i + 1);
	}

	// E il confronto fra la prima e le altre, che e' come e' arrivata la
	// segnalazione: la prima si sente, le altre no.
	if (t[0].frames_written > 0) {
		long worst = t[1].frames_written < t[2].frames_written ? t[1].frames_written : t[2].frames_written;
		ok(worst * 100 / t[0].frames_written >= 80, "le tracce dopo la prima scorrono quanto la prima (%ld%%)",
		   worst * 100 / t[0].frames_written);
	}

	if (noise) {
		noise_stop = 1;
		pthread_join(noise_id, NULL);
		printf("\nil thread di fondo ha fatto %lu giri\n", noise_turns);
	}

	printf("\n%d controlli, %d falliti\n", checks, failures);
	return failures ? 1 : 0;
}

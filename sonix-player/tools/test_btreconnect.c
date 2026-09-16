// Quando il player deve smettere di rincorrere il dispositivo di ieri.
//
// Lo scenario che ha fatto nascere questo banco: si spegne il lettore con le
// cuffie collegate, lo si riaccende, e mentre il rientro automatico prova a
// tornare su quelle vecchie l'utente ne collega di nuove. Prima di questa
// funzione succedevano due cose, tutte e due sbagliate:
//
//   la pagina Bluetooth non reagiva, perche' il rientro gira sul worker dentro
//   il lavoro di accensione e puo' tenerlo occupato per quasi un minuto, e
//   niente di quello che l'utente chiede parte finche' non ha finito;
//
//   e alla fine il rientro chiamava connect_device(), che stacca quello che
//   trova collegato ("un sink alla volta") -- quindi buttava giu' le cuffie
//   appena collegate per rimettere su quelle di prima.
//
// La funzione provata qui e' quella vera, estratta da bluetooth.c da
// tools/extract_reconnect.py. Il contorno (la coda dei lavori, il flag
// dell'interruttore, il sink corrente) e' finto ed e' tutto quello che serve.
//
//   gcc -I. -o /tmp/test_btreconnect tools/test_btreconnect.c -lpthread
//
// e poi tools/run_btreconnect_bench.sh, che fa prima l'estrazione.

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define BT_MAC_MAX 20
#define JOB_QUEUE_LEN 12

typedef enum {
	JOB_NONE = 0,
	JOB_POWER,
	JOB_SCAN,
	JOB_SCAN_STOP,
	JOB_REFRESH,
	JOB_PAIR,
	JOB_CONNECT,
	JOB_DISCONNECT,
	JOB_FORGET,
	JOB_CODECS,
	JOB_SET_CODEC,
	JOB_VOLUME,
} job_type_t;

typedef struct {
	job_type_t type;
} job_t;

// --- il contorno finto, con gli stessi nomi che la funzione vera usa ---

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static job_t queue[JOB_QUEUE_LEN];
static int queue_head, queue_count;
static bool g_enabled = true;

static char fake_sink[BT_MAC_MAX];

// La chiave di configurazione, ridotta a una stringa e un contatore di
// salvataggi: quello che conta e' che venga svuotata solo per il dispositivo
// giusto, e che non si scriva sulla scheda per niente.
#define BT_LAST_DEVICE_KEY "bt_last_device"
static char fake_last[BT_MAC_MAX];
static int saves;

static const char *config_get(const char *section, const char *key, const char *fallback) {
	(void)section;
	(void)key;
	return fake_last[0] ? fake_last : fallback;
}

static void config_set(const char *section, const char *key, const char *value) {
	(void)section;
	(void)key;
	snprintf(fake_last, sizeof(fake_last), "%s", value);
}

static void config_save(void) { saves++; }

static bool btstack_audio_sink(char *out, size_t size) {
	if (!fake_sink[0]) {
		if (out && size) {
			out[0] = '\0';
		}
		return false;
	}
	snprintf(out, size, "%s", fake_sink);
	return true;
}

#include "reconnect.inc"

// --- il banco ---

static int failures;
static int checks;

static void ok(bool condition, const char *what) {
	checks++;
	if (!condition) {
		failures++;
		printf("  FALLITO  %s\n", what);
	} else {
		printf("  ok       %s\n", what);
	}
}

static void reset(void) {
	queue_head = 0;
	queue_count = 0;
	g_enabled = true;
	fake_sink[0] = '\0';
	fake_last[0] = '\0';
	saves = 0;
}

static void enqueue(job_type_t type) {
	queue[(queue_head + queue_count) % JOB_QUEUE_LEN].type = type;
	queue_count++;
}

#define OLD "AA:BB:CC:DD:EE:FF"
#define NEW "11:22:33:44:55:66"

int main(void) {
	setvbuf(stdout, NULL, _IOLBF, 0);

	printf("\n-- niente in mezzo: il rientro va avanti\n");
	reset();
	ok(!reconnect_should_stop(OLD), "coda vuota, radio accesa, nessun sink");

	reset();
	fake_sink[0] = '\0';
	enqueue(JOB_REFRESH);
	enqueue(JOB_VOLUME);
	enqueue(JOB_CODECS);
	ok(!reconnect_should_stop(OLD), "i lavori che non vengono dall'utente non lo fermano");

	printf("\n-- il dispositivo di prima e' tornato da solo\n");
	reset();
	snprintf(fake_sink, sizeof(fake_sink), OLD);
	ok(!reconnect_should_stop(OLD), "il suo stesso sink non e' un motivo per mollare");

	printf("\n-- qualcun altro sta suonando\n");
	reset();
	snprintf(fake_sink, sizeof(fake_sink), NEW);
	ok(reconnect_should_stop(OLD), "un sink diverso ferma il rientro");
	reset();
	snprintf(fake_sink, sizeof(fake_sink), "aa:bb:cc:dd:ee:ff");
	ok(!reconnect_should_stop(OLD), "e il confronto non guarda le maiuscole");

	printf("\n-- l'utente ha chiesto qualcos'altro\n");
	const struct {
		job_type_t type;
		const char *name;
	} user_jobs[] = {
		{JOB_SCAN, "una ricerca"},		 {JOB_PAIR, "un accoppiamento"},	{JOB_CONNECT, "una connessione"},
		{JOB_DISCONNECT, "una disconnessione"}, {JOB_FORGET, "un dimentica"},
	};
	for (unsigned i = 0; i < sizeof(user_jobs) / sizeof(user_jobs[0]); i++) {
		reset();
		enqueue(user_jobs[i].type);
		ok(reconnect_should_stop(OLD), user_jobs[i].name);
	}

	printf("\n-- il lavoro dell'utente in fondo a una coda lunga\n");
	reset();
	for (int i = 0; i < 6; i++) {
		enqueue(JOB_REFRESH);
	}
	enqueue(JOB_CONNECT);
	ok(reconnect_should_stop(OLD), "viene visto anche se non e' il primo");

	printf("\n-- la coda che ha girato attorno al fondo dell'array\n");
	reset();
	queue_head = JOB_QUEUE_LEN - 2;
	enqueue(JOB_REFRESH);
	enqueue(JOB_REFRESH);
	enqueue(JOB_PAIR);
	ok(reconnect_should_stop(OLD), "l'indice circolare non se lo perde");

	printf("\n-- l'interruttore Bluetooth spento\n");
	reset();
	g_enabled = false;
	ok(reconnect_should_stop(OLD), "radio spenta: si molla");
	reset();
	g_enabled = false;
	snprintf(fake_sink, sizeof(fake_sink), OLD);
	ok(reconnect_should_stop(OLD), "anche se il dispositivo di prima e' collegato");

	printf("\n-- dimenticare un dispositivo dimentica anche il rientro\n");
	reset();
	snprintf(fake_last, sizeof(fake_last), OLD);
	forget_last_device(OLD);
	ok(fake_last[0] == '\0', "l'ultimo dispositivo viene svuotato");
	ok(saves == 1, "e scritto una volta sola");

	reset();
	snprintf(fake_last, sizeof(fake_last), OLD);
	forget_last_device(NEW);
	ok(strcmp(fake_last, OLD) == 0, "dimenticarne un altro non lo tocca");
	ok(saves == 0, "e non scrive sulla scheda per niente");

	reset();
	snprintf(fake_last, sizeof(fake_last), OLD);
	forget_last_device("aa:bb:cc:dd:ee:ff");
	ok(fake_last[0] == '\0', "il confronto non guarda le maiuscole");

	reset();
	forget_last_device(OLD);
	ok(saves == 0, "senza niente scritto non si scrive niente");
	reset();
	forget_last_device("");
	ok(saves == 0, "e un indirizzo vuoto non fa danni");

	printf("\n%d controlli, %d falliti\n", checks, failures);
	return failures ? 1 : 0;
}

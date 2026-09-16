// pthread_getattr_np, for the stack bounds the crash handler reports.
#define _GNU_SOURCE

#include <malloc.h>
#include "utils.h"

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

// The classic restoring square root: one bit of the answer per round, starting
// from the highest power of four that fits. Exact -- it returns the largest r
// with r*r <= v -- and nothing in it can overflow, which is the reason it is
// written out rather than done in floating point and rounded.
uint32_t utils_isqrt32(uint32_t v) {
	uint32_t rest = v, root = 0, bit = 1u << 30;
	while (bit > rest) {
		bit >>= 2;
	}
	while (bit) {
		if (rest >= root + bit) {
			rest -= root + bit;
			root = (root >> 1) + bit;
		} else {
			root >>= 1;
		}
		bit >>= 2;
	}
	return root;
}

uint64_t utils_isqrt64(uint64_t v) {
	uint64_t rest = v, root = 0, bit = (uint64_t)1 << 62;
	while (bit > rest) {
		bit >>= 2;
	}
	while (bit) {
		if (rest >= root + bit) {
			rest -= root + bit;
			root = (root >> 1) + bit;
		} else {
			root >>= 1;
		}
		bit >>= 2;
	}
	return root;
}

char *read_file_content(const char *filename) {
	FILE *file = fopen(filename, "rb");
	if (file == NULL) {
		return NULL;
	}

	fseek(file, 0, SEEK_END);
	long filesize = ftell(file);
	fseek(file, 0, SEEK_SET);

	char *content = (char *)malloc(filesize + 1); // +1 for the null terminator
	if (content == NULL) {
		fclose(file);
		return NULL;
	}

	// Terminate at what was actually read, which can be short of filesize.
	size_t bytes_read = fread(content, 1, filesize, file);
	content[bytes_read] = '\0';

	fclose(file);

	return content;
}

bool file_matches(const char *filename, const char *expected) {
	FILE *file = fopen(filename, "rb");
	if (file == NULL) {
		return false;
	}

	char buffer[64];

	size_t bytes_read = fread(buffer, 1, sizeof(buffer) - 1, file);
	fclose(file);

	buffer[bytes_read] = '\0';

	// sysfs values come back with a trailing newline the caller never expects.
	buffer[strcspn(buffer, "\r\n")] = '\0';

	return strcmp(buffer, expected) == 0;
}

bool has_extension(const char *name, const char *ext) {
	size_t name_len = strlen(name);
	size_t ext_len = strlen(ext);

	if (name_len < ext_len)
		return false;
	return strcasecmp(name + (name_len - ext_len), ext) == 0;
}

long get_file_size(const char *filepath) {
	FILE *file = fopen(filepath, "rb");
	if (file == NULL) {
		return -1;
	}

	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	fclose(file);

	return size;
}

// Formats seconds as HH:MM:SS, hiding the hours when there are none and always
// printing two seconds digits. Returns the number of characters written.
int formatDoubleSeconds(double total_seconds, char *buffer, size_t max_len) {
	// A negative time does not exist, and casting it to uint32_t turns it into
	// 1193046:28:15 -- the number seen on the progress bar of a long audiobook.
	// The real cause was elsewhere (a 32-bit byte counter, see audio.c), but a
	// formatter that prints an absurd figure instead of zero for an impossible
	// value is a second bug, not a symptom of the first: clamping here makes
	// any future bad number show as 0:00 rather than six digits of hours.
	if (!(total_seconds > 0)) { // negated comparison also catches NaN
		total_seconds = 0;
	}
	uint32_t total_secs = (uint32_t)total_seconds;

	uint32_t hours = total_secs / 3600;
	uint32_t rem_secs = total_secs % 3600;
	uint32_t minutes = rem_secs / 60;
	uint32_t seconds = rem_secs % 60;

	if (hours > 0) {
		return snprintf(buffer, max_len, "%u:%02u:%02u", hours, minutes, seconds);
	} else {
		return snprintf(buffer, max_len, "%u:%02u", minutes, seconds);
	}
}

// Formats a pair of times as "HH:MM:SS/HH:MM:SS", each part as
// formatDoubleSeconds writes it.
void formatDoubleProgress(double current_secs, double total_secs, char *buffer, size_t max_len) {
	if (max_len == 0 || buffer == NULL)
		return;

	int chars_written = formatDoubleSeconds(current_secs, buffer, max_len);

	// snprintf returns what it wanted to write, not what it wrote, so a value
	// at or past the capacity means the buffer is already full.
	if ((size_t)chars_written >= max_len) {
		return;
	}

	buffer += chars_written;
	max_len -= chars_written;

	// Room for '/' and the terminator.
	if (max_len < 2) {
		return;
	}
	*buffer++ = '/';
	max_len--;

	formatDoubleSeconds(total_secs, buffer, max_len);
}

// See utils.h. SCHED_IDLE is per-thread on Linux and needs no privileges to
// *lower* a thread this far. The fallback nice(19)-equivalent via setpriority
// covers kernels where pthread_setschedparam is refused.
#include <pthread.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SCHED_IDLE
#define SCHED_IDLE 5
#endif

// Gives back the stack when the thread that owns it ends. The scan thread is
// created again for every rescan, so without this each one would leave its
// thirty-two kilobytes behind.
static pthread_key_t signal_stack_key;
static pthread_once_t signal_stack_once = PTHREAD_ONCE_INIT;

static void signal_stack_release(void *mem) {
	stack_t off;
	memset(&off, 0, sizeof(off));
	off.ss_flags = SS_DISABLE;
	sigaltstack(&off, NULL);
	free(mem);
}

static bool signal_stack_key_ready;

static void signal_stack_init(void) {
	signal_stack_key_ready = pthread_key_create(&signal_stack_key, signal_stack_release) == 0;
}

// Where this thread's stack starts and ends, noted while the thread is healthy.
//
// The crash handler cannot ask for it: pthread_getattr_np reads /proc/self/maps
// and allocates, neither of which belongs in a signal handler, and a thread
// whose stack has just run out is in no state to be doing either. Noted here
// instead, once, on the way in.
//
// It answers the first question to ask of any fault on this device -- did the
// stack simply run out -- which otherwise cannot be answered at all, because the
// kernel fills si_addr with zero for every user fault here (si_code comes back
// SI_KERNEL, 0x80) and so the faulting address is not in the siginfo.
static __thread uintptr_t stack_low_addr;
static __thread uintptr_t stack_high_addr;

static void stack_bounds_note(void) {
	pthread_attr_t attr;
	if (pthread_getattr_np(pthread_self(), &attr) != 0) {
		return;
	}
	void *base = NULL;
	size_t size = 0;
	if (pthread_attr_getstack(&attr, &base, &size) == 0 && base && size) {
		stack_low_addr = (uintptr_t)base;
		stack_high_addr = (uintptr_t)base + size;
	}
	pthread_attr_destroy(&attr);
}

bool thread_stack_bounds(uintptr_t *low, uintptr_t *high) {
	if (!stack_low_addr || !stack_high_addr) {
		return false;
	}
	if (low) {
		*low = stack_low_addr;
	}
	if (high) {
		*high = stack_high_addr;
	}
	return true;
}

void thread_signal_stack(void) {
	stack_bounds_note();

	// An alternate stack is per-thread, and a thread that runs out of stack
	// raises SIGSEGV with nothing left to run the handler in: without this the
	// crash handler faults again and the log never says what happened.
	stack_t current;
	memset(&current, 0, sizeof(current));
	if (sigaltstack(NULL, &current) == 0 && current.ss_sp && !(current.ss_flags & SS_DISABLE)) {
		return; // already has one
	}

	pthread_once(&signal_stack_once, signal_stack_init);

	size_t size = SIGSTKSZ < 32768 ? 32768 : (size_t)SIGSTKSZ;
	void *mem = malloc(size);
	if (!mem) {
		return;
	}

	stack_t alt;
	memset(&alt, 0, sizeof(alt));
	alt.ss_sp = mem;
	alt.ss_size = size;
	if (sigaltstack(&alt, NULL) != 0) {
		free(mem);
		return;
	}
	// Without the key the memory cannot be reclaimed at thread exit, but the
	// threads that never exit are the ones that most need the stack: keep it
	// and let the thirty-two kilobytes stand.
	if (signal_stack_key_ready && pthread_setspecific(signal_stack_key, mem) != 0) {
		signal_stack_release(mem);
	}
}

// A thread that must keep up with the interface without fighting it.
//
// Not SCHED_IDLE. That policy means "only when nothing else wants the core",
// and on this single-core device that is nearly a promise of never: against one
// busy thread an idle-class worker gets under half a percent of the core, so
// while the interface is redrawing -- which is the whole of a scroll --
// twenty milliseconds of work takes seconds.
//
// Nice ten instead: about a tenth of a saturated core, the whole of an idle one,
// and prompt when it wakes from a read. It stays below the interface, which is
// the part of SCHED_IDLE worth keeping.
void thread_be_low_priority(const char *name) {
	thread_signal_stack();

	pid_t tid = (pid_t)syscall(SYS_gettid);
	setpriority(PRIO_PROCESS, (id_t)tid, 10);

	if (name) {
		prctl(PR_SET_NAME, name, 0, 0, 0);
		fprintf(stderr, "thread: %s runs below the interface (nice 10)\n", name);
	}
}

// Moves the calling thread between the two background levels, and says nothing.
//
// The two named functions announce themselves once, when a thread starts. This
// one is for a thread that moves between the levels while it runs, where a line
// per move would be a line per job.
void thread_set_background_level(bool above_idle) {
	pid_t tid = (pid_t)syscall(SYS_gettid);
	if (above_idle) {
		struct sched_param param;
		memset(&param, 0, sizeof(param));
		pthread_setschedparam(pthread_self(), SCHED_OTHER, &param);
		setpriority(PRIO_PROCESS, (id_t)tid, 10);
		return;
	}
	struct sched_param param;
	memset(&param, 0, sizeof(param));
	if (pthread_setschedparam(pthread_self(), SCHED_IDLE, &param) != 0) {
		setpriority(PRIO_PROCESS, (id_t)tid, 19);
	}
}

void thread_be_background(const char *name) {
	thread_signal_stack();

	struct sched_param param;
	memset(&param, 0, sizeof(param));

	if (pthread_setschedparam(pthread_self(), SCHED_IDLE, &param) != 0) {
		// Old kernel or unusual policy restrictions: at least drop the nice
		// level as far as it goes. syscall(gettid) because glibc only grew a
		// wrapper for it recently.
		pid_t tid = (pid_t)syscall(SYS_gettid);
		setpriority(PRIO_PROCESS, (id_t)tid, 19);
	}

	if (name) {
		// Shows up in /proc/self/task/*/comm, which is what the watchdog's
		// stall dump prints -- so a stuck thread has a name, not just a tid.
		prctl(PR_SET_NAME, name, 0, 0, 0);
		fprintf(stderr, "thread: %s runs in the background (SCHED_IDLE)\n", name);
	}
}

void thread_be_realtime(const char *name, int priority) {
	thread_signal_stack();

	if (name) {
		prctl(PR_SET_NAME, name, 0, 0, 0);
	}

	struct sched_param rt;
	memset(&rt, 0, sizeof(rt));
	rt.sched_priority = priority;
	bool got = pthread_setschedparam(pthread_self(), SCHED_RR, &rt) == 0;
	if (name) {
		fprintf(stderr, "thread: %s runs %s\n", name,
				got ? "at RT priority" : "at normal priority (RT refused)");
	}
}

void deadline_in_ms(struct timespec *out, unsigned int ms) {
	if (!out) {
		return;
	}

	clock_gettime(CLOCK_REALTIME, out);

	long long ns = (long long)out->tv_nsec + (long long)ms * 1000000LL;
	out->tv_sec += (time_t)(ns / 1000000000LL);
	out->tv_nsec = (long)(ns % 1000000000LL);
}

// ---------------------------------------------------------------------------
// The heap
//
// mallinfo2() is the one that returns the figures as size_t; it arrived in
// glibc 2.33, and the toolchain for this device is older than that, so the
// deprecated mallinfo() is what actually gets compiled. Its fields are int,
// which would wrap past two gigabytes -- a limit this player is in no danger
// of reaching on a machine with fifty-five megabytes.
// ---------------------------------------------------------------------------

void heap_usage(size_t *in_use, size_t *from_system) {
	size_t used = 0;
	size_t total = 0;

#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
	struct mallinfo2 info = mallinfo2();
	used = info.uordblks;
	total = info.arena + info.hblkhd;
#elif defined(__GLIBC__)
	struct mallinfo info = mallinfo();
	used = (size_t)(unsigned int)info.uordblks;
	total = (size_t)(unsigned int)info.arena + (size_t)(unsigned int)info.hblkhd;
#endif

	if (in_use) {
		*in_use = used;
	}
	if (from_system) {
		*from_system = total;
	}
}

// ---------------------------------------------------------------------------
// Page-lifetime buffers
// ---------------------------------------------------------------------------

// Whole pages, because that is the unit the kernel hands out and takes back.
static size_t whole_pages(size_t size) {
	long page = sysconf(_SC_PAGESIZE);
	if (page <= 0) {
		page = 4096;
	}
	size_t unit = (size_t)page;
	return (size + unit - 1) / unit * unit;
}

void *big_alloc(size_t size) {
	if (size == 0) {
		return NULL;
	}
	void *p = mmap(NULL, whole_pages(size), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	return p == MAP_FAILED ? NULL : p;
}

void big_free(void *ptr, size_t size) {
	if (!ptr || size == 0) {
		return;
	}
	munmap(ptr, whole_pages(size));
}

// ---------------------------------------------------------------------------
// The breadcrumb
// ---------------------------------------------------------------------------

// Volatile: the writers are ordinary threads and the reader is a signal handler
// on whichever thread died. Neither a lock nor an atomic would help -- a
// handler cannot wait for a mutex, and a torn path still names the folder.
//
// One buffer per kind rather than one between them all: the scan, the track and
// the artwork are written by three different threads, and with a single buffer
// whichever wrote last would be the only one left to read. Three answers to
// three different questions is the whole value of the thing.
static volatile char crumb[600];
static volatile char crumb_track_path[600];
static volatile char crumb_artwork_path[600];

static void crumb_copy(volatile char *into, size_t size, const char *what) {
	if (!what) {
		into[0] = '\0';
		return;
	}
	size_t i = 0;
	while (what[i] && i < size - 1) {
		into[i] = what[i];
		i++;
	}
	into[i] = '\0';
}

void crumb_set(const char *what) { crumb_copy(crumb, sizeof(crumb), what); }

const char *crumb_get(void) { return (const char *)crumb; }

void crumb_set_track(const char *path) { crumb_copy(crumb_track_path, sizeof(crumb_track_path), path); }

const char *crumb_track(void) { return (const char *)crumb_track_path; }

void crumb_set_artwork(const char *path) {
	crumb_copy(crumb_artwork_path, sizeof(crumb_artwork_path), path);
}

const char *crumb_artwork(void) { return (const char *)crumb_artwork_path; }

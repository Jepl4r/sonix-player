// sonix_launch: starts the player and reboots the device when it exits.
//
//     sonix_launch [/path/to/player [args...]]
//
// sonix_player.sh ends with `exec sonix_launch /usr/bin/sonix_player`, so this
// replaces the shell that would otherwise sit in memory for the whole session
// waiting to run `sleep 1; reboot`. It does exactly that and nothing else: the
// player runs as its child with the same environment, limits and signal
// dispositions, and whatever ends it (an exit, a kill, a loader that refuses
// the binary) is followed by one second and a reboot through init.
//
// Freestanding: no libc, raw o32 system calls, one static binary of a few kB
// that depends on nothing in the rootfs. MIPS o32 Linux only.

#include <asm/unistd.h>

#define LINUX_REBOOT_MAGIC1 0xfee1dead
#define LINUX_REBOOT_MAGIC2 672274793
#define LINUX_REBOOT_CMD_RESTART 0x01234567
#define EINTR 4

#define DEFAULT_PLAYER "/usr/bin/sonix_player"

// o32: number in $2, arguments in $4-$7. On return $2 holds the result and $7
// is non-zero when $2 is an errno, handed back here as a negative value.
static long sys3(long n, long a, long b, long c) {
	register long r4 __asm__("$4") = a;
	register long r5 __asm__("$5") = b;
	register long r6 __asm__("$6") = c;
	register long r7 __asm__("$7");
	register long r2 __asm__("$2");
	__asm__ __volatile__("addu $2, $0, %2\n\tsyscall"
						 : "=&r"(r2), "=r"(r7)
						 : "ir"(n), "r"(r4), "r"(r5), "r"(r6)
						 : "$1", "$3", "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15", "$24", "$25", "hi",
						   "lo", "memory");
	return r7 && r2 > 0 ? -r2 : r2;
}

static long sys4(long n, long a, long b, long c, long d) {
	register long r4 __asm__("$4") = a;
	register long r5 __asm__("$5") = b;
	register long r6 __asm__("$6") = c;
	register long r7 __asm__("$7") = d;
	register long r2 __asm__("$2");
	__asm__ __volatile__("addu $2, $0, %2\n\tsyscall"
						 : "=&r"(r2), "+r"(r7)
						 : "ir"(n), "r"(r4), "r"(r5), "r"(r6)
						 : "$1", "$3", "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15", "$24", "$25", "hi",
						   "lo", "memory");
	return r7 && r2 > 0 ? -r2 : r2;
}

static void say(const char *s) {
	long n = 0;
	while (s[n]) {
		n++;
	}
	sys3(__NR_write, 2, (long)s, n);
}

static void say_num(unsigned v) {
	char buf[12];
	int i = sizeof(buf);
	buf[--i] = 0;
	do {
		buf[--i] = (char)('0' + v % 10);
		v /= 10;
	} while (v && i > 0);
	say(buf + i);
}

static void run(const char *path, char **argv, char **envp) {
	sys3(__NR_execve, (long)path, (long)argv, (long)envp);
}

__attribute__((noreturn, used)) void launch_main(long *sp) {
	int argc = (int)sp[0];
	char **argv = (char **)(sp + 1);
	char **envp = argv + argc + 1;

	static char default_player[] = DEFAULT_PLAYER;
	static char *default_argv[] = {default_player, 0};
	char **player_argv = argc > 1 ? argv + 1 : default_argv;

	long pid = sys3(__NR_fork, 0, 0, 0);
	if (pid == 0) {
		run(player_argv[0], player_argv, envp);
		say("sonix_launch: cannot start ");
		say(player_argv[0]);
		say("\n");
		sys3(__NR_exit, 127, 0, 0);
	}
	if (pid < 0) {
		// No second process: the player takes this one, as a plain exec would.
		run(player_argv[0], player_argv, envp);
	} else {
		int status = 0;
		long r;
		do {
			r = sys4(__NR_wait4, pid, (long)&status, 0, 0);
		} while (r == -EINTR);

		if ((status & 0x7f) == 0) {
			say("sonix_launch: player exited with status ");
			say_num((unsigned)(status >> 8) & 0xff);
		} else {
			say("sonix_launch: player killed by signal ");
			say_num((unsigned)status & 0x7f);
		}
		say(", rebooting\n");
	}

	long pause[2] = {1, 0}; // struct timespec: one second
	while (sys3(__NR_nanosleep, (long)pause, (long)pause, 0) == -EINTR) {
	}

	// The reboot applet goes through init, which runs the shutdown scripts and
	// unmounts the card. The system call is only for a rootfs without one.
	static char reboot_name[] = "reboot";
	static char *reboot_argv[] = {reboot_name, 0};
	run("/sbin/reboot", reboot_argv, envp);
	run("/bin/reboot", reboot_argv, envp);
	run("/usr/sbin/reboot", reboot_argv, envp);
	run("/usr/bin/reboot", reboot_argv, envp);
	sys3(__NR_sync, 0, 0, 0);
	sys4(__NR_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_RESTART, 0);
	for (;;) {
		sys3(__NR_exit, 1, 0, 0);
	}
}

// The kernel enters with $sp on argc, argv[] and envp[]. The o32 calling
// convention wants 16 bytes of argument space below the caller's frame.
__asm__(".text\n"
		".globl __start\n"
		".type __start, @function\n"
		".set push\n"
		".set noreorder\n"
		"__start:\n"
		"	move $4, $sp\n"
		"	li $8, -8\n"
		"	and $sp, $sp, $8\n"
		"	addiu $sp, $sp, -16\n"
		"	jal launch_main\n"
		"	nop\n"
		".set pop\n");

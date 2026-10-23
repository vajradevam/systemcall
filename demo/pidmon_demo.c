/*
 * pidmon_demo: a guided tour of the pidmon_start / pidmon_read system calls.
 *
 *   gcc -O1 -Wall -pthread -isystem <headers_install dir>/include -o pidmon_demo pidmon_demo.c
 *
 * The two calls (glibc has no wrapper, so we use syscall(2)):
 *
 *   pidmon_start(pid, flags, &limits, size)
 *       Opens a monitoring window on pid (0 = myself) and arms its limits. From then on the kernel
 *       watches the target by itself: a soft-limit breach gets one warning signal, a hard-limit
 *       breach gets SIGKILL. Nobody has to call pidmon_read for that.
 *         limits: soft/hard values for any of the 7 limits; limits not given (or no struct at all)
 *                 are the target's own rlimits. They belong to the window; the target's real
 *                 rlimits are never changed.
 *         flags:  PIDMON_F_NO_ENFORCE  judge and report only, never signal
 *
 *   pidmon_read(pid, flags, &usage, size)
 *       Reports what happened in the window. It never signals anything.
 *         flags:  PIDMON_F_STOP        close the window after reading
 *
 * The target must be the caller or one of its descendants.
 *
 * Scenarios (see main):
 *    1  monitoring myself                      9  a breach is remembered after the target recovers
 *    2  who may be monitored; orphans          10 enforcement exists only inside the window
 *    3  a full report                          11 targets that already exited, and zombies
 *    4  memory gauges and peaks                12 threads: a pid is a whole process
 *    5  limits: the target's own vs given      13 records belong to their owner
 *    6  enforcement without any read           14 the cap of 64 monitors
 *    7  every limit, one by one                15 every error the calls can return
 *    8  report-only windows (NO_ENFORCE)
 *
 * Several scenarios need a second process doing something on cue, so each "actor" runs in a forked
 * child and takes its cues from the parent through a pipe.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/pidmon.h>

#ifndef __NR_pidmon_start
#define __NR_pidmon_start 473
#define __NR_pidmon_read 474
#endif

/* ---- units --------------------------------------------------------------------------------- */

/* The kernel reports time in nanoseconds and memory in bytes; people read milliseconds and MB. */
#define NSEC_PER_SEC	1000000000ULL
#define NSEC_PER_MSEC	1000000ULL
#define BYTES_PER_MB	(1024ULL * 1024ULL)
#define MB(n)		((uint64_t)(n) * BYTES_PER_MB)
#define UNLIMITED	(~0ULL)

static double to_ms(uint64_t ns)    { return (double)ns / NSEC_PER_MSEC; }
static double to_mb(uint64_t bytes) { return (double)bytes / BYTES_PER_MB; }

/*
 * cpu_permille is "per mille of one CPU": 1000 means one CPU fully busy for the whole window
 * (2000 would be two CPUs). Dividing by 10 gives the familiar percentage that top shows.
 */
#define PERMILLE_PER_PERCENT 10.0

static double to_percent(uint64_t permille) { return (double)permille / PERMILLE_PER_PERCENT; }

/* ---- what the scenarios do (all sizes in one place) ---------------------------------------- */

#define SELF_CPU_MS		100	/* scenario 1: work done by this process itself */
#define SELF_MEM_MB		8

#define WORKER_PRE_CPU_MS	200	/* scenario 3: CPU burnt before the window opens */
#define WORKER_MEM_MB		32	/* ... memory the worker touches in the window */
#define WORKER_FILES		10	/* ... files it opens */
#define WORKER_CPU_MS		300	/* ... CPU it burns */
#define WORKER_READ_KB		256	/* ... bytes it reads from /dev/zero */
#define WORKER_WRITE_KB		128	/* ... and writes to /dev/null */

#define MEM_BIG_MB		24	/* scenario 4: private memory */
#define MEM_LOCKED_MB		4	/* ... memory it mlocks */

#define RSS_SOFT_MB		8	/* scenarios 6-10: the RSS limit given to pidmon_start */
#define RSS_HARD_MB		48
#define RSS_FIRST_MB		16	/* memory held first: above soft, below hard */
#define RSS_GROW_MB		64	/* memory added later: 16 + 64 = 80, above hard */
#define RSS_ROOMY_HARD_MB	512	/* a hard limit nobody reaches, for the soft-only scenarios */

#define SHORT_MEM_MB		16	/* scenario 11: the short-lived child */
#define SHORT_CPU_MS		150

#define THREAD_COUNT		3	/* scenario 12: threads besides the main one */
#define THREAD_CPU_MS		300

#define CAP_EXTRA		1	/* scenario 14: processes beyond the cap of 64 */

/* How long we wait for a child before giving up. */
#define WAIT_QUICK_MS		2000
#define WAIT_NORMAL_MS		5000
#define WAIT_LONG_MS		20000

/* How long we watch for a signal that must NOT arrive. */
#define QUIET_MS		400

/* A pid that cannot exist: above the kernel's maximum of 4194304. */
#define NONEXISTENT_PID		99999999

/* ---- the two system calls ------------------------------------------------------------------ */

/* Both return 0 on success or the errno value, which keeps the call sites short. */
static int pm_start_lim(pid_t pid, unsigned int flags, const struct pidmon_limits *lim)
{
	return syscall(__NR_pidmon_start, pid, flags, lim, lim ? sizeof(*lim) : 0) < 0 ? errno : 0;
}

/* the target's own rlimits are the limits */
static int pm_start(pid_t pid)
{
	return pm_start_lim(pid, 0, NULL);
}

static int pm_read(pid_t pid, unsigned int flags, struct pidmon_usage *u)
{
	return syscall(__NR_pidmon_read, pid, flags, u, sizeof(*u)) < 0 ? errno : 0;
}

/* Build a limits struct one limit at a time. */
static void lim_clear(struct pidmon_limits *lim)
{
	memset(lim, 0, sizeof(*lim));
}

static void lim_set(struct pidmon_limits *lim, int which, uint64_t soft, uint64_t hard)
{
	lim->set |= 1U << which;
	lim->lim[which].soft = soft;
	lim->lim[which].hard = hard;
}

/* ---- printing ------------------------------------------------------------------------------ */

static const char *errname(int e)
{
	switch (e) {
	case 0:		return "ok";
	case EPERM:	return "EPERM: not me or my descendant";
	case ESRCH:	return "ESRCH: no such process (or it already finished exiting)";
	case ENOENT:	return "ENOENT: no monitor for it";
	case EBUSY:	return "EBUSY: already monitored";
	case EINVAL:	return "EINVAL: bad argument";
	case E2BIG:	return "E2BIG: size too big, or an unknown field is set";
	case EFAULT:	return "EFAULT: bad pointer";
	case ENOSPC:	return "ENOSPC: 64 monitors already held";
	default:	return strerror(e);
	}
}

static void title(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void title(const char *fmt, ...)
{
	char text[200];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	printf("\n\033[1m== %s\033[0m\n", text);
}

/* One line: what we asked, and how the kernel answered. */
static void say(const char *what, int e)
{
	printf("  %-56s -> %s\n", what, errname(e));
}

static const char *signame(unsigned int sig)
{
	switch (sig) {
	case 0:		return "none";
	case SIGTERM:	return "SIGTERM";
	case SIGKILL:	return "SIGKILL";
	case SIGXCPU:	return "SIGXCPU";
	default:	return "other";
	}
}

/* Names of the limits that pidmon judges, indexed by PIDMON_LIM_*. */
static const char *const limit_names[PIDMON_NLIM] = {
	[PIDMON_LIM_CPU]	= "CPU",
	[PIDMON_LIM_AS]		= "AS",
	[PIDMON_LIM_RSS]	= "RSS",
	[PIDMON_LIM_DATA]	= "DATA",
	[PIDMON_LIM_STACK]	= "STACK",
	[PIDMON_LIM_MEMLOCK]	= "MEMLOCK",
	[PIDMON_LIM_NOFILE]	= "NOFILE",
};

/* exceeded_soft / exceeded_hard are bitmasks: bit n set means limit n was breached. */
static const char *limit_list(unsigned int mask)
{
	static char text[96];
	size_t len = 0;
	int n;

	text[0] = '\0';
	for (n = 0; n < PIDMON_NLIM; n++) {
		if (!(mask & (1U << n)))
			continue;
		len += snprintf(text + len, sizeof(text) - len, "%s%s", len ? "," : "", limit_names[n]);
	}
	return len ? text : "-";
}

#define ULL(v) ((unsigned long long)(v))

/* A limit value in the unit of its rlimit: CPU in seconds, NOFILE as a count, the rest in bytes. */
static void fmt_limit(char *out, size_t size, int which, uint64_t v)
{
	if (v == UNLIMITED)
		snprintf(out, size, "unlimited");
	else if (which == PIDMON_LIM_CPU)
		snprintf(out, size, "%llu s", ULL(v));
	else if (which == PIDMON_LIM_NOFILE)
		snprintf(out, size, "%llu", ULL(v));
	else
		snprintf(out, size, "%.2f MB", to_mb(v));
}

/* The limits that are in force for the window: given ones, and the target's own for the rest. */
static void show_limits(const struct pidmon_usage *u, unsigned int given)
{
	char soft[32], hard[32];
	int n;

	for (n = 0; n < PIDMON_NLIM; n++) {
		fmt_limit(soft, sizeof(soft), n, u->lim[n].soft);
		fmt_limit(hard, sizeof(hard), n, u->lim[n].hard);
		printf("    %-8s soft %-12s hard %-12s %s\n", limit_names[n], soft, hard,
		       (given & (1U << n)) ? "(given to pidmon_start)" : "(the target's own rlimit)");
	}
}

/* Print a pidmon_usage as four lines. */
static void show(const struct pidmon_usage *u)
{
	const char *note = "";

	if (u->state & PIDMON_ST_EXITED)
		note = " | target has EXITED (these are its final numbers)";

	printf("    window %7.1f ms | cpu %6.1f ms (user %.1f, sys %.1f) = %.1f%% of one cpu | total cpu since start %.1f ms\n",
	       to_ms(u->wall_ns), to_ms(u->utime_ns + u->stime_ns), to_ms(u->utime_ns), to_ms(u->stime_ns),
	       to_percent(u->cpu_permille), to_ms(u->cpu_total_ns));
	printf("    memory: rss %.1f MB (anon %.1f, file %.1f), peak %.1f MB | virtual %.1f MB, peak %.1f MB | stack %.2f MB\n",
	       to_mb(u->rss_bytes), to_mb(u->rss_anon), to_mb(u->rss_file), to_mb(u->rss_peak),
	       to_mb(u->vm_bytes), to_mb(u->vm_peak), to_mb(u->stack_vm));
	printf("    faults: %llu minor, %llu major | context switches: %llu voluntary, %llu forced | threads %llu | open fds %llu\n",
	       ULL(u->minor_faults), ULL(u->major_faults), ULL(u->vol_ctxsw), ULL(u->invol_ctxsw),
	       ULL(u->nr_threads), ULL(u->nr_open_fds));
	printf("    io: %llu B read, %llu B written | limits breached: soft [%s] hard [%s] | signal sent in the window: %s%s\n",
	       ULL(u->rchar), ULL(u->wchar), limit_list(u->exceeded_soft), limit_list(u->exceeded_hard),
	       signame(u->signal_sent), note);
}

/* Print every field of a pidmon_usage, grouped as in the header file. */
static void show_full(const struct pidmon_usage *u)
{
	printf("    header   size %u, state 0x%x%s, breached soft [%s] hard [%s], signal sent %s\n",
	       u->size, u->state, (u->state & PIDMON_ST_EXITED) ? " (EXITED)" : "",
	       limit_list(u->exceeded_soft), limit_list(u->exceeded_hard), signame(u->signal_sent));
	printf("    time     wall_ns %llu (%.1f ms: the length of the window)\n", ULL(u->wall_ns), to_ms(u->wall_ns));
	printf("    cpu      utime_ns %llu, stime_ns %llu, cpu_total_ns %llu (since the process began)\n",
	       ULL(u->utime_ns), ULL(u->stime_ns), ULL(u->cpu_total_ns));
	printf("             cpu_permille %llu (%.1f%% of one cpu), vol_ctxsw %llu, invol_ctxsw %llu\n",
	       ULL(u->cpu_permille), to_percent(u->cpu_permille), ULL(u->vol_ctxsw), ULL(u->invol_ctxsw));
	printf("    faults   minor_faults %llu, major_faults %llu\n", ULL(u->minor_faults), ULL(u->major_faults));
	printf("    io       rchar %llu, wchar %llu, read_bytes %llu, write_bytes %llu (the last two need storage I/O)\n",
	       ULL(u->rchar), ULL(u->wchar), ULL(u->read_bytes), ULL(u->write_bytes));
	printf("    memory   rss_bytes %.2f MB = rss_anon %.2f + rss_file %.2f (+ shared), rss_peak %.2f MB\n",
	       to_mb(u->rss_bytes), to_mb(u->rss_anon), to_mb(u->rss_file), to_mb(u->rss_peak));
	printf("             vm_bytes %.2f MB, vm_peak %.2f MB, data_vm %.2f MB, stack_vm %.2f MB, locked_vm %.2f MB\n",
	       to_mb(u->vm_bytes), to_mb(u->vm_peak), to_mb(u->data_vm), to_mb(u->stack_vm), to_mb(u->locked_vm));
	printf("    process  nr_threads %llu, nr_open_fds %llu\n", ULL(u->nr_threads), ULL(u->nr_open_fds));
}

/* ---- parent/child plumbing ----------------------------------------------------------------- */

/* One-byte messages between the parent and an actor child. */
enum msg {
	MSG_READY = 'r',	/* child -> parent: set up, waiting for my cue */
	MSG_GO    = 'g',	/* parent -> child: do the next step */
	MSG_DONE  = 'd',	/* child -> parent: step finished */
	MSG_TERM  = 'T',	/* child -> parent: I received a warning signal */
	MSG_EXIT  = 'x',	/* parent -> child: exit now */
	MSG_FREE  = 's',	/* parent -> child: give back the memory you hold */
	MSG_FAIL  = 'F',	/* child -> parent: the step could not be done here */
};

static void send_msg(int fd, enum msg m)
{
	char c = m;

	if (write(fd, &c, 1) != 1)
		_exit(9);
}

/* Wait up to timeout_ms for one message; returns it, or -1 on timeout. */
static int wait_msg(int fd, int timeout_ms)
{
	struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
	fd_set set;
	char c;
	int n;

	do {	/* a signal (the warning, say) must not look like a cue; select leaves the time left in tv */
		FD_ZERO(&set);
		FD_SET(fd, &set);
		n = select(fd + 1, &set, NULL, NULL, &tv);
	} while (n < 0 && errno == EINTR);
	if (n <= 0 || read(fd, &c, 1) != 1)
		return -1;
	return (unsigned char)c;
}

static double now_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return now.tv_sec * 1000.0 + now.tv_nsec / 1e6;
}

/* Skip other messages until `want` arrives; 1 if it did within timeout_ms, 0 if not. */
static int wait_for(int fd, enum msg want, int timeout_ms)
{
	double end = now_ms() + timeout_ms;
	int m;

	while (now_ms() < end) {
		m = wait_msg(fd, (int)(end - now_ms()) + 1);
		if (m == (int)want)
			return 1;
		if (m < 0)
			return 0;
	}
	return 0;
}

/*
 * Fork a child that runs actor(from_parent, to_parent). The parent gets the other ends:
 * *cue to talk to the child, *report to hear from it.
 */
typedef void (*actor_fn)(int from_parent, int to_parent);

static pid_t spawn(actor_fn actor, int *cue, int *report)
{
	int down[2], up[2];	/* parent -> child, child -> parent */
	pid_t pid;

	if (pipe(down) || pipe(up))
		exit(9);
	pid = fork();
	if (pid < 0)
		exit(9);
	if (pid == 0) {
		close(down[1]);
		close(up[0]);
		actor(down[0], up[1]);
		_exit(0);
	}
	close(down[0]);
	close(up[1]);
	*cue = down[1];
	*report = up[0];
	return pid;
}

/*
 * Stop an actor for good: kill it, reap it, and release its monitor. A monitor is not freed by the
 * target's death (the owner may still want the final numbers), so each one costs a slot of the 64
 * until it is read with PIDMON_F_STOP.
 */
static void put_down(pid_t pid, int cue, int report)
{
	struct pidmon_usage u;

	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
	pm_read(pid, PIDMON_F_STOP, &u);
	close(cue);
	close(report);
}

static void sleep_ms(int ms)
{
	struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * (long)NSEC_PER_MSEC };

	nanosleep(&ts, NULL);
}

/* ---- things an actor can do ---------------------------------------------------------------- */

/* CPU time this thread has used so far, in ns (CLOCK_THREAD_CPUTIME_ID counts only time on a CPU). */
static uint64_t thread_cpu_ns(void)
{
	struct timespec now;

	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
	return now.tv_sec * NSEC_PER_SEC + now.tv_nsec;
}

/* Keep the CPU busy until this thread has *used* ms of CPU (not merely let ms of wall time pass). */
static void burn_cpu_ms(int ms)
{
	volatile unsigned long spin = 0;
	uint64_t end_ns = thread_cpu_ns() + (uint64_t)ms * NSEC_PER_MSEC;

	while (thread_cpu_ns() < end_ns)
		spin = spin + 1;
}

/* Map mb megabytes and write to every page, so they really become resident (count as RSS). */
static void *touch_memory_mb(size_t mb)
{
	size_t len = mb * BYTES_PER_MB;
	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (p == MAP_FAILED)
		exit(9);
	memset(p, 1, len);
	return p;
}

static void open_files(int count)
{
	while (count--)
		if (open("/dev/null", O_RDONLY) < 0)
			exit(9);
}

/* Read and write some bytes, so rchar and wchar have something to count. */
static void do_io(int read_kb, int write_kb)
{
	char buf[1024];
	int zero = open("/dev/zero", O_RDONLY), null = open("/dev/null", O_WRONLY);
	int i;

	if (zero < 0 || null < 0)
		exit(9);
	for (i = 0; i < read_kb; i++)
		if (read(zero, buf, sizeof(buf)) != sizeof(buf))
			exit(9);
	for (i = 0; i < write_kb; i++)
		if (write(null, buf, sizeof(buf)) != sizeof(buf))
			exit(9);
	close(zero);
	close(null);
}

/* Parent pid of pid, read from /proc/<pid>/stat; -1 if the process does not exist. */
static pid_t parent_of(pid_t pid)
{
	char path[64], line[512], state;
	pid_t ppid = -1;
	char *after_comm;
	int fd, n;

	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, line, sizeof(line) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	line[n] = '\0';
	/*
	 * The line is "pid (comm) state ppid ...". The command name may itself contain spaces and
	 * parentheses, so find the LAST ')' and read "state ppid" from after it.
	 */
	after_comm = strrchr(line, ')');
	if (!after_comm || sscanf(after_comm + 1, " %c %d", &state, &ppid) != 2)
		return -1;
	return ppid;
}

/* Wait (up to ~2 s) until pid's parent is not `old` any more. */
static void wait_reparented(pid_t pid, pid_t old)
{
	int i;

	for (i = 0; i < 200 && parent_of(pid) == old; i++)
		sleep_ms(10);
}

/* ---- the actors: each runs in its own child process ---------------------------------------- */

/* Does nothing until it is killed. */
static void actor_idle(int from_parent, int to_parent)
{
	(void)from_parent;
	send_msg(to_parent, MSG_READY);
	for (;;)
		pause();
}

/* Scenario 3: burns CPU before the window, then uses memory, files, CPU and I/O on cue. */
static void actor_worker(int from_parent, int to_parent)
{
	burn_cpu_ms(WORKER_PRE_CPU_MS);
	send_msg(to_parent, MSG_READY);
	wait_msg(from_parent, WAIT_LONG_MS);

	touch_memory_mb(WORKER_MEM_MB);
	open_files(WORKER_FILES);
	do_io(WORKER_READ_KB, WORKER_WRITE_KB);
	burn_cpu_ms(WORKER_CPU_MS);

	send_msg(to_parent, MSG_DONE);
	for (;;)
		pause();
}

/* Scenario 4: step 1 takes private memory, step 2 locks some, step 3 gives everything back. */
static void actor_memory(int from_parent, int to_parent)
{
	void *big = NULL, *locked = NULL;

	send_msg(to_parent, MSG_READY);
	for (;;) {
		int cue = wait_msg(from_parent, WAIT_LONG_MS);

		if (cue == '1') {
			big = touch_memory_mb(MEM_BIG_MB);
		} else if (cue == '2') {
			locked = touch_memory_mb(MEM_LOCKED_MB);
			if (mlock(locked, MEM_LOCKED_MB * BYTES_PER_MB)) {
				send_msg(to_parent, MSG_FAIL);
				continue;
			}
		} else if (cue == '3') {
			if (locked) {
				munlock(locked, MEM_LOCKED_MB * BYTES_PER_MB);
				munmap(locked, MEM_LOCKED_MB * BYTES_PER_MB);
			}
			if (big)
				munmap(big, MEM_BIG_MB * BYTES_PER_MB);
			big = locked = NULL;
		} else {
			_exit(0);
		}
		send_msg(to_parent, MSG_DONE);
	}
}

static int warning_report_fd;

static void on_warning(int sig)
{
	(void)sig;
	send_msg(warning_report_fd, MSG_TERM);	/* tell the parent the warning reached us */
}

/* Catch the two warning signals pidmon uses, and report each to the parent. */
static void catch_warnings(int to_parent)
{
	struct sigaction sa = { .sa_handler = on_warning };

	warning_report_fd = to_parent;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGXCPU, &sa, NULL);
}

/*
 * Scenarios 6-10: holds RSS_FIRST_MB, and on cue grows by RSS_GROW_MB (cue 'g') or gives everything
 * back (cue 's'). It has no rlimit of its own: the limits come from the window.
 */
static void actor_greedy(int from_parent, int to_parent)
{
	void *first, *grown = NULL;

	catch_warnings(to_parent);
	first = touch_memory_mb(RSS_FIRST_MB);
	send_msg(to_parent, MSG_READY);

	for (;;) {
		int cue = wait_msg(from_parent, WAIT_LONG_MS);

		if (cue == MSG_GO) {
			grown = touch_memory_mb(RSS_GROW_MB);
		} else if (cue == MSG_FREE) {
			munmap(first, RSS_FIRST_MB * BYTES_PER_MB);
			if (grown)
				munmap(grown, RSS_GROW_MB * BYTES_PER_MB);
		} else {
			continue;
		}
		send_msg(to_parent, MSG_DONE);
	}
}

/* Scenario 7: on cue, goes over one limit. Which one is chosen by `breach_which` before the fork. */
static int breach_which;

static void deep_stack(void)
{
	volatile char buf[3 * 1024 * 1024];
	size_t i;

	for (i = 0; i < sizeof(buf); i += 4096)
		buf[i] = 1;
}

static void actor_breacher(int from_parent, int to_parent)
{
	catch_warnings(to_parent);
	send_msg(to_parent, MSG_READY);
	wait_msg(from_parent, WAIT_LONG_MS);

	switch (breach_which) {
	case PIDMON_LIM_CPU:
		burn_cpu_ms(1300);
		break;
	case PIDMON_LIM_AS:	/* address space only: nothing is touched */
		mmap(NULL, MB(64), PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		break;
	case PIDMON_LIM_RSS:
		touch_memory_mb(RSS_FIRST_MB);
		break;
	case PIDMON_LIM_DATA:	/* private writable mapping, not touched */
		mmap(NULL, MB(32), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		break;
	case PIDMON_LIM_STACK:
		deep_stack();
		break;
	case PIDMON_LIM_MEMLOCK: {
		void *p = mmap(NULL, MB(4), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		if (p == MAP_FAILED || mlock(p, MB(4))) {
			send_msg(to_parent, MSG_FAIL);	/* this system does not allow it */
			for (;;)
				pause();
		}
		break;
	}
	case PIDMON_LIM_NOFILE:
		open_files(20);
		break;
	}
	send_msg(to_parent, MSG_DONE);
	for (;;)
		pause();
}

/* Scenarios 2: forks a grandchild, reports its pid as text, then exits when told. */
static void actor_parent_of_grandchild(int from_parent, int to_parent)
{
	char text[16];
	pid_t grandchild = fork();

	if (grandchild == 0)
		for (;;)
			pause();
	snprintf(text, sizeof(text), "%d\n", grandchild);
	if (write(to_parent, text, strlen(text)) < 0)
		_exit(9);
	wait_msg(from_parent, WAIT_LONG_MS);	/* on cue: exit, which orphans the grandchild */
}

/* Scenario 11: does some work and exits by itself. */
static void actor_short_lived(int from_parent, int to_parent)
{
	send_msg(to_parent, MSG_READY);
	wait_msg(from_parent, WAIT_LONG_MS);
	touch_memory_mb(SHORT_MEM_MB);
	burn_cpu_ms(SHORT_CPU_MS);
	_exit(0);
}

/* Scenario 12: several threads each burn CPU, then all of them idle. */
static volatile int threads_done;

static void *thread_main(void *arg)
{
	(void)arg;
	burn_cpu_ms(THREAD_CPU_MS);
	__sync_fetch_and_add(&threads_done, 1);
	for (;;)
		pause();
	return NULL;
}

static void actor_threads(int from_parent, int to_parent)
{
	pthread_t t;
	int i;

	send_msg(to_parent, MSG_READY);
	wait_msg(from_parent, WAIT_LONG_MS);
	for (i = 0; i < THREAD_COUNT; i++)
		if (pthread_create(&t, NULL, thread_main, NULL))
			_exit(9);
	burn_cpu_ms(THREAD_CPU_MS);
	while (threads_done < THREAD_COUNT)
		sleep_ms(5);
	send_msg(to_parent, MSG_DONE);
	for (;;)
		pause();
}

/* ---- the scenarios ------------------------------------------------------------------------- */

/* Read the pid a parent-of-grandchild actor printed. */
static pid_t read_pid(int fd)
{
	char text[16] = "";
	int i;

	for (i = 0; i < (int)sizeof(text) - 1; i++) {
		int c = wait_msg(fd, WAIT_NORMAL_MS);	/* one byte at a time */

		if (c < 0 || c == '\n')
			break;
		text[i] = c;
	}
	return atoi(text);
}

/* 1. A process may monitor itself. */
static void demo_self(void)
{
	struct pidmon_usage u;
	void *memory;

	title("1. Monitoring myself: start a window, do some work, read");
	say("pidmon_start(0): no limits given, my own rlimits apply", pm_start(0));
	burn_cpu_ms(SELF_CPU_MS);
	memory = touch_memory_mb(SELF_MEM_MB);
	say("pidmon_read(0)", pm_read(0, 0, &u));
	show(&u);
	say("pidmon_read(0, STOP): close the window", pm_read(0, PIDMON_F_STOP, &u));
	munmap(memory, SELF_MEM_MB * BYTES_PER_MB);	/* children forked later must not inherit it */
}

/* Build the family "me -> child -> grandchild"; returns the child and sets *grandchild. */
static pid_t make_family(int *cue, int *report, pid_t *grandchild)
{
	pid_t child = spawn(actor_parent_of_grandchild, cue, report);

	*grandchild = read_pid(*report);
	printf("  tree: me(%d) -> child(%d) -> grandchild(%d)\n", getpid(), child, *grandchild);
	return child;
}

/* 2. Only my own subtree may be monitored, and "my subtree" follows reparenting. */
static void demo_ancestry(void)
{
	struct pidmon_usage u;
	char what[96];
	int cue, report;
	pid_t child, grandchild;

	title("2. Who may be monitored: myself and my descendants, nobody else");
	child = make_family(&cue, &report, &grandchild);
	snprintf(what, sizeof(what), "start my child %d", child);
	say(what, pm_start(child));
	snprintf(what, sizeof(what), "start my grandchild %d", grandchild);
	say(what, pm_start(grandchild));
	snprintf(what, sizeof(what), "start my parent %d", getppid());
	say(what, pm_start(getppid()));
	say("start pid 1 (init)", pm_start(1));
	say("start a pid that does not exist", pm_start(NONEXISTENT_PID));
	say("read grandchild while its parent is alive", pm_read(grandchild, 0, &u));

	printf("\n  -- the child exits: the grandchild is orphaned and adopted by init --\n");
	send_msg(cue, MSG_EXIT);
	waitpid(child, NULL, 0);
	wait_reparented(grandchild, child);
	printf("  the grandchild's parent is now pid %d (not me)\n", parent_of(grandchild));
	say("read grandchild after reparenting", pm_read(grandchild, 0, &u));
	say("start grandchild after reparenting", pm_start(grandchild));
	pm_read(child, PIDMON_F_STOP, &u);		/* the child's own window: free it */
	kill(grandchild, SIGKILL);
	/*
	 * The orphan's monitor cannot be read or stopped while the orphan lives (EPERM). Once it is
	 * dead, one read frees the record and reports EPERM a last time.
	 */
	while (parent_of(grandchild) != -1)
		sleep_ms(10);
	say("read the dead orphan (frees its record)", pm_read(grandchild, 0, &u));
	close(cue);
	close(report);

	printf("\n  -- the same, but I declare myself a subreaper (PR_SET_CHILD_SUBREAPER) --\n");
	prctl(PR_SET_CHILD_SUBREAPER, 1);
	child = make_family(&cue, &report, &grandchild);
	say("start grandchild", pm_start(grandchild));
	send_msg(cue, MSG_EXIT);
	waitpid(child, NULL, 0);
	wait_reparented(grandchild, child);
	printf("  the grandchild's parent is now pid %d (me)\n", parent_of(grandchild));
	say("read grandchild after reparenting", pm_read(grandchild, 0, &u));
	pm_read(grandchild, PIDMON_F_STOP, &u);
	kill(grandchild, SIGKILL);
	waitpid(grandchild, NULL, 0);
	prctl(PR_SET_CHILD_SUBREAPER, 0);
	close(cue);
	close(report);
}

/* 3. The report: what a busy child used, and how the window differs from its whole life. */
static void demo_report(void)
{
	struct pidmon_usage u;
	int cue, report;
	pid_t worker;

	title("3. A full report: a worker that uses %d MB, %d files, %d ms of CPU and some I/O",
	      WORKER_MEM_MB, WORKER_FILES, WORKER_CPU_MS);
	worker = spawn(actor_worker, &cue, &report);
	wait_msg(report, WAIT_NORMAL_MS);			/* worker has already burnt its first CPU */
	printf("  before the window the worker burnt %d ms of CPU: that is in cpu_total_ns only\n",
	       WORKER_PRE_CPU_MS);
	say("pidmon_start(worker)", pm_start(worker));
	send_msg(cue, MSG_GO);
	wait_msg(report, WAIT_LONG_MS);				/* worker has finished its work */
	say("pidmon_read(worker)", pm_read(worker, 0, &u));
	show_full(&u);
	printf("  expected in the window: ~%d ms CPU, ~%d KB read, ~%d KB written, %d more fds\n",
	       WORKER_CPU_MS, WORKER_READ_KB, WORKER_WRITE_KB, WORKER_FILES);
	put_down(worker, cue, report);
}

/* One gauge line for scenario 4. */
static void show_memory(const char *when, const struct pidmon_usage *u)
{
	printf("    %-26s rss %6.1f MB (peak %6.1f) | virtual %6.1f MB (peak %6.1f) | data %6.1f | locked %5.1f\n",
	       when, to_mb(u->rss_bytes), to_mb(u->rss_peak), to_mb(u->vm_bytes), to_mb(u->vm_peak),
	       to_mb(u->data_vm), to_mb(u->locked_vm));
}

/* 4. Memory: gauges follow the address space, peaks remember the high point. */
static void demo_memory(void)
{
	struct pidmon_usage u;
	int cue, report, locked;
	pid_t child;

	title("4. Memory: gauges follow the address space, peaks remember the high point");
	child = spawn(actor_memory, &cue, &report);
	wait_msg(report, WAIT_NORMAL_MS);
	say("pidmon_start(child)  (this also restarts its peak marks)", pm_start(child));
	pm_read(child, 0, &u);
	show_memory("at the start", &u);

	send_msg(cue, '1');
	wait_msg(report, WAIT_NORMAL_MS);
	pm_read(child, 0, &u);
	show_memory("took 24 MB", &u);

	send_msg(cue, '2');
	locked = wait_msg(report, WAIT_NORMAL_MS) == MSG_DONE;
	pm_read(child, 0, &u);
	show_memory(locked ? "mlocked 4 MB more" : "mlock refused, 4 MB more", &u);

	send_msg(cue, '3');
	wait_msg(report, WAIT_NORMAL_MS);
	pm_read(child, 0, &u);
	show_memory("freed everything", &u);
	printf("  the gauges dropped, the peaks (rss_peak, vm_peak) did not\n");
	put_down(child, cue, report);
}

/* Print the limits a window judges and the target's actual rlimit for comparison. */
static void show_real_nofile(pid_t pid)
{
	struct rlimit rl;

	if (prlimit(pid, RLIMIT_NOFILE, NULL, &rl) == 0)
		printf("    prlimit(NOFILE) of the target itself: soft %llu, hard %llu (never touched by pidmon)\n",
		       ULL(rl.rlim_cur), ULL(rl.rlim_max));
}

/* 5. Limits: the target's own by default, or the ones given to pidmon_start. */
static void demo_limits(void)
{
	struct pidmon_limits lim;
	struct pidmon_usage u;
	int cue, report;
	pid_t child;

	title("5. Limits: the target's own rlimits by default, or the ones given to pidmon_start");
	child = spawn(actor_idle, &cue, &report);
	wait_msg(report, WAIT_NORMAL_MS);
	{
		struct rlimit rl = { 123, 456 };

		prlimit(child, RLIMIT_NOFILE, &rl, NULL);	/* give it a recognisable own limit */
	}

	say("pidmon_start(child) with no limits struct", pm_start(child));
	pm_read(child, 0, &u);
	printf("  the limits in force are all the target's own:\n");
	show_limits(&u, 0);
	pm_read(child, PIDMON_F_STOP, &u);

	lim_clear(&lim);
	lim_set(&lim, PIDMON_LIM_RSS, MB(64), MB(128));
	lim_set(&lim, PIDMON_LIM_NOFILE, 50, 60);
	say("pidmon_start(child) giving only RSS and NOFILE", pm_start_lim(child, 0, &lim));
	pm_read(child, 0, &u);
	printf("  given limits replace the target's own for those two; the rest stay the target's:\n");
	show_limits(&u, lim.set);
	show_real_nofile(child);
	pm_read(child, PIDMON_F_STOP, &u);
	put_down(child, cue, report);
}

/* 6. Enforcement in the window: no read is needed. Soft limit warns once, hard limit kills. */
static void demo_enforcement(void)
{
	struct pidmon_limits lim;
	struct pidmon_usage u;
	int cue, report, status;
	pid_t greedy;

	title("6. Enforcement without any read: RSS soft %d MB / hard %d MB, given to pidmon_start",
	      RSS_SOFT_MB, RSS_HARD_MB);
	greedy = spawn(actor_greedy, &cue, &report);
	wait_msg(report, WAIT_NORMAL_MS);			/* it now holds RSS_FIRST_MB */
	printf("  the child holds %d MB: over the soft limit, under the hard limit\n", RSS_FIRST_MB);
	lim_clear(&lim);
	lim_set(&lim, PIDMON_LIM_RSS, MB(RSS_SOFT_MB), MB(RSS_HARD_MB));
	say("pidmon_start(greedy, RSS limits)", pm_start_lim(greedy, 0, &lim));

	printf("  no read yet: the window itself warns the child within about 10 ms\n");
	printf("    child says: %s\n", wait_for(report, MSG_TERM, WAIT_NORMAL_MS) ?
	       "I received SIGTERM (a warning, I can still react)" : "no signal arrived?!");

	say("pidmon_read #1: reports the breach", pm_read(greedy, 0, &u));
	show(&u);
	say("pidmon_read #2: reading again signals nothing", pm_read(greedy, 0, &u));
	show(&u);
	printf("    child says: %s\n", wait_for(report, MSG_TERM, QUIET_MS) ?
	       "a second warning?!" : "(no second warning: each severity is sent once)");

	send_msg(cue, MSG_GO);					/* grow past the hard limit */
	waitpid(greedy, &status, 0);
	printf("  the child was growing to %d MB: over the hard limit, and nobody called pidmon_read\n",
	       RSS_FIRST_MB + RSS_GROW_MB);
	if (WIFSIGNALED(status))
		printf("    child ended: killed by signal %d (%s), although it had been warned before\n",
		       WTERMSIG(status), signame(WTERMSIG(status)));
	else
		printf("    child ended: exit status %d\n", WEXITSTATUS(status));
	say("pidmon_read #3 after it was killed and reaped, with STOP", pm_read(greedy, PIDMON_F_STOP, &u));
	show(&u);
	close(cue);
	close(report);
}

/* The soft limit given for each limit in scenario 7, and what the child does to pass it. */
static const struct {
	int which;
	uint64_t soft;
	const char *action;
} breaches[] = {
	{ PIDMON_LIM_CPU,     1,        "burns 1.3 s of CPU, soft limit 1 s" },
	{ PIDMON_LIM_AS,      MB(48),   "maps 64 MB of address space (untouched), soft 48 MB" },
	{ PIDMON_LIM_RSS,     MB(8),    "touches 16 MB, soft 8 MB" },
	{ PIDMON_LIM_DATA,    MB(16),   "maps 32 MB of private data, soft 16 MB" },
	{ PIDMON_LIM_STACK,   MB(1),    "grows its stack to 3 MB, soft 1 MB" },
	{ PIDMON_LIM_MEMLOCK, MB(1),    "mlocks 4 MB, soft 1 MB" },
	{ PIDMON_LIM_NOFILE,  10,       "opens 20 files, soft 10" },
};

/* 7. Every one of the seven limits, one child each. */
static void demo_every_limit(void)
{
	struct pidmon_limits lim;
	struct pidmon_usage u;
	unsigned int i;

	title("7. Every limit, one child each: a soft breach is answered with one warning");
	printf("  %-8s %-58s %-22s %s\n", "limit", "what the child does", "warning", "reported by pidmon_read");
	for (i = 0; i < sizeof(breaches) / sizeof(breaches[0]); i++) {
		int which = breaches[i].which, cue, report, got, failed = 0;
		double sent;
		pid_t child;

		breach_which = which;
		child = spawn(actor_breacher, &cue, &report);
		wait_msg(report, WAIT_NORMAL_MS);
		lim_clear(&lim);
		lim_set(&lim, which, breaches[i].soft, UNLIMITED);
		if (pm_start_lim(child, 0, &lim)) {
			printf("  %-8s pidmon_start failed\n", limit_names[which]);
			put_down(child, cue, report);
			continue;
		}
		sent = now_ms();
		send_msg(cue, MSG_GO);
		got = wait_for(report, MSG_TERM, WAIT_NORMAL_MS);
		sent = now_ms() - sent;
		if (!got && wait_msg(report, 10) == MSG_FAIL)
			failed = 1;
		pm_read(child, 0, &u);
		if (failed)
			printf("  %-8s %-58s (this system refused the action)\n", limit_names[which],
			       breaches[i].action);
		else
			printf("  %-8s %-58s %-8s after %5.0f ms  breached [%s], sent %s\n", limit_names[which],
			       breaches[i].action, got ? "received" : "MISSING", sent,
			       limit_list(u.exceeded_soft), signame(u.signal_sent));
		put_down(child, cue, report);
	}
	printf("  CPU is answered with SIGXCPU, everything else with SIGTERM; a hard breach is always SIGKILL.\n");
}

/* 8. A report-only window records breaches and sends nothing. */
static void demo_no_enforce(void)
{
	struct pidmon_limits lim;
	struct pidmon_usage u;
	int cue, report;
	pid_t greedy;

	title("8. Report only: PIDMON_F_NO_ENFORCE judges the target but never signals it");
	greedy = spawn(actor_greedy, &cue, &report);
	wait_msg(report, WAIT_NORMAL_MS);
	lim_clear(&lim);
	lim_set(&lim, PIDMON_LIM_RSS, MB(RSS_SOFT_MB), MB(RSS_HARD_MB));
	say("pidmon_start(greedy, NO_ENFORCE)", pm_start_lim(greedy, PIDMON_F_NO_ENFORCE, &lim));
	printf("    child says: %s\n", wait_for(report, MSG_TERM, QUIET_MS) ?
	       "I received a signal?!" : "(no signal, as asked)");
	send_msg(cue, MSG_GO);					/* past the hard limit as well */
	wait_for(report, MSG_DONE, WAIT_NORMAL_MS);
	say("pidmon_read", pm_read(greedy, 0, &u));
	show(&u);
	printf("  the child is %s although it is over both limits\n", kill(greedy, 0) == 0 ? "still alive" : "dead?!");
	put_down(greedy, cue, report);
}

/* 9. The window remembers a breach after the target has gone back under the limit. */
static void demo_sticky(void)
{
	struct pidmon_limits lim;
	struct pidmon_usage u;
	int cue, report;
	pid_t greedy;

	title("9. A breach is remembered: the report covers the whole window, not just the moment of the read");
	greedy = spawn(actor_greedy, &cue, &report);
	wait_msg(report, WAIT_NORMAL_MS);
	lim_clear(&lim);
	lim_set(&lim, PIDMON_LIM_RSS, MB(RSS_SOFT_MB), MB(RSS_ROOMY_HARD_MB));
	say("pidmon_start(greedy): holds 16 MB, soft limit 8 MB", pm_start_lim(greedy, 0, &lim));
	printf("    child says: %s\n", wait_for(report, MSG_TERM, WAIT_NORMAL_MS) ?
	       "I was warned" : "no warning?!");
	send_msg(cue, MSG_FREE);
	wait_for(report, MSG_DONE, WAIT_NORMAL_MS);
	printf("  the child has now given all its memory back\n");
	say("pidmon_read", pm_read(greedy, 0, &u));
	show(&u);
	printf("  rss is %.1f MB now, under the limit, yet the soft breach and the signal are still reported\n",
	       to_mb(u.rss_bytes));
	put_down(greedy, cue, report);
}

/* 10. Enforcement lives exactly as long as the window. */
static void demo_window_scope(void)
{
	struct pidmon_limits lim;
	struct pidmon_usage u;
	int cue, report;
	pid_t greedy;

	title("10. Enforcement exists only inside the window");
	greedy = spawn(actor_greedy, &cue, &report);
	wait_msg(report, WAIT_NORMAL_MS);
	lim_clear(&lim);
	lim_set(&lim, PIDMON_LIM_RSS, MB(RSS_SOFT_MB), MB(RSS_ROOMY_HARD_MB));

	printf("  the child holds %d MB, over a limit nobody has set yet\n", RSS_FIRST_MB);
	printf("    child says: %s\n", wait_for(report, MSG_TERM, QUIET_MS) ?
	       "I was warned?!" : "(nothing happens: there is no window)");
	say("pidmon_start: the target is already over the limit", pm_start_lim(greedy, 0, &lim));
	printf("    child says: %s\n", wait_for(report, MSG_TERM, WAIT_NORMAL_MS) ?
	       "I was warned at once" : "no warning?!");
	say("pidmon_read(STOP): close the window", pm_read(greedy, PIDMON_F_STOP, &u));

	send_msg(cue, MSG_GO);
	wait_for(report, MSG_DONE, WAIT_NORMAL_MS);
	printf("  the child grew by %d MB with the window closed\n", RSS_GROW_MB);
	printf("    child says: %s\n", wait_for(report, MSG_TERM, QUIET_MS) ?
	       "I was warned?!" : "(nothing happens: the window is closed)");
	say("pidmon_read after STOP", pm_read(greedy, 0, &u));
	put_down(greedy, cue, report);
}

/* 11. A target that exited: its final numbers are kept, even as an unreaped zombie. */
static void demo_exited(void)
{
	struct pidmon_usage u;
	siginfo_t info;
	int cue, report;
	pid_t child;

	title("11. The numbers of a child that already exited are not lost");
	child = spawn(actor_short_lived, &cue, &report);
	wait_msg(report, WAIT_NORMAL_MS);
	say("pidmon_start(short-lived child)", pm_start(child));
	send_msg(cue, MSG_GO);
	waitid(P_PID, child, &info, WEXITED | WNOWAIT);		/* it exited; do not reap it yet */
	printf("  the child ran (%d MB, %d ms CPU) and exited; I have NOT reaped it: it is a zombie\n",
	       SHORT_MEM_MB, SHORT_CPU_MS);
	say("pidmon_read(zombie)", pm_read(child, 0, &u));
	show(&u);
	say("pidmon_start(zombie): no new window on a finished process", pm_start(child));
	waitpid(child, NULL, 0);
	printf("  now reaped: its pid is gone, but the window's final numbers are still there\n");
	say("pidmon_read(child) after reaping, with STOP", pm_read(child, PIDMON_F_STOP, &u));
	show(&u);
	say("pidmon_read(child) again (STOP released the monitor)", pm_read(child, 0, &u));
	close(cue);
	close(report);
}

/* 12. A pid names a whole process: its threads are added up, a thread id is not a valid target. */
static void demo_threads(void)
{
	struct pidmon_usage u;
	struct dirent *e;
	pid_t thread_id = 0;
	char path[64], what[96];
	int cue, report;
	DIR *dir;
	pid_t child;

	title("12. Threads: a pid is a whole process (%d threads burning %d ms each)",
	      THREAD_COUNT + 1, THREAD_CPU_MS);
	child = spawn(actor_threads, &cue, &report);
	wait_msg(report, WAIT_NORMAL_MS);
	say("pidmon_start(child)", pm_start(child));
	send_msg(cue, MSG_GO);
	wait_for(report, MSG_DONE, WAIT_LONG_MS);
	say("pidmon_read(child)", pm_read(child, 0, &u));
	show(&u);
	printf("  cpu is summed over %llu threads: %.0f ms of CPU in a %.0f ms window (%ld cpus online)\n",
	       ULL(u.nr_threads), to_ms(u.utime_ns + u.stime_ns), to_ms(u.wall_ns),
	       sysconf(_SC_NPROCESSORS_ONLN));

	snprintf(path, sizeof(path), "/proc/%d/task", child);
	dir = opendir(path);
	while (dir && (e = readdir(dir)))
		if (e->d_name[0] != '.' && atoi(e->d_name) != child)
			thread_id = atoi(e->d_name);
	if (dir)
		closedir(dir);
	snprintf(what, sizeof(what), "start with a thread id (%d), not the process id", thread_id);
	say(what, pm_start(thread_id));
	snprintf(what, sizeof(what), "read with a thread id (%d)", thread_id);
	say(what, pm_read(thread_id, 0, &u));
	put_down(child, cue, report);
}

/* 13. Records belong to the process that made them. */
static void demo_scope(void)
{
	struct pidmon_usage u;
	int cue, report, status;
	pid_t target, forked;

	title("13. Monitors belong to their owner: a forked child starts with none");
	target = spawn(actor_idle, &cue, &report);
	wait_msg(report, WAIT_NORMAL_MS);
	say("pidmon_start(target)", pm_start(target));
	say("pidmon_start(0): a window on myself too", pm_start(0));

	forked = fork();
	if (forked == 0) {
		printf("  in a child I forked after those starts (pid %d):\n", getpid());
		say("  pidmon_read(my parent's target)", pm_read(target, 0, &u));
		say("  pidmon_read(0): nobody started one for me", pm_read(0, 0, &u));
		say("  pidmon_start(my parent's target): a sibling", pm_start(target));
		_exit(0);
	}
	waitpid(forked, &status, 0);
	say("back in the owner: pidmon_read(target) still works", pm_read(target, PIDMON_F_STOP, &u));
	pm_read(0, PIDMON_F_STOP, &u);
	put_down(target, cue, report);
}

/* 14. An owner may hold at most 64 monitors. */
static void demo_cap(void)
{
	enum { CAP = 64 };
	pid_t kids[CAP + CAP_EXTRA];
	struct pidmon_usage u;
	int i, ok = 0, rc = 0;

	title("14. At most %d monitors per owner", CAP);
	for (i = 0; i < CAP + CAP_EXTRA; i++) {
		kids[i] = fork();
		if (kids[i] == 0)
			for (;;)
				pause();
	}
	for (i = 0; i < CAP + CAP_EXTRA; i++) {
		rc = pm_start(kids[i]);
		if (rc == 0)
			ok++;
		else
			break;
	}
	printf("  %d starts accepted; the next one:\n", ok);
	say("pidmon_start(one process too many)", rc);
	say("pidmon_read(first, STOP): free one slot", pm_read(kids[0], PIDMON_F_STOP, &u));
	say("pidmon_start(the refused one) again", pm_start(kids[ok]));
	for (i = 0; i < CAP + CAP_EXTRA; i++) {
		pm_read(kids[i], PIDMON_F_STOP, &u);
		kill(kids[i], SIGKILL);
		waitpid(kids[i], NULL, 0);
	}
}

/* 15. Every error, in one place. */
static void demo_errors(void)
{
	struct pidmon_limits lim;
	struct pidmon_usage u;
	unsigned char fat[512];
	long rc;

	title("15. Errors");
	say("start: unknown flag", pm_start_lim(0, 0x80, NULL));
	say("start: a read flag (STOP)", pm_start_lim(0, PIDMON_F_STOP, NULL));
	say("start: negative pid", pm_start(-1));
	say("read: unknown flag", pm_read(0, 0x80, &u));
	say("read: a start flag (NO_ENFORCE)", pm_read(0, PIDMON_F_NO_ENFORCE, &u));
	say("read: negative pid", pm_read(-1, 0, &u));
	say("read: nothing was started yet", pm_read(0, 0, &u));
	say("read: a pid that does not exist", pm_read(NONEXISTENT_PID, 0, &u));
	say("start: a pid that does not exist", pm_start(NONEXISTENT_PID));

	lim_clear(&lim);
	lim_set(&lim, PIDMON_LIM_RSS, 10, 5);
	say("limits: soft above hard", pm_start_lim(0, 0, &lim));
	lim_clear(&lim);
	lim.set = 1U << PIDMON_NLIM;
	say("limits: a limit bit that does not exist", pm_start_lim(0, 0, &lim));
	lim_clear(&lim);
	rc = syscall(__NR_pidmon_start, 0, 0, &lim, 8);
	say("limits: size too small (8 bytes)", rc < 0 ? errno : 0);
	rc = syscall(__NR_pidmon_start, 0, 0, &lim, 1 << 20);
	say("limits: size too big (1 MB)", rc < 0 ? errno : 0);
	rc = syscall(__NR_pidmon_start, 0, 0, NULL, sizeof(lim));
	say("limits: a size but no struct", rc < 0 ? errno : 0);
	rc = syscall(__NR_pidmon_start, 0, 0, (void *)1, sizeof(lim));
	say("limits: a bad pointer", rc < 0 ? errno : 0);
	memset(fat, 0, sizeof(fat));
	fat[sizeof(lim) + 5] = 1;
	rc = syscall(__NR_pidmon_start, 0, 0, fat, sizeof(fat));
	say("limits: a longer struct with an unknown field set", rc < 0 ? errno : 0);
	fat[sizeof(lim) + 5] = 0;
	rc = syscall(__NR_pidmon_start, 0, PIDMON_F_NO_ENFORCE, fat, sizeof(fat));
	say("limits: a longer struct, extra part all zero", rc < 0 ? errno : 0);

	rc = syscall(__NR_pidmon_read, 0, 0, &u, 8);
	say("read: buffer too small (8 bytes)", rc < 0 ? errno : 0);
	rc = syscall(__NR_pidmon_read, 0, 0, &u, 1 << 20);
	say("read: buffer too big (1 MB)", rc < 0 ? errno : 0);
	rc = syscall(__NR_pidmon_read, 0, 0, (void *)1, sizeof(u));
	say("read: a bad pointer (window stays open)", rc < 0 ? errno : 0);
	say("start: the same target twice", pm_start(0));
	say("read(STOP)", pm_read(0, PIDMON_F_STOP, &u));
	say("read after STOP", pm_read(0, 0, &u));
}

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);	/* flush per line so parent and child output interleave sensibly */
	printf("pidmon demo, pid %d\n", getpid());
	if (system("uname -srm"))
		return 1;

	demo_self();
	demo_ancestry();
	demo_report();
	demo_memory();
	demo_limits();
	demo_enforcement();
	demo_every_limit();
	demo_no_enforce();
	demo_sticky();
	demo_window_scope();
	demo_exited();
	demo_threads();
	demo_scope();
	demo_cap();
	demo_errors();

	printf("\ndone.\n");
	return 0;
}

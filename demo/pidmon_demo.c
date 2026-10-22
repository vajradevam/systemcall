/*
 * pidmon_demo: a guided tour of the pidmon_start / pidmon_read system calls.
 *
 *   gcc -O1 -Wall -isystem <headers_install dir>/include -o pidmon_demo pidmon_demo.c
 *
 * The two calls (glibc has no wrapper, so we use syscall(2)):
 *
 *   pidmon_start(pid, 0)                     open a monitoring window on pid (0 = myself)
 *   pidmon_read(pid, flags, &usage, size)    report what happened in the window, compare it with
 *                                            the target's rlimits and signal it if one is exceeded
 *     flags: PIDMON_F_NO_ENFORCE  report only, never signal
 *            PIDMON_F_STOP        free the monitor after reading
 *
 * The demo has six scenarios (see main). Several of them need a second process doing something
 * on cue, so each "actor" runs in a forked child and takes its cues from the parent through a pipe.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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

static double to_ms(uint64_t ns)    { return (double)ns / NSEC_PER_MSEC; }
static double to_mb(uint64_t bytes) { return (double)bytes / BYTES_PER_MB; }

/*
 * cpu_permille is "per mille of one CPU": 1000 means one CPU fully busy for the whole window
 * (2000 would be two CPUs). Dividing by 10 gives the familiar percentage that top shows.
 */
#define PERMILLE_PER_PERCENT 10.0

static double to_percent(uint64_t permille) { return (double)permille / PERMILLE_PER_PERCENT; }

/* ---- what the scenarios do (all sizes in one place) ---------------------------------------- */

#define WORKER_MEM_MB		32	/* scenario 4: memory the worker touches */
#define WORKER_FILES		10	/* ... files it opens */
#define WORKER_CPU_MS		300	/* ... CPU it burns */

#define RSS_SOFT_MB		8	/* scenario 5: RLIMIT_RSS soft limit */
#define RSS_HARD_MB		48	/* ... and hard limit */
#define RSS_FIRST_MB		16	/* memory held first: above soft, below hard */
#define RSS_GROW_MB		64	/* memory added later: 16 + 64 = 80, above hard */

#define SHORT_MEM_MB		16	/* scenario 6: the short-lived child */
#define SHORT_CPU_MS		150

#define SELF_CPU_MS		100	/* scenario 1: work done by this process itself */
#define SELF_MEM_MB		8

/* How long we wait for a child before giving up. */
#define WAIT_QUICK_MS		2000
#define WAIT_NORMAL_MS		5000
#define WAIT_LONG_MS		20000

/* A pid that cannot exist: above the kernel's maximum of 4194304. */
#define NONEXISTENT_PID		99999999

/* ---- the two system calls ------------------------------------------------------------------ */

/* Both return 0 on success or the errno value, which keeps the call sites short. */
static int pm_start(pid_t pid)
{
	return syscall(__NR_pidmon_start, pid, 0) < 0 ? errno : 0;
}

static int pm_read(pid_t pid, unsigned int flags, struct pidmon_usage *u)
{
	return syscall(__NR_pidmon_read, pid, flags, u, sizeof(*u)) < 0 ? errno : 0;
}

/* ---- printing ------------------------------------------------------------------------------ */

static const char *errname(int e)
{
	switch (e) {
	case 0:		return "ok";
	case EPERM:	return "EPERM: not me or my descendant";
	case ESRCH:	return "ESRCH: no such process";
	case ENOENT:	return "ENOENT: no monitor for it";
	case EBUSY:	return "EBUSY: already monitored";
	default:	return strerror(e);
	}
}

static void title(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void title(const char *fmt, ...)
{
	char text[160];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	printf("\n\033[1m== %s\033[0m\n", text);
}

/* One line: what we asked, and how the kernel answered. */
static void say(const char *what, int e)
{
	printf("  %-52s -> %s\n", what, errname(e));
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

/* exceeded_soft / exceeded_hard are bitmasks: bit n set means limit n is over. */
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

/* Print a whole struct pidmon_usage as four lines. */
static void show(const struct pidmon_usage *u)
{
	const char *note = "";

	if (u->state & PIDMON_ST_EXITED)
		note = " | target has EXITED (these are its final numbers)";
	else if (u->state & PIDMON_ST_ALREADY_SIGNALLED)
		note = " | this severity was signalled by an earlier read";

	printf("    window %7.1f ms | cpu %6.1f ms (user %.1f, sys %.1f) = %.1f%% of one cpu | total cpu since start %.1f ms\n",
	       to_ms(u->wall_ns), to_ms(u->utime_ns + u->stime_ns), to_ms(u->utime_ns), to_ms(u->stime_ns),
	       to_percent(u->cpu_permille), to_ms(u->cpu_total_ns));
	printf("    memory: rss %.1f MB (anon %.1f, file %.1f), peak %.1f MB | virtual %.1f MB, peak %.1f MB | stack %.2f MB\n",
	       to_mb(u->rss_bytes), to_mb(u->rss_anon), to_mb(u->rss_file), to_mb(u->rss_peak),
	       to_mb(u->vm_bytes), to_mb(u->vm_peak), to_mb(u->stack_vm));
	printf("    faults: %llu minor, %llu major | context switches: %llu voluntary, %llu forced | threads %llu | open fds %llu\n",
	       ULL(u->minor_faults), ULL(u->major_faults), ULL(u->vol_ctxsw), ULL(u->invol_ctxsw),
	       ULL(u->nr_threads), ULL(u->nr_open_fds));
	printf("    io: %llu B read, %llu B written | limits over: soft [%s] hard [%s] | signal sent: %s%s\n",
	       ULL(u->rchar), ULL(u->wchar), limit_list(u->exceeded_soft), limit_list(u->exceeded_hard),
	       signame(u->signal_sent), note);
}

/* ---- parent/child plumbing ----------------------------------------------------------------- */

/* One-byte messages between the parent and an actor child. */
enum msg {
	MSG_READY = 'r',	/* child -> parent: set up, waiting for my cue */
	MSG_GO    = 'g',	/* parent -> child: do the next step */
	MSG_DONE  = 'd',	/* child -> parent: step finished */
	MSG_TERM  = 'T',	/* child -> parent: I received SIGTERM */
	MSG_EXIT  = 'x',	/* parent -> child: exit now */
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

	FD_ZERO(&set);
	FD_SET(fd, &set);
	if (select(fd + 1, &set, NULL, NULL, &tv) <= 0 || read(fd, &c, 1) != 1)
		return -1;
	return (unsigned char)c;
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
static void touch_memory_mb(size_t mb)
{
	size_t len = mb * BYTES_PER_MB;
	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (p == MAP_FAILED)
		exit(9);
	memset(p, 1, len);
}

static void open_files(int count)
{
	while (count--)
		if (open("/dev/null", O_RDONLY) < 0)
			exit(9);
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

/* ---- the actors: each runs in its own child process ---------------------------------------- */

/* Scenario 4: uses memory, files and CPU on cue, then idles. */
static void actor_worker(int from_parent, int to_parent)
{
	send_msg(to_parent, MSG_READY);
	wait_msg(from_parent, WAIT_LONG_MS);

	touch_memory_mb(WORKER_MEM_MB);
	open_files(WORKER_FILES);
	burn_cpu_ms(WORKER_CPU_MS);

	send_msg(to_parent, MSG_DONE);
	for (;;)
		pause();
}

static int sigterm_report_fd;

static void on_sigterm(int sig)
{
	(void)sig;
	send_msg(sigterm_report_fd, MSG_TERM);	/* tell the parent the warning reached us */
}

/* Scenario 5: has an RSS limit, holds memory above its soft limit, later grows past its hard limit. */
static void actor_greedy(int from_parent, int to_parent)
{
	struct sigaction sa = { .sa_handler = on_sigterm };
	struct rlimit limit = { .rlim_cur = RSS_SOFT_MB * BYTES_PER_MB, .rlim_max = RSS_HARD_MB * BYTES_PER_MB };

	sigterm_report_fd = to_parent;
	sigaction(SIGTERM, &sa, NULL);
	setrlimit(RLIMIT_RSS, &limit);

	touch_memory_mb(RSS_FIRST_MB);
	send_msg(to_parent, MSG_READY);

	wait_msg(from_parent, WAIT_LONG_MS);
	touch_memory_mb(RSS_GROW_MB);
	send_msg(to_parent, MSG_DONE);
	for (;;)
		pause();
}

/* Scenarios 2 and 3: forks a grandchild, reports its pid as text, then exits when told. */
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

/* Scenario 6: does some work and exits by itself. */
static void actor_short_lived(int from_parent, int to_parent)
{
	send_msg(to_parent, MSG_READY);
	wait_msg(from_parent, WAIT_LONG_MS);
	touch_memory_mb(SHORT_MEM_MB);
	burn_cpu_ms(SHORT_CPU_MS);
	_exit(0);
}

/* ---- the scenarios ------------------------------------------------------------------------- */

/* 1. A process may monitor itself. */
static void demo_self(void)
{
	struct pidmon_usage u;

	title("1. Monitoring myself: start a window, do some work, read");
	say("pidmon_start(0)", pm_start(0));
	burn_cpu_ms(SELF_CPU_MS);
	touch_memory_mb(SELF_MEM_MB);
	say("pidmon_read(0)", pm_read(0, 0, &u));
	show(&u);
	pm_read(0, PIDMON_F_STOP, &u);		/* release the monitor */
}

/* 2 and 3. Only my own subtree may be monitored, and "my subtree" follows reparenting. */
static void demo_ancestry(void)
{
	struct pidmon_usage u;
	char text[16] = "";
	char what[80];
	int cue, report, i;
	pid_t child, grandchild;

	title("2. Who may be monitored: myself and my descendants, nobody else");
	child = spawn(actor_parent_of_grandchild, &cue, &report);
	for (i = 0; i < (int)sizeof(text) - 1 && read(report, &text[i], 1) == 1 && text[i] != '\n'; i++)
		;
	grandchild = atoi(text);
	printf("  tree: me(%d) -> child(%d) -> grandchild(%d)\n", getpid(), child, grandchild);

	snprintf(what, sizeof(what), "start my child %d", child);
	say(what, pm_start(child));
	snprintf(what, sizeof(what), "start my grandchild %d", grandchild);
	say(what, pm_start(grandchild));
	snprintf(what, sizeof(what), "start my parent %d", getppid());
	say(what, pm_start(getppid()));
	say("start pid 1 (init)", pm_start(1));
	say("start a pid that does not exist", pm_start(NONEXISTENT_PID));

	title("3. The grandchild gets orphaned: it is no longer my descendant");
	say("read grandchild while its parent is alive", pm_read(grandchild, 0, &u));
	send_msg(cue, MSG_EXIT);		/* the child exits; the kernel reparents the grandchild */
	waitpid(child, NULL, 0);
	while (parent_of(grandchild) == child)	/* reparenting happens just after the exit */
		sleep_ms(10);
	printf("  child %d exited; the grandchild's parent is now pid %d (not me)\n", child, parent_of(grandchild));
	say("read grandchild after reparenting", pm_read(grandchild, 0, &u));
	say("start grandchild after reparenting", pm_start(grandchild));
	kill(grandchild, SIGKILL);
}

/* 4. The report: what a busy child used. */
static void demo_report(void)
{
	struct pidmon_usage u;
	int cue, report;
	pid_t worker;

	title("4. Reporting: a worker that uses %d MB, %d ms of CPU and %d files",
	      WORKER_MEM_MB, WORKER_CPU_MS, WORKER_FILES);
	worker = spawn(actor_worker, &cue, &report);
	wait_msg(report, WAIT_NORMAL_MS);			/* worker is ready */
	say("pidmon_start(worker)", pm_start(worker));
	send_msg(cue, MSG_GO);
	wait_msg(report, WAIT_LONG_MS);				/* worker has finished its work */
	say("pidmon_read(worker)", pm_read(worker, 0, &u));
	show(&u);
	kill(worker, SIGKILL);
	waitpid(worker, NULL, 0);
}

/* 5. Enforcement: soft limit warns once, NO_ENFORCE only reports, hard limit kills. */
static void demo_enforcement(void)
{
	struct pidmon_usage u;
	int cue, report, status;
	pid_t greedy;

	title("5. Enforcement: RSS limit soft %d MB / hard %d MB (Linux never enforces RLIMIT_RSS itself)",
	      RSS_SOFT_MB, RSS_HARD_MB);
	greedy = spawn(actor_greedy, &cue, &report);
	wait_msg(report, WAIT_NORMAL_MS);			/* it now holds RSS_FIRST_MB */
	printf("  the child holds %d MB: over the soft limit, under the hard limit\n", RSS_FIRST_MB);
	say("pidmon_start(greedy)", pm_start(greedy));

	say("pidmon_read #1: over the soft limit", pm_read(greedy, 0, &u));
	show(&u);
	printf("    child says: %s\n", wait_msg(report, WAIT_QUICK_MS) == MSG_TERM ?
	       "I received SIGTERM (a warning, I can still react)" : "no signal arrived?!");

	say("pidmon_read #2: still over, but already warned", pm_read(greedy, 0, &u));
	show(&u);

	say("pidmon_read #3 with NO_ENFORCE (report only)", pm_read(greedy, PIDMON_F_NO_ENFORCE, &u));
	printf("    signal sent: %s\n", signame(u.signal_sent));

	send_msg(cue, MSG_GO);					/* grow past the hard limit */
	wait_msg(report, WAIT_NORMAL_MS);
	printf("  the child now holds %d MB: over the hard limit\n", RSS_FIRST_MB + RSS_GROW_MB);
	say("pidmon_read #4", pm_read(greedy, 0, &u));
	show(&u);

	waitpid(greedy, &status, 0);
	if (WIFSIGNALED(status))
		printf("    child ended: killed by signal %d (%s)\n", WTERMSIG(status), signame(WTERMSIG(status)));
	else
		printf("    child ended: exit status %d\n", WEXITSTATUS(status));
	say("pidmon_read #5 after it was killed and reaped", pm_read(greedy, 0, &u));
	show(&u);
}

/* 6. A child that exits and is reaped before we read: its numbers are kept. */
static void demo_exited_child(void)
{
	struct pidmon_usage u;
	int cue, report;
	pid_t child;

	title("6. The numbers of a child that already exited are not lost");
	child = spawn(actor_short_lived, &cue, &report);
	wait_msg(report, WAIT_NORMAL_MS);
	say("pidmon_start(short-lived child)", pm_start(child));
	send_msg(cue, MSG_GO);
	waitpid(child, NULL, 0);
	printf("  the child ran (%d MB, %d ms CPU), exited, and was reaped by me\n", SHORT_MEM_MB, SHORT_CPU_MS);
	say("pidmon_read(child), afterwards, with STOP", pm_read(child, PIDMON_F_STOP, &u));
	show(&u);
	say("pidmon_read(child) again (STOP released the monitor)", pm_read(child, 0, &u));
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
	demo_enforcement();
	demo_exited_child();

	printf("\ndone.\n");
	return 0;
}

/* guard.c - run a runaway child under pidmon limits and report what happened.
 *
 *   gcc -O1 -Wall -isystem <headers_install dir>/include -o guard guard.c
 *
 * The child allocates 4 MB every 20 ms, without end.  The parent opens a window on it that warns at
 * 24 MB of resident memory and kills at 48 MB, waits for it to die, and prints the final report.
 * Nobody calls pidmon_read while the child runs: the window does the enforcing. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/pidmon.h>

#ifndef __NR_pidmon_start
#define __NR_pidmon_start 473
#endif
#ifndef __NR_pidmon_read
#define __NR_pidmon_read 474
#endif

#define MB (1024UL * 1024)

static const char *const lim_name[PIDMON_NLIM] = {
	"CPU", "AS", "RSS", "DATA", "STACK", "MEMLOCK", "NOFILE"
};

static void warned(int sig)
{
	static const char m[] = "child: warned, but carrying on\n";

	(void)sig;
	if (write(1, m, sizeof(m) - 1) < 0)
		_exit(1);
}

static void runaway(void)
{
	signal(SIGTERM, warned);
	for (;;) {
		void *p = mmap(NULL, 4 * MB, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		if (p != MAP_FAILED)
			memset(p, 1, 4 * MB);
		usleep(20000);
	}
}

static void print_mask(const char *what, unsigned int mask)
{
	int n;

	printf("  %-14s", what);
	if (!mask)
		printf(" none");
	for (n = 0; n < PIDMON_NLIM; n++)
		if (mask & (1U << n))
			printf(" %s", lim_name[n]);
	printf("\n");
}

int main(void)
{
	struct pidmon_limits lim;
	struct pidmon_usage u;
	int status;
	pid_t child;

	child = fork();
	if (child < 0) {
		perror("fork");
		return 1;
	}
	if (child == 0) {
		runaway();
		_exit(0);
	}

	/* only RSS is given; the other six limits stay the child's own rlimits */
	memset(&lim, 0, sizeof(lim));
	lim.set = 1U << PIDMON_LIM_RSS;
	lim.lim[PIDMON_LIM_RSS].soft = 24 * MB;
	lim.lim[PIDMON_LIM_RSS].hard = 48 * MB;

	if (syscall(__NR_pidmon_start, child, 0, &lim, sizeof(lim)) < 0) {
		perror("pidmon_start");
		kill(child, SIGKILL);
		return 1;
	}

	waitpid(child, &status, 0);	/* the window kills it for us */
	if (WIFSIGNALED(status))
		printf("child ended by signal %d (%s)\n", WTERMSIG(status),
		       strsignal(WTERMSIG(status)));

	/* the window outlives the child; read the final numbers and release the slot */
	memset(&u, 0, sizeof(u));
	if (syscall(__NR_pidmon_read, child, PIDMON_F_STOP, &u, sizeof(u)) < 0) {
		perror("pidmon_read");
		return 1;
	}

	printf("final report (%s):\n", (u.state & PIDMON_ST_EXITED) ? "target exited" : "live");
	printf("  window         %.1f ms, cpu %.1f ms\n", u.wall_ns / 1e6, u.cpu_total_ns / 1e6);
	printf("  rss peak       %.1f MB, virtual peak %.1f MB\n", u.rss_peak / (double)MB,
	       u.vm_peak / (double)MB);
	print_mask("exceeded soft", u.exceeded_soft);
	print_mask("exceeded hard", u.exceeded_hard);
	printf("  signal sent    %s\n", u.signal_sent ? strsignal(u.signal_sent) : "none");
	return 0;
}

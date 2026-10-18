/* Shared helpers for the pidmon tests. Every test is one small program: it prints what failed
 * and exits non-zero. Each test forks its own process tree, so a kill cannot hit the runner. */
#ifndef PIDMON_TEST_COMMON_H
#define PIDMON_TEST_COMMON_H

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <sys/types.h>
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

#define UNUSED __attribute__((unused)) static

/* kernel/pidmon.c PIDMON_MAX_RECS: monitors one owner may hold at a time */
#define PIDMON_MAX_RECS_FOR_TEST 64

static int failures;
static const char *test_name = "?";

#define CHECK(cond, ...) do {						\
	if (!(cond)) {							\
		printf("  FAIL %s:%d: %s\n    ", __FILE__, __LINE__, #cond);	\
		printf(__VA_ARGS__);					\
		printf("\n");						\
		failures++;						\
	}								\
} while (0)

/* return value of the syscall: 0 on success, otherwise the (positive) errno */
UNUSED int pm_start(pid_t pid, unsigned int flags)
{
	return syscall(__NR_pidmon_start, pid, flags) < 0 ? errno : 0;
}

UNUSED int pm_read(pid_t pid, unsigned int flags, struct pidmon_usage *u)
{
	return syscall(__NR_pidmon_read, pid, flags, u, sizeof(*u)) < 0 ? errno : 0;
}

#define EXPECT_ERR(what, got, want) \
	CHECK((got) == (want), "%s: got %d (%s), want %d (%s)", what, \
	      got, strerror(got), want, strerror(want))

UNUSED void test_begin(const char *name)
{
	test_name = name;
	setvbuf(stdout, NULL, _IOLBF, 0);
	alarm(60);	/* a hung test must fail, not stall the whole run */
}

UNUSED int test_end(void)
{
	printf("%s: %s\n", test_name, failures ? "FAILED" : "ok");
	return failures ? 1 : 0;
}

UNUSED void die(const char *m)
{
	perror(m);
	_exit(2);
}

/*
 * Fork a child that runs fn(in, out) then exits. The parent gets a pipe to
 * send bytes to the child (*to_child) and one to receive bytes from it
 * (*from_child), used as simple handshakes.
 */
UNUSED pid_t spawn(void (*fn)(int in, int out), int *to_child, int *from_child)
{
	int a[2], b[2];
	pid_t pid;

	if (pipe(a) || pipe(b))
		die("pipe");
	pid = fork();
	if (pid < 0)
		die("fork");
	if (pid == 0) {
		close(a[1]);
		close(b[0]);
		fn(a[0], b[1]);
		_exit(0);
	}
	close(a[0]);
	close(b[1]);
	if (to_child)
		*to_child = a[1];
	else
		close(a[1]);
	if (from_child)
		*from_child = b[0];
	else
		close(b[0]);
	return pid;
}

UNUSED void put_byte(int fd, char c)
{
	if (write(fd, &c, 1) != 1)
		die("put_byte");
}

/* wait up to ms for a byte; returns it, or -1 on timeout / EOF */
UNUSED int get_byte(int fd, int ms)
{
	struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
	fd_set s;
	char c;

	FD_ZERO(&s);
	FD_SET(fd, &s);
	if (select(fd + 1, &s, NULL, NULL, &tv) <= 0)
		return -1;
	return read(fd, &c, 1) == 1 ? (unsigned char)c : -1;
}

/* read exactly n bytes within ms in total; returns 1 on success */
UNUSED int get_exact(int fd, void *buf, size_t n, int ms)
{
	size_t got = 0;

	while (got < n && ms > 0) {
		struct timeval tv = { 0, 50000 };
		fd_set s;

		FD_ZERO(&s);
		FD_SET(fd, &s);
		if (select(fd + 1, &s, NULL, NULL, &tv) > 0) {
			ssize_t r = read(fd, (char *)buf + got, n - got);

			if (r <= 0)
				return 0;
			got += r;
		}
		ms -= 50;
	}
	return got == n;
}

UNUSED void msleep(int ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

UNUSED uint64_t cpu_ns(clockid_t id)
{
	struct timespec ts;

	clock_gettime(id, &ts);
	return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* spin until this thread has used ms of CPU */
UNUSED void burn_ms(int ms)
{
	uint64_t end = cpu_ns(CLOCK_THREAD_CPUTIME_ID) + ms * 1000000ULL;
	volatile unsigned long x = 0;

	while (cpu_ns(CLOCK_THREAD_CPUTIME_ID) < end)
		x = x + 1;
	(void)x;
}

/* map and touch mb megabytes of anonymous memory */
UNUSED void *touch_mb(size_t mb)
{
	void *p = mmap(NULL, mb << 20, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (p == MAP_FAILED)
		die("mmap");
	memset(p, 1, mb << 20);
	return p;
}

/* value in kB of a "Key:" line of /proc/<pid>/status, or -1 */
UNUSED long proc_status(pid_t pid, const char *key)
{
	char path[64], line[256];
	FILE *f;
	long v = -1;

	snprintf(path, sizeof(path), "/proc/%d/status", pid);
	f = fopen(path, "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, key, strlen(key))) {
			v = atol(line + strlen(key));
			break;
		}
	fclose(f);
	return v;
}

UNUSED int count_fds(pid_t pid)
{
	char path[64];
	struct dirent *e;
	DIR *d;
	int n = 0;

	snprintf(path, sizeof(path), "/proc/%d/fd", pid);
	d = opendir(path);
	if (!d)
		return -1;
	while ((e = readdir(d)))
		if (e->d_name[0] != '.')
			n++;
	closedir(d);
	return n;
}

/* parent pid of pid from /proc, or -1 if it does not exist */
UNUSED pid_t proc_ppid(pid_t pid)
{
	char path[64], buf[512];
	int fd, n;
	char *p;

	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	p = strrchr(buf, ')');	/* the comm field may contain spaces */
	return p ? atoi(p + 4) : -1;	/* ") S ppid" */
}

/* state letter of pid from /proc ('R','S','Z',...), or 0 if it does not exist */
UNUSED char proc_state(pid_t pid)
{
	char path[64], buf[512];
	int fd, n;
	char *p;

	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = 0;
	p = strrchr(buf, ')');
	return p ? p[2] : 0;
}

UNUSED void kill_reap(pid_t pid)
{
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
}

UNUSED void sleeper(int in, int out)
{
	(void)in;
	put_byte(out, 'r');
	for (;;)
		pause();
}

/* poll until pred(arg) holds or ~3 s pass */
UNUSED int wait_until(int (*pred)(long), long arg)
{
	int i;

	for (i = 0; i < 300; i++) {
		if (pred(arg))
			return 1;
		msleep(10);
	}
	return 0;
}

#endif /* PIDMON_TEST_COMMON_H */

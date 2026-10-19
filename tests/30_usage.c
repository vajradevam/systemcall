/* The numbers pidmon reports must agree with what the kernel shows in /proc and prlimit(). */
#include "common.h"

#define MB (1024UL * 1024)

/* alloc 32 MB, open files, burn CPU, do some file I/O, then idle */
static void worker(int in, int out)
{
	char buf[4096] = { 0 };
	int i, fd;

	put_byte(out, 'r');
	get_byte(in, 10000);

	touch_mb(32);
	for (i = 0; i < 10; i++)
		if (open("/dev/null", O_RDONLY) < 0)
			die("open");
	burn_ms(300);

	fd = open("/tmp/pidmon_io", O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		die("open tmp");
	for (i = 0; i < 256; i++)	/* 1 MB written, 1 MB read back */
		if (write(fd, buf, sizeof(buf)) != sizeof(buf))
			die("write");
	lseek(fd, 0, SEEK_SET);
	for (i = 0; i < 256; i++)
		if (read(fd, buf, sizeof(buf)) != sizeof(buf))
			die("read");
	put_byte(out, 'd');
	for (;;)
		pause();
}

static const struct { int lim; int rlimit; } map[] = {
	{ PIDMON_LIM_CPU, RLIMIT_CPU }, { PIDMON_LIM_AS, RLIMIT_AS },
	{ PIDMON_LIM_RSS, RLIMIT_RSS }, { PIDMON_LIM_DATA, RLIMIT_DATA },
	{ PIDMON_LIM_STACK, RLIMIT_STACK }, { PIDMON_LIM_MEMLOCK, RLIMIT_MEMLOCK },
	{ PIDMON_LIM_NOFILE, RLIMIT_NOFILE },
};

#define KB(v) ((unsigned long long)(v) * 1024)
#define ABSDIFF(a, b) ((a) > (b) ? (a) - (b) : (b) - (a))

int main(void)
{
	struct pidmon_usage u;
	struct rlimit rl;
	int to, from, i;
	pid_t c;

	test_begin("30_usage");

	c = spawn(worker, &to, &from);
	CHECK(get_byte(from, 5000) == 'r', "worker not ready");
	EXPECT_ERR("start", pm_start(c, 0), 0);
	put_byte(to, 'g');
	CHECK(get_byte(from, 20000) == 'd', "worker did not finish");

	EXPECT_ERR("read", pm_read(c, 0, &u), 0);

	/* time and CPU */
	CHECK(u.wall_ns >= 300000000ULL, "wall_ns=%llu", (unsigned long long)u.wall_ns);
	CHECK(u.utime_ns + u.stime_ns >= 250000000ULL && u.utime_ns + u.stime_ns < 3000000000ULL,
	      "cpu in window = %llu ns", (unsigned long long)(u.utime_ns + u.stime_ns));
	CHECK(u.cpu_total_ns >= u.utime_ns + u.stime_ns, "total %llu < window %llu",
	      (unsigned long long)u.cpu_total_ns, (unsigned long long)(u.utime_ns + u.stime_ns));
	CHECK(u.cpu_permille >= 50 && u.cpu_permille <= 1100, "cpu_permille=%llu",
	      (unsigned long long)u.cpu_permille);
	CHECK(u.vol_ctxsw < 100000 && u.invol_ctxsw < 100000, "ctxsw %llu/%llu",
	      (unsigned long long)u.vol_ctxsw, (unsigned long long)u.invol_ctxsw);

	/* faults: 32 MB touched = 8192 first-touch faults */
	CHECK(u.minor_faults >= 8000, "minor_faults=%llu", (unsigned long long)u.minor_faults);

	/* memory agrees with /proc */
	CHECK(ABSDIFF(u.rss_bytes, KB(proc_status(c, "VmRSS:"))) <= 256 * 1024,
	      "rss_bytes=%llu VmRSS=%ld kB", (unsigned long long)u.rss_bytes, proc_status(c, "VmRSS:"));
	CHECK(ABSDIFF(u.vm_bytes, KB(proc_status(c, "VmSize:"))) <= 64 * 1024,
	      "vm_bytes=%llu VmSize=%ld kB", (unsigned long long)u.vm_bytes, proc_status(c, "VmSize:"));
	CHECK(ABSDIFF(u.stack_vm, KB(proc_status(c, "VmStk:"))) <= 64 * 1024,
	      "stack_vm=%llu VmStk=%ld kB", (unsigned long long)u.stack_vm, proc_status(c, "VmStk:"));
	CHECK(ABSDIFF(u.rss_peak, KB(proc_status(c, "VmHWM:"))) <= 256 * 1024,
	      "rss_peak=%llu VmHWM=%ld kB", (unsigned long long)u.rss_peak, proc_status(c, "VmHWM:"));
	CHECK(u.rss_anon >= 32 * MB, "rss_anon=%llu", (unsigned long long)u.rss_anon);
	CHECK(u.rss_bytes >= u.rss_anon + u.rss_file, "rss parts exceed the whole");
	CHECK(u.rss_peak >= u.rss_bytes, "peak %llu < now %llu",
	      (unsigned long long)u.rss_peak, (unsigned long long)u.rss_bytes);
	CHECK(u.vm_peak >= u.vm_bytes, "vm peak below vm");
	CHECK(u.data_vm >= 32 * MB, "data_vm=%llu", (unsigned long long)u.data_vm);
	CHECK(u.locked_vm == 0, "locked_vm=%llu", (unsigned long long)u.locked_vm);

	/* process */
	CHECK(u.nr_threads == 1, "nr_threads=%llu", (unsigned long long)u.nr_threads);
	CHECK((int)u.nr_open_fds == count_fds(c), "nr_open_fds=%llu /proc says %d",
	      (unsigned long long)u.nr_open_fds, count_fds(c));
	CHECK(u.nr_open_fds >= 13, "nr_open_fds=%llu", (unsigned long long)u.nr_open_fds);

	/* I/O accounting is enabled in our kernel config */
	CHECK(u.wchar >= MB, "wchar=%llu", (unsigned long long)u.wchar);
	CHECK(u.rchar >= MB, "rchar=%llu", (unsigned long long)u.rchar);

	/* limits are the target's rlimits, unit for unit */
	for (i = 0; i < (int)(sizeof(map) / sizeof(map[0])); i++) {
		unsigned long long soft, hard;

		if (prlimit(c, map[i].rlimit, NULL, &rl))
			die("prlimit");
		soft = rl.rlim_cur == RLIM_INFINITY ? ~0ULL : rl.rlim_cur;
		hard = rl.rlim_max == RLIM_INFINITY ? ~0ULL : rl.rlim_max;
		CHECK(u.lim[map[i].lim].soft == soft && u.lim[map[i].lim].hard == hard,
		      "limit %d: pidmon %llx/%llx prlimit %llx/%llx", map[i].lim,
		      (unsigned long long)u.lim[map[i].lim].soft,
		      (unsigned long long)u.lim[map[i].lim].hard, soft, hard);
	}

	/* nothing tripped, nothing signalled */
	CHECK(u.exceeded_soft == 0 && u.exceeded_hard == 0 && u.signal_sent == 0 && u.state == 0,
	      "soft=%x hard=%x sig=%u state=%x", u.exceeded_soft, u.exceeded_hard,
	      u.signal_sent, u.state);
	CHECK(kill(c, 0) == 0, "target must still be alive");

	/* reading again later: window keeps growing, counters never go backwards */
	{
		struct pidmon_usage v;

		msleep(50);
		EXPECT_ERR("read again", pm_read(c, 0, &v), 0);
		CHECK(v.wall_ns > u.wall_ns, "wall did not advance");
		CHECK(v.cpu_total_ns >= u.cpu_total_ns, "cpu went backwards");
		CHECK(v.utime_ns + v.stime_ns >= u.utime_ns + u.stime_ns, "window cpu went backwards");
	}

	kill_reap(c);
	return test_end();
}

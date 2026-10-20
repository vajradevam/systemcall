/* A target that dies after pidmon_start(): the exit hook keeps its final numbers, the monitor is
 * freed with its owner, and a record nobody reads causes no leak or crash. */
#include "common.h"

#define MB (1024ULL * 1024)

/* wait for go, then use 16 MB and 150 ms of CPU, then exit */
static void shortlived(int in, int out)
{
	put_byte(out, 'r');
	get_byte(in, 10000);
	touch_mb(16);
	burn_ms(150);
	_exit(0);
}

static void check_final(const struct pidmon_usage *u, const char *when)
{
	CHECK(u->state & PIDMON_ST_EXITED, "%s: state=%x", when, u->state);
	CHECK(u->rss_peak >= 16 * MB, "%s: rss_peak=%llu", when, (unsigned long long)u->rss_peak);
	CHECK(u->vm_peak >= 16 * MB, "%s: vm_peak=%llu", when, (unsigned long long)u->vm_peak);
	CHECK(u->utime_ns + u->stime_ns >= 100000000ULL, "%s: cpu=%llu", when,
	      (unsigned long long)(u->utime_ns + u->stime_ns));
	CHECK(u->minor_faults >= 3500, "%s: faults=%llu", when, (unsigned long long)u->minor_faults);
	CHECK(u->signal_sent == 0, "%s: signalled a dead process", when);
	CHECK(u->wall_ns >= 150000000ULL, "%s: wall=%llu", when, (unsigned long long)u->wall_ns);
}

/* the monitor process: starts monitors and exits without reading them */
static void abandoning_owner(int in, int out)
{
	int fd;
	pid_t t = spawn(sleeper, NULL, &fd);

	(void)in;
	get_byte(fd, 2000);
	if (pm_start(t, 0) || pm_start(0, 0))
		die("start");
	kill(t, SIGKILL);		/* target dies while we still own its record ... */
	put_byte(out, 'x');
	/* ... and we exit now, holding two records; the exit hooks free them */
}

int main(void)
{
	struct pidmon_usage u, v;
	siginfo_t si;
	int to, from, st, i;
	pid_t c, o;

	test_begin("60_exit");

	/* 1. read while the child is a zombie (not yet reaped): mm is gone, numbers must not be */
	c = spawn(shortlived, &to, &from);
	CHECK(get_byte(from, 5000) == 'r', "not ready");
	EXPECT_ERR("start", pm_start(c, 0), 0);
	put_byte(to, 'g');
	CHECK(waitid(P_PID, c, &si, WEXITED | WNOWAIT) == 0, "waitid");
	EXPECT_ERR("read zombie", pm_read(c, 0, &u), 0);
	check_final(&u, "zombie");

	/* 2. reaped: the record still has them */
	waitpid(c, &st, 0);
	EXPECT_ERR("read reaped", pm_read(c, 0, &v), 0);
	check_final(&v, "reaped");
	CHECK(v.rss_peak == u.rss_peak && v.cpu_total_ns == u.cpu_total_ns && v.wall_ns == u.wall_ns,
	      "the final snapshot must not change between reads");
	EXPECT_ERR("start dead pid", pm_start(c, 0), ESRCH);
	EXPECT_ERR("read STOP", pm_read(c, PIDMON_F_STOP, &v), 0);
	EXPECT_ERR("read after STOP", pm_read(c, 0, &v), ENOENT);

	/* 3. killed by someone else mid-window */
	c = spawn(sleeper, NULL, &from);
	get_byte(from, 2000);
	EXPECT_ERR("start", pm_start(c, 0), 0);
	msleep(20);
	kill(c, SIGKILL);
	waitpid(c, &st, 0);
	EXPECT_ERR("read killed", pm_read(c, 0, &u), 0);
	CHECK(u.state & PIDMON_ST_EXITED, "state=%x", u.state);
	CHECK(u.wall_ns >= 20000000ULL, "wall=%llu", (unsigned long long)u.wall_ns);

	/* 4. an owner that exits holding records */
	o = spawn(abandoning_owner, NULL, &from);
	get_byte(from, 5000);
	waitpid(o, &st, 0);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "owner status %x", st);

	/* 5. many targets die unread, then we die holding the records (cap is 64) */
	for (i = 0; i < 40; i++) {
		c = spawn(shortlived, &to, &from);
		get_byte(from, 5000);
		if (pm_start(c, 0))
			die("start");
		put_byte(to, 'g');
		waitpid(c, NULL, 0);
	}
	/* a few are read, most are not */
	return test_end();
}

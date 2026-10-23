/* A monitor can be re-armed: STOP then start opens a fresh window, a zombie keeps its numbers but
 * cannot be monitored again (and leaves no unreachable record behind), and a full owner can free
 * room and go on. */
#include "common.h"

static void burner(int in, int out)
{
	put_byte(out, 'r');
	for (;;) {
		if (get_byte(in, 10000) < 0)
			_exit(0);
		burn_ms(100);
		put_byte(out, 'd');
	}
}

static void exiter(int in, int out)
{
	put_byte(out, 'r');
	get_byte(in, 10000);
	burn_ms(50);
	_exit(0);
}

/* each part runs in its own forked owner, so the records one leaves behind cannot affect another */
static void part(void (*fn)(void))
{
	int st;
	pid_t p = fork();

	if (p < 0)
		die("fork");
	if (p == 0) {
		fn();
		_exit(failures ? 1 : 0);
	}
	waitpid(p, &st, 0);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "part failed: status %x", st);
}

/* STOP + start: the second window forgets the first */
static void rearm(void)
{
	struct pidmon_usage a, b;
	int to, from;
	pid_t c;

	c = spawn(burner, &to, &from);
	CHECK(get_byte(from, 10000) == 'r', "worker not ready");
	EXPECT_ERR("start 1", pm_start(c, 0), 0);
	put_byte(to, 'g');
	CHECK(get_byte(from, 10000) == 'd', "burn 1");
	EXPECT_ERR("read 1 STOP", pm_read(c, PIDMON_F_STOP, &a), 0);
	CHECK(a.utime_ns + a.stime_ns >= 80000000ULL, "window 1 cpu=%llu",
	      (unsigned long long)(a.utime_ns + a.stime_ns));

	EXPECT_ERR("start 2", pm_start(c, 0), 0);
	EXPECT_ERR("read 2 right away", pm_read(c, 0, &b), 0);
	CHECK(b.utime_ns + b.stime_ns < (a.utime_ns + a.stime_ns) / 2,
	      "window 2 starts from zero: cpu=%llu (window 1 had %llu)",
	      (unsigned long long)(b.utime_ns + b.stime_ns),
	      (unsigned long long)(a.utime_ns + a.stime_ns));
	CHECK(b.wall_ns < a.wall_ns, "wall_ns %llu vs %llu", (unsigned long long)b.wall_ns,
	      (unsigned long long)a.wall_ns);
	CHECK(b.cpu_total_ns >= a.cpu_total_ns, "cpu_total went backwards");
	EXPECT_ERR("start while armed", pm_start(c, 0), EBUSY);
	kill_reap(c);
}

/* start n sleepers, each with a window, and clean up; every start must succeed */
static void fill_slots(int n)
{
	struct pidmon_usage u;
	pid_t k[PIDMON_MAX_RECS_FOR_TEST];
	int i, fd;

	for (i = 0; i < n; i++) {
		k[i] = spawn(sleeper, NULL, &fd);
		get_byte(fd, 2000);
		close(fd);
		EXPECT_ERR("slot", pm_start(k[i], 0), 0);
	}
	for (i = 0; i < n; i++) {
		pm_read(k[i], PIDMON_F_STOP, &u);
		kill_reap(k[i]);
	}
}

/* a zombie that is not reaped keeps its numbers, but no window can be opened on it again */
static void zombie(void)
{
	struct pidmon_usage a, z;
	siginfo_t si;
	int to, from;
	pid_t c;

	c = spawn(exiter, &to, &from);
	CHECK(get_byte(from, 10000) == 'r', "exiter not ready");
	EXPECT_ERR("start", pm_start(c, 0), 0);
	put_byte(to, 'g');
	CHECK(waitid(P_PID, c, &si, WEXITED | WNOWAIT) == 0, "waitid");
	EXPECT_ERR("read zombie", pm_read(c, 0, &a), 0);
	CHECK(a.state & PIDMON_ST_EXITED, "state=%x", a.state);
	/* a window cannot be opened on a process that has finished exiting, and nothing is left behind */
	EXPECT_ERR("start zombie again", pm_start(c, 0), ESRCH);
	EXPECT_ERR("read zombie 2", pm_read(c, 0, &z), 0);
	CHECK(z.state & PIDMON_ST_EXITED, "state=%x", z.state);
	CHECK(z.signal_sent == 0, "signalled a zombie: %u", z.signal_sent);
	CHECK(z.cpu_total_ns == a.cpu_total_ns, "the frozen numbers changed: %llu vs %llu",
	      (unsigned long long)z.cpu_total_ns, (unsigned long long)a.cpu_total_ns);
	waitpid(c, NULL, 0);
	EXPECT_ERR("start reaped", pm_start(c, 0), ESRCH);
	EXPECT_ERR("read STOP", pm_read(c, PIDMON_F_STOP, &a), 0);
	EXPECT_ERR("read after STOP", pm_read(c, 0, &a), ENOENT);
	fill_slots(PIDMON_MAX_RECS_FOR_TEST);	/* all 64 slots are still available */
}

/* fill the owner, free one slot, start one more */
static void slots(void)
{
	struct pidmon_usage a;
	pid_t kids[PIDMON_MAX_RECS_FOR_TEST + 1];
	int fd, i;

	for (i = 0; i <= PIDMON_MAX_RECS_FOR_TEST; i++) {
		kids[i] = spawn(sleeper, NULL, &fd);
		get_byte(fd, 2000);
		close(fd);
	}
	for (i = 0; i < PIDMON_MAX_RECS_FOR_TEST; i++)
		EXPECT_ERR("fill", pm_start(kids[i], 0), 0);
	EXPECT_ERR("full", pm_start(kids[PIDMON_MAX_RECS_FOR_TEST], 0), ENOSPC);
	EXPECT_ERR("free one", pm_read(kids[10], PIDMON_F_STOP, &a), 0);
	EXPECT_ERR("room again", pm_start(kids[PIDMON_MAX_RECS_FOR_TEST], 0), 0);
	EXPECT_ERR("full again", pm_start(kids[10], 0), ENOSPC);
	/* a record whose target died still counts until it is read out or its owner exits */
	kill_reap(kids[3]);
	EXPECT_ERR("dead target's record holds its slot", pm_start(kids[10], 0), ENOSPC);
	EXPECT_ERR("read the dead one out", pm_read(kids[3], PIDMON_F_STOP, &a), 0);
	CHECK(a.state & PIDMON_ST_EXITED, "state=%x", a.state);
	EXPECT_ERR("slot freed", pm_start(kids[10], 0), 0);
	for (i = 0; i <= PIDMON_MAX_RECS_FOR_TEST; i++)
		if (i != 3)
			kill_reap(kids[i]);
}

int main(void)
{
	test_begin("68_restart");
	part(rearm);
	part(zombie);
	part(slots);
	return test_end();
}

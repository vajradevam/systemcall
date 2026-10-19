/* Enforcement: soft limit -> warning signal, hard limit -> SIGKILL, once per severity, never with
 * NO_ENFORCE. Driven by RLIMIT_RSS, which the kernel never enforces itself, plus NOFILE and CPU. */
#include "common.h"

#define MB (1024UL * 1024)

static int ctl_out;

static void on_term(int sig)
{
	char c = 'T';

	(void)sig;
	if (write(ctl_out, &c, 1) < 0)
		_exit(3);
}

static void catch_term(void)
{
	struct sigaction sa = { .sa_handler = on_term };

	sigaction(SIGTERM, &sa, NULL);
}

static void set_limit(int what, rlim_t soft, rlim_t hard)
{
	struct rlimit rl = { soft, hard };

	if (setrlimit(what, &rl))
		die("setrlimit");
}

/* RSS soft 8 MB, hard 96 MB, holds 16 MB */
static void rss_soft(int in, int out)
{
	(void)in;
	ctl_out = out;
	catch_term();
	set_limit(RLIMIT_RSS, 8 * MB, 96 * MB);
	touch_mb(16);
	put_byte(out, 'r');
	for (;;)
		pause();
}

/* RSS soft 8 MB, hard 16 MB, holds 24 MB */
static void rss_hard(int in, int out)
{
	(void)in;
	set_limit(RLIMIT_RSS, 8 * MB, 16 * MB);
	touch_mb(24);
	put_byte(out, 'r');
	for (;;)
		pause();
}

/* 20 fds open, then the NOFILE soft limit is lowered below that */
static void nofile_soft(int in, int out)
{
	int i;

	(void)in;
	ctl_out = out;
	catch_term();
	for (i = 0; i < 20; i++)
		if (open("/dev/null", O_RDONLY) < 0)
			die("open");
	set_limit(RLIMIT_NOFILE, 10, 4096);
	put_byte(out, 'r');
	for (;;)
		pause();
}

/* CPU soft 1 s: ignore SIGXCPU, burn 1.3 s */
static void cpu_soft(int in, int out)
{
	(void)in;
	signal(SIGXCPU, SIG_IGN);
	set_limit(RLIMIT_CPU, 1, 100);
	burn_ms(1300);
	put_byte(out, 'r');
	for (;;)
		pause();
}

/* CPU soft 1 s, hard 2 s: the kernel itself kills this one at 2 s */
static void cpu_hard(int in, int out)
{
	(void)out;
	signal(SIGXCPU, SIG_IGN);
	get_byte(in, 10000);
	set_limit(RLIMIT_CPU, 1, 2);
	burn_ms(6000);
}

static volatile sig_atomic_t self_term;

static void on_self_term(int sig)
{
	(void)sig;
	self_term = 1;
}

int main(void)
{
	struct pidmon_usage u;
	struct sigaction sa = { .sa_handler = on_self_term };
	int to, from, st;
	pid_t c;

	test_begin("50_enforce");

	/* --- soft limit: warned once --- */
	c = spawn(rss_soft, &to, &from);
	CHECK(get_byte(from, 10000) == 'r', "child not ready");
	EXPECT_ERR("start", pm_start(c, 0), 0);
	EXPECT_ERR("read", pm_read(c, 0, &u), 0);
	CHECK(u.exceeded_soft == (1U << PIDMON_LIM_RSS), "exceeded_soft=%x", u.exceeded_soft);
	CHECK(u.exceeded_hard == 0, "exceeded_hard=%x", u.exceeded_hard);
	CHECK(u.signal_sent == SIGTERM, "signal_sent=%u", u.signal_sent);
	CHECK(!(u.state & PIDMON_ST_ALREADY_SIGNALLED), "state=%x", u.state);
	CHECK(get_byte(from, 3000) == 'T', "child's SIGTERM handler did not run");

	EXPECT_ERR("read again", pm_read(c, 0, &u), 0);
	CHECK(u.exceeded_soft == (1U << PIDMON_LIM_RSS), "still over: %x", u.exceeded_soft);
	CHECK(u.signal_sent == 0, "signalled twice: %u", u.signal_sent);
	CHECK(u.state & PIDMON_ST_ALREADY_SIGNALLED, "state=%x", u.state);
	CHECK(get_byte(from, 300) == -1, "second SIGTERM arrived");
	CHECK(kill(c, 0) == 0, "child must be alive after a warning");

	EXPECT_ERR("read STOP", pm_read(c, PIDMON_F_STOP, &u), 0);
	EXPECT_ERR("read after STOP", pm_read(c, 0, &u), ENOENT);
	kill_reap(c);

	/* --- NO_ENFORCE: reported, never signalled --- */
	c = spawn(rss_soft, &to, &from);
	CHECK(get_byte(from, 10000) == 'r', "child not ready");
	EXPECT_ERR("start", pm_start(c, 0), 0);
	EXPECT_ERR("read NO_ENFORCE", pm_read(c, PIDMON_F_NO_ENFORCE, &u), 0);
	CHECK(u.exceeded_soft == (1U << PIDMON_LIM_RSS), "exceeded_soft=%x", u.exceeded_soft);
	CHECK(u.signal_sent == 0, "signalled despite NO_ENFORCE: %u", u.signal_sent);
	CHECK(get_byte(from, 300) == -1, "SIGTERM arrived despite NO_ENFORCE");
	/* and a later enforcing read still acts: NO_ENFORCE must not consume the warning */
	EXPECT_ERR("read", pm_read(c, 0, &u), 0);
	CHECK(u.signal_sent == SIGTERM, "signal_sent=%u", u.signal_sent);
	CHECK(get_byte(from, 3000) == 'T', "SIGTERM did not arrive");
	kill_reap(c);

	/* --- hard limit: killed --- */
	c = spawn(rss_hard, &to, &from);
	CHECK(get_byte(from, 10000) == 'r', "child not ready");
	EXPECT_ERR("start", pm_start(c, 0), 0);
	EXPECT_ERR("read", pm_read(c, 0, &u), 0);
	CHECK(u.exceeded_soft == (1U << PIDMON_LIM_RSS) && u.exceeded_hard == (1U << PIDMON_LIM_RSS),
	      "soft=%x hard=%x", u.exceeded_soft, u.exceeded_hard);
	CHECK(u.signal_sent == SIGKILL, "signal_sent=%u", u.signal_sent);
	waitpid(c, &st, 0);
	CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL, "child status %x", st);
	/* the final numbers survive the kill and the reaping */
	EXPECT_ERR("read after kill", pm_read(c, 0, &u), 0);
	CHECK(u.state & PIDMON_ST_EXITED, "state=%x", u.state);
	CHECK(u.signal_sent == 0, "a dead target is not signalled: %u", u.signal_sent);
	CHECK(u.exceeded_hard == (1U << PIDMON_LIM_RSS), "final hard mask %x", u.exceeded_hard);

	/* --- open files --- */
	c = spawn(nofile_soft, &to, &from);
	CHECK(get_byte(from, 10000) == 'r', "child not ready");
	EXPECT_ERR("start", pm_start(c, 0), 0);
	EXPECT_ERR("read", pm_read(c, 0, &u), 0);
	CHECK(u.exceeded_soft == (1U << PIDMON_LIM_NOFILE), "exceeded_soft=%x", u.exceeded_soft);
	CHECK(u.signal_sent == SIGTERM, "signal_sent=%u", u.signal_sent);
	CHECK(get_byte(from, 3000) == 'T', "SIGTERM did not arrive");
	kill_reap(c);

	/*
	 * --- CPU time ---
	 * The kernel enforces RLIMIT_CPU on its own and raises the soft limit by a second each time
	 * it signals, so by the time we look the soft limit may already have moved. What must hold
	 * is that our verdict agrees with the numbers we report.
	 */
	c = spawn(cpu_soft, &to, &from);
	CHECK(get_byte(from, 20000) == 'r', "child not ready");
	EXPECT_ERR("start", pm_start(c, 0), 0);
	EXPECT_ERR("read", pm_read(c, 0, &u), 0);
	{
		int over = u.cpu_total_ns / 1000000000ULL >= u.lim[PIDMON_LIM_CPU].soft;

		CHECK(u.cpu_total_ns >= 1000000000ULL, "cpu_total=%llu", (unsigned long long)u.cpu_total_ns);
		CHECK(u.lim[PIDMON_LIM_CPU].hard == 100, "hard=%llu", (unsigned long long)u.lim[PIDMON_LIM_CPU].hard);
		CHECK(!!(u.exceeded_soft & (1U << PIDMON_LIM_CPU)) == over,
		      "exceeded_soft=%x but cpu=%llu s, soft limit=%llu s", u.exceeded_soft,
		      (unsigned long long)(u.cpu_total_ns / 1000000000ULL),
		      (unsigned long long)u.lim[PIDMON_LIM_CPU].soft);
		CHECK(u.signal_sent == (over ? SIGXCPU : 0), "signal_sent=%u over=%d", u.signal_sent, over);
		CHECK(!u.exceeded_hard, "exceeded_hard=%x", u.exceeded_hard);
	}
	kill_reap(c);

	/* hard CPU limit: the kernel kills at 2 s, the frozen verdict must say why */
	c = spawn(cpu_hard, &to, &from);
	EXPECT_ERR("start", pm_start(c, 0), 0);
	put_byte(to, 'g');
	waitpid(c, &st, 0);
	CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL, "child status %x", st);
	EXPECT_ERR("read", pm_read(c, 0, &u), 0);
	CHECK(u.state & PIDMON_ST_EXITED, "state=%x", u.state);
	CHECK(u.exceeded_hard & (1U << PIDMON_LIM_CPU), "exceeded_hard=%x", u.exceeded_hard);
	CHECK(u.exceeded_soft & (1U << PIDMON_LIM_CPU), "exceeded_soft=%x", u.exceeded_soft);
	CHECK(u.signal_sent == 0, "a dead target is not signalled: %u", u.signal_sent);
	CHECK(u.cpu_total_ns >= 2000000000ULL, "cpu_total=%llu", (unsigned long long)u.cpu_total_ns);

	/* --- the caller can monitor itself --- */
	sigaction(SIGTERM, &sa, NULL);
	set_limit(RLIMIT_RSS, 4 * MB, RLIM_INFINITY);
	touch_mb(8);
	EXPECT_ERR("start self", pm_start(0, 0), 0);
	EXPECT_ERR("read self", pm_read(0, 0, &u), 0);
	CHECK(u.signal_sent == SIGTERM, "signal_sent=%u", u.signal_sent);
	CHECK(self_term, "own SIGTERM handler did not run");

	return test_end();
}

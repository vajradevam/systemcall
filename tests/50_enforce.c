/* Enforcement happens in the window, between pidmon_start() and the end of the monitor, without
 * anyone calling pidmon_read(): soft limit -> warning signal, hard limit -> SIGKILL, once per
 * severity, never with PIDMON_F_NO_ENFORCE. Limits come from the target's own rlimits or from the
 * struct given to pidmon_start(). Driven by RLIMIT_RSS, which the kernel never enforces itself,
 * plus NOFILE and CPU. */
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

/* burns 1.3 s of CPU once told to, with no rlimit of its own: the monitor's limit is the only one */
static void cpu_soft(int in, int out)
{
	struct sigaction sa = { .sa_handler = on_term };

	ctl_out = out;
	sigaction(SIGXCPU, &sa, NULL);
	get_byte(in, 10000);
	burn_ms(1300);
	put_byte(out, 'r');
	for (;;)
		pause();
}

/* burns CPU for up to 6 s, with no rlimit of its own */
static void cpu_hard(int in, int out)
{
	(void)out;
	signal(SIGXCPU, SIG_IGN);	/* the soft limit's warning must not end it early */
	get_byte(in, 10000);
	burn_ms(6000);
}

/* a bystander with no limits of its own that grows when told, then shrinks again */
static void grower(int in, int out)
{
	void *p;

	catch_term();
	ctl_out = out;
	put_byte(out, 'r');
	get_byte(in, 10000);
	p = touch_mb(24);
	put_byte(out, 'g');
	get_byte(in, 10000);
	munmap(p, 24 << 20);
	put_byte(out, 's');
	for (;;)
		pause();
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
	struct pidmon_limits lim;
	int to, from, st;
	pid_t c;

	test_begin("50_enforce");

	/* --- soft limit: warned once, no pidmon_read() needed --- */
	c = spawn(rss_soft, &to, &from);
	CHECK(get_byte(from, 10000) == 'r', "child not ready");
	EXPECT_ERR("start", pm_start(c, 0), 0);
	CHECK(get_byte(from, 3000) == 'T', "the window did not warn the child by itself");
	CHECK(get_byte(from, 400) == -1, "warned more than once");
	CHECK(kill(c, 0) == 0, "child must be alive after a warning");
	EXPECT_ERR("read", pm_read(c, 0, &u), 0);
	CHECK(u.exceeded_soft == (1U << PIDMON_LIM_RSS), "exceeded_soft=%x", u.exceeded_soft);
	CHECK(u.exceeded_hard == 0, "exceeded_hard=%x", u.exceeded_hard);
	CHECK(u.signal_sent == SIGTERM, "signal_sent=%u", u.signal_sent);
	EXPECT_ERR("read again", pm_read(c, 0, &u), 0);
	CHECK(u.signal_sent == SIGTERM, "signal_sent is the window's, not the call's: %u", u.signal_sent);
	CHECK(get_byte(from, 300) == -1, "reading must not signal");
	EXPECT_ERR("read STOP", pm_read(c, PIDMON_F_STOP, &u), 0);
	EXPECT_ERR("read after STOP", pm_read(c, 0, &u), ENOENT);
	kill_reap(c);

	/* --- NO_ENFORCE: judged and reported, never signalled --- */
	c = spawn(rss_soft, &to, &from);
	CHECK(get_byte(from, 10000) == 'r', "child not ready");
	EXPECT_ERR("start NO_ENFORCE", pm_start(c, PIDMON_F_NO_ENFORCE), 0);
	CHECK(get_byte(from, 600) == -1, "SIGTERM arrived despite NO_ENFORCE");
	EXPECT_ERR("read", pm_read(c, 0, &u), 0);
	CHECK(u.exceeded_soft == (1U << PIDMON_LIM_RSS), "exceeded_soft=%x", u.exceeded_soft);
	CHECK(u.signal_sent == 0, "signalled despite NO_ENFORCE: %u", u.signal_sent);
	CHECK(get_byte(from, 300) == -1, "SIGTERM arrived despite NO_ENFORCE");
	kill_reap(c);

	/* --- hard limit: killed, and the verdict outlives the process --- */
	c = spawn(rss_hard, &to, &from);
	CHECK(get_byte(from, 10000) == 'r', "child not ready");
	EXPECT_ERR("start", pm_start(c, 0), 0);
	waitpid(c, &st, 0);
	CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL, "child status %x", st);
	EXPECT_ERR("read after kill", pm_read(c, 0, &u), 0);
	CHECK(u.state & PIDMON_ST_EXITED, "state=%x", u.state);
	CHECK(u.exceeded_soft == (1U << PIDMON_LIM_RSS) && u.exceeded_hard == (1U << PIDMON_LIM_RSS),
	      "soft=%x hard=%x", u.exceeded_soft, u.exceeded_hard);
	CHECK(u.signal_sent == SIGKILL, "signal_sent=%u", u.signal_sent);

	/* --- open files --- */
	c = spawn(nofile_soft, &to, &from);
	CHECK(get_byte(from, 10000) == 'r', "child not ready");
	EXPECT_ERR("start", pm_start(c, 0), 0);
	CHECK(get_byte(from, 3000) == 'T', "SIGTERM did not arrive");
	EXPECT_ERR("read", pm_read(c, 0, &u), 0);
	CHECK(u.exceeded_soft == (1U << PIDMON_LIM_NOFILE), "exceeded_soft=%x", u.exceeded_soft);
	CHECK(u.signal_sent == SIGTERM, "signal_sent=%u", u.signal_sent);
	kill_reap(c);

	/* --- limits given to pidmon_start() replace the target's own, which are not touched --- */
	c = spawn(rss_soft, &to, &from);	/* its own RSS limit is 8 MB soft, 96 MB hard */
	CHECK(get_byte(from, 10000) == 'r', "child not ready");
	memset(&lim, 0, sizeof(lim));
	lim_set(&lim, PIDMON_LIM_RSS, 64 * MB, 128 * MB);
	EXPECT_ERR("start with roomy RSS", pm_start_lim(c, 0, &lim), 0);
	CHECK(get_byte(from, 600) == -1, "warned although inside the given limit");
	EXPECT_ERR("read", pm_read(c, 0, &u), 0);
	CHECK(u.exceeded_soft == 0 && u.signal_sent == 0, "soft=%x sig=%u", u.exceeded_soft, u.signal_sent);
	CHECK(u.lim[PIDMON_LIM_RSS].soft == 64 * MB && u.lim[PIDMON_LIM_RSS].hard == 128 * MB,
	      "reported RSS limits %llu / %llu", (unsigned long long)u.lim[PIDMON_LIM_RSS].soft,
	      (unsigned long long)u.lim[PIDMON_LIM_RSS].hard);
	{
		struct rlimit rl;

		CHECK(prlimit(c, RLIMIT_RSS, NULL, &rl) == 0 && rl.rlim_cur == 8 * MB && rl.rlim_max == 96 * MB,
		      "the target's own rlimit changed: %llu / %llu", (unsigned long long)rl.rlim_cur,
		      (unsigned long long)rl.rlim_max);
	}
	kill_reap(c);

	/* ... and a tight given limit trips a target that has no limit of its own */
	c = spawn(grower, &to, &from);
	CHECK(get_byte(from, 10000) == 'r', "child not ready");
	memset(&lim, 0, sizeof(lim));
	lim_set(&lim, PIDMON_LIM_RSS, 12 * MB, 64 * MB);
	EXPECT_ERR("start", pm_start_lim(c, 0, &lim), 0);
	put_byte(to, 'g');
	{
		/* the window may warn while the child is still growing, so 'T' can come before 'g' */
		int b1 = get_byte(from, 10000), b2 = get_byte(from, 3000);

		CHECK((b1 == 'g' && b2 == 'T') || (b1 == 'T' && b2 == 'g'),
		      "growth and warning expected, got %d then %d", b1, b2);
	}
	/* the breach is the window's, so it stays on record after the child shrinks back */
	put_byte(to, 's');
	CHECK(get_byte(from, 10000) == 's', "child did not shrink");
	EXPECT_ERR("read", pm_read(c, 0, &u), 0);
	CHECK(u.rss_bytes < 12 * MB, "rss_bytes=%llu", (unsigned long long)u.rss_bytes);
	CHECK(u.exceeded_soft == (1U << PIDMON_LIM_RSS), "breach forgotten: exceeded_soft=%x", u.exceeded_soft);
	CHECK(u.signal_sent == SIGTERM, "signal_sent=%u", u.signal_sent);
	kill_reap(c);

	/* --- nothing is enforced outside the window --- */
	c = spawn(grower, &to, &from);
	CHECK(get_byte(from, 10000) == 'r', "child not ready");
	memset(&lim, 0, sizeof(lim));
	lim_set(&lim, PIDMON_LIM_RSS, 12 * MB, 64 * MB);
	put_byte(to, 'g');			/* grows before any window exists */
	CHECK(get_byte(from, 10000) == 'g', "child did not grow");
	EXPECT_ERR("start", pm_start_lim(c, PIDMON_F_NO_ENFORCE, &lim), 0);
	EXPECT_ERR("read STOP", pm_read(c, PIDMON_F_STOP, &u), 0);	/* the window ends here */
	CHECK(u.exceeded_soft == (1U << PIDMON_LIM_RSS), "exceeded_soft=%x", u.exceeded_soft);
	EXPECT_ERR("start enforcing, over already", pm_start_lim(c, 0, &lim), 0);
	CHECK(get_byte(from, 3000) == 'T', "a target already over its limit must be warned");
	EXPECT_ERR("read STOP", pm_read(c, PIDMON_F_STOP, &u), 0);
	/* the window is closed: a new breach goes unanswered */
	put_byte(to, 's');
	CHECK(get_byte(from, 10000) == 's', "child did not shrink");
	kill_reap(c);

	/*
	 * --- CPU time ---
	 * Neither child has an RLIMIT_CPU, so the kernel does nothing and any SIGXCPU or SIGKILL
	 * comes from the window.
	 */
	c = spawn(cpu_soft, &to, &from);
	memset(&lim, 0, sizeof(lim));
	lim_set(&lim, PIDMON_LIM_CPU, 1, 100);
	EXPECT_ERR("start", pm_start_lim(c, 0, &lim), 0);
	put_byte(to, 'g');
	CHECK(get_byte(from, 5000) == 'T', "SIGXCPU did not arrive");
	CHECK(get_byte(from, 20000) == 'r', "child not done");
	EXPECT_ERR("read", pm_read(c, 0, &u), 0);
	CHECK(u.cpu_total_ns >= 1000000000ULL, "cpu_total=%llu", (unsigned long long)u.cpu_total_ns);
	CHECK(u.exceeded_soft == (1U << PIDMON_LIM_CPU), "exceeded_soft=%x", u.exceeded_soft);
	CHECK(u.exceeded_hard == 0, "exceeded_hard=%x", u.exceeded_hard);
	CHECK(u.signal_sent == SIGXCPU, "signal_sent=%u", u.signal_sent);
	kill_reap(c);

	c = spawn(cpu_hard, &to, &from);
	memset(&lim, 0, sizeof(lim));
	lim_set(&lim, PIDMON_LIM_CPU, 1, 2);
	EXPECT_ERR("start", pm_start_lim(c, 0, &lim), 0);
	put_byte(to, 'g');
	waitpid(c, &st, 0);
	CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL, "child status %x", st);
	EXPECT_ERR("read", pm_read(c, 0, &u), 0);
	CHECK(u.state & PIDMON_ST_EXITED, "state=%x", u.state);
	CHECK(u.exceeded_hard == (1U << PIDMON_LIM_CPU), "exceeded_hard=%x", u.exceeded_hard);
	CHECK(u.exceeded_soft == (1U << PIDMON_LIM_CPU), "exceeded_soft=%x", u.exceeded_soft);
	CHECK(u.signal_sent == SIGKILL, "signal_sent=%u", u.signal_sent);
	CHECK(u.cpu_total_ns >= 2000000000ULL && u.cpu_total_ns < 3000000000ULL,
	      "killed late: cpu_total=%llu", (unsigned long long)u.cpu_total_ns);

	/* --- the caller can monitor itself --- */
	{
		struct sigaction sa = { .sa_handler = on_self_term };

		sigaction(SIGTERM, &sa, NULL);
	}
	memset(&lim, 0, sizeof(lim));
	lim_set(&lim, PIDMON_LIM_RSS, 4 * MB, RLIM_INFINITY);
	touch_mb(8);
	EXPECT_ERR("start self", pm_start_lim(0, 0, &lim), 0);
	{
		int i;

		for (i = 0; i < 300 && !self_term; i++)
			msleep(10);
	}
	CHECK(self_term, "own SIGTERM handler did not run");
	EXPECT_ERR("read self", pm_read(0, 0, &u), 0);
	CHECK(u.signal_sent == SIGTERM, "signal_sent=%u", u.signal_sent);

	return test_end();
}

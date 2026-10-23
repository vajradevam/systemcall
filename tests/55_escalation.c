/* Enforcement across limits and severities, done by the window with no read in between: a warned
 * target that then crosses its hard limit is still killed, several exceeded limits are all
 * reported, and hard beats soft. */
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

static void set_limit(int what, rlim_t soft, rlim_t hard)
{
	struct rlimit rl = { soft, hard };

	if (setrlimit(what, &rl))
		die("setrlimit");
}

static void catch_term(int out)
{
	struct sigaction sa = { .sa_handler = on_term };

	ctl_out = out;
	sigaction(SIGTERM, &sa, NULL);
}

/* RSS soft 8 MB / hard 48 MB; holds 16 MB, grows to 64 MB on request */
static void escalating(int in, int out)
{
	catch_term(out);
	set_limit(RLIMIT_RSS, 8 * MB, 48 * MB);
	touch_mb(16);
	put_byte(out, 'r');
	get_byte(in, 10000);
	touch_mb(48);
	put_byte(out, 'g');
	for (;;)
		pause();
}

/* three soft limits exceeded at once: RSS, DATA and NOFILE */
static void many_soft(int in, int out)
{
	int i;

	(void)in;
	catch_term(out);
	touch_mb(16);
	for (i = 0; i < 20; i++)
		if (open("/dev/null", O_RDONLY) < 0)
			die("open");
	set_limit(RLIMIT_RSS, 8 * MB, RLIM_INFINITY);
	set_limit(RLIMIT_DATA, 4 * MB, RLIM_INFINITY);
	set_limit(RLIMIT_NOFILE, 10, 4096);
	put_byte(out, 'r');
	for (;;)
		pause();
}

/* RSS over its hard limit, NOFILE over its soft limit only */
static void mixed(int in, int out)
{
	int i;

	(void)in;
	touch_mb(16);
	for (i = 0; i < 20; i++)
		if (open("/dev/null", O_RDONLY) < 0)
			die("open");
	set_limit(RLIMIT_RSS, 4 * MB, 8 * MB);
	set_limit(RLIMIT_NOFILE, 10, 4096);
	put_byte(out, 'r');
	for (;;)
		pause();
}

int main(void)
{
	struct pidmon_usage u;
	int to, from, st;
	pid_t c;

	test_begin("55_escalation");

	/* --- warn, then kill --- */
	c = spawn(escalating, &to, &from);
	CHECK(get_byte(from, 10000) == 'r', "child not ready");
	EXPECT_ERR("start", pm_start(c, 0), 0);
	CHECK(get_byte(from, 3000) == 'T', "warning not delivered");
	put_byte(to, 'g');
	waitpid(c, &st, 0);
	CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL,
	      "a warned target must still be killed at its hard limit: status %x", st);
	EXPECT_ERR("read", pm_read(c, 0, &u), 0);
	CHECK(u.state & PIDMON_ST_EXITED, "state=%x", u.state);
	CHECK(u.exceeded_hard == (1U << PIDMON_LIM_RSS), "exceeded_hard=%x", u.exceeded_hard);
	CHECK(u.exceeded_soft == (1U << PIDMON_LIM_RSS), "exceeded_soft=%x", u.exceeded_soft);
	CHECK(u.signal_sent == SIGKILL, "the strongest signal is reported: %u", u.signal_sent);

	/* --- several soft limits at once: one warning, every bit set --- */
	c = spawn(many_soft, &to, &from);
	CHECK(get_byte(from, 10000) == 'r', "child not ready");
	EXPECT_ERR("start", pm_start(c, 0), 0);
	CHECK(get_byte(from, 3000) == 'T', "warning not delivered");
	CHECK(get_byte(from, 500) == -1, "more than one warning for three limits");
	EXPECT_ERR("read", pm_read(c, 0, &u), 0);
	CHECK(u.exceeded_soft == ((1U << PIDMON_LIM_RSS) | (1U << PIDMON_LIM_DATA) |
				  (1U << PIDMON_LIM_NOFILE)), "exceeded_soft=%x", u.exceeded_soft);
	CHECK(u.exceeded_hard == 0, "exceeded_hard=%x", u.exceeded_hard);
	CHECK(u.signal_sent == SIGTERM, "signal_sent=%u", u.signal_sent);
	kill_reap(c);

	/* --- a hard breach wins over a soft one; a report-only window leaves it alive --- */
	c = spawn(mixed, &to, &from);
	CHECK(get_byte(from, 10000) == 'r', "child not ready");
	EXPECT_ERR("start NO_ENFORCE", pm_start(c, PIDMON_F_NO_ENFORCE), 0);
	msleep(300);
	EXPECT_ERR("read", pm_read(c, 0, &u), 0);
	CHECK(u.exceeded_hard == (1U << PIDMON_LIM_RSS), "exceeded_hard=%x", u.exceeded_hard);
	CHECK(u.exceeded_soft == ((1U << PIDMON_LIM_RSS) | (1U << PIDMON_LIM_NOFILE)),
	      "exceeded_soft=%x", u.exceeded_soft);
	CHECK(u.signal_sent == 0 && kill(c, 0) == 0, "NO_ENFORCE must not kill");
	EXPECT_ERR("read STOP", pm_read(c, PIDMON_F_STOP, &u), 0);
	kill_reap(c);

	c = spawn(mixed, &to, &from);
	CHECK(get_byte(from, 10000) == 'r', "child not ready");
	EXPECT_ERR("start", pm_start(c, 0), 0);
	waitpid(c, &st, 0);
	CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL, "hard beats soft: status %x", st);
	EXPECT_ERR("read", pm_read(c, 0, &u), 0);
	CHECK(u.signal_sent == SIGKILL, "signal_sent=%u", u.signal_sent);

	return test_end();
}

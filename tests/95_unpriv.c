/* Ancestry is the authorization, not uid or capabilities: an unprivileged owner can monitor and
 * signal a descendant that kill(2) would refuse to touch. */
#include "common.h"

static int ctl_out;

static void on_term(int sig)
{
	char c = 'T';

	(void)sig;
	if (write(ctl_out, &c, 1) < 0)
		_exit(3);
	_exit(0);	/* our parent cannot SIGKILL us, so leave on the warning */
}

/* a root-owned child with a tiny RSS limit and a SIGTERM handler */
static void victim(int in, int out)
{
	struct sigaction sa = { .sa_handler = on_term };
	struct rlimit rl = { 64 * 1024, RLIM_INFINITY };

	(void)in;
	ctl_out = out;
	sigaction(SIGTERM, &sa, NULL);
	if (setrlimit(RLIMIT_RSS, &rl))
		die("setrlimit");
	put_byte(out, 'r');
	for (;;)
		pause();
}

int main(void)
{
	struct pidmon_usage u;
	int to, from, fd;
	pid_t c, mine;

	test_begin("95_unpriv");

	c = spawn(victim, &to, &from);
	CHECK(get_byte(from, 5000) == 'r', "victim not ready");

	if (setresgid(1000, 1000, 1000) || setresuid(1000, 1000, 1000))
		die("drop privileges");
	CHECK(geteuid() == 1000, "still root");

	/* the precondition: plain kill(2) is refused */
	CHECK(kill(c, SIGTERM) == -1 && errno == EPERM, "kill(2) should be EPERM here, errno=%d", errno);
	CHECK(get_byte(from, 200) == -1, "the victim got a signal it should not have");

	/* but pidmon works through ancestry */
	EXPECT_ERR("start", pm_start(c, 0), 0);
	EXPECT_ERR("read", pm_read(c, 0, &u), 0);
	CHECK(u.exceeded_soft & (1U << PIDMON_LIM_RSS), "exceeded_soft=%x", u.exceeded_soft);
	CHECK(u.signal_sent == SIGTERM, "signal_sent=%u", u.signal_sent);
	CHECK(get_byte(from, 3000) == 'T', "SIGTERM did not arrive");
	waitpid(c, NULL, 0);

	/* and a child created after the drop */
	mine = spawn(sleeper, NULL, &fd);
	get_byte(fd, 2000);
	EXPECT_ERR("start own child", pm_start(mine, 0), 0);
	EXPECT_ERR("read own child", pm_read(mine, 0, &u), 0);
	kill_reap(mine);

	return test_end();
}

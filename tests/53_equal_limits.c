/* With soft == hard there is no warning step: crossing the line kills, and both severities
 * are reported. */
#include "common.h"

#define MB (1024ULL * 1024)

static void grower(int in, int out)
{
	put_byte(out, 'r');
	if (get_byte(in, 10000) != 'g')
		_exit(1);
	touch_mb(16);
	for (;;)
		pause();
}

int main(void)
{
	struct pidmon_limits lim = {};
	struct pidmon_usage u;
	int to, from, st;
	pid_t c;

	test_begin("53_equal_limits");

	c = spawn(grower, &to, &from);
	CHECK(get_byte(from, 10000) == 'r', "not ready");
	lim_set(&lim, PIDMON_LIM_RSS, 8 * MB, 8 * MB);
	EXPECT_ERR("start", pm_start_lim(c, 0, &lim), 0);
	put_byte(to, 'g');

	CHECK(waitpid(c, &st, 0) == c, "waitpid");
	CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL, "status %#x, want SIGKILL", st);
	EXPECT_ERR("read", pm_read(c, PIDMON_F_STOP, &u), 0);
	CHECK(u.state & PIDMON_ST_EXITED, "state=%#x", u.state);
	CHECK(u.signal_sent == SIGKILL, "signal_sent=%u", u.signal_sent);
	CHECK(u.exceeded_soft == (1U << PIDMON_LIM_RSS), "soft=%#x", u.exceeded_soft);
	CHECK(u.exceeded_hard == (1U << PIDMON_LIM_RSS), "hard=%#x", u.exceeded_hard);

	return test_end();
}

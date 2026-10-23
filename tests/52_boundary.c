/* A limit is breached only when usage goes above it: a target sitting exactly at its open-file
 * limit is not over, and one more descriptor is. */
#include "common.h"

static void opener(int in, int out)
{
	put_byte(out, 'r');
	if (get_byte(in, 10000) != 'o')
		_exit(1);
	if (open("/dev/null", O_RDONLY) < 0)
		_exit(2);
	put_byte(out, 'd');
	for (;;)
		pause();
}

int main(void)
{
	struct pidmon_limits lim = {};
	struct pidmon_usage u;
	int to, from, n;
	pid_t c;

	test_begin("52_boundary");

	c = spawn(opener, &to, &from);
	CHECK(get_byte(from, 10000) == 'r', "not ready");
	n = count_fds(c);
	lim_set(&lim, PIDMON_LIM_NOFILE, n, n + 100);
	EXPECT_ERR("start", pm_start_lim(c, PIDMON_F_NO_ENFORCE, &lim), 0);

	EXPECT_ERR("read at the limit", pm_read(c, 0, &u), 0);
	CHECK((int)u.nr_open_fds == n, "nr_open_fds=%llu, /proc says %d",
	      (unsigned long long)u.nr_open_fds, n);
	CHECK(u.exceeded_soft == 0, "at the limit counts as over: soft=%#x", u.exceeded_soft);

	put_byte(to, 'o');
	CHECK(get_byte(from, 10000) == 'd', "child did not open");
	EXPECT_ERR("read over", pm_read(c, 0, &u), 0);
	CHECK((int)u.nr_open_fds == n + 1, "nr_open_fds=%llu, want %d",
	      (unsigned long long)u.nr_open_fds, n + 1);
	CHECK(u.exceeded_soft == (1U << PIDMON_LIM_NOFILE), "soft=%#x", u.exceeded_soft);
	CHECK(u.exceeded_hard == 0, "hard=%#x", u.exceeded_hard);

	kill_reap(c);
	return test_end();
}

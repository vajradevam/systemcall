/* Voluntary context switches are counted in the window: a target that sleeps many times shows
 * at least that many, a target that only waits shows few. */
#include "common.h"

static void napper(int in, int out)
{
	int i;

	put_byte(out, 'r');
	if (get_byte(in, 10000) != 'g')
		_exit(1);
	for (i = 0; i < 50; i++)
		msleep(2);
	put_byte(out, 'd');
	for (;;)
		pause();
}

int main(void)
{
	struct pidmon_usage u;
	int to, from;
	pid_t c;

	test_begin("72_ctxsw");

	c = spawn(napper, &to, &from);
	CHECK(get_byte(from, 10000) == 'r', "not ready");
	EXPECT_ERR("start", pm_start(c, PIDMON_F_NO_ENFORCE), 0);
	msleep(200);
	EXPECT_ERR("idle read", pm_read(c, 0, &u), 0);
	CHECK(u.vol_ctxsw < 10, "idle vol_ctxsw=%llu", (unsigned long long)u.vol_ctxsw);

	put_byte(to, 'g');
	CHECK(get_byte(from, 10000) == 'd', "child did not finish");
	EXPECT_ERR("busy read", pm_read(c, 0, &u), 0);
	CHECK(u.vol_ctxsw >= 50, "50 sleeps gave vol_ctxsw=%llu", (unsigned long long)u.vol_ctxsw);
	CHECK(u.vol_ctxsw < 500, "vol_ctxsw=%llu", (unsigned long long)u.vol_ctxsw);

	kill_reap(c);
	return test_end();
}

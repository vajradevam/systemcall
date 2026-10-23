/* A limit that is not given to pidmon_start() is the target's own rlimit, read again at every
 * check: lowering it inside the window takes effect, and the usage report shows the current value. */
#include "common.h"

#define MB (1024ULL * 1024)

static void tuner(int in, int out)
{
	struct rlimit rl = { 1024 * MB, 1024 * MB };
	void *p;

	setrlimit(RLIMIT_RSS, &rl);
	put_byte(out, 'r');
	if (get_byte(in, 10000) != 'g')
		_exit(1);
	p = touch_mb(20);
	put_byte(out, 'a');
	if (get_byte(in, 10000) != 'l')
		_exit(2);
	rl.rlim_cur = 4 * MB;
	if (setrlimit(RLIMIT_RSS, &rl))
		_exit(3);
	put_byte(out, 'b');
	for (;;)
		pause();
	(void)p;
}

int main(void)
{
	struct pidmon_usage u;
	int to, from;
	pid_t c;

	test_begin("54_live_rlimit");

	c = spawn(tuner, &to, &from);
	CHECK(get_byte(from, 10000) == 'r', "not ready");
	EXPECT_ERR("start", pm_start(c, PIDMON_F_NO_ENFORCE), 0);
	put_byte(to, 'g');
	CHECK(get_byte(from, 10000) == 'a', "did not grow");

	EXPECT_ERR("read before", pm_read(c, 0, &u), 0);
	CHECK(u.lim[PIDMON_LIM_RSS].soft == 1024 * MB, "soft=%llu",
	      (unsigned long long)u.lim[PIDMON_LIM_RSS].soft);
	CHECK(u.exceeded_soft == 0, "soft=%#x", u.exceeded_soft);

	put_byte(to, 'l');
	CHECK(get_byte(from, 10000) == 'b', "did not lower");
	EXPECT_ERR("read after", pm_read(c, 0, &u), 0);
	CHECK(u.lim[PIDMON_LIM_RSS].soft == 4 * MB, "soft=%llu",
	      (unsigned long long)u.lim[PIDMON_LIM_RSS].soft);
	CHECK(u.exceeded_soft == (1U << PIDMON_LIM_RSS), "soft=%#x", u.exceeded_soft);
	CHECK(u.exceeded_hard == 0, "hard=%#x", u.exceeded_hard);

	kill_reap(c);
	return test_end();
}

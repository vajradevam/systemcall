/* Limits are reported exactly as set (RLIM_INFINITY as ~0), and a target with every limit
 * unlimited is never judged over, however much it uses. */
#include "common.h"

#define MB (1024UL * 1024)

static const int rl_of[PIDMON_NLIM] = {
	[PIDMON_LIM_CPU] = RLIMIT_CPU, [PIDMON_LIM_AS] = RLIMIT_AS, [PIDMON_LIM_RSS] = RLIMIT_RSS,
	[PIDMON_LIM_DATA] = RLIMIT_DATA, [PIDMON_LIM_STACK] = RLIMIT_STACK,
	[PIDMON_LIM_MEMLOCK] = RLIMIT_MEMLOCK, [PIDMON_LIM_NOFILE] = RLIMIT_NOFILE,
};

/* every limit unlimited except NOFILE, which the kernel caps at fs.nr_open and cannot be infinite */
static void unlimited(int in, int out)
{
	struct rlimit rl = { RLIM_INFINITY, RLIM_INFINITY };
	int i;

	(void)in;
	for (i = 0; i < PIDMON_NLIM; i++)
		if (i != PIDMON_LIM_NOFILE && setrlimit(rl_of[i], &rl))
			die("setrlimit");
	touch_mb(32);
	for (i = 0; i < 40; i++)
		if (open("/dev/null", O_RDONLY) < 0)
			die("open");
	burn_ms(100);
	put_byte(out, 'r');
	for (;;)
		pause();
}

/* distinct soft / hard values for the limits that can be set without privilege trouble */
static void exact(int in, int out)
{
	struct rlimit rl;
	int i;

	(void)in;
	for (i = 0; i < PIDMON_NLIM; i++) {
		getrlimit(rl_of[i], &rl);
		rl.rlim_max = rl.rlim_max == RLIM_INFINITY ? 900 * MB : rl.rlim_max;
		rl.rlim_cur = rl.rlim_max / 2 + 7;
		if (rl_of[i] == RLIMIT_CPU) {
			rl.rlim_max = 5000;
			rl.rlim_cur = 4000;
		}
		if (rl_of[i] == RLIMIT_NOFILE) {
			rl.rlim_max = 4000;
			rl.rlim_cur = 3000;
		}
		if (setrlimit(rl_of[i], &rl))
			die("setrlimit");
	}
	put_byte(out, 'r');
	for (;;)
		pause();
}

int main(void)
{
	struct pidmon_usage u;
	struct rlimit rl;
	int to, from, i;
	pid_t c;

	test_begin("15_unlimited");

	c = spawn(unlimited, &to, &from);
	CHECK(get_byte(from, 10000) == 'r', "child not ready");
	EXPECT_ERR("start", pm_start(c, 0), 0);
	EXPECT_ERR("read", pm_read(c, 0, &u), 0);
	for (i = 0; i < PIDMON_NLIM; i++) {
		if (i == PIDMON_LIM_NOFILE)
			continue;
		CHECK(u.lim[i].soft == ~0ULL, "lim[%d].soft=%llx", i, (unsigned long long)u.lim[i].soft);
		CHECK(u.lim[i].hard == ~0ULL, "lim[%d].hard=%llx", i, (unsigned long long)u.lim[i].hard);
	}
	CHECK(u.exceeded_soft == 0 && u.exceeded_hard == 0, "soft=%x hard=%x",
	      u.exceeded_soft, u.exceeded_hard);
	CHECK(u.signal_sent == 0, "signal_sent=%u", u.signal_sent);
	CHECK(u.rss_bytes >= 32 * MB, "rss_bytes=%llu", (unsigned long long)u.rss_bytes);
	CHECK(u.nr_open_fds >= 40, "nr_open_fds=%llu", (unsigned long long)u.nr_open_fds);
	kill_reap(c);

	c = spawn(exact, &to, &from);
	CHECK(get_byte(from, 10000) == 'r', "child not ready");
	EXPECT_ERR("start", pm_start(c, 0), 0);
	EXPECT_ERR("read", pm_read(c, PIDMON_F_NO_ENFORCE, &u), 0);
	for (i = 0; i < PIDMON_NLIM; i++) {
		if (prlimit(c, rl_of[i], NULL, &rl))
			die("prlimit");
		CHECK(u.lim[i].soft == rl.rlim_cur, "lim[%d].soft=%llu want %llu", i,
		      (unsigned long long)u.lim[i].soft, (unsigned long long)rl.rlim_cur);
		CHECK(u.lim[i].hard == rl.rlim_max, "lim[%d].hard=%llu want %llu", i,
		      (unsigned long long)u.lim[i].hard, (unsigned long long)rl.rlim_max);
	}
	kill_reap(c);

	return test_end();
}

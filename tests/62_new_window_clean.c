/* A breach belongs to its window: after STOP and a new start, the exceeded masks and signal_sent
 * begin empty again even though the target was over its limit before. */
#include "common.h"

#define MB (1024ULL * 1024)

static void cycler(int in, int out)
{
	void *p = NULL;
	int c;

	put_byte(out, 'r');
	while ((c = get_byte(in, 10000)) >= 0) {
		if (c == 'g' && !p)
			p = touch_mb(16);
		if (c == 'f' && p) {
			munmap(p, 16 * MB);
			p = NULL;
		}
		put_byte(out, c);
	}
}

int main(void)
{
	struct pidmon_limits lim = {};
	struct pidmon_usage u;
	int to, from;
	pid_t c;

	test_begin("62_new_window_clean");

	c = spawn(cycler, &to, &from);
	CHECK(get_byte(from, 10000) == 'r', "not ready");
	lim_set(&lim, PIDMON_LIM_RSS, 8 * MB, 64 * MB);

	EXPECT_ERR("start 1", pm_start_lim(c, PIDMON_F_NO_ENFORCE, &lim), 0);
	put_byte(to, 'g');
	CHECK(get_byte(from, 5000) == 'g', "no growth ack");
	EXPECT_ERR("read 1", pm_read(c, 0, &u), 0);
	CHECK(u.exceeded_soft == (1U << PIDMON_LIM_RSS), "window 1 soft=%#x", u.exceeded_soft);
	EXPECT_ERR("stop 1", pm_read(c, PIDMON_F_STOP, &u), 0);

	put_byte(to, 'f');
	CHECK(get_byte(from, 5000) == 'f', "no free ack");
	EXPECT_ERR("start 2", pm_start_lim(c, PIDMON_F_NO_ENFORCE, &lim), 0);
	msleep(100);
	EXPECT_ERR("read 2", pm_read(c, 0, &u), 0);
	CHECK(u.exceeded_soft == 0, "window 2 inherited soft=%#x", u.exceeded_soft);
	CHECK(u.exceeded_hard == 0, "window 2 inherited hard=%#x", u.exceeded_hard);
	CHECK(u.signal_sent == 0, "window 2 inherited signal=%u", u.signal_sent);

	kill_reap(c);
	return test_end();
}

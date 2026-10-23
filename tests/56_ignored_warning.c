/* A target that handles or ignores the warning signal is not escalated on its own: the signal is
 * sent once, reported in signal_sent, and the target stays alive. */
#include "common.h"

#define MB (1024ULL * 1024)

static int warn_fd;

static void on_term(int sig)
{
	(void)sig;
	if (write(warn_fd, "T", 1) != 1)
		_exit(9);
}

static void stubborn(int in, int out)
{
	struct sigaction sa = { .sa_handler = on_term };

	warn_fd = out;
	sigaction(SIGTERM, &sa, NULL);
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

	test_begin("56_ignored_warning");

	c = spawn(stubborn, &to, &from);
	CHECK(get_byte(from, 10000) == 'r', "not ready");
	lim_set(&lim, PIDMON_LIM_RSS, 8 * MB, ~0ULL);
	EXPECT_ERR("start", pm_start_lim(c, 0, &lim), 0);
	put_byte(to, 'g');

	CHECK(get_byte(from, 5000) == 'T', "no warning arrived");
	CHECK(get_byte(from, 400) == -1, "warning repeated");
	CHECK(waitpid(c, &st, WNOHANG) == 0, "target did not survive");
	EXPECT_ERR("read", pm_read(c, 0, &u), 0);
	CHECK(u.signal_sent == SIGTERM, "signal_sent=%u", u.signal_sent);
	CHECK(u.exceeded_soft == (1U << PIDMON_LIM_RSS), "soft=%#x", u.exceeded_soft);
	CHECK(u.exceeded_hard == 0, "hard=%#x", u.exceeded_hard);

	kill_reap(c);
	return test_end();
}

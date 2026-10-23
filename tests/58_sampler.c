/* The window's sampler: a breach is acted on soon after it happens, many windows with different
 * limits are judged independently, and enforcement stops with the window's owner and for a target
 * that has left the owner's tree. */
#include "common.h"

#define MB (1024UL * 1024)
#define NKIDS 8

static int ctl_out;
static int warn_fd = -1;	/* when set, warnings go here instead of to the parent */

static void on_term(int sig)
{
	char c = 'T';

	(void)sig;
	if (write(ctl_out, &c, 1) < 0)
		_exit(3);
}

/* waits for go (or for its parent to die), touches 16 MB and reports; a warning is reported too */
static void grower(int in, int out)
{
	struct sigaction sa = { .sa_handler = on_term };

	ctl_out = warn_fd >= 0 ? warn_fd : out;
	sigaction(SIGTERM, &sa, NULL);
	put_byte(out, 'r');
	get_byte(in, 10000);	/* also returns when the parent is gone */
	touch_mb(16);
	if (warn_fd < 0)
		put_byte(out, 'g');
	for (;;)
		pause();
}

static uint64_t now_ms(void)
{
	return cpu_ns(CLOCK_MONOTONIC) / 1000000;
}

int main(void)
{
	struct pidmon_limits lim;
	struct pidmon_usage u;
	int to[NKIDS], from[NKIDS], i, warned;
	pid_t kid[NKIDS], c;
	uint64_t t0;

	test_begin("58_sampler");

	/* --- latency: from the breach to the warning --- */
	c = spawn(grower, &to[0], &from[0]);
	CHECK(get_byte(from[0], 5000) == 'r', "child not ready");
	memset(&lim, 0, sizeof(lim));
	lim_set(&lim, PIDMON_LIM_RSS, 8 * MB, ~0ULL);
	EXPECT_ERR("start", pm_start_lim(c, 0, &lim), 0);
	msleep(100);		/* the sampler is running and finds nothing wrong */
	t0 = now_ms();
	put_byte(to[0], 'g');
	for (i = 0; i < 2; i++) {
		int b = get_byte(from[0], 3000);

		CHECK(b == 'g' || b == 'T', "unexpected byte %d", b);
	}
	CHECK(now_ms() - t0 < 500, "the warning took %llu ms", (unsigned long long)(now_ms() - t0));
	kill_reap(c);

	/* --- many windows, each with its own limit: those below 16 MB warn, the rest do not --- */
	for (i = 0; i < NKIDS; i++) {
		kid[i] = spawn(grower, &to[i], &from[i]);
		CHECK(get_byte(from[i], 5000) == 'r', "child %d not ready", i);
		memset(&lim, 0, sizeof(lim));
		lim_set(&lim, PIDMON_LIM_RSS, (i + 1) * 4 * MB, ~0ULL);	/* 4, 8, 12, 16, 20, ... MB */
		EXPECT_ERR("start", pm_start_lim(kid[i], 0, &lim), 0);
	}
	for (i = 0; i < NKIDS; i++)
		put_byte(to[i], 'g');
	msleep(800);
	warned = 0;
	for (i = 0; i < NKIDS; i++) {
		int saw_t = 0, b;

		while ((b = get_byte(from[i], 100)) >= 0)
			saw_t |= b == 'T';
		EXPECT_ERR("read", pm_read(kid[i], 0, &u), 0);
		/* 16 MB of data plus the program's own pages pass limits of 4, 8, 12 and 16 MB */
		if (i < 3) {
			CHECK(saw_t && u.signal_sent == SIGTERM, "child %d (limit %d MB) was not warned", i,
			      (i + 1) * 4);
			warned++;
		}
		if (i > 3)
			CHECK(!saw_t && u.signal_sent == 0 && u.exceeded_soft == 0,
			      "child %d (limit %d MB) was warned: sig=%u soft=%x", i, (i + 1) * 4,
			      u.signal_sent, u.exceeded_soft);
		CHECK(kill(kid[i], 0) == 0, "child %d died from a warning", i);
	}
	CHECK(warned == 3, "warned=%d", warned);
	for (i = 0; i < NKIDS; i++)
		kill_reap(kid[i]);

	/*
	 * --- the owner exits, or the target leaves the owner's tree: the target grows unwatched ---
	 * The target's parent dying makes it read EOF on its start pipe, which sends it growing; its
	 * warnings go to a pipe the test holds, since nobody else is left to hear them.
	 */
	for (i = 0; i < 2; i++) {
		int wp[2], pp[2], go[2];
		pid_t holder;

		if (pipe(wp) || pipe(pp) || pipe(go))
			die("pipe");
		holder = fork();	/* i == 0: owns the window; i == 1: only the target's parent */
		if (holder == 0) {
			int t, f;
			pid_t g;

			warn_fd = wp[1];
			g = spawn(grower, &t, &f);
			get_byte(f, 5000);
			if (i == 0) {
				memset(&lim, 0, sizeof(lim));
				lim_set(&lim, PIDMON_LIM_RSS, 8 * MB, ~0ULL);
				if (pm_start_lim(g, 0, &lim))
					_exit(2);
			}
			if (write(pp[1], &g, sizeof(g)) != sizeof(g))
				_exit(3);
			get_byte(go[0], 10000);		/* the test says when to die */
			_exit(0);
		}
		{
			pid_t g;

			CHECK(read(pp[0], &g, sizeof(g)) == sizeof(g), "no target pid");
			if (i == 1) {
				/* the test is the owner, and the target is its grandchild for now */
				memset(&lim, 0, sizeof(lim));
				lim_set(&lim, PIDMON_LIM_RSS, 8 * MB, ~0ULL);
				EXPECT_ERR("start grandchild", pm_start_lim(g, 0, &lim), 0);
			}
			put_byte(go[1], 'x');
			waitpid(holder, NULL, 0);
			CHECK(get_byte(wp[0], 800) == -1, "%s: an unwatched target was warned",
			      i ? "orphan" : "owner gone");
			CHECK(kill(g, 0) == 0, "target must still be running");
			if (i == 1)
				EXPECT_ERR("orphan is no longer ours", pm_read(g, 0, &u), EPERM);
			kill(g, SIGKILL);
		}
		close(wp[0]);
		close(wp[1]);
		close(pp[0]);
		close(pp[1]);
		close(go[0]);
		close(go[1]);
	}

	return test_end();
}

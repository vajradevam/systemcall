/* cpu_permille is CPU time over wall time in the window: ~0 for a sleeper, ~1000 for one busy
 * thread, ~2000 for two; wall_ns agrees with the clock the caller sees. */
#include <pthread.h>
#include "common.h"

static void *spin(void *arg)
{
	(void)arg;
	burn_ms(700);
	return NULL;
}

static void one_busy(int in, int out)
{
	(void)in;
	put_byte(out, 'r');
	burn_ms(700);
	put_byte(out, 'd');
	for (;;)
		pause();
}

static void two_busy(int in, int out)
{
	pthread_t t;

	(void)in;
	put_byte(out, 'r');
	pthread_create(&t, NULL, spin, NULL);
	burn_ms(700);
	pthread_join(t, NULL);
	put_byte(out, 'd');
	for (;;)
		pause();
}

int main(void)
{
	struct pidmon_usage u, v;
	uint64_t t0, t1;
	int to, from, fd;
	pid_t c;

	test_begin("85_rate");

	/* a sleeper */
	c = spawn(sleeper, NULL, &fd);
	get_byte(fd, 2000);
	close(fd);
	t0 = cpu_ns(CLOCK_MONOTONIC);
	EXPECT_ERR("start", pm_start(c, 0), 0);
	msleep(400);
	EXPECT_ERR("read", pm_read(c, PIDMON_F_NO_ENFORCE, &u), 0);
	t1 = cpu_ns(CLOCK_MONOTONIC);
	CHECK(u.cpu_permille < 50, "sleeper cpu_permille=%llu", (unsigned long long)u.cpu_permille);
	CHECK(u.wall_ns >= 400000000ULL && u.wall_ns <= t1 - t0,
	      "wall_ns=%llu, caller measured %llu", (unsigned long long)u.wall_ns,
	      (unsigned long long)(t1 - t0));
	msleep(100);
	EXPECT_ERR("read later", pm_read(c, PIDMON_F_NO_ENFORCE, &v), 0);
	CHECK(v.wall_ns >= u.wall_ns + 90000000ULL, "wall_ns advanced %llu -> %llu",
	      (unsigned long long)u.wall_ns, (unsigned long long)v.wall_ns);
	kill_reap(c);

	/* one busy thread: the whole window is CPU */
	c = spawn(one_busy, &to, &from);
	CHECK(get_byte(from, 10000) == 'r', "not ready");
	EXPECT_ERR("start", pm_start(c, 0), 0);
	CHECK(get_byte(from, 10000) == 'd', "did not finish");
	EXPECT_ERR("read", pm_read(c, PIDMON_F_NO_ENFORCE, &u), 0);
	CHECK(u.cpu_permille >= 850 && u.cpu_permille <= 1100, "one thread: cpu_permille=%llu",
	      (unsigned long long)u.cpu_permille);
	kill_reap(c);

	/* two busy threads on two CPUs: twice that (this needs the guest's 2 CPUs) */
	if (sysconf(_SC_NPROCESSORS_ONLN) >= 2) {
		c = spawn(two_busy, &to, &from);
		CHECK(get_byte(from, 10000) == 'r', "not ready");
		EXPECT_ERR("start", pm_start(c, 0), 0);
		CHECK(get_byte(from, 10000) == 'd', "did not finish");
		EXPECT_ERR("read", pm_read(c, PIDMON_F_NO_ENFORCE, &u), 0);
		CHECK(u.cpu_permille >= 1500 && u.cpu_permille <= 2200,
		      "two threads: cpu_permille=%llu", (unsigned long long)u.cpu_permille);
		kill_reap(c);
	}

	return test_end();
}

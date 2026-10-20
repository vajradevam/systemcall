/* A pid is a whole process: CPU is summed over all threads, memory is read once, a thread id is
 * not a valid target, and the group keeps working after its leader thread has exited. */
#include "common.h"
#include <pthread.h>

#define MB (1024ULL * 1024)

static volatile int burnt;
static volatile pid_t some_tid;

static void *spinner(void *arg)
{
	(void)arg;
	some_tid = syscall(SYS_gettid);
	burn_ms(400);
	__sync_fetch_and_add(&burnt, 1);
	for (;;)
		pause();
	return NULL;
}

/* 24 MB shared by 4 threads; 3 of them burn 400 ms each */
static void threaded(int in, int out)
{
	pthread_t t[3];
	pid_t tid;
	int i;

	(void)in;
	touch_mb(24);
	for (i = 0; i < 3; i++)
		pthread_create(&t[i], NULL, spinner, NULL);
	put_byte(out, 'r');
	while (burnt < 3)
		msleep(10);
	tid = some_tid;
	if (write(out, &tid, sizeof(tid)) != sizeof(tid))
		die("write tid");
	for (;;)
		pause();
}

static void *napper(void *arg)
{
	(void)arg;
	msleep(1500);
	return NULL;
}

/* the leader thread exits at once; two other threads keep the process alive for 1.5 s */
static void leaderless(int in, int out)
{
	pthread_t t[2];
	int i;

	(void)in;
	touch_mb(8);
	for (i = 0; i < 2; i++)
		pthread_create(&t[i], NULL, napper, NULL);
	put_byte(out, 'r');
	pthread_exit(NULL);
}

static int leader_is_zombie(long pid)
{
	return proc_state(pid) == 'Z';
}

int main(void)
{
	struct pidmon_usage u;
	int to, from, st;
	pid_t c, tid = 0;

	test_begin("70_threads");

	c = spawn(threaded, &to, &from);
	CHECK(get_byte(from, 10000) == 'r', "not ready");
	EXPECT_ERR("start", pm_start(c, 0), 0);
	/* the tid follows once all three threads have finished burning */
	CHECK(get_exact(from, &tid, sizeof(tid), 20000), "no tid from the child");

	EXPECT_ERR("read", pm_read(c, 0, &u), 0);
	CHECK(u.nr_threads == 4, "nr_threads=%llu", (unsigned long long)u.nr_threads);
	CHECK((long)u.nr_threads == proc_status(c, "Threads:"), "/proc says %ld threads",
	      proc_status(c, "Threads:"));
	CHECK(u.utime_ns + u.stime_ns >= 1000000000ULL,
	      "cpu=%llu ns: three threads burnt 400 ms each, a leader-only sum cannot reach 1 s",
	      (unsigned long long)(u.utime_ns + u.stime_ns));
	CHECK(u.rss_anon >= 24 * MB && u.rss_anon < 40 * MB,
	      "rss_anon=%llu: shared memory must be counted once, not once per thread",
	      (unsigned long long)u.rss_anon);

	CHECK(tid > 0 && tid != c, "tid=%d", tid);
	EXPECT_ERR("a thread id is not a process", pm_start(tid, 0), ESRCH);
	EXPECT_ERR("read by thread id", pm_read(tid, 0, &u), ENOENT);
	kill_reap(c);

	/* leader thread gone, group alive */
	c = spawn(leaderless, NULL, &from);
	CHECK(get_byte(from, 5000) == 'r', "not ready");
	EXPECT_ERR("start", pm_start(c, 0), 0);
	CHECK(wait_until(leader_is_zombie, c), "leader did not become a zombie, state %c", proc_state(c));
	EXPECT_ERR("read leaderless group", pm_read(c, 0, &u), 0);
	CHECK(u.nr_threads >= 2, "nr_threads=%llu", (unsigned long long)u.nr_threads);
	CHECK(u.rss_bytes > 0, "no memory found once the leader exited");
	CHECK(u.nr_open_fds > 0, "no fds found once the leader exited");
	CHECK(!(u.state & PIDMON_ST_EXITED), "group is still alive, state=%x", u.state);

	waitpid(c, &st, 0);	/* returns when the last thread is gone */
	EXPECT_ERR("read finished group", pm_read(c, 0, &u), 0);
	CHECK(u.state & PIDMON_ST_EXITED, "state=%x", u.state);
	CHECK(u.rss_peak >= 8 * MB, "rss_peak=%llu: final numbers must be taken by the last thread",
	      (unsigned long long)u.rss_peak);

	return test_end();
}

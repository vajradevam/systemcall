/* Records belong to the owning process: threads of the owner share them, a forked child or an
 * exec'd program does not inherit them, and a descendant cannot see its owner's monitors. */
#include <pthread.h>
#include "common.h"

static pid_t target;
static int thread_start_rc = -1, thread_read_rc = -1, thread_stop_rc = -1;

static void *thread_reads(void *arg)
{
	struct pidmon_usage u;

	(void)arg;
	thread_read_rc = pm_read(target, 0, &u);	/* started by the main thread */
	thread_start_rc = pm_start(0, 0);		/* a record only this thread created ... */
	return NULL;
}

static void *thread_stops(void *arg)
{
	struct pidmon_usage u;

	(void)arg;
	thread_stop_rc = pm_read(0, PIDMON_F_STOP, &u);	/* ... freed by another thread */
	return NULL;
}

/* the child of a monitoring parent tries to use the parent's records */
static void child_probe(int in, int out)
{
	struct pidmon_usage u;
	int rc;

	(void)in;
	rc = pm_read(0, 0, &u);			/* nothing was started by us yet */
	put_byte(out, rc == ENOENT ? 'e' : 'x');
	rc = pm_read(getppid(), 0, &u);		/* the parent is not in our tree at all */
	put_byte(out, rc == ENOENT || rc == EPERM ? 'e' : 'x');
	rc = pm_start(getppid(), 0);
	put_byte(out, rc == EPERM ? 'e' : 'x');
	/* our own, independent record on ourselves works */
	put_byte(out, pm_start(0, 0) == 0 && pm_read(0, 0, &u) == 0 ? 'e' : 'x');
	for (;;)
		pause();
}

int main(int argc, char **argv)
{
	struct pidmon_usage u;
	pthread_t t1, t2;
	int fd, to, from, i;
	pid_t c;

	/* re-exec'd copy of ourselves, run as a child: it must start with no records */
	if (argc > 1 && !strcmp(argv[1], "probe")) {
		return pm_read(0, 0, &u) == ENOENT ? 0 : 1;
	}

	test_begin("65_scope");

	target = spawn(sleeper, NULL, &fd);
	get_byte(fd, 2000);
	EXPECT_ERR("start target", pm_start(target, 0), 0);

	/* threads of the owner share its records */
	pthread_create(&t1, NULL, thread_reads, NULL);
	pthread_join(t1, NULL);
	EXPECT_ERR("thread read", thread_read_rc, 0);
	EXPECT_ERR("thread start self", thread_start_rc, 0);
	EXPECT_ERR("main sees thread's record", pm_read(0, 0, &u), 0);
	pthread_create(&t2, NULL, thread_stops, NULL);
	pthread_join(t2, NULL);
	EXPECT_ERR("thread stop", thread_stop_rc, 0);
	EXPECT_ERR("record gone for main too", pm_read(0, 0, &u), ENOENT);

	/* a forked child starts with no records and cannot reach ours */
	c = spawn(child_probe, &to, &from);
	for (i = 0; i < 4; i++) {
		int b = get_byte(from, 5000);

		CHECK(b == 'e', "probe step %d answered %d", i, b);
	}
	EXPECT_ERR("target still ours", pm_read(target, 0, &u), 0);
	kill_reap(c);

	/* a fork() that does not exec has none of our records either */
	c = fork();
	if (c == 0) {
		int rc = pm_read(target, 0, &u);

		_exit(rc == ENOENT || rc == EPERM ? 0 : 1);
	}
	{
		int st;

		waitpid(c, &st, 0);
		CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "forked child saw our record: %x", st);
	}

	/* an exec'd child starts clean as well */
	c = fork();
	if (c == 0) {
		execl("/proc/self/exe", argv[0], "probe", (char *)NULL);
		_exit(2);
	} else {
		int st;

		waitpid(c, &st, 0);
		CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "exec'd child status %x", st);
	}
	EXPECT_ERR("target still monitored", pm_read(target, PIDMON_F_STOP, &u), 0);

	kill_reap(target);
	return test_end();
}

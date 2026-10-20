/* PID namespaces: pids are resolved in the caller's namespace, and ancestry crosses the boundary. */
#define _GNU_SOURCE
#include "common.h"
#include <sched.h>

/* init of a new pid namespace: inner pids are 1 (us), 2 (our child) */
static void ns_init(int in, int out)
{
	struct pidmon_usage u;
	int fd;
	pid_t m;

	get_byte(in, 10000);
	CHECK(getpid() == 1, "not pid 1 in the new namespace: %d", getpid());
	EXPECT_ERR("inner self 0", pm_start(0, 0), 0);
	EXPECT_ERR("inner self read", pm_read(0, PIDMON_F_STOP, &u), 0);
	EXPECT_ERR("inner self 1", pm_start(1, 0), 0);
	EXPECT_ERR("inner self 1 read", pm_read(1, PIDMON_F_STOP, &u), 0);

	m = spawn(sleeper, NULL, &fd);
	get_byte(fd, 2000);
	CHECK(m == 2, "child's inner pid is %d", m);
	EXPECT_ERR("inner child", pm_start(2, 0), 0);
	EXPECT_ERR("inner child read", pm_read(2, 0, &u), 0);
	EXPECT_ERR("inner nonexistent", pm_start(3, 0), ESRCH);
	kill_reap(m);

	put_byte(out, failures ? 'F' : 'k');
	get_byte(in, 10000);
}

/* lives in the test's namespace, creates the new one */
static void ns_parent(int in, int out)
{
	char buf[32];
	pid_t n;

	if (unshare(CLONE_NEWPID))
		die("unshare(CLONE_NEWPID)");
	n = fork();
	if (n < 0)
		die("fork");
	if (n == 0)
		ns_init(in, out);	/* pid 1 of the new namespace */
	else {
		snprintf(buf, sizeof(buf), "%d\n", n);	/* its pid as we see it */
		if (write(out, buf, strlen(buf)) < 0)
			die("write");
		waitpid(n, NULL, 0);
		return;
	}
	_exit(0);
}

int main(void)
{
	struct pidmon_usage u;
	char buf[32] = "";
	int to, from, i = 0;
	pid_t c, n;

	test_begin("80_ns");

	c = spawn(ns_parent, &to, &from);
	while (i < (int)sizeof(buf) - 1 && read(from, buf + i, 1) == 1 && buf[i] != '\n')
		i++;
	n = atoi(buf);
	CHECK(n > 1, "bad outer pid '%s'", buf);

	/* the inside checks run first: only one monitor may exist per target at a time */
	put_byte(to, 'i');
	CHECK(get_byte(from, 20000) == 'k', "checks inside the namespace failed");

	/* outer view: c -> n is our grandchild, whatever number it has inside */
	EXPECT_ERR("start grandchild in child ns", pm_start(n, 0), 0);
	EXPECT_ERR("read grandchild in child ns", pm_read(n, 0, &u), 0);
	CHECK(u.nr_threads == 1, "nr_threads=%llu", (unsigned long long)u.nr_threads);
	EXPECT_ERR("start middle", pm_start(c, 0), 0);
	EXPECT_ERR("outer init is not ours", pm_start(1, 0), EPERM);

	/* still monitored from outside after it did all that */
	EXPECT_ERR("read again", pm_read(n, 0, &u), 0);
	put_byte(to, 'q');
	waitpid(c, NULL, 0);
	EXPECT_ERR("read after exit", pm_read(n, 0, &u), 0);
	CHECK(u.state & PIDMON_ST_EXITED, "state=%x", u.state);

	return test_end();
}

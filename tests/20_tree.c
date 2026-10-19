/* Authority comes from ancestry: self, children and grandchildren are allowed; the parent, siblings,
 * init and orphans (reparented away) are refused. Genealogy is judged at the time of each call. */
#include "common.h"
#include <sys/prctl.h>

static pid_t sibling_pid, parent_pid;

/* runs as a child of the test process: everything outside its own subtree must be refused */
static void sibling_checks(int in, int out)
{
	struct pidmon_usage u;

	(void)in;
	EXPECT_ERR("child -> parent", pm_start(parent_pid, 0), EPERM);
	EXPECT_ERR("child -> sibling", pm_start(sibling_pid, 0), EPERM);
	EXPECT_ERR("child -> init", pm_start(1, 0), EPERM);
	EXPECT_ERR("child -> init read", pm_read(1, 0, &u), ENOENT);
	EXPECT_ERR("child -> self", pm_start(0, 0), 0);
	put_byte(out, failures ? 'F' : 'k');
}

static void grandchild_maker(int in, int out)
{
	char buf[32];
	int fd;
	pid_t g = spawn(sleeper, NULL, &fd);

	get_byte(fd, 2000);
	snprintf(buf, sizeof(buf), "%d", g);
	if (write(out, buf, strlen(buf) + 1) < 0)
		die("write");
	get_byte(in, 10000);		/* wait for the parent's go, then exit: orphans g */
}

static int reparented_to_init(long g)
{
	return proc_ppid(g) == 1;
}

static int reparented_to(long arg)
{
	return proc_ppid(arg >> 32) == (pid_t)(arg & 0xffffffff);
}

/* pid 1 here is busybox sh, which does not reap strangers: a dead orphan stays a zombie */
static int gone(long pid)
{
	char s = proc_state(pid);

	return s == 0 || s == 'Z';
}

/* child, grandchild, orphan */
static void tree_checks(int subreaper)
{
	struct pidmon_usage u;
	int to, from, fd;
	char buf[32] = "";
	pid_t c, g;

	if (subreaper && prctl(PR_SET_CHILD_SUBREAPER, 1))
		die("subreaper");

	c = spawn(grandchild_maker, &to, &from);
	if (read(from, buf, sizeof(buf)) <= 0)
		die("read grandchild pid");
	g = atoi(buf);
	(void)fd;

	EXPECT_ERR("start child", pm_start(c, 0), 0);
	EXPECT_ERR("start grandchild", pm_start(g, 0), 0);
	EXPECT_ERR("read grandchild", pm_read(g, 0, &u), 0);
	CHECK(proc_ppid(g) == c, "g's parent %d, want %d", proc_ppid(g), c);

	put_byte(to, 'g');		/* child exits, grandchild is orphaned */
	waitpid(c, NULL, 0);

	if (subreaper) {
		/* adopted by us, so it is now our child and we keep authority */
		CHECK(wait_until(reparented_to, ((long)g << 32) | getpid()),
		      "g not adopted, ppid=%d", proc_ppid(g));
		EXPECT_ERR("subreaper keeps grandchild", pm_read(g, 0, &u), 0);
		EXPECT_ERR("subreaper starts it afresh", pm_start(g, 0), EBUSY);
		kill_reap(g);
		EXPECT_ERR("read after it died", pm_read(g, 0, &u), 0);
		CHECK(u.state & PIDMON_ST_EXITED, "state=%x", u.state);
		return;
	}

	CHECK(wait_until(reparented_to_init, g), "g not reparented, ppid=%d", proc_ppid(g));
	EXPECT_ERR("read orphan", pm_read(g, 0, &u), EPERM);
	EXPECT_ERR("read orphan (still)", pm_read(g, PIDMON_F_STOP, &u), EPERM);
	EXPECT_ERR("start orphan", pm_start(g, 0), EPERM);

	/* an orphan that dies is still not ours: its final numbers are withheld */
	kill(g, SIGKILL);
	CHECK(wait_until(gone, g), "orphan did not die");
	EXPECT_ERR("read dead orphan", pm_read(g, 0, &u), EPERM);
	EXPECT_ERR("read dead orphan again", pm_read(g, 0, &u), ENOENT);
}

int main(void)
{
	struct pidmon_usage u;
	int to, from, fd;
	pid_t b, a;
	pid_t pid;
	int st;

	test_begin("20_tree");

	/* self, both spellings */
	EXPECT_ERR("self 0", pm_start(0, 0), 0);
	EXPECT_ERR("self 0 read", pm_read(0, PIDMON_F_STOP, &u), 0);
	EXPECT_ERR("self pid", pm_start(getpid(), 0), 0);
	EXPECT_ERR("self pid read", pm_read(getpid(), PIDMON_F_STOP, &u), 0);

	/* not ours: our parent and init */
	EXPECT_ERR("parent", pm_start(getppid(), 0), EPERM);
	EXPECT_ERR("init", pm_start(1, 0), EPERM);

	/* siblings: a is forked after b, so it knows b's pid */
	parent_pid = getpid();
	b = spawn(sleeper, NULL, &fd);
	get_byte(fd, 2000);
	sibling_pid = b;
	a = spawn(sibling_checks, NULL, &from);
	CHECK(get_byte(from, 5000) == 'k', "sibling checks failed in the child");
	waitpid(a, &st, 0);
	kill_reap(b);

	/* orphans: reparented to init, and to a subreaper */
	tree_checks(0);

	pid = fork();
	if (pid == 0) {		/* subreaper is process-wide, keep it out of the main test */
		failures = 0;
		tree_checks(1);
		_exit(failures ? 1 : 0);
	}
	waitpid(pid, &st, 0);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "subreaper variant failed, st=%x", st);

	(void)to;
	return test_end();
}

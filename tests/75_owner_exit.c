/* When an owner exits, every record it holds goes with it, whether its targets are alive,
 * dead, reaped or orphaned. Repeating that many times must never exhaust the 64-record cap. */
#include "common.h"

#define ROUNDS 6

static void full_owner(int in, int out)
{
	pid_t kids[PIDMON_MAX_RECS_FOR_TEST];
	int i, fd, bad = 0;

	(void)in;
	for (i = 0; i < PIDMON_MAX_RECS_FOR_TEST; i++) {
		kids[i] = spawn(sleeper, NULL, &fd);
		get_byte(fd, 2000);
		close(fd);
		if (pm_start(kids[i], 0))
			bad++;
	}
	if (pm_start(0, 0) != ENOSPC)		/* really full */
		bad++;
	for (i = 0; i < PIDMON_MAX_RECS_FOR_TEST; i += 3)
		kill(kids[i], SIGKILL);		/* some targets die, some are reaped, some linger */
	for (i = 0; i < PIDMON_MAX_RECS_FOR_TEST; i += 6)
		waitpid(kids[i], NULL, 0);
	put_byte(out, bad ? 'x' : 'k');
	/* the remaining children are killed with the owner; its records go with it */
	for (i = 0; i < PIDMON_MAX_RECS_FOR_TEST; i++)
		if (i % 3)
			kill(kids[i], SIGKILL);
}

int main(void)
{
	struct pidmon_usage u;
	int from, st, i, fd;
	pid_t o, c;

	test_begin("75_owner_exit");

	for (i = 0; i < ROUNDS; i++) {
		o = spawn(full_owner, NULL, &from);
		CHECK(get_byte(from, 20000) == 'k', "round %d: owner could not fill its cap", i);
		close(from);
		waitpid(o, &st, 0);
		CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "round %d: owner status %x", i, st);
	}

	/* the owner's pid is gone and nothing of it is left to read */
	EXPECT_ERR("read dead owner", pm_read(o, 0, &u), ENOENT);

	/* a new owner starts clean: no stale record from the dead owners */
	c = spawn(sleeper, NULL, &fd);
	get_byte(fd, 2000);
	close(fd);
	EXPECT_ERR("start fresh", pm_start(c, 0), 0);
	EXPECT_ERR("read fresh", pm_read(c, 0, &u), 0);
	CHECK(u.state == 0 && u.signal_sent == 0, "state=%x sig=%u", u.state, u.signal_sent);
	kill_reap(c);

	return test_end();
}
